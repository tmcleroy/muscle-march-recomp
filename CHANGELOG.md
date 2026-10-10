# Changelog

## Unreleased

- Android: on a first start the app asks for your own WAD and Wii common key and builds the game
  files on the phone, so an APK no longer has to be built with the game files on a computer.

## 0.1.0-beta.3

- Android (arm64, Vulkan): the game builds as an APK (`tools/release/android.sh`). There is no
  launcher on Android yet, so for now it is a build for people with a checkout and their own game
  files; see docs/BUILDING.md.
- Touch controls (Enhanced, on touch screens): the stick guides move to the sides of the screen
  and are the sticks themselves, with a pause button; taps point and press A in menus.
- Android: screen copies (the game's blur) are no longer also read back into game memory, which
  stalled phones once per frame (`video.copies_to_ram=1` in settings.ini or `WP_COPIES_TO_RAM=1`
  restores it; other platforms keep it).

## 0.1.0-beta.2

- Windows: races no longer drop to 30 fps and half speed
  ([#1](https://github.com/tmcleroy/muscle-march-recomp/issues/1)). The game looked a setting up
  through the environment on every graphics call, which is slow on Windows; the on-screen stick
  guides and part of the CPU emulation also used slow math-library calls there.
- Key bindings saved by an earlier build that still equal an old default become the current
  default, so W/A/S/D and the arrow keys work on existing installs. Bindings you changed are kept.

## 0.1.0-beta.1


First public beta.

- Native builds for macOS (Apple Silicon), Linux x86_64 / Steam Deck (AppImage) and Windows.
- Launcher: choose your WAD and give your console's common key once; it builds the game's files on
  your machine. Pure and Enhanced presets with a toggle per feature, a controller test page,
  **Add to Steam** on Linux and Steam Deck (straight into the game from Game Mode), **Report a
  bug** (opens an issue filled in with your setup) and **Open folder**; the game's output is saved
  to `logs/game.log`.
- Enhanced features:
  - dual-stick poses: one stick per arm, instant, with a window that narrows through each stage;
  - shaking by wiggling the sticks;
  - on-screen stick guides in the side pillars, with "POSE!" before each wall and "SHAKE!" when
    it's time to run;
  - pause with Start;
  - skip the wrist-strap screen, skip intro screens with any button, no controller notice.
- Pure mode plays exactly as on the Wii.
- Gamepad, keyboard and mouse, and real Wii Remote + Nunchuk support.
