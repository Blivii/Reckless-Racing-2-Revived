# Development notes

Everything that isn't obvious from the code: how to build, where things are in the game, and why
things are the way they are.

## Build

```bash
eval "$(tools/setup_env.sh ~/rr2-tools | tail -1)"   # NDK r27c, build-tools 34, platform 34, apktool, boost
tools/build.sh Reckless-Racing-2-v1.0.4.apk [main.103.com.polarbit.RecklessRacing2.obb]
                                                       # -> out/RR2-arm64.apk (with the OBB: embeds it)
patcher/build.sh                                       # -> patcher/build/RR2-Patcher.apk (after build.sh)
```

`tools/build.sh` (apktool) and the patcher app (zip edits on the phone) must produce the same game:
same dex, libraries and manifest tree. Compare them after changing either one.

PC test of the patcher core: `java -cp patcher/build/classes:$BUILD_TOOLS/lib/apksigner.jar
rr2.patcher.Patcher original.apk game.obb build-android/libRecklessRacing2.so
patcher/build/assets/key.pk8 patcher/build/assets/cert.der out.apk`

PC harness: `cmake -S . -B build && cmake --build build && ./build/rr2_host <original libRecklessRacing2.so>`
loads the original library and runs its initializers under the JIT, without a phone.

`tools/debug.keystore` (password `android`) signs everything. Keep it: a different key makes
Android refuse updates over existing installs, and players would have to uninstall and lose saves.
The patcher embeds this key, so it is effectively public.

## Settings

`/data/data/com.polarbit.RecklessRacing2/files/rr2.cfg`, lines `name=value`, written by the menu.
Lookup order: file, then system property `debug.rr2.<name>` (env `RR2_<name>` on PC), then default.

| Name | Default | Meaning |
|---|---|---|
| tick | 60 | game tick Hz (stock 30) |
| grip | 40 | % of the 60 Hz sideways slip correction (see below) |
| traction | 65 | % of the 60 Hz forward slip correction |
| drive | 130 | drive force multiplier at 60 Hz, % |
| lab | 0 | 1 shows the Physics lab row in Settings (or `adb shell setprop debug.rr2.lab 1`) |
| aa | 0 | supersampling (2x2); forced off if 2x the surface exceeds GL_MAX_TEXTURE/RENDERBUFFER_SIZE |
| gamepad | 0 | triggers drive, Start pauses, driving buttons hidden |

## In-game menu

Three-finger tap opens/closes. Pages (`overlay.cpp` kPages): main -> Settings / Cheats / Save data,
each with "< Back". Rows marked confirm need a second tap.

Menu actions that touch game state (cheats, save data) queue via `on_game_thread()`: taps arrive
on the UI thread, and the queue runs from the game thread's next `OnEvent`, outside guest code.

Save data (`save.cpp`): one backup slot, `files/rr2-backup/`, holding everything in `files/` except
`rr2.cfg` (the game keeps `content.sav`, `lb.sav`, ... there). Restore/reset swap files then
`_exit(0)` so the game can't write its in-memory copy back.

Cheats: profile = `*(Game() + 0x104)`, the same calls as the game's own `give_cash` /
`unlock_all` debug commands (`CGame::OnUserCommand`), then `CGame::SaveGame`.

## Guest addresses (v1.0.4, offsets from load base)

| Offset | Original | What |
|---|---|---|
| 0x1e4d34 | `ldr r1,[r4,#0x144]` | CPlatformFUSE::Init tick rate -> `mov r1,#hz` |
| 0x40a8a0 | `vmls.f32 s24,s14,s15` | CCarConstraint::ComputeSolve slip -> svc grip hook |
| 0x26a474 / 0x26a488 | `ldr r3/r2,[r8,#0x30]` | CEngineGameApp::Init options.msaa -> 2 |
| 0x26a490 | `ldrb r3,[r8,#0x34]` | options.always_msaa -> setting |
| 0x438de0 | `ldr r3,[r4]` | CGameUIButtons::CreateButton -> svc (record name) |
| 0x438bb8 | `ldrb r12,[r0,#0x54]` | CGameUIButtons::Draw hidden flag -> svc |
| 0x56a764 | data | CRenderGL2 instance pointer |

