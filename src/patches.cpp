// Game-specific fixes to the original code, applied once after relocation (before the JIT
// has compiled anything). Each patch checks the word it replaces, so a different build of
// the game is left untouched rather than corrupted.
#include <algorithm>
#include <atomic>
#include <cmath>
#include <ctime>
#include <cstdio>
#include <map>
#include <mutex>
#include <vector>
#include <string>
#include <unistd.h>
#include "rr2.h"

// Frame rate. The engine is fixed-timestep: CPlatformFUSE keeps the tick in ms at +0x1ac
// (re-read every frame by MainLoop) and in seconds at +0x1a8. Stock is 30 Hz.
//
// Grip and traction. CCarConstraint::ComputeSolve takes the tyre contact's ground-plane
// displacement for this step ("slip", s22..s24) and applies friction capped per step plus the
// drive, with no time factor, so at 60 Hz the car slides more and accelerates more slowly.
// The hook scales the slip into 30 Hz-step units (x k, k = 33 ms / tick), separately for its
// sideways part (k^(grip%/100)) and its forward part (k^(traction%/100)): one factor can't match
// both. Physics lab, same track: sideways slide matches the original near grip 40%, the
// 10 -> 20 units/s time needs traction above 60% (50% took 2.7 s vs 2.0 s). At 30 Hz, k = 1.
// Drive. At 60 Hz the car (player and AI) accelerates ~20% slower at low speed, the same with
// traction 65/80 and with the brake force ([constraint +516]) scaled, so it isn't grip-limited:
// the drive itself ([constraint +512], set per tick in CCarActor::UpdateConstraint from gas x
// power x dt) comes out weaker. Top speed is a hard limit, unchanged. The hook scales the drive
// by drive% / 100 at 60 Hz. Physics lab, standing starts on one track (0-10 / 10-20 units/s):
// original 1.15 / 1.82 s, 100% 1.38 / 2.20, 120% 1.39 / 2.10, 140% 1.06 / 1.60; AI 0-10 1.81 s
// original, 2.17 / 1.91 / 1.63. Matching falls at ~127% (AI) to ~131-135% (player): 130%.
static u32 g_hz = 30, g_grip = 40, g_traction = 65, g_drive = 130;
static float g_k_side = 1, g_k_fwd = 1, g_k_drive = 1;
static void set_rate_vars(u32 hz, u32 grip, u32 traction, u32 drive) {
    float k = 33.0f / (1000 / hz);
    g_hz = hz, g_grip = grip, g_traction = traction, g_drive = drive;
    g_k_side = std::pow(k, grip / 100.0f), g_k_fwd = std::pow(k, traction / 100.0f);
    g_k_drive = hz == 30 ? 1.0f : drive / 100.0f;
}

static Shims hooks = {
    // replaces "vmls.f32 s24, s14, s15" at the point the slip vector (s22..s24) is complete
    {"hook:ComputeSolve slip", [](Ctx& c) {
         float product = c.s(14) * c.s(15);         // the instruction we replaced (unfused,
         c.s(24) -= product;                        // like the original VMLS)
         // slip per step -> per 30 Hz step, forward and sideways parts separately (r6 = body)
         const float* fwd = ptr<float>(c.r[6] + 224);
         const float* side = ptr<float>(c.r[6] + 200);
         float v[3] = {c.s(22), c.s(23), c.s(24)};
         float f = (g_k_fwd - 1) * (v[0] * fwd[0] + v[1] * fwd[1] + v[2] * fwd[2]);
         float sd = (g_k_side - 1) * (v[0] * side[0] + v[1] * side[1] + v[2] * side[2]);
         for (int i = 0; i < 3; i++) c.s(22 + i) = v[i] + fwd[i] * f + side[i] * sd;
         c.s(13) = c.s(23) * c.s(23);               // redo the length's first term (used old s23)
         lab_sample(c.r[6], (1000 / g_hz) / 1000.0f);  // r6 = this car's rigid body
     }},
    // replaces "vstr s15, [r5, #512]" in CCarActor::UpdateConstraint: the constraint's drive
    {"hook:Drive", [](Ctx& c) { *ptr<float>(c.r[5] + 512) = c.s(15) * g_k_drive; }},
    // replaces CHumanPlayer::SetInput's call to CCarActor::Action_SetInput, which only stores the
    // inputs: do the same stores here, and note which car is the player's (for the physics lab)
    {"hook:HumanInput", [](Ctx& c) {
         u32 a = c.r[0];
         u8 b5 = *ptr<u8>(c.r[13] + 4), b6 = *ptr<u8>(c.r[13] + 8);   // stack args 5 and 6
         *ptr<u32>(a + 1340) = c.r[1], *ptr<u32>(a + 1344) = c.r[2], *ptr<u32>(a + 1348) = c.r[3];  // steer, gas, brake
         *ptr<u8>(a + 0x548) = b5, *ptr<u8>(a + 0x549) = b6;
         if (!b5) *ptr<u8>(a + 0x58c) = 0;
         lab_player(a);
     }},
};

