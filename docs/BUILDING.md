# Building from source

Everything here runs from a checkout with your own WAD in the repository folder (it's ignored by
Git). Nothing built from it is ever committed: see [CONTRIBUTING.md](../CONTRIBUTING.md).

Target WAD: `Muscle March (USA) (WiiWare).wad`, 40,086,976 bytes,
SHA-1 `ed99ce114d9793dbf21d5c9589d7947afebd4259`, title `00010001-574D4D45` (WMME).

The recompiler and runtime come from
[wiiparty-recomp](https://github.com/arelkair/wiiparty-recomp) (GPL-3.0), vendored in `port/` with
this project's changes on top (see [PORT.md](PORT.md)).

## Layout

| Path | What |
| --- | --- |
| `tools/extract.py` | Verifies the decrypted contents against the TMD and unpacks the DOL (LZ11), U8 archives and LZ11 assets into `extracted/` |
| `tools/dolphin_run.sh`, `tools/dolphin_config.py` | Headless reference runs in Dolphin (PNG frame dump) |
| `tools/lockstep.py` | Lockstep comparison with Dolphin (GDB stub) at a sync function |
| `config/WMME01/config.yml` | decomp-toolkit config. dtk finds the function boundaries the recompiler uses |
| `port/` | Recompiler (`recompiler/`) and runtime (`engine/`), from wiiparty-recomp |
| `port/games/musclemarch/` | Game definition: hand-identified SDK symbols, HLE list, sync hooks |
| `docs/ANALYSIS.md`, `docs/PORT.md`, `docs/dev/PROGRESS.md` | Findings, port notes, status |

Generated and never committed: `extracted/`, `dolphin_user/` (Dolphin's NAND,
which holds the decrypted title), `build/`, `port/build/` (translated code), and
dtk's symbol and split lists.

## Requirements (macOS)

For development the game's files can also come from Dolphin's WAD install (`make nand`), as below; players use the launcher, which decrypts the WAD with their own key instead.


`brew install --cask dolphin` and `brew install cmake ninja sdl3 python@3.13`, plus `gh`.

## Workflow

```sh
make nand          # Dolphin installs (decrypts) the WAD into ./dolphin_user/Wii
make extract       # -> extracted/main.dol, contents/, files/
make port          # dtk analysis -> recompile 7.7K functions -> build port/build/out/musclemarch
make run           # play (window). HEADLESS=1 for no window
make app           # build/MuscleMarch.app: double-click to play; saves in ~/Library/Application Support/MuscleMarch
make lockstep HITS=300            # compare game memory with Dolphin, frame by frame
make lockstep HITS=101 DUMP=100   # ... and byte-diff frame 100
make ref SECS=40   # Dolphin reference run, frames in dolphin_user/Dump/Frames
```

Useful environment variables for the port: `WP_CAPTURE_AT=5,10` (save frames
at those seconds into `captures/`), `WP_INPUT_SCRIPT="from_ms,to_ms,hexbuttons;..."`,
`WP_LOG_ES=1`, `WP_LOG_FS=1`, `WP_LOG_IOS=1`, `WP_LOG_WIIMOTE=1`, `WP_LOG_FPS=1`.

## Tests

- `make replay-test`: replays the input movies in `movies/` deterministically and compares state
  hashes with the golden results (about 25 minutes). `smoke_warmup` and `autoplay_story` play
  Pure; `enhanced_story` plays with every feature on (autoplay posing instantly, as the sticks do).
  The first run on a machine records the golden results (`build/golden/`). Run it before any change to game code; with
  every feature off the port must behave exactly as before.
- `WP_VERIFY=1 WP_VERIFY_FAST=1 WP_VERIFY_STACK=1`: checks each lifted (hand-written) game function
  against the translated original on every call.

## Launcher

`make launcher` builds `port/build/out/musclemarch-launcher` next to the game. See
[FEATURES.md](FEATURES.md) for what it does.

## Release builds

Release builds contain code translated from the game, so they are made by someone with the WAD,
on their own machines; they are never built by CI or committed. Each script needs the generated
code (`make port` once, on any machine; copy `port/build/musclemarch/generated/` to the build
machine and pass `GENERATED=...`). All write to `build/release/`.

`tools/release/all.sh` runs all four on an Apple Silicon Mac with Docker (OrbStack) and writes
`SHA256SUMS.txt`. Its APK must be signed with the release key (see "Android").

| Script | Where | Output |
| --- | --- | --- |
| `tools/release/macos.sh` | macOS on Apple Silicon | `MuscleMarch-macOS.zip`: `Muscle March.app` (macOS 13+, SDL3 static, signed ad hoc) |
| `tools/release/linux.sh` | x86_64 Linux, e.g. an Ubuntu 24.04 container | `MuscleMarch-x86_64.AppImage` (also for Steam Deck) |
| `tools/release/windows.sh` | x86_64 Linux with MinGW-w64 (cross-compile) | `MuscleMarch-windows.zip` |
| `tools/release/android.sh` | macOS or Linux with the Android SDK | `MuscleMarch-android.apk` (arm64, Android 8+) |

Linux packages (Ubuntu 24.04): `g++ cmake ninja-build python3 curl file libx11-dev libxext-dev
libxrandr-dev libxcursor-dev libxi-dev libxss-dev libxtst-dev libxkbcommon-dev libwayland-dev
wayland-protocols libdecor-0-dev libegl-dev libgl-dev libpulse-dev libasound2-dev libudev-dev
libdbus-1-dev libpipewire-0.3-dev`; for Windows add `g++-mingw-w64-x86-64-posix zip`.

A Steam Deck works as the Linux build machine: in Desktop Mode, `podman run` an
`ubuntu:24.04` container with the checkout mounted, install the packages, and run the scripts in it.

## Android

`tools/release/android.sh` (or `make android`) builds an arm64 APK of the game on a Mac or Linux
machine: `build/release/MuscleMarch-android.apk`. It needs the generated code (`make port`), CMake
3.24 or newer and Ninja, a JDK 17, and the Android SDK with platform 34, build-tools and NDK 27.2
(`ANDROID_HOME`; with Homebrew: `brew install openjdk@17 android-commandlinetools`, then
`sdkmanager "platforms;android-34" "build-tools;34.0.0" "ndk;27.2.12479018"`). SDL3's source is
downloaded to `port/build/deps` (its Java half is compiled into the app). The Gradle project is
`android/`; the native half is the same CMake build as everywhere else, as `libmain.so`.

On a first start the app asks for the player's own WAD and Wii common key (pasted, typed, or a
key file) and builds the game files in its folder, as the launcher does on a computer
(`launcher/src/android_setup.cpp`, with the launcher's `install.cpp`). The key is used once and
not kept. This APK has no game files in it.

- `make android` (`GAME_FILES=extracted tools/release/android.sh`) instead makes a **development
  build** with your own extracted game files packed in, which skips that screen:
  `build/release/MuscleMarch-android-with-game-DO-NOT-SHARE.apk`. It unpacks them into the app's
  folder on the first start. It contains the game: never publish or share it.

The app's folder (`Android/data/io.github.tmcleroy.musclemarch/files`) also holds `settings.ini`
(a first start writes one with the Enhanced preset), the saves (`nand/`) and, for testing,
`env.txt`: one `NAME=value` per line, applied as environment variables at start (the `WP_...`
variables above). The game's output goes to the system log: `adb logcat -s musclemarch SDL`.

The APK is signed with the Android debug key unless `KEYSTORE`, `KEYSTORE_PASSWORD`, `KEY_ALIAS`
and `KEY_PASSWORD` name your own, in the environment or as `NAME="value"` lines in
`~/.config/musclemarch/android-signing.env` (`SIGNING=...` names another file). Android only
installs an update over an app signed with the same key, so the published APKs all carry one
release key, which lives outside the repository with whoever makes the releases: `RELEASE=1`
(which `all.sh` sets) refuses to build without it or with game files packed in. Install with `adb install -r <apk>`, or copy it to the phone and
open it (allow installing from that source).

On the emulator (an arm64 image on Apple Silicon, started with `-gpu host`), `adb shell input tap`
drives menus, and `WP_TOUCH_SCRIPT` in `env.txt` scripts several fingers at once (docs/FEATURES.md,
`touch_controls`).
