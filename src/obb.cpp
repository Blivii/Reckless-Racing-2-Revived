// The game's data (OBB) lives inside our own APK: the patcher stores it uncompressed as
// "rr2/main.obb". When the game opens its hard-coded OBB path, we open the APK instead and
// present that entry's byte range as the whole file: positions, reads and sizes are offset and
// clamped to it. Without the entry, the game's own path (Android/obb) is used as before.
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <string>
#include <vector>
#include "rr2.h"

static const char kGamePath[] = "/main.103.com.polarbit.RecklessRacing2.obb";  // the end of it
static const char kEntry[] = "rr2/main.obb";
static std::string g_apk;
static off_t g_base = -1, g_size;                // the entry's data in the APK
static std::atomic<bool> g_is_obb[1 << 16];      // per fd: opened through the OBB path

static u32 le16(const u8* p) { return p[0] | p[1] << 8; }
static u32 le32(const u8* p) { return le16(p) | le16(p + 2) << 16; }

// Finds the uncompressed entry via the zip central directory.
static bool find_entry(int fd) {
    struct stat st;
    if (fstat(fd, &st) != 0) return false;
    std::vector<u8> tail(std::min<off_t>(st.st_size, 0xFFFF + 22));
    off_t tail_at = st.st_size - off_t(tail.size());
    if (pread(fd, tail.data(), tail.size(), tail_at) != ssize_t(tail.size())) return false;
    for (size_t i = tail.size() - 21; i-- > 0;) {  // end-of-central-directory record, from the end
        if (le32(&tail[i]) != 0x06054b50) continue;
        std::vector<u8> cd(le32(&tail[i + 12]));
        if (pread(fd, cd.data(), cd.size(), le32(&tail[i + 16])) != ssize_t(cd.size())) return false;
        for (size_t p = 0; p + 46 <= cd.size() && le32(&cd[p]) == 0x02014b50;
             p += 46 + le16(&cd[p + 28]) + le16(&cd[p + 30]) + le16(&cd[p + 32])) {
            std::string name((const char*)&cd[p + 46], le16(&cd[p + 28]));
            if (name != kEntry || le16(&cd[p + 10]) != 0) continue;  // must be stored, not deflated
            u8 local[30];
            off_t at = le32(&cd[p + 42]);
            if (pread(fd, local, 30, at) != 30) return false;
            g_base = at + 30 + le16(local + 26) + le16(local + 28);
            g_size = le32(&cd[p + 24]);
            return true;
        }
        return false;
    }
    return false;
}

void find_game_data(const std::string& apk) {
    static const char kOld[] = "/sdcard/Android/obb/com.polarbit.RecklessRacing2/main.103.com.polarbit.RecklessRacing2.obb";
    int fd = open(apk.c_str(), O_RDONLY);
    bool found = fd >= 0 && find_entry(fd);
    if (fd >= 0) close(fd);
    if (found) g_apk = apk, LOG("game data: %s in %s (%lld MB)", kEntry, apk.c_str(), (long long)g_size >> 20);
    else if (access(kOld, R_OK) == 0) LOG("game data: %s", kOld);
    else LOG("GAME DATA MISSING: patch the APK with the OBB, or put the OBB at %s", kOld);
}

bool is_obb_path(const char* p) {
    size_t n = strlen(p), k = sizeof kGamePath - 1;
    return g_base >= 0 && n >= k && !strcmp(p + n - k, kGamePath);
}
static bool is_obb(int fd) { return fd >= 0 && fd < (1 << 16) && g_is_obb[fd]; }

int game_open(const char* path, int flags, int mode) {
    if (!is_obb_path(path)) return open(path, flags, mode);
    int fd = open(g_apk.c_str(), O_RDONLY | (flags & O_CLOEXEC));
    if (fd < 0) return fd;
    if (fd >= (1 << 16)) return close(fd), errno = EMFILE, -1;
    lseek(fd, g_base, SEEK_SET);
    g_is_obb[fd] = true;
    return fd;
}

off_t game_lseek(int fd, off_t off, int whence) {
    if (!is_obb(fd)) return lseek(fd, off, whence);
    off_t r = whence == SEEK_SET ? lseek(fd, g_base + off, SEEK_SET)
            : whence == SEEK_END ? lseek(fd, g_base + g_size + off, SEEK_SET)
            : lseek(fd, off, whence);
    return r < 0 ? r : r - g_base;
}

ssize_t game_read(int fd, void* buf, size_t n) {
    if (!is_obb(fd)) return read(fd, buf, n);
    off_t at = lseek(fd, 0, SEEK_CUR), left = g_base + g_size - at;  // never read past the entry
    return read(fd, buf, size_t(std::clamp<off_t>(left, 0, off_t(n))));
}

int game_close(int fd) {
    if (is_obb(fd)) g_is_obb[fd] = false;
    return close(fd);
}

int game_fstat(int fd, struct stat* s) {
    int r = fstat(fd, s);
    if (r == 0 && is_obb(fd)) s->st_size = g_size;
    return r;
}

int game_stat(const char* path, struct stat* s) {
    if (!is_obb_path(path)) return stat(path, s);
    int r = stat(g_apk.c_str(), s);
    if (r == 0) s->st_size = g_size;
    return r;
}
