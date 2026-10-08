# Muscle March game code notes

What the game's own code does, as far as it has been read or lifted. Addresses are for the
USA WiiWare release (WMME). Lifted code lives in `port/games/musclemarch/src/`.

## Game flow (src/scene/)

`Game_Update` (fn_8002B2FC) runs once per frame: it clears the in-race flag, runs the controller
check, and dispatches on the game sequence (+0x0) to the scene updates (all in src/scene/ except
the race):

| Seq | Function | Scene |
| --- | --- | --- |
| 0 | fn_8002B528 | boot: notices, save loading |
| 1 | fn_8002B844 | reset the game, on to the title |
| 2 | (inline) | on to 3 (or 4) |
| 3 | fn_8002B9C0 | title menu (600 idle frames start the attract demo) |
| 4 | fn_8002BE38 | opening movie (A, B, 1, 2 skip) |
| 5 | (inline) | a layout, back to 3 |
| 6 | fn_8002C0D0 | select menu |
| 7 | fn_8002C3E0 | next stage card |
| 8 | fn_8002C544 | the race (src/race/race_scene.cpp) |
| 9 | fn_8002D990 | ending |
| 10 | fn_8002DB98 | final results |
| 11 | fn_8002DD50 | endless round transition |
| 12 | fn_8002DEB8 | saving |
| 13, 14 | fn_8002D688, fn_8002D810 | result screens (13: A retries) |
| 15 | fn_8002E9B0 | info screen (one of six pages at random) |
| 17 | fn_8002465C | debug parameter menu |

While the controller warning menu (0x1F) is open only its handler (fn_8002E014) runs. Menus
are layouts driven by events: the pointer position, or key codes ('w' 's' 'a' 'd' for the
d-pad, 'x' to confirm, 'z' to go back, 'q' for +).

**Boot** (fn_8002B528, sub-state +0x8): 1-2 the first notice (any button 0x7F1F fades it out),
3-5 the second notice (A, B, 1 or 2), 6-9 save check and load, 0xA the controller check raises
its first warning (the "Wii Remote / Nunchuk required" screen), 0xB on to sequence 1.

**Controller check** (fn_8002ED1C): in play, a missing Remote raises warning 3 and a missing
Nunchuk warning 4; each opens menu 4 with the "w_start" animation and pauses the game.

**System menus** (fn_8002E014, while menu 0x1F is open): the controller warning dialog (menu
4): warning 1 is the boot notice, which closes by itself after 300 frames; warnings 2-4 track the
Remote/Nunchuk and close with A. And the race's pause menu (menu 1, the Remote's 1 button, 0x200 in the WPAD layout the game reads, during the race frame):
the pointer or keys 'w' 's' 'a' 'd' (d-pad), 'x' (A/1), 'z' (B/2), 'q' (+) drive the layout's
menu; the choice (+0x18) resumes, retries or quits to the results.

Planned features hook here: skipping the notices with any button (sub-states 2 and 5) and
removing the controller screen (warning 1), each behind its own flag.

## Controls: from WPAD to poses

```
WPADRead (native, input_hle.cpp)
  -> pad update            fn_8021FA1C   (lifted: src/input/pad.cpp, "Pad_Update") button masks + key repeat
  -> player motion update  fn_80041800   (lifted: src/input/motion.cpp, "PlayerInput_UpdateMotion")
       -> pose (+0x940), pose meters, arm tilts, sideways tilt flags
  -> shake power           fn_8002F09C   (lifted: src/race/shake_power.cpp, "Race_UpdateShakePower")
       -> sideways shaking   fn_80042490   (lifted: src/input/shake.cpp, "PlayerInput_AverageSampleChange")
  -> race update           fn_80030EB8   (lifted: src/race/race_update.cpp, "Race_Update") wall check reads the pose
  -> race speed            fn_80032420   (lifted: src/race/race_speed.cpp, "Race_UpdateSpeed")
```

### Player input object (`PlayerInput`, src/input/player_input.h)

One per player, at 0x90105980 for player 1 in a typical run, reached through the game object
(fn_80020F4C) and fn_80020F68(game, player). It starts with a controller-sample history: entries
of 0x44 bytes, entry 1 = this frame, up to entry 32, shifted every frame; each entry holds 14
signed 16-bit samples at +0x28 (sample 11 = Nunchuk sideways tilt, 13 = Remote sideways tilt).