// Supersampling. The engine can render each frame into a 2x-width, 2x-height target and scale
// it down (CRenderGL2::SupportMSAA / AlwaysUseMSAA), but CEngineGameApp::Init only does so when
// the platform options ask for it, which Android's never do. We always create the target at
// init; whether frames use it is the renderer's "always" flag (+0x21034), checked by Begin()
// and End() every frame, so the menu can flip it live. The flip waits for the frame boundary
// so a frame never begins supersampled and ends without it.
static bool g_aa, g_aa_unsupported;
static std::atomic<int> g_game_tid{-1};  // set each frame; cheats run only on this thread
static std::atomic<int> g_aa_pending{-1};
static u32 g_base;

static u32 renderer() { return g_base ? *ptr<u32>(g_base + 0x56a764) : 0; }  // CRenderGL2::Get()'s global
bool aa_rendering() { u32 r = renderer(); return r && *ptr<u8>(r + 0x21034); }
bool aa_target_bound() {  // current render target (+0x21048) is the supersampling target (+0x21044)
    u32 r = renderer();
    return r && *ptr<u32>(r + 0x21044) && *ptr<u32>(r + 0x21048) == *ptr<u32>(r + 0x21044);
}

bool supersampling() { return g_aa; }
void set_supersampling(bool on) {
    if (g_aa_unsupported) return;
    g_aa = on, g_aa_pending = on, save_setting("aa", on);
}
// First GL call (gl.cpp): if a 2x2 target of the surface won't fit the GPU, undo the target patches
// (CEngineGameApp::Init runs after this) and keep supersampling off.
void check_supersampling(int w, int h, int max_size) {
    if (2 * w <= max_size && 2 * h <= max_size) return;
    g_aa_unsupported = true, g_aa = false;
    const u32 kTarget[][2] = {{0x26a474, 0xE5983030}, {0x26a488, 0xE5982030}, {0x26a490, 0xE5D83034}};
    for (auto& t : kTarget) *ptr<u32>(g_base + t[0]) = t[1];
    LOG("supersampling unavailable: %dx%d doubled exceeds GPU limit %d", w, h, max_size);
}
void on_frame() {  // game thread, between End() and the next Begin()
    g_game_tid = gettid();
    lab_frame();
    if (int p = g_aa_pending.exchange(-1); p >= 0 && renderer()) *ptr<u8>(renderer() + 0x21034) = u8(p);
}


u32 tick_rate() { return g_hz; }
u32 grip_fix() { return g_grip; }
u32 traction_fix() { return g_traction; }

void set_grip_fix(u32 percent) {
    set_rate_vars(g_hz, percent, g_traction, g_drive), save_setting("grip", int(percent));
    LOG("grip fix: %u%%", percent);
}
void set_traction_fix(u32 percent) {
    set_rate_vars(g_hz, g_grip, percent, g_drive), save_setting("traction", int(percent));
    LOG("traction fix: %u%%", percent);
}

// Menu actions that touch game state (cheats, save data). The overlay's taps arrive on the UI
// thread, so they only queue; the game thread runs the queue from its next OnEvent (outside guest
// code), so the game never sees its profile change underneath it.
static std::mutex g_queue_lock;
static std::vector<void (*)()> g_queue;
void on_game_thread(void (*action)()) { std::lock_guard l(g_queue_lock); g_queue.push_back(action); }
void run_game_thread_queue() {
    if (gettid() != g_game_tid) return;
    std::vector<void (*)()> q;
    { std::lock_guard l(g_queue_lock); q.swap(g_queue); }
    for (auto a : q) a();
}

