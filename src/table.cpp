// The one table: name -> host shim. Guest calls reach it through an 8-byte thunk
// ("svc #index; bx lr"). Names without a shim log once and return 0.
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include "rr2.h"

static std::unordered_map<std::string, Shim>& registry() {
    static std::unordered_map<std::string, Shim> m;
    return m;
}
Shims::Shims(std::initializer_list<std::pair<const char*, Shim>> list) {
    for (auto& [name, fn] : list) registry()[name] = fn;
}

struct Entry { std::string name; Shim fn; bool warned; };
static constexpr size_t kMaxEntries = 4096;  // reserved up front so dispatch never sees a realloc
static std::vector<Entry> g_entries = [] { std::vector<Entry> v; v.reserve(kMaxEntries); return v; }();
static std::unordered_map<std::string, u32> g_thunks;
static std::mutex g_lock;  // dlsym can run on the game thread while others dispatch

static std::unordered_map<std::string, u32> g_index;

static u32 index_locked(const char* name) {
    if (auto it = g_index.find(name); it != g_index.end()) return it->second;
    if (g_entries.size() == kMaxEntries) return LOG("shim table full at %s", name), 0;
    auto shim = registry().find(name);
    g_entries.push_back({name, shim == registry().end() ? nullptr : shim->second, false});
    return g_index[name] = u32(g_entries.size() - 1);
}

u32 shim_index(const char* name) {
    std::lock_guard lk(g_lock);
    return index_locked(name);
}

u32 thunk_for(const char* name) {
    std::lock_guard lk(g_lock);
    if (auto it = g_thunks.find(name); it != g_thunks.end()) return it->second;
    u32* t = (u32*)guest_malloc(8);
    t[0] = 0xEF000000 | index_locked(name);  // svc #index
    t[1] = 0xE12FFF1E;                       // bx lr
    return g_thunks[name] = gaddr(t);
}

void dispatch(u32 index, Ctx c) {
    Entry& e = g_entries[index];
    if (e.fn) return e.fn(c);
    if (!e.warned) e.warned = true, LOG("unimplemented: %s", e.name.c_str());
    c.ret(0);
}
