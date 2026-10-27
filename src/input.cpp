// Gamepad mode. The game only understands keys: Android turns the analog stick into D-pad
// presses for apps that don't read joysticks, and the game drives with D-pad up/down as
// gas/brake. In gamepad mode:
//  - R2/L2 become gas/brake and the stick's up/down are dropped, so the stick only steers;
//  - Start becomes Back, which pauses;
//  - the on-screen driving controls (left/right/wheel/gas/brake) are not drawn. Respawn and
//    pause stay. The buttons are a per-steering-mode list from the game database; we note
//    which are driving controls as they are created, and hide those in the draw loop.
#include <cstring>
#include <mutex>
#include <set>
#include <unordered_map>
#include "rr2.h"

enum { kBack = 4, kDpadUp = 19, kDpadDown = 20, kButtonL2 = 104, kButtonR2 = 105, kButtonStart = 108 };

static int g_gamepad = -1;  // loaded on first use
bool gamepad_mode() { return g_gamepad < 0 ? (g_gamepad = setting("gamepad", 0)) : g_gamepad; }
void set_gamepad_mode(bool on) { g_gamepad = on, save_setting("gamepad", on); }

bool remap_key(int& code) {
    static std::set<int> seen;
    static std::mutex lock;
    if (std::lock_guard lk(lock); seen.insert(code).second) LOG("key code %d", code);
    if (!gamepad_mode()) return true;
    if (code == kButtonR2) return code = kDpadUp, true;      // gas
    if (code == kButtonL2) return code = kDpadDown, true;    // brake
    if (code == kButtonStart) return code = kBack, true;     // pause
    return code != kDpadUp && code != kDpadDown;             // stick up/down: steering only
}

static std::unordered_map<u32, bool> g_driving;  // CGameButton* -> is a driving control (game thread)

static Shims hooks = {
    // CGameUIButtons::CreateButton, just after "new CGameButton": r4 = the button, and the
    // button's database name is a short bite::TString on the stack at sp+24 (length, then
    // inline characters at +8). Replaces "ldr r3, [r4]".
    {"hook:CreateButton", [](Ctx& c) {
         c.r[3] = *ptr<u32>(c.r[4]);
         u32 s = c.r[13] + 24;
         const char* name = *ptr<int16_t>(s) <= 32 ? ptr<char>(s + 8) : ptr<char>(*ptr<u32>(s + 8) + 4);
         bool driving = false;
         for (const char* n : {"left", "right", "wheel", "gas", "brake"}) driving |= !strcmp(name, n);
         g_driving[c.r[4]] = driving;  // overwritten on every creation, so reused memory is safe
     }},
    // CGameUIButtons::Draw's loop reads each button's hidden flag. Replaces
    // "ldrb r12, [r0, #0x54]": also report driving controls as hidden in gamepad mode.
    {"hook:ButtonDraw", [](Ctx& c) {
         c.r[12] = *ptr<u8>(c.r[0] + 0x54);
         if (!c.r[12] && gamepad_mode()) {
             auto it = g_driving.find(c.r[0]);
             c.r[12] = it != g_driving.end() && it->second;
         }
     }},
};
