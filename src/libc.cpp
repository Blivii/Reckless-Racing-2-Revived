// libc for the guest. Most calls pass straight through via wrap<>; the hand-written ones
// translate 32-bit struct layouts (stat, timeval, tm), opaque host pointers (FILE*, DIR*),
// varargs (printf family) or keep the game offline (sockets).
#include <dirent.h>
#include <fcntl.h>
#include <sched.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#include <cerrno>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <string>
#include "rr2.h"

template <class T> static void put(u32 g, u32 off, T v) { memcpy(ptr<u8>(g + off), &v, sizeof v); }

static u32 guest_strdup(const char* s) {
    size_t n = strlen(s) + 1;
    return gaddr(memcpy(guest_malloc(n), s, n));
}

// Opaque host handles (FILE*, DIR*) live in a small guest cell; the cell is the guest's handle.
template <class T> static T& cell(u32 g) { return *ptr<T>(g); }

// ARM32 bionic struct stat (104 bytes)
static void put_stat(u32 g, const struct stat& s) {
    memset(ptr(g), 0, 104);
    put<uint64_t>(g, 0, s.st_dev), put<u32>(g, 12, u32(s.st_ino)), put<u32>(g, 16, s.st_mode);
    put<u32>(g, 20, u32(s.st_nlink)), put<u32>(g, 24, s.st_uid), put<u32>(g, 28, s.st_gid);
    put<uint64_t>(g, 32, s.st_rdev), put<int64_t>(g, 48, s.st_size), put<u32>(g, 56, u32(s.st_blksize));
    put<uint64_t>(g, 64, s.st_blocks), put<u32>(g, 72, u32(s.st_atime)), put<u32>(g, 80, u32(s.st_mtime));
    put<u32>(g, 88, u32(s.st_ctime)), put<uint64_t>(g, 96, s.st_ino);
}
static int do_stat(int r, const struct stat& s, u32 g) { return r == 0 ? put_stat(g, s), 0 : r; }

// ARM32 struct tm: 9 ints, long tm_gmtoff, const char* tm_zone (44 bytes), one per thread
static u32 put_tm(const struct tm* t) {
    thread_local u32 g = gaddr(guest_malloc(44));
    if (!t) return 0;
    const int f[9] = {t->tm_sec, t->tm_min, t->tm_hour, t->tm_mday, t->tm_mon,
                      t->tm_year, t->tm_wday, t->tm_yday, t->tm_isdst};
    memcpy(ptr(g), f, sizeof f), put<int32_t>(g, 36, int32_t(t->tm_gmtoff)), put<u32>(g, 40, 0);
    return g;
}

// printf with guest varargs: walk the format, hand each conversion to the host snprintf.
static std::string format(const char* f, VaReader va) {
    std::string out;
    char buf[1024];
    while (*f) {
        if (*f != '%') { out += *f++; continue; }
        std::string spec = "%";
        for (f++; *f && strchr("-+ #0123456789.*", *f); f++)
            spec += *f == '*' ? std::to_string(int32_t(va.word())) : std::string(1, *f);
        int longs = 0;
        for (; *f && strchr("hlLqjzt", *f); f++) longs += (*f == 'l' || *f == 'q' || *f == 'j');
        char conv = *f ? *f++ : '%';
        switch (conv) {
        case 'd': case 'i': case 'u': case 'x': case 'X': case 'o': case 'c':
            if (longs >= 2) snprintf(buf, sizeof buf, (spec + "ll" + conv).c_str(), (long long)va.dword());
            else snprintf(buf, sizeof buf, (spec + conv).c_str(), int(va.word()));
            break;
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A':
            snprintf(buf, sizeof buf, (spec + conv).c_str(), std::bit_cast<double>(va.dword()));
            break;
        case 's': { u32 s = va.word(); snprintf(buf, sizeof buf, (spec + 's').c_str(), s ? ptr<char>(s) : "(null)"); break; }
        case 'p': snprintf(buf, sizeof buf, "0x%x", va.word()); break;
        default: snprintf(buf, sizeof buf, "%%"); break;
        }
        out += buf;
    }
    return out;
}

