// Physics lab: measures how the game behaves under each physics configuration, to match 60 FPS
// to the original 30. While on, each race runs one configuration (the next one starts when the
// cars stop: results screen, menu or restart), and is logged per race for the player's car and
// the AI cars separately, including standing-start acceleration from the race start.
//
// Background: the physics is position-based (rigid bodies keep pos +248 and previous pos +96),
// and CPhysics::Update splits each tick into m_snPhysicsIterations steps (1 in the original).
// 60 Hz therefore behaves like 2 steps per 30 Hz tick; config C reproduces exactly that at 30 Hz.
#include <time.h>
#include <algorithm>
#include <cmath>
#include <map>
#include <string>
#include "rr2.h"

struct Config { const char* name; u32 hz, grip, traction, drive, substeps; };
static const Config kConfigs[] = {  // the original every other race, as the baseline
    {"A 30Hz original", 30, 0, 0, 100, 1},
    {"B 60Hz defaults", 60, 40, 65, 130, 1},
};
constexpr int kN = int(sizeof kConfigs / sizeof *kConfigs);
constexpr int kBands = 6;        // acceleration by speed band: 0-8, 8-16, ... 40+ units/s

// Speeds, acceleration and turn rate are measured against the phone's clock (distance between
// samples / real time), so they mean what the player sees whatever the engine does inside a tick.
// "vstep" is the engine's own per-step displacement / step length, kept for comparison.
struct Stats {
    double real, dist, vmax, vstep, samples;     // real seconds sampled, distance, top speed (0.5 s smoothed)
    double slide, yaw, moving;                   // moving: half-second windows above 3 units/s
    double gas, brake, steer, inputs;            // player only
    double acc[kBands], acc_n[kBands];
    double to10, n10, to20, n20;                 // standing start: seconds from moving off to 10 / 20 units/s
};
struct Car { float last[3], win_fwd[3]; double last_t, win_t, win_v, ema, go; bool have, win_gas, hit10, hit20; };
static bool g_on;
static int g_cfg;
static u32 g_player_actor, g_iterations;
static Stats g_player, g_ai;
static std::map<u32, Car> g_cars;
static double g_real_start, g_real_last;  // real time span of samples in this race
static double g_still;                    // when the player's car stopped (0 = moving)
static bool g_moved, g_ended;             // player has driven this race; the race is over