// Game calls for the menu: the profile is *(Game() + 0x104); cheats use the same calls as the
// game's own debug commands (CGame::OnUserCommand's "give_cash" / "unlock_all"). Game thread only.
static int g_cash = -1;
int last_cash() { return g_cash; }
static u32 game() { return call(guest_sym("_Z4Gamev")); }
static u32 profile() {
    u32 g = game(), p = g ? *ptr<u32>(g + 0x104) : 0;
    if (!p) LOG("menu: no profile yet");
    return p;
}
void save_game() {
    if (u32 p = profile()) g_cash = int(call(guest_sym("_ZNK12CGameProfile13GetPlayerCashEv"), p));
    if (u32 g = game()) call(guest_sym("_ZN5CGame8SaveGameEv"), g);
}
void cheat_cash(int amount) {
    if (u32 p = profile()) call(guest_sym("_ZN12CGameProfile8GiveCashEib"), p, amount, 0), save_game(), LOG("cheat: +$%d, now $%d", amount, g_cash);
}
void cheat_unlock_all() {
    if (u32 p = profile()) call(guest_sym("_ZN12CGameProfile9UnlockAllEv"), p), save_game(), LOG("cheat: unlocked all");
}

// Live rate change (menu, physics lab). Must run outside guest code (it calls CPlatform::Get).
void set_rates(u32 hz, u32 grip, u32 traction, u32 drive, bool save) {
    static u32 platform;
    if (!platform) platform = call(guest_sym("_ZN4bite9CPlatform3GetEv"));
    if (!platform) return;
    u32 tick_ms = 1000 / hz;
    *ptr<u32>(platform + 0x1ac) = tick_ms;
    *ptr<float>(platform + 0x1a8) = tick_ms / 1000.0f;
    set_rate_vars(hz, grip, traction, drive);
    if (save) save_setting("tick", int(hz));
    LOG("game tick: %u Hz, grip fix %u%%, traction fix %u%%, drive %u%% (live)", hz, grip, traction, drive);
}
void set_tick_rate(u32 hz) { set_rates(hz, g_grip, g_traction, g_drive, true); }

void apply_patches(u32 base) {
    g_base = base;
    set_rate_vars(std::clamp(setting("tick", 60), 10, 240), std::clamp(setting("grip", 40), 0, 100),
                  std::clamp(setting("traction", 65), 0, 100), std::clamp(setting("drive", 130), 50, 200));
    g_aa = setting("aa", 0);
    LOG("game tick: %u Hz (%u ms), grip fix %u%%, traction fix %u%%, drive %u%%", g_hz, 1000 / g_hz, g_grip, g_traction, g_drive);
    const struct { u32 off, old, word; bool apply; const char* what; } kPatches[] = {
        // CPlatformFUSE::Init: tick = 1000 / rate.  ldr r1,[r4,#0x144] -> mov r1,#hz
        {0x1e4d34, 0xE5941144, 0xE3A01000 | g_hz, g_hz != 30, "game tick rate"},
        // always installed so the overlay can switch rates live (a no-op at 30 Hz)
        {0x40a8a0, 0xEE07CA67, 0xEF000000 | shim_index("hook:ComputeSolve slip"), true, "grip hook"},
        // CEngineGameApp::Init: options.msaa (+0x30) -> 2, so the supersampling target is created
        {0x26a474, 0xE5983030, 0xE3A03002, true, "supersampling target (enable)"},  // ldr r3,[r8,#0x30]
        {0x26a488, 0xE5982030, 0xE3A02002, true, "supersampling target (samples)"}, // ldr r2,[r8,#0x30]
        // options.always_msaa (+0x34) -> saved setting; the menu flips it live afterwards
        {0x26a490, 0xE5D83034, 0xE3A03000 | g_aa, true, "supersampling on/off"},     // ldrb r3,[r8,#0x34]
        // gamepad mode (input.cpp): note driving controls as created, hide them when drawn
        {0x438de0, 0xE5943000, 0xEF000000 | shim_index("hook:CreateButton"), true, "button create hook"},
        {0x438bb8, 0xE5D0C054, 0xEF000000 | shim_index("hook:ButtonDraw"), true, "button draw hook"},
        // CHumanPlayer::SetInput: bl CCarActor::Action_SetInput -> svc (same stores + player's car)
        {0x420a5c, 0xEBF66326, 0xEF000000 | shim_index("hook:HumanInput"), true, "player input hook"},
        // CCarActor::UpdateConstraint: vstr s15, [r5, #512] (drive) -> svc (scaled at 60 Hz)
        {0x407014, 0xEDC57A80, 0xEF000000 | shim_index("hook:Drive"), true, "drive hook"},
    };
    for (auto& p : kPatches) {
        if (!p.apply) continue;
        u32* w = ptr<u32>(base + p.off);
        if (*w == p.old) *w = p.word, LOG("patched: %s", p.what);
        else LOG("patch skipped (unexpected code): %s", p.what);
    }
}