Platform object (`bite::CPlatform::Get()`): +0x1ac tick ms, +0x1a8 tick seconds.
Renderer: +0x21034 "always supersample" flag, +0x21044 SS target, +0x21048 current target.
Rigid body: pos +248, prev pos +96 (position-based physics: velocity = pos - prev per step),
forward +224, side +200. Car actor: body +0x78, inputs steer +1340 / gas +1344 / brake +1348.
Physics substeps per tick: `bite::CPhysics::m_snPhysicsIterations` (1 in the original).

## APK changes

- Libraries: original 32-bit lib -> `lib/arm64-v8a/libguest.so`, our loader takes its name; other ABIs removed.
- Manifest: targetSdk 14 -> 24 (Android 14+ refuses < 24). The original declares `<uses-sdk>` after
  `<application>`, so Android sets up activities with old-app defaults (letterboxed); the patcher
  moves it to the top, as apktool's rebuild does.
- Dex patches (`tools/patch_dex.py` = `Patcher.DEX_PATCHES`): FuseEgl red size 5 -> 8 (24-bit
  colour); `FileCheck.expansionFilesDelivered` -> true (OBB is inside the APK); `IabHelper.startSetup`
  -> "billing unavailable" (its implicit-intent bindService throws on Android 5+ at targetSdk >= 21,
  which crashed the store screen).
