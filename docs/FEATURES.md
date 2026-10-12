# Optional features and the launcher (design)

Status: registry, presets, launcher v1 and the first features are in. The table below lists them.

## Principle

With every feature off, the port must behave exactly like the game on a Wii: same rules,
same timing, same controls (Wii Remote + Nunchuk, or the keyboard/gamepad stand-ins for them).
Anything that changes gameplay or controls is an opt-in **feature**. The golden replay tests
(`make replay-test`) prove the "all off" path stays identical to the original.

## Features

A feature is a named, self-contained module:

```cpp
// port/games/musclemarch/features/<name>.cpp
WP_FEATURE(dual_stick_poses, {
    .title = "Dual-stick poses",
    .description = "Each analog stick controls one arm; hold both in the pose's slice.",
    .category = Category::Controls,
    .default_on = false,
    .options = {{"difficulty_curve", "normal"}},   // feature-specific settings
});
```

- The registry lists every feature: name, title, description, category (controls, gameplay,
  graphics, audio, quality of life), and its own options.
- State lives in `settings.ini` as `features.<name> = 0/1` (+ `features.<name>.<option>`), like
  the existing options. It is read at boot. Features that can't change mid-game say so.
- Code checks `features::enabled("dual_stick_poses")` only at its hook points. No scattered
  `if`s in translated code: a feature attaches through one of the hook kinds below.
- Replays record the enabled features in the movie header. A movie made with a feature on
  replays with it on, and the golden tests run every movie with its own feature set.

## Presets

Features are grouped into presets the launcher offers:

