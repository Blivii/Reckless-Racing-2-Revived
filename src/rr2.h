// Reckless Racing 2 on 64-bit: run the original 32-bit libRecklessRacing2.so under dynarmic.
// Guest memory is identity-mapped in the low 4 GB, so a guest address IS a host pointer.
#pragma once
#include <array>
#include <bit>
#include <cstdint>
#include <sys/stat.h>
#include <sys/types.h>
#include <cstring>
#include <string>
#include <initializer_list>
#include <type_traits>
#include <utility>

using u8 = uint8_t;
using u32 = uint32_t;

void rr2_log(const char* fmt, ...);
#define LOG(...) rr2_log(__VA_ARGS__)
int setting(const char* name, int fallback);  // main.cpp: saved value, else debug override, else fallback
void save_setting(const char* name, int value);

template <class T = void> inline T* ptr(u32 a) { return reinterpret_cast<T*>(uintptr_t(a)); }
inline u32 gaddr(const void* p) { return u32(uintptr_t(p)); }

// loader.cpp
bool load_guest(const char* path);
// obb.cpp: the game's data, served from inside our APK (entry "rr2/main.obb") when present
void find_game_data(const std::string& apk);
bool is_obb_path(const char* path);
int game_open(const char* path, int flags, int mode);  // these act like the libc call, except on the OBB
ssize_t game_read(int fd, void* buf, size_t n);
off_t game_lseek(int fd, off_t off, int whence);
int game_close(int fd);
int game_fstat(int fd, struct stat* s);
int game_stat(const char* path, struct stat* s);
void run_initializers();
u32 guest_sym(const char* name);  // exported guest symbol address, 0 if absent
u32 guest_exidx(int* count);      // .ARM.exidx for the unwinder
void* guest_malloc(size_t n);
void* guest_realloc(void* p, size_t n);
void guest_free(void* p);

void apply_patches(u32 base);     // patches.cpp: game-specific fixes, applied after relocation
u32 tick_rate();
void set_tick_rate(u32 hz);       // live; call from outside guest code
u32 grip_fix();                   // % of the 60 Hz sideways slip correction applied
void set_grip_fix(u32 percent);
u32 traction_fix();               // % of the 60 Hz forward slip correction applied
void set_traction_fix(u32 percent);
bool supersampling();             // 2x2 supersampled rendering
void set_supersampling(bool on);  // live, applied at the next frame boundary
void check_supersampling(int w, int h, int max_size);  // GPU limits, at the first GL call
void on_game_thread(void (*action)());  // queue from any thread...
void run_game_thread_queue();          // ...run at a JNI entry on the game thread (outside guest code)
// game thread only (queue them from the menu):
void save_game();                 // CGame::SaveGame
void cheat_cash(int amount);      // saves the game
void cheat_unlock_all();          // saves the game
int last_cash();                  // after the last save_game, -1 before
// save.cpp, game thread only: one backup slot inside the game's own files folder
void backup_save();
void restore_save();              // then exits: the game must not write its in-memory copy back
void reset_save();                // then exits
std::string backup_info();        // "none" or when it was made (any thread)
bool aa_rendering();              // the renderer is currently drawing via the 2x target
bool aa_target_bound();           // ...and that target is the current render target
void on_frame();                  // game thread, at each frame boundary
void set_rates(u32 hz, u32 grip, u32 traction, u32 drive, bool save);  // live tick rate + grip fix; outside guest code
// lab.cpp: physics lab, cycles physics configs during races and logs comparisons
void lab_sample(u32 body, float tick_seconds);  // per car per physics step
void lab_player(u32 actor);       // the player's car actor, from the input hook
void lab_frame();                 // game thread, every frame
bool lab();
void set_lab(bool on);            // game thread
void apply_substeps(int n);
std::string lab_label();          // "" when off; shown on screen

// input.cpp
bool remap_key(int& keycode);     // false = drop the key event
bool gamepad_mode();              // triggers drive, Start pauses, on-screen driving controls hidden
void set_gamepad_mode(bool on);

// overlay.cpp (Android)
void overlay_draw();                          // just before each frame is presented
bool overlay_touch(int x, int y, int packed); // true = consumed by the menu
void overlay_surface(int w, int h);

// cpu.cpp
u32 call_guest(u32 fn, std::initializer_list<u32> args = {});
template <class... A> u32 call(u32 fn, A... a) { return call_guest(fn, {u32(a)...}); }

// table.cpp — one table maps a name to the host code that replaces it
struct Ctx {
    std::array<u32, 16>& r;
    u32* ext;  // S0..S31 (VFP), for hooks placed inside game code
    float& s(int n) { return *reinterpret_cast<float*>(&ext[n]); }
    u32 arg(int i) const { return i < 4 ? r[i] : ptr<u32>(r[13])[i - 4]; }
    float f(int i) const { return std::bit_cast<float>(arg(i)); }  // softfp: floats ride in core regs
    void ret(u32 v) { r[0] = v; }
    void retf(float v) { r[0] = std::bit_cast<u32>(v); }
};
using Shim = void (*)(Ctx&);
struct Shims { Shims(std::initializer_list<std::pair<const char*, Shim>> list); };  // registers at startup
u32 thunk_for(const char* name);  // guest address that, when called, runs the host shim
u32 shim_index(const char* name); // the SVC number for a shim, to patch into game code
void dispatch(u32 index, Ctx c);
constexpr u32 kReturnSvc = 0xFFFFFF;  // SVC number that ends call_guest

// wrap<f>: a shim for any host function with a plain C signature of 32-bit-sized params
// (ints, floats, pointers into guest memory). Signed ints are sign-extended to host width.
template <class T> T from_guest(u32 v) {
    static_assert(!std::is_same_v<T, double>, "doubles need a hand-written shim");
    if constexpr (std::is_pointer_v<T>) return reinterpret_cast<T>(uintptr_t(v));
    else if constexpr (std::is_floating_point_v<T>) return std::bit_cast<float>(v);
    else if constexpr (std::is_signed_v<T>) return T(int32_t(v));
    else return T(v);
}
template <class T> u32 to_guest(T v) {
    if constexpr (std::is_floating_point_v<T>) return std::bit_cast<u32>(float(v));
    else if constexpr (std::is_pointer_v<T>) return gaddr(v);
    else return u32(v);
}
template <auto F> void wrap(Ctx& c) {
    [&]<class R, class... A>(R (*)(A...)) {
        [&]<size_t... I>(std::index_sequence<I...>) {
            if constexpr (std::is_void_v<R>) F(from_guest<A>(c.arg(I))...);
            else c.ret(to_guest(F(from_guest<A>(c.arg(I))...)));
        }(std::index_sequence_for<A...>{});
    }(F);
}

// Reads AAPCS variadic args, from registers/stack (Ctx, starting at arg index i) or from a
// guest va_list pointer. 8-byte values sit on 8-byte boundaries.
struct VaReader {
    Ctx* c;
    int i;
    u32 ap;
    u32 word() {
        if (c) return c->arg(i++);
        u32 v = *ptr<u32>(ap);
        return ap += 4, v;
    }
    uint64_t dword() {
        if (c) {
            i += i & 1;
            uint64_t lo = word();
            return lo | uint64_t(word()) << 32;
        }
        ap = (ap + 7) & ~7u;
        uint64_t v;
        memcpy(&v, ptr(ap), 8);
        return ap += 8, v;
    }
};