- OBB: Android blocks writing another app's obb folder, so it goes inside the APK as the entry
  `rr2/main.obb`, stored uncompressed (outside `lib/`, so Android doesn't unpack a second copy).
  `obb.cpp` finds it via the zip central directory; when the game opens its hard-coded
  `.../main.103.com.polarbit.RecklessRacing2.obb`, it gets our APK, with lseek/read/fstat/stat
  offset and clamped to the entry. Logcat shows `game data: rr2/main.obb in <apk>`.
- The patcher accepts .apk/.xapk and .obb/zip/.xapk, aligns stored entries itself (build-tools 34
  apksig does not), and signs with the repo key.

Targeting SDK 24 also turns on Android Auto Backup for the game's data.

## Physics lab (60 vs 30 Hz)

The physics is position-based, and `CPhysics::Update` runs `m_snPhysicsIterations` steps per tick.
Gravity and damping scale with the step, but the car constraint solver (`CCarConstraint::Update`,
`SolvePosition`, `SolveRotation`) corrects per step with no time factor, so at 60 Hz every
correction runs twice as often. The grip hook compensates one of them (tyre slip).

Menu > Settings > Physics lab (hidden unless the "lab" setting is 1) runs one config per race,
alternating A 30 Hz original and B the 60 Hz defaults; edit kConfigs in lab.cpp to compare others; a race ends when the player's car stands for 4 s after
driving, or samples stop for 2 s. Per race, player and AI separately ("lab ..." lines): standing-
start time to 10 and 20 units/s, average/top speed,
acceleration per 8-unit speed band (player: full gas only) and yaw rate, all measured against the
phone's clock; slide; inputs; samples per second (tps); and "vstep", the engine's own per-step
displacement / step length. First results (30 s segments, both cycles): top speed equal in all
configs; AI slide original 0.063, 60 Hz no fix 0.094, grip 60% 0.052; 30 Hz with 2 substeps
0.134 (the car constraint runs per tick, not per substep, so that config is not 60 Hz physics).
(A first version derived speed from pos - prev per step; at 30 Hz that
read half the real speed, so real-time measurement is the reference.) `CWorld::Construct` calls
`CPhysics::Init(1)`, nothing else writes the substep count. Note the tick is whole milliseconds: "60 Hz" is 16 ms
= 62.5 ticks/s, so a phone that can't run 62.5 FPS would run the game slightly slow.

## Decisions and why

- **Grip 40% / traction 65%.** CCarConstraint::ComputeSolve works on the tyre contact's
  ground-plane displacement per step, with per-step friction caps and drive and no time factor.
  The hook scales that slip into 30 Hz-step units, x k^(p/100) with k = 33 ms / tick, separately
  for its sideways and forward parts. One shared factor (the first version) could not match
  both: physics lab on one track, AI slide original 0.063, 60 Hz at 50% 0.058, at 60% 0.050;
  player 10 -> 20 units/s original 2.0 s, 50% 2.7 s, 60% 2.2 s. So sideways ~40%, forward > 60%.
  Split fix (grip 40): AI slide matches exactly (0.087); 10 -> 20 stays 2.2 s at traction 65 and 80
  (original 1.92 s), launch to 10 about 0.5 s slower. [constraint +516] looked like a traction cap
  but is the brake force (brake input x power x dt); scaling it changed nothing (lab: 0-10 1.38 s
  at 60 Hz with cap 0/65/100 vs 1.15 s original; 10-20 2.2 vs 1.82 s, same for the AI). So the
  car isn't grip-limited at low speed; the drive ([constraint +512], gas x power x dt, added along
  the car's forward axis in ComputeSolve) comes out ~20% weaker. Hook at 0x407014 multiplies it
  by drive% at 60 Hz. Standing starts, one track, 0-10 / 10-20 units/s: original 1.15 / 1.82 s,
  100% 1.38 / 2.20, 120% 1.39 / 2.10, 140% 1.06 / 1.60 (AI 0-10: 1.81 vs 2.17 / 1.91 / 1.63).
  Matching falls at ~127% (AI) to ~131-135% (player): default 130%. Top speed unchanged (27.2).
  Dead ends: the per-step displacement check in UpdateConstraint is only BIG_EPSILON (stopped);
  rigid body damping is set per tick from the constraint (+524) and scaled by P in CRigidbody.
- **Supersampling** uses the engine's own 2x render target (meant for MSAA, unused on Android).
  Fixes in gl.cpp: clear screen depth/stencil when binding FB 0 (else the downscale quad fails the
  depth test: black screen), and double glScissor while the SS target is bound.
- Gamepad mode hides only left/right/wheel/gas/brake; respawn and pause stay.

## Compatibility risks (ranked) and what is built in

Logcat lines to ask reporters for are in quotes.

1. **16 KB-page kernels** (Android 15+): loader linked with max-page-size=16384; "page size N"
   logged; loader only uses large aligned mmaps. Untested on a 16 KB device.
2. **Sideload verification** (Sep 2026 BR/ID/SG/TH, 2027 global): can't be fixed in-app. The
   patcher explains installer failures and offers "Save patched APK" for file-manager/adb install.
   The patched game keeps Pixelbite's package name, so it can't be registered.
3. **Where players get files**: .apk/.xapk and .obb/zip/.xapk accepted; an APK picked as game data
   is rejected. Loader logs "GAME DATA MISSING" when neither copy exists.
4. **Original game still installed**: the patcher warns if the installed copy has another signer;
   uninstalling it loses saves (the original never had Auto Backup).
5. **Texture format**: the game picks .pvr/.dds/.etc by GL_VENDOR (Imagination/NVIDIA/else). If the
   driver lacks that vendor's format, glGetString reports vendor "ARM" so .etc is used ("textures: ...").
   If ETC1 isn't advertised on a GLES 3 context, ETC1 uploads become ETC2 (a superset).
6. **EGL 24-bit config**: FuseEgl falls back to its default config when no red=8 config exists; the
   "GL ... RGBA" line shows what was granted.
7. **Supersampling cost** on mid-range GPUs: off by default.
8. **GL fixes (depth clear, scissor x2)**: plain GLES2, only tested on Adreno 830.

Also: patcher minSdk 29 (the loader needs API 29); the low-4GB guest region falls back to
512/256 MB ("low memory is crowded").

## Ideas

- Optional 120 Hz tick (needs the grip re-check via telemetry).
- x86/x86_64 devices (Chromebooks, emulators): not supported, the build is arm64-only. dynarmic has
  an x64 backend, so the loader could be built for x86_64 if there is demand.
- "Update installed game" in the patcher: rebuild from the installed APK (it holds the original
  library, the OBB and the dex), so updates need no file picking.
