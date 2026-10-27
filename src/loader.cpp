// Maps the 32-bit ELF into a low-4GB region at its own addresses, relocates it,
// and serves the rest of the region as the guest heap.
#include <elf.h>
#include <sys/mman.h>
#include <algorithm>
#include <fstream>
#include <iterator>
#include <mutex>
#include <vector>
#include "rr2.h"

static size_t g_region_size;  // image + heap: 1 GB, less if the low 4 GB is crowded
static u8* g_region;
static u32 g_base, g_symtab, g_strtab, g_nsyms, g_init, g_ninit, g_exidx, g_nexidx;

static void heap_init(u8* begin, u8* end);
static const Elf32_Sym* sym(u32 i) { return ptr<Elf32_Sym>(g_symtab) + i; }
static const char* sym_name(u32 i) { return ptr<char>(g_strtab + sym(i)->st_name); }

static bool reserve_region() {
    for (size_t size : {1u << 30, 1u << 29, 1u << 28})
        for (uintptr_t hint = 0x40000000; hint + size <= 0x100000000; hint += 0x10000000) {
            void* p = mmap((void*)hint, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
            if (p == (void*)hint) {
                if (size != 1u << 30) LOG("low memory is crowded: guest region %zu MB", size >> 20);
                return g_region = (u8*)p, g_region_size = size, true;
            }
            if (p != MAP_FAILED) munmap(p, size);
        }
    return false;
}

static void relocate(u32 table, u32 size) {
    for (auto* r = ptr<Elf32_Rel>(table); size; ++r, size -= sizeof(Elf32_Rel)) {
        u32* where = ptr<u32>(g_base + r->r_offset);
        u32 s = ELF32_R_SYM(r->r_info);
        u32 S = !s ? 0 : sym(s)->st_shndx != SHN_UNDEF ? g_base + sym(s)->st_value : thunk_for(sym_name(s));
        switch (ELF32_R_TYPE(r->r_info)) {
        case R_ARM_RELATIVE: *where += g_base; break;
        case R_ARM_ABS32: *where += S; break;
        case R_ARM_GLOB_DAT:
        case R_ARM_JUMP_SLOT: *where = S; break;
        default: LOG("unhandled relocation type %u", ELF32_R_TYPE(r->r_info));
        }
    }
}

bool load_guest(const char* path) {
    std::ifstream f(path, std::ios::binary);
    std::vector<u8> file{std::istreambuf_iterator<char>(f), {}};
    auto* eh = (Elf32_Ehdr*)file.data();
    if (file.size() < sizeof(*eh) || memcmp(eh->e_ident, ELFMAG, SELFMAG) || eh->e_machine != EM_ARM)
        return LOG("not a 32-bit ARM ELF: %s", path), false;
    if (!reserve_region()) return LOG("could not reserve low-4GB region"), false;
    g_base = gaddr(g_region);

    u32 rel = 0, relsz = 0, jmprel = 0, jmprelsz = 0, image_end = 0;
    auto* ph = (Elf32_Phdr*)(file.data() + eh->e_phoff);
    for (int i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type == PT_LOAD) {
            memcpy(g_region + ph[i].p_vaddr, file.data() + ph[i].p_offset, ph[i].p_filesz);
            image_end = std::max(image_end, ph[i].p_vaddr + ph[i].p_memsz);
        }
        if (ph[i].p_type == PT_ARM_EXIDX) g_exidx = g_base + ph[i].p_vaddr, g_nexidx = ph[i].p_memsz / 8;
    }
    for (int i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_DYNAMIC) continue;
        for (auto* d = ptr<Elf32_Dyn>(g_base + ph[i].p_vaddr); d->d_tag != DT_NULL; d++) {
            u32 v = d->d_un.d_val;
            switch (d->d_tag) {
            case DT_SYMTAB: g_symtab = g_base + v; break;
            case DT_STRTAB: g_strtab = g_base + v; break;
            case DT_HASH: g_nsyms = ptr<u32>(g_base + v)[1]; break;  // nchain == symbol count
            case DT_REL: rel = g_base + v; break;
            case DT_RELSZ: relsz = v; break;
            case DT_JMPREL: jmprel = g_base + v; break;
            case DT_PLTRELSZ: jmprelsz = v; break;
            case DT_INIT_ARRAY: g_init = g_base + v; break;
            case DT_INIT_ARRAYSZ: g_ninit = v / 4; break;
            }
        }
    }
    // Heap starts on the next 1 MB boundary after the image.
    heap_init(g_region + ((image_end + 0xFFFFF) & ~0xFFFFFu), g_region + g_region_size);
    relocate(rel, relsz);
    relocate(jmprel, jmprelsz);
    apply_patches(g_base);
    LOG("loaded %s at 0x%08x (%u symbols)", path, g_base, g_nsyms);
    return true;
}

void run_initializers() {
    for (u32 i = 0; i < g_ninit; i++)
        if (u32 fn = ptr<u32>(g_init)[i]; fn && fn != ~0u) call_guest(fn);
    LOG("ran %u initializers", g_ninit);
}

u32 guest_sym(const char* name) {
    for (u32 i = 1; i < g_nsyms; i++)
        if (sym(i)->st_shndx != SHN_UNDEF && !strcmp(sym_name(i), name)) return g_base + sym(i)->st_value;
    return 0;
}

u32 guest_exidx(int* count) { return *count = g_nexidx, g_exidx; }

// Guest heap: power-of-two size classes with free lists. Wasteful but tiny; a 2012
// game sized for 1 GB phones fits comfortably.
static u8 *g_heap, *g_heap_end;
static void* g_free[32];
static std::mutex g_heap_lock;

static void heap_init(u8* begin, u8* end) { g_heap = begin, g_heap_end = end; }

static u32 size_class(size_t n) { return std::bit_width(std::max<size_t>(n + 15, 32) - 1); }

void* guest_malloc(size_t n) {
    std::lock_guard lk(g_heap_lock);
    u32 c = size_class(n);
    u8* b = (u8*)g_free[c];
    if (b) g_free[c] = *(void**)(b + 16);
    else if (g_heap + (size_t(1) << c) <= g_heap_end) b = g_heap, g_heap += size_t(1) << c;
    else return LOG("guest heap exhausted (%zu bytes)", n), nullptr;
    *(u32*)b = c;
    return b + 16;  // 16-byte header keeps blocks 16-aligned
}

void guest_free(void* p) {
    if (!p) return;
    std::lock_guard lk(g_heap_lock);
    u8* b = (u8*)p - 16;
    *(void**)p = g_free[*(u32*)b], g_free[*(u32*)b] = b;
}

void* guest_realloc(void* p, size_t n) {
    if (!p) return guest_malloc(n);
    size_t cap = (size_t(1) << *(u32*)((u8*)p - 16)) - 16;
    if (n <= cap) return p;
    void* q = guest_malloc(n);
    if (q) memcpy(q, p, cap), guest_free(p);
    return q;
}