| Offset | Meaning |
| --- | --- |
| 0x000 | WPAD channel |
| 0x008 | read state: nonzero = controllers are not read (stack garbage is used, as in the original); -1 = no controller (fn_8004239C tests `!= -1`) |
| 0x010 | extension type (2 = Nunchuk) |
| 0x044 | this frame's WPADFSStatus* |
| 0x082 / 0x086 | Nunchuk / Remote sideways tilt, -256 * y of the normalized acceleration |
| 0x8C8..0x8D4 | pose meters 0..3 (0..1, +/-0.1 per frame toward the held pose) |
| 0x8D8 / 0x8DC | Remote / Nunchuk "tilted" flag (on beyond +-170, off inside +-160); 0x8E0/0x8E4 last frame's |
| 0x908 | OSGetTick of the last update |
| 0x90C | a meter is between 0 and 1 (pose changes wait) |
| 0x90D | the pose changed this frame |
| 0x910..0x924 | Remote arm: tilt, speed, class, moving, previous tilt, peak speed |
| 0x928..0x93C | Nunchuk arm: same |
| 0x940 | **pose** (0 both up, 1 both down, 2 Remote up + Nunchuk down, 3 Remote down + Nunchuk up) |
| 0x944 | hold timer (seconds) |

### Arm classification

Tilt = half the z component of the normalized acceleration (after swapping y/z), unwrapped to
[-1, 1] when the controller faces away. Thresholds (table at 0x8027EA98): up above 0.003, level
down to -0.075, down below. The "level" band is narrow, so in practice each arm is either up or
down. Either arm level means "no pose" (4).

### Accepting a pose

Both arms must be still (speed under 0.1 tilt per frame). The pose is then accepted at once after
a fast swing (peak speed above 0.3), or after holding still for 2 frames (10 frames for "no pose").
It is stored only while no pose meter is mid-transition.

### Wall check (fn_80030EB8, around 0x80031324)

Each frame the race takes the runner's distance minus 1 (race +0x5E0), finds the segment there
(fn_8003BA48) and its required pose in the course's wall table (course +0xC04 + 4 x segment;
3..6 = pose + 3, 2 = any). When distance - 1 and distance fall in different segments
(fn_8003C280 with step 1), the runner is at the wall, and a wrong pose crashes through it: crash
animation 0xB, the wall entry becomes 2, one life is lost (race +0x194; 0 lives sets state 4 =
game over) and hits are counted (+0x41C, +0x464). The pose is PlayerInput +0x940 (above).

Walls are dealt by the special runner (runner 4, race +0x12CA0, the one being chased; fn_8002FBDC,
src/race/deal_walls.cpp): when it enters a segment whose wall is still undecided (1), the wall's
required pose becomes the segment's kind + 3 (race +0x5A804, 0..3; the kind is rolled at random
in endless mode), or "any" (2) while the player is in animations 0xC, 0xE, 0x10 or 0x12. So
the next wall's pose is known as soon as the special runner passes it; a pose-difficulty
feature would change the kind before it is dealt or the check in the race update.

### The race scene (src/race/race_scene.cpp)

fn_8002C544 runs the race scene by phase (race +0x4): 0 set up, 1 wait for loading, 2 start,
4 race start (counters reset), 5 wait for the intro, 6 the race frame, 7/8 race end, 9 results
(+0x0 = the next scene: next stage, retry, menu). Game kind (+0x54): 0 story, 1 endless, 2 the
attract demo (fades out after +0x1FC frames; A, B, 1 or 2 skip to its end). In the race frame:
the countdown is 300 frames (race state 0); in state 1 button 0x200 pauses; state 2 is the
rival-crash slow motion; states 3/4 show the results (after 180 frames, or A/B to retry).
Then shake power, the race update and the other systems run, and when the player enters a new
segment the passed wall is counted (combo +1, sound 9) and the segment sets the race mode
(+0x198): 1 = the special runner's dash zone (a segment longer than 100 while chasing it),
3 = course end, else 0; plus the running effect by difficulty.

### The race update (src/race/race_update.cpp)

Every race frame, in order: move the runners and update speeds; course-end and game-over states
(race +0x38: 3 = course end, 4 = game over, 2 = rival crashed, 6 = special runner finished);
running effects (shake modes) or pose effects and the pose-change pop; the player's wall check;
the gauge streak and cheers; the combo crash (after CRASH_COMBO_COUNT_MAX walls in a row the
target runner, race +0x53440, crashes; 4 = the special runner); the feint pick; then each of
the five runners (0x49B0 bytes each from race +0x5E0) reacts to the wall ahead: pose animations
1..4 or dash animations 0x16..0x19 by the wall's kind (race +0x5A804 per segment), timed so the
pose lands at the wall (45 / time to the wall); the target runner may feint (animation 0x27).
The current race is also reachable through the global at 0x804223D0.

### Speeds (src/race/race_speed.cpp)

