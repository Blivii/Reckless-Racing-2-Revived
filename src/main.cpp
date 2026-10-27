// Entry point. On Android, Java's System.loadLibrary("RecklessRacing2") lands here and we
// boot the original 32-bit library (shipped beside us as libguest.so). On a PC, a small
// test harness does the same from the command line.
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include "rr2.h"

// Settings: the overlay menu saves to a small "name=value" file in the app's data folder.
// With no saved value, a debug override (Android property / PC env var) or the default applies.
static std::string override_value(const char* name);
#ifdef __ANDROID__
static const char* kSettingsFile = "/data/data/com.polarbit.RecklessRacing2/files/rr2.cfg";
#else
static const char* kSettingsFile = "rr2.cfg";
#endif

static std::map<std::string, int> load_settings() {
    std::map<std::string, int> m;
    if (FILE* f = fopen(kSettingsFile, "r")) {
        char k[64];
        int v;
        while (fscanf(f, " %63[^=]=%d", k, &v) == 2) m[k] = v;
        fclose(f);
    }
    return m;
}

int setting(const char* name, int fallback) {
    auto m = load_settings();
    if (auto it = m.find(name); it != m.end()) return it->second;
    std::string v = override_value(name);
    return v.empty() ? fallback : atoi(v.c_str());
}

void save_setting(const char* name, int value) {
    auto m = load_settings();
    m[name] = value;
    if (FILE* f = fopen(kSettingsFile, "w")) {
        for (auto& [k, v] : m) fprintf(f, "%s=%d\n", k.c_str(), v);
        fclose(f);
    }
}

#ifdef __ANDROID__
#include <android/log.h>
#include <dlfcn.h>
#include <jni.h>
#include <sys/system_properties.h>
#include <unistd.h>

void rr2_log(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    __android_log_vprint(ANDROID_LOG_INFO, "RR2", fmt, ap);
    va_end(ap);
}

static std::string override_value(const char* name) {
    char v[PROP_VALUE_MAX] = {};
    __system_property_get((std::string("debug.rr2.") + name).c_str(), v);
    return v;
}

void jni_init(JavaVM* vm);

extern "C" JNIEXPORT jint JNI_OnLoad(JavaVM* vm, void*) {
    Dl_info info;
    dladdr((void*)&JNI_OnLoad, &info);
    std::string dir = info.dli_fname;
    dir.resize(dir.rfind('/'));
    LOG("page size %ld", sysconf(_SC_PAGESIZE));  // 16384 on 16 KB-page kernels
    if (!load_guest((dir + "/libguest.so").c_str())) return JNI_ERR;
    find_game_data(dir.substr(0, dir.rfind("/lib/")) + "/base.apk");  // <app>/lib/arm64 -> <app>/base.apk
    run_initializers();
    jni_init(vm);
    return JNI_VERSION_1_4;
}
#else
void rr2_log(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

static std::string override_value(const char* name) {
    const char* v = getenv((std::string("RR2_") + name).c_str());
    return v ? v : "";
}

int main(int argc, char** argv) {
    if (argc < 2) return fprintf(stderr, "usage: %s libRecklessRacing2.so\n", argv[0]), 1;
    if (!load_guest(argv[1])) return 1;
    run_initializers();
    return 0;
}
#endif