- **Pure**: everything off, exactly the Wii game.
- **Enhanced** (the launcher's default): most or all improvement features on.
- **Custom**: any combination; starts from whichever preset was chosen.

Each feature declares which presets include it. Replays store the resolved feature list, not
the preset name, so they keep working when presets change.

## The features

All are in the Enhanced preset and off in Pure. `musclemarch --list-features` prints them.

| Feature (`settings.ini` key) | Kind | What it does |
| --- | --- | --- |
| `skip_intros` | function hooks | The Bandai Namco logo, the controller notice and the opening movie end on any button, not only A, B, 1 or 2 (the strap notice already took any button). Hooks: scene/boot.cpp, scene/system_menu.cpp, scene/opening_select.cpp. |
| `no_controller_notice` | function hook | The controller check doesn't raise the "Wii Remote and Nunchuk required" screen (warning 1) at start-up. Hook: scene/controller_check.cpp. |
| `dual_stick_poses` | input transform + game state reads | Right stick = right arm (Remote), left stick = left arm (Nunchuk), pointed the way the arm goes: both up = both arms up, left stick down-left + right stick up-right = right arm raised, left lowered, and so on. An arm counts while its stick is pushed at least half way inside a slice centred 22.5° outward of vertical, so straight and outward-diagonal both work; inward and sideways drop out as the slice narrows from a stage's start to its end and with difficulty: Easy 130°→100°, Normal 115°→80°, Hard 100°→60°. Stick poses are instant: the game takes the pose the sticks ask for the same frame (not the one read from the emulated accelerometers, which shaking wobbles), its pose gauge jumps so the arms go straight into the pose without the swing animation, and the pose drops the moment a stick is let go; the Wii's arm-turn and settle time are skipped (input bit `kPointInstant`, recorded in movies). Replaces the default "right stick picks one of four poses". `WP_DUAL_STICK_DEBUG=1` logs arms and slice; `WP_MOTION_DEBUG=1` logs the pose the game accepts. |
| `stick_shaking` | input transform | Shake by wiggling both sticks back and forth, or round and round; the left trigger no longer shakes. A stick counts a reversal when it turns back by 0.4 on either axis, and shakes while each stick has made 2 in the last half second, so striking or letting go of a pose (one movement) never shakes. `WP_STICK_SHAKE_DEBUG=1` logs on/off. |
| `on_screen_guides` | HUD sprites + game state reads | Dials in the picture's side pillars (left stick left, right stick right), on screen throughout racing (race state 1) with a dot for the live stick. Ahead of a pose wall (up to 3 segments, with `dual_stick_poses`): the stick's up and down windows at their current width, the one the wall needs in yellow with a marker on the rim, and "POSE!" bouncing above each dial with an arrow down to it. In a shake section (race mode 1, 2, with `stick_shaking`): a left-right arrow with a swinging dot and "SHAKE!". Hidden outside racing (countdown, a rival's slow-motion crash). The words are stroked block capitals drawn once and only moved while bouncing; dials redraw when they change; unchanged frames send nothing to the renderer (`wp/hud.h`; SDL_GPU, OpenGL and D3D11). `WP_GUIDE_DEBUG=1` logs; tests: `WP_GUIDE_TEST=1` (no gamepad needed), `=wiggle`, `WP_CAPTURE_HUD=1`. |
| `start_pause` | input transform | Start pauses a race: while racing it also sends the Remote's 1 button, the game's pause (a gamepad's west face button and the keyboard's 1 key send it too). |
| `touch_controls` | virtual gamepad + HUD sprites | On a touch screen without a gamepad (the Android build), the stick guides are the controls. The dials move out to the window's side margins and the picture's pillars, centred 60% down and up to 46% of the picture's height across, and each is its stick: a finger on a dial puts the stick at that point (full push at the dot's travel, clamped beyond), so a pose is a touch on the highlighted window, and rubbing or drumming on both dials shakes. Fingers landing on a side of the screen (out from the pillar's inner edge) take that side's stick until they lift. A pause button (the Remote's 1) sits in the top right corner while racing, and the system's back gesture pauses a race and is B elsewhere. The sticks are a virtual gamepad (engine `wp/touch.h`), so `dual_stick_poses` and `stick_shaking` read them as a real one; the guides are drawn even with `on_screen_guides` off. A touch anywhere else points and presses A like a mouse click, with or without the feature. `WP_TOUCH_SCRIPT="from_ms,to_ms,x0,y0,x1,y1;..."` scripts fingers (window fractions) on any platform; `WP_TOUCH_DEBUG=1` logs them. |
| `skip_strap_screen` | function hook | The wrist-strap safety notice fades out as soon as it accepts a button (it ignores them for its first seconds): a "press" every 10 frames. Hook: scene/boot.cpp sub-state 2. |

## Hook kinds

1. **Input transforms** (host side, before the game sees input): turn gamepad state into the
   Wii Remote/Nunchuk data the game expects. Examples: dual-stick poses become arm orientations,
   stick wiggles or button mashing become shake intensity. Most control ideas live here and need
   no game code changes.
2. **Game state reads**: read-only access to named game variables (stage, combo, wall index,
   difficulty) through typed accessors, e.g. to narrow the pose slices as a stage progresses.
   These come from the lifted, readable game code (roadmap step 5).
3. **Function hooks**: replace or wrap a game function (lifted C++) when a feature is on. This
   uses the same mechanism as `hle_functions.csv`, plus the differential verifier that proves the
   lifted version equals the original when the feature is off.

## Launcher

`musclemarch-launcher` (port/games/musclemarch/launcher; `make launcher`) is a separate SDL3 +
Dear ImGui app (the look is the upstream launcher's ui.cpp) that starts the game program found
next to it:

- **Game**: drag and drop, or choose, the player's Muscle March WAD. The path is remembered in
  `launcher.ini`; the file is only ever read (title ID from the ticket, SHA-1 against the known USA
  release). The launcher ships no keys: the player gives their console's Wii common key once, as a
  key file (chosen or dropped; e.g. keys.bin from a NAND backup, with the key at any offset) or as
  32 hexadecimal digits. A key is accepted only if it decrypts this WAD (the smallest content then
  matches its SHA-1 in the TMD); it is saved in the player's `launcher.ini`. With file and key, the
  launcher builds the game's files into `game/` in the player's folder (src/install.cpp: AES-128
  title key and contents, every content checked against the TMD, the LZ11 program unpacked to
  main.dol, the U8 archives to files/NN/; about a second). The result is byte-identical to
  `tools/extract.py`'s for everything the game reads. **Play** runs the game in the player's folder
  (`~/Library/Application Support/Muscle March/`: settings.ini, launcher.ini, game/, nand/;
  `MUSCLE_MARCH_HOME` overrides it for tests; a `portable.txt` next to the program makes it a
  `user/` folder there). Without a build there, a development checkout's
  `extracted/` is used.
- **Features**: preset Pure / Enhanced / Custom and one toggle per feature, as listed by the game
  program itself (`musclemarch --list-features`). Changing a toggle switches to Custom. New
  players start on Enhanced; the game alone, without settings, runs Pure.
- **Controls**: how to play with a gamepad, keyboard or Wii Remote, and connected gamepads and Wii Remotes (SDL's Wii Remote support) with a live
  test of buttons, sticks and motion. Wii Remotes are paired in the system's Bluetooth settings
  for now; pairing from the launcher is planned.

## Order of work

1. Deterministic replays and golden tests (done, see dev/PROGRESS.md).
2. Generic lift/verify toolkit (from the GX library's differential verifier).
3. Feature registry + settings + movie header (no features yet).
4. Lift the input manager and pose/wall logic; name their state.
5. First features: dual-stick poses, controller shaking.
6. Launcher (game file, features, controllers): v1 done; building the game's files from the WAD
   with the player's key done; Wii Remote pairing to come.