static thread_local u32 t_errno = gaddr(guest_malloc(4));
static void offline(Ctx& c) { errno = ENETUNREACH, c.ret(u32(-1)); }

static Shims libc = {
    // memory and strings
    {"malloc", [](Ctx& c) { c.ret(gaddr(guest_malloc(c.arg(0)))); }},
    {"free", [](Ctx& c) { guest_free(ptr(c.arg(0))); }},
    {"realloc", [](Ctx& c) { c.ret(gaddr(guest_realloc(ptr(c.arg(0)), c.arg(1)))); }},
    {"memcpy", wrap<memcpy>},
    {"memset", wrap<memset>},
    {"strcpy", wrap<strcpy>},
    // math (softfp: floats travel in core registers)
    {"sinf", wrap<sinf>}, {"cosf", wrap<cosf>}, {"tanf", wrap<tanf>}, {"asinf", wrap<asinf>},
    {"acosf", wrap<acosf>}, {"atanf", wrap<atanf>}, {"sqrtf", wrap<sqrtf>}, {"atan2f", wrap<atan2f>},
    {"fmodf", wrap<fmodf>},
    // files (O_* flags and errno values match between ARM32 and ARM64 Linux)
    // open/read/lseek/close/stat go through obb.cpp, which serves the OBB from inside our APK
    {"open", wrap<game_open>},  // engine probes for loose files: misses are normal
    {"read", wrap<game_read>}, {"write", wrap<write>}, {"close", wrap<game_close>}, {"lseek", wrap<game_lseek>},
    {"fcntl", [](Ctx& c) { c.ret(u32(fcntl(int(c.arg(0)), int(c.arg(1)), c.arg(2)))); }},
    {"mkdir", wrap<mkdir>}, {"rmdir", wrap<rmdir>}, {"remove", wrap<remove>}, {"rename", wrap<rename>},
    {"stat", [](Ctx& c) { struct stat s; c.ret(u32(do_stat(game_stat(ptr<char>(c.arg(0)), &s), s, c.arg(1)))); }},
    {"fstat", [](Ctx& c) { struct stat s; c.ret(u32(do_stat(game_fstat(int(c.arg(0)), &s), s, c.arg(1)))); }},
    {"fopen", [](Ctx& c) {
         FILE* f = fopen(ptr<char>(c.arg(0)), ptr<char>(c.arg(1)));
         if (!f) return c.ret(0);
         u32 g = gaddr(guest_malloc(8));
         cell<FILE*>(g) = f, c.ret(g);
     }},
    {"fclose", [](Ctx& c) { c.ret(u32(fclose(cell<FILE*>(c.arg(0))))), guest_free(ptr(c.arg(0))); }},
    {"fwrite", [](Ctx& c) { c.ret(u32(fwrite(ptr(c.arg(0)), c.arg(1), c.arg(2), cell<FILE*>(c.arg(3))))); }},
    {"fflush", [](Ctx& c) { c.ret(u32(fflush(c.arg(0) ? cell<FILE*>(c.arg(0)) : nullptr))); }},
    {"opendir", [](Ctx& c) {
         DIR* d = opendir(ptr<char>(c.arg(0)));
         if (!d) return c.ret(0);
         u32 g = gaddr(guest_malloc(8 + sizeof(dirent)));  // dirent layout matches ARM32 bionic
         cell<DIR*>(g) = d, c.ret(g);
     }},
    {"readdir", [](Ctx& c) {
         dirent* e = readdir(cell<DIR*>(c.arg(0)));
         c.ret(e ? gaddr(memcpy(ptr(c.arg(0) + 8), e, sizeof *e)) : 0);
     }},
    {"closedir", [](Ctx& c) { c.ret(u32(closedir(cell<DIR*>(c.arg(0))))), guest_free(ptr(c.arg(0))); }},
    // time (time_t and long are 32-bit in the guest)
    {"time", [](Ctx& c) { u32 t = u32(time(nullptr)); if (c.arg(0)) put(c.arg(0), 0, t); c.ret(t); }},
    {"gettimeofday", [](Ctx& c) {
         timeval tv;
         gettimeofday(&tv, nullptr);
         put<u32>(c.arg(0), 0, u32(tv.tv_sec)), put<u32>(c.arg(0), 4, u32(tv.tv_usec)), c.ret(0);
     }},
    {"gmtime", [](Ctx& c) { time_t t = *ptr<int32_t>(c.arg(0)); c.ret(put_tm(gmtime(&t))); }},
    {"localtime", [](Ctx& c) { time_t t = *ptr<int32_t>(c.arg(0)); c.ret(put_tm(localtime(&t))); }},
    {"usleep", wrap<usleep>},
    // environment
    {"getenv", [](Ctx& c) { const char* v = getenv(ptr<char>(c.arg(0))); c.ret(v ? guest_strdup(v) : 0); }},
    {"setenv", wrap<setenv>},
    // printf family
    {"sprintf", [](Ctx& c) {
         std::string s = format(ptr<char>(c.arg(1)), {&c, 2, 0});
         strcpy(ptr<char>(c.arg(0)), s.c_str()), c.ret(u32(s.size()));
     }},
    {"vsnprintf", [](Ctx& c) {
         std::string s = format(ptr<char>(c.arg(2)), {nullptr, 0, c.arg(3)});
         if (u32 n = c.arg(1)) memcpy(ptr(c.arg(0)), s.c_str(), std::min<size_t>(s.size() + 1, n)), ptr<char>(c.arg(0))[n - 1] = 0;
         c.ret(u32(s.size()));
     }},
    {"__android_log_print", [](Ctx& c) {
         LOG("[%s] %s", ptr<char>(c.arg(1)), format(ptr<char>(c.arg(2)), {&c, 3, 0}).c_str()), c.ret(0);
     }},
    // threads: a guest pthread_mutex_t is one int; a yielding spinlock on it is enough here
    {"pthread_mutex_init", [](Ctx& c) { *ptr<int>(c.arg(0)) = 0, c.ret(0); }},
    {"pthread_mutex_destroy", [](Ctx& c) { c.ret(0); }},
    {"pthread_mutex_lock", [](Ctx& c) {
         for (int expected = 0; !__atomic_compare_exchange_n(ptr<int>(c.arg(0)), &expected, 1, false,
                                                             __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
              expected = 0)
             sched_yield();
         c.ret(0);
     }},
    {"pthread_mutex_unlock", [](Ctx& c) { __atomic_store_n(ptr<int>(c.arg(0)), 0, __ATOMIC_RELEASE), c.ret(0); }},
    // runtime
    {"__errno", [](Ctx& c) { *ptr<int>(t_errno) = errno, c.ret(t_errno); }},
    {"__aeabi_atexit", [](Ctx& c) { c.ret(0); }},  // static destructors never need to run
    {"__gnu_Unwind_Find_exidx", [](Ctx& c) { int n; c.ret(guest_exidx(&n)); *ptr<int>(c.arg(1)) = n; }},
    {"abort", [](Ctx&) { LOG("guest called abort()"), abort(); }},
    {"raise", [](Ctx& c) { LOG("guest raised signal %u", c.arg(0)), abort(); }},
    // network: the game's servers are long gone, so fail every socket call cleanly
    {"socket", offline}, {"connect", offline},
    {"bind", offline}, {"listen", offline},
    {"accept", offline}, {"send", offline},
    {"sendto", offline}, {"recv", offline},
    {"recvfrom", offline}, {"setsockopt", offline},
    {"getsockopt", offline}, {"getsockname", offline},
    {"getpeername", offline}, {"shutdown", offline},
    {"ioctl", offline},
    {"select", [](Ctx& c) { c.ret(0); }},       // "nothing ready" (also covers select-as-sleep)
    {"getaddrinfo", [](Ctx& c) { c.ret(4); }},  // EAI_FAIL
    {"gai_strerror", [](Ctx& c) { static u32 s = guest_strdup("offline"); c.ret(s); }},
};
