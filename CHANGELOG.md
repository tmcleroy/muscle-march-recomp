# Changelog

## Unreleased

- Android (arm64, Vulkan): the game builds as an APK (`tools/release/android.sh`). There is no
  launcher on Android yet, so for now it is a build for people with a checkout and their own game
  files; see docs/BUILDING.md.
- Touch controls (Enhanced, on touch screens): the stick guides move to the sides of the screen
  and are the sticks themselves, with a pause button; taps point and press A in menus.

## 0.1.0-beta.1 (unreleased)

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