static double now() { timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }
static float dot(const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

void apply_substeps(int n) {  // the engine's own physics steps per tick (1 in the original)
    if (!g_iterations) g_iterations = guest_sym("_ZN4bite8CPhysics21m_snPhysicsIterationsE");
    if (g_iterations) *ptr<int>(g_iterations) = n;
}
static void apply(const Config& c) {
    set_rates(c.hz, c.grip, c.traction, c.drive, false), apply_substeps(int(c.substeps));
    LOG("lab: now %s", c.name);
}

static std::string line(const char* who, const Stats& s, int cars) {
    char b[400];
    if (s.real < 1) return std::string(who) + ": no data";
    double m = std::max(1.0, s.moving);
    int n = snprintf(b, sizeof b, "%s%s: v %.1f max %.1f vstep %.1f tps %.1f slide %.3f yaw %.2f", who,
                     cars ? (" x" + std::to_string(cars)).c_str() : "", s.dist / s.real, s.vmax, s.vstep / s.real,
                     s.samples / s.real, s.slide / m, s.yaw / m);
    if (s.inputs) n += snprintf(b + n, sizeof b - n, " gas %.2f brk %.2f steer %.2f", s.gas / s.inputs, s.brake / s.inputs, s.steer / s.inputs);
    if (s.n10) n += snprintf(b + n, sizeof b - n, " start 0-10 %.2fs", s.to10 / s.n10);
    if (s.n20) n += snprintf(b + n, sizeof b - n, " 0-20 %.2fs", s.to20 / s.n20);
    n += snprintf(b + n, sizeof b - n, " acc");
    for (int i = 0; i < kBands; i++) n += s.acc_n[i] >= 3 ? snprintf(b + n, sizeof b - n, " %.1f", s.acc[i] / s.acc_n[i]) : snprintf(b + n, sizeof b - n, " -");
    return b;
}

static bool report() {  // true if a race was logged
    int ai_cars = std::max(0, int(g_cars.size()) - (g_player.real > 0));
    bool race = g_real_last - g_real_start > 20;  // skip menus and aborted starts
    if (race)
        LOG("lab %s | race %.0fs | %s | %s", kConfigs[g_cfg].name, g_real_last - g_real_start,
            line("P", g_player, 0).c_str(), line("AI", g_ai, ai_cars).c_str());
    g_player = g_ai = {}, g_cars.clear(), g_real_start = g_real_last = g_still = 0, g_moved = g_ended = false;
    return race;
}

// Per car, from the grip hook in CCarConstraint::ComputeSolve (r6 = rigid body); it can run
// several times per tick, so samples closer than 5 ms are skipped.
void lab_sample(u32 body, float tick_seconds) {
    if (!g_on) return;
    const float* pos = ptr<float>(body + 248);
    const float* fwd = ptr<float>(body + 224);
    double t = now();
    Car& c = g_cars[body];
    if (!c.have) {
        std::copy(pos, pos + 3, c.last), std::copy(fwd, fwd + 3, c.win_fwd);
        c.last_t = c.win_t = t, c.win_v = c.ema = -1, c.win_gas = true, c.have = true;  // -1: no speed yet
        return;
    }
    double dt = t - c.last_t;
    if (dt < 0.005) return;
    float d[3] = {pos[0] - c.last[0], pos[1] - c.last[1], pos[2] - c.last[2]};
    double dist = std::sqrt(dot(d, d)), speed = dist / dt;
    std::copy(pos, pos + 3, c.last), c.last_t = t;
    if (speed > 150 || dt > 0.5) { c.have = false; return; }  // respawn, teleport or a pause
    bool player = g_player_actor && (body == *ptr<u32>(g_player_actor + 0x78) ||
                  (*ptr<u32>(g_player_actor + 0x74) && body == *ptr<u32>(*ptr<u32>(g_player_actor + 0x74) + 0x14)));
    Stats& s = player ? g_player : g_ai;
    if (!g_real_start) g_real_start = t;
    g_real_last = t;
    s.real += dt, s.dist += dist, s.samples++;
    if (c.ema < 0) c.ema = c.win_v = speed;  // first speed of this car: start smoothing from it
    c.ema += (speed - c.ema) * std::min(1.0, dt / 0.5);
    s.vmax = std::max(s.vmax, c.ema);
    if (!c.go && c.ema > 1) c.go = t;        // standing start: moved off
    if (c.go && !c.hit10 && c.ema >= 10) c.hit10 = true, s.to10 += t - c.go, s.n10++;
    if (c.go && !c.hit20 && c.ema >= 20) c.hit20 = true, s.to20 += t - c.go, s.n20++;
    if (player) {  // the race is over when the player's car has driven and then stands for 4 s
        if (c.ema > 5) g_moved = true, g_still = 0;
        else if (c.ema < 1 && !g_still) g_still = t;
        else if (c.ema < 1 && g_moved && t - g_still >= 4) g_ended = true;
    }
    float step = tick_seconds / float(kConfigs[g_cfg].substeps), v[3];  // the engine's own view
    for (int i = 0; i < 3; i++) v[i] = (pos[i] - *ptr<float>(body + 96 + 4 * i)) / step;
    float vf = dot(v, fwd), vs = dot(v, ptr<float>(body + 200)), vstep = std::hypot(vf, vs);
    s.vstep += vstep * dt;
    if (player) {
        float in_steer = *ptr<float>(g_player_actor + 1340), in_gas = *ptr<float>(g_player_actor + 1344),
              in_brake = *ptr<float>(g_player_actor + 1348);
        s.gas += in_gas, s.brake += in_brake, s.steer += std::fabs(in_steer), s.inputs++;
        c.win_gas = c.win_gas && in_gas > 0.9f && in_brake < 0.1f;
    }
    double w = t - c.win_t;  // half-second windows: acceleration, turn rate, slide
    if (w < 0.5) return;
    if (c.ema > 3) {
        s.yaw += std::acos(std::clamp(dot(fwd, c.win_fwd), -1.0f, 1.0f)) / w;
        s.slide += vstep > 1 ? std::fabs(vs) / vstep : 0, s.moving++;
        if (c.win_gas) {
            int band = std::min(kBands - 1, int(c.ema / 8));
            s.acc[band] += (c.ema - c.win_v) / w, s.acc_n[band]++;
        }
    }
    std::copy(fwd, fwd + 3, c.win_fwd), c.win_t = t, c.win_v = c.ema, c.win_gas = true;
}

void lab_player(u32 actor) { g_player_actor = actor; }

static void next() { g_cfg = (g_cfg + 1) % kN, apply(kConfigs[g_cfg]); }
void lab_frame() {  // game thread, every frame: a race ends when the cars stop, or stop updating
    if (g_on && g_real_start && (g_ended || now() - g_real_last > 2) && report()) on_game_thread(next);
}

bool lab() { return g_on; }
std::string lab_label() { return g_on ? std::string("LAB ") + kConfigs[g_cfg].name : std::string(); }
void set_lab(bool on) {  // game thread (queued from the menu)
    if (on == g_on) return;
    if (!on) report();
    g_on = on, g_cfg = 0, g_player = g_ai = {}, g_cars.clear(), g_real_start = g_real_last = 0;
    if (on) apply(kConfigs[0]);
    else set_rates(setting("tick", 60), setting("grip", 40), setting("traction", 65), setting("drive", 130), false), apply_substeps(1), LOG("lab: off, settings restored");
}
