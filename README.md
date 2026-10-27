<p align="center">
  <img src="docs/icon.png" width="128" alt="Reckless Racing 2 Revived">
</p>
<h1 align="center">Reckless Racing 2 Revived</h1>
Play Reckless Racing 2 (v1.0.4) on modern 64-bit-only Android phones.

The game was a 32-bit ARM app, making it incompatible with modern 64-bit devices. This project runs
the game's original, unmodified code under the [dynarmic](https://github.com/azahar-emu/dynarmic)
JIT inside a new 64-bit library, and features:

- optional 60 FPS mode (the original runs at 30), with grip, traction and drive tuned so cars handle and
  accelerate like the original
- 24-bit colour, optional 2x2 supersampling
- Controller support: L2 and R2 for brake and gas, left stick and dpad steers, on-screen driving buttons hidden
- An in-game menu (tap screen with three fingers): graphical settings, cheats (Add $100,000/Unlock all cars, save backup/restore/reset, gamepad mode (hides on screen controls)
- The store screen no longer crashes; the game runs fully offline

No game files are included. You patch your own copy. Vision has what you need. 

## Install

You need your own copy of the game: `Reckless-Racing-2-v1.0.4.apk` and its game data
`main.103.com.polarbit.RecklessRacing2.obb` (a zip or .xapk containing it works too).

1. Install `RR2-Patcher.apk` from the releases page (Android 10 or newer, 64-bit ARM).
2. Open it, choose the game APK (step 1) and the game data (step 2), then tap **Patch and install**.
3. Allow installs from RR2 Patcher when Android asks.

The patched game contains its data, so nothing needs copying into `Android/obb`. To update,
install the new patcher and patch again; your saves are kept.

If the original 32-bit game is still installed, uninstall it first (this deletes its saves).
If Android blocks the install, use **Save patched APK instead** and install it with a file manager
or `adb install`.

## How it works

The patched APK keeps all of the game's Java code and assets. The original library moves to
`lib/arm64-v8a/libguest.so`, and our library takes its name. When the game calls into its library,
ours runs the original 32-bit code through the JIT, providing everything the game expects from the
system by hand: libc, OpenGL ES 2, and the Java Native Interface in both directions.

| File | Role |
|---|---|
| `src/loader.cpp` | Maps the 32-bit ELF into the low 4 GB, relocates it, guest heap |
| `src/cpu.cpp` | One dynarmic A32 JIT per thread; `call_guest()` |
| `src/table.cpp` | The one table: name -> host shim, reached via `svc` thunks |
| `src/libc.cpp` | The game's libc imports (32-bit struct layouts, printf varargs, offline sockets) |
| `src/gl.cpp` | GLES2 via `dlsym` thunks; supersampling and texture-format fixes |
| `src/jni.cpp` | Java bridge: the `Java_*` natives and a fake JNIEnv/JavaVM for the guest |
| `src/patches.cpp` | Game patches: 60 Hz tick, grip/traction/drive hooks, supersampling, cheats |
| `src/input.cpp` | Gamepad mode |
| `src/overlay.cpp` | In-game menu |
| `src/save.cpp` | Save backup, restore and reset |
| `src/obb.cpp` | Serves the game data (OBB) straight from inside the APK |
| `src/lab.cpp` | Physics lab: logs 30 Hz vs 60 Hz handling (developer tool, hidden by default) |
| `src/main.cpp` | `JNI_OnLoad`, settings file, PC test harness |
| `tools/patch_dex.py` | Byte patches to the Java code |
| `tools/build.sh` | Builds the loader and repacks an APK on a PC (apktool) |
| `patcher/` | The patcher app: the same changes, on the phone |

## Build

Building needs Linux (Debian/Ubuntu) or WSL on Windows. `tools/setup_env.sh` downloads the toolchain
(Android NDK, build-tools, platform, apktool) and installs Boost, CMake, Ninja and Java with `apt-get`:

```bash
eval "$(tools/setup_env.sh ~/rr2-tools | tail -1)"
tools/build.sh Reckless-Racing-2-v1.0.4.apk [main.103.com.polarbit.RecklessRacing2.obb]  # -> out/RR2-arm64.apk
patcher/build.sh                                                                          # -> patcher/build/RR2-Patcher.apk
```

Details, game addresses and design notes: [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md).

## Credits

- Created by **Blivii**.
- Reckless Racing 2 is © Pixelbite. This project contains none of its code or assets.
- [dynarmic](https://github.com/azahar-emu/dynarmic) by merryhime and contributors, via the Azahar fork.
- [ShadowGun Revived](https://github.com/RedMemories/ShadowGunRevived) by RedMemories showed the approach could work.

## License

The code in this repository is under the 0BSD license (see [LICENSE](LICENSE)): use it for anything,
no conditions. It does not cover Reckless Racing 2 itself, which belongs to Pixelbite, or dynarmic,
which the build fetches under its own license.