Called first in the race update, by race state. Racing (state 1), each runner's speed is:
base = SELF_DASH_RATE for the player in the shake modes (in mode 2 plus AUTO_DASH_MUL_COMBO x
combo, capped at AUTO_DASH_COMBO_SPEED_MAX), RIVAL_DASH_RATE for the others, plus
RIVAL_MUL_NUM_SPEED x the target runner's index; then multiplied by the course's speed zones
(zone lists per segment: type 10 = speed factor over a position range, type 0x11 = "in a zone",
which also drives the running sound), raised by MIDDLE_LENGTH_MAP_SPEED_UP on long segments, read
from a data sheet in the special mode (capped at SPECIAL_SPEED_MAX), and lowered 1% when within
2.5 of the runner ahead. In the shake modes the player also gains shake power x 0.2 (mode 2) or x
GACHA_DASH_RATE (mode 1) while the target is ahead. Mode 1 rolls the special runner's dash
speed every 30 frames from the DASH_ZONE_* parameters. Other states: 0 countdown, 2 rival
crashed, 3 course end, 4 game over, 6 the special runner caught (slow motion, line-up), 7 the
celebration (flexes and cheers, then the race ends at 240).

### Small accessors (not lifted; trivial)

| Function | Returns |
| --- | --- |
| fn_8004239C | read state (+0x8) != -1: a controller is there |
| fn_800423B4 | extension (+0x10) == 2: Nunchuk attached |
| fn_80042584(input, 0/1) | Remote/Nunchuk just became tilted sideways (+0x8D8/+0x8DC set, last frame's clear) |
| fn_800423C8, fn_80042460 | rumble pattern table at 0x8027EA28 (stride 0x1C per channel) |
| fn_80042E58/74, fn_80042E90, fn_80042FB4..FCC | pointer (IR) state at +0x948..+0x97C |

## Buttons (pad object, fn_8021FA1C)

One per channel, updated every frame: WPADRead, then held buttons (+0x1C), pressed (+0x14),
released (+0x18), ~held (+0x20) and key-repeat pulses (+0x24). Repeat timing is in ms
(delay +0x6F4, interval +0x6F8); mode +0x6EC = 0 repeats the whole button set, 1 repeats each
button separately (per-button timers at +0x6FC, states at +0x77C). Menus and "press A" screens
read `pressed`/`repeat`, so skipping a screen with any button means acting on `pressed != 0`.

## Course layout (src/race/course.cpp)

The course object (race +0x59CF0) is a list of segments: type per segment at +0xED4, length per
type at +0x21D4, count at +0x11A8. Its tail loops: past `loop_end` (+0x11C0, int) the stretch
from `loop_begin` (+0xA2E0) repeats. fn_8003C204 gives the lap in the loop for a distance;
fn_8003BA48 gives the segment index (the wall check looks up the wall this way).

## Shaking: running speed

There is no separate shake sensor: shaking is measured from the sideways tilt the motion update
already stores (-256 * normalized y, per controller).

- **fn_80042490** (input, sample, frames): average of |sample(entry i) - sample(entry i+1)| / 512
  over the last `frames` entries (frames <= 30, else 0).
- **fn_8002F09C**, once per race frame: shake power (race +0x5D4) = sum over Remote and Nunchuk of
  2 x average change over 20 frames x GACHA_LEVER_POWER (1.2), capped at 1. In auto-dash mode
  (race +0x198 == 2) it is AUTO_DASH_RATE[difficulty]. It also copies the four pose meters into
  the race object (+0x6D890) and scales them to HUD gauge animation frames (+0x6D8A0).
- **fn_80032420** (race speed) moves the runner by shake power x GACHA_DASH_RATE[difficulty].

A controller-shake feature therefore only needs to produce sideways-tilt history (or replace shake
power directly) and leaves the rest of the game untouched.

## Tuning parameters (race object +0x484..+0x5D0)

The debug parameter menu (fn_800246DC) names 102 values; the name table is at 0x8026F5B0 and each
value lives in the race object. Notable ones (EASY/NORMAL/HARD are consecutive floats, indexed by
difficulty at race +0x1C0):

| Offset | Name |
| --- | --- |
| 0x484 | SPEED_* |
| 0x490 | FEINT_RATE_* |
| 0x4AC | SELF_DASH_RATE_* |
| 0x4C4 | GACHA_DASH_RATE_* (shake power -> speed) |
| 0x4D0 | DEFALT_LIFE_COUNT_* |
| 0x4E8 | AUTO_DASH_FLAG |
| 0x4EC | AUTO_DASH_RATE_* |
| 0x538..0x5AC | DASH_ZONE_* (random count, base speed, exchange rates, both-up speed) |
| 0x5B8 | GACHA_LEVER_POWER (shake sensitivity) |

The arm tilt thresholds (POSE_MOTION_RANGE_* / NEUTRAL_RANGE_*) are the table at 0x8027EA98, and
SUCCESS_RANGE_UP/DOWN/RIGHT/LEFT follow; these are not in the race object.

## Ideas for features (see docs/FEATURES.md)

- Dual-stick poses: an input transform that writes accelerometer values putting each arm up or
  down (or the pose directly), with stick slices that narrow over time.
- Controller shaking: drive the sideways-tilt samples (or shake power) from stick wiggles or
  button mashing.
- Difficulty: the tilt thresholds, hold times and the tuning parameters above are the natural knobs.
