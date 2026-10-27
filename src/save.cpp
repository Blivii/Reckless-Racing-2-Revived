// The menu's "Save data" page: one backup slot inside the game's own files folder, so it survives
// updates and goes along with Android's automatic backup. Everything in that folder except our own
// files counts as the save (the game keeps content.sav, lb.sav, ... there).
#include <sys/stat.h>
#include <unistd.h>
#include <ctime>
#include <filesystem>
#include "rr2.h"

namespace fs = std::filesystem;
#ifdef __ANDROID__
static const fs::path kDir = "/data/data/com.polarbit.RecklessRacing2/files";
#else
static const fs::path kDir = ".";
#endif
static const fs::path kSlot = kDir / "rr2-backup";

static bool ours(const fs::path& p) { return p.filename() == "rr2.cfg" || p.filename() == "rr2-backup"; }

static void clear(const fs::path& dir) {
    std::error_code ec;
    for (auto& e : fs::directory_iterator(dir, ec)) if (!ours(e.path())) fs::remove_all(e.path(), ec);
}

static int copy_all(const fs::path& from, const fs::path& to) {
    std::error_code ec;
    int n = 0;
    for (auto& e : fs::directory_iterator(from, ec)) {
        if (ours(e.path())) continue;
        fs::copy(e.path(), to / e.path().filename(), fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
        if (ec) return LOG("save data: copying %s failed: %s", e.path().c_str(), ec.message().c_str()), -1;
        LOG("save data: %s", e.path().filename().c_str());
        n++;
    }
    return n;
}

void backup_save() {
    save_game();  // the game's latest state on disk first
    std::error_code ec;
    fs::remove_all(kSlot, ec);
    fs::create_directories(kSlot, ec);
    LOG("save data: backed up %d item(s)", copy_all(kDir, kSlot));
}

void restore_save() {
    if (!fs::exists(kSlot)) return LOG("save data: no backup to restore");
    clear(kDir);
    LOG("save data: restored %d item(s), closing the game", copy_all(kSlot, kDir));
    _exit(0);
}

void reset_save() {
    clear(kDir);
    LOG("save data: reset, closing the game");
    _exit(0);
}

std::string backup_info() {
    struct stat st;
    if (stat(kSlot.c_str(), &st) != 0) return "none";
    char buf[32];
    strftime(buf, sizeof buf, "%b %d %H:%M", localtime(&st.st_mtime));
    return buf;
}
