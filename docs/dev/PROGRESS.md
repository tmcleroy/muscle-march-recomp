# Progress

## 2026-10-07 — The race and the game flow lifted; fast verification

- **Lifted** (all verified call by call with the stack, 0 mismatches, over both movies):
  - `race/race_speed.cpp` fn_80032420: every runner's speed by race state.
  - `race/race_scene.cpp` fn_8002C544: the race scene phases and the race frame (countdown,
    pause, rival-crash slow motion, results, wall bookkeeping, the race mode per segment).
  - `scene/game_update.cpp` fn_8002B2FC: the per-frame dispatch over game sequences.
  - `scene/boot.cpp` fn_8002B528: the notices and save loading at boot.
  - `scene/controller_check.cpp` fn_8002ED1C and `scene/system_menu.cpp` fn_8002E014: the
    controller warnings (incl. the boot "Wii Remote / Nunchuk" notice) and the pause menu.
  - These are where the planned intro-skip and controller-screen features will hook (docs/GAME.md).
  - Every other scene of the game flow: `scene/transitions.cpp` (to the title, next stage),
    `scene/title.cpp`, `scene/opening_select.cpp` (opening movie, select menu),
    `scene/screens.cpp` (ending, final results, endless rounds, saving, result screens, info).
    docs/GAME.md has the full sequence table.
- **Verification:** `WP_VERIFY_FAST=1` tracks written pages copy-on-write (~50x faster, so every
  call can be verified); `WP_LIFT_WHEN` keeps calls that wait for I/O out of verification.
- Not exercised by any movie yet: the pause menu, controller warnings 2-4, endless mode and its
  round screen, result screen B, the info screen, the course end. Saving is checked by replay
  only (it waits for I/O).

## 2026-10-07 — Race code lifted: shaking, buttons, course, the race update (step 4 continues)

- **Lifted, readable C++** (`port/games/musclemarch/src/`), each verified byte-exact against the
  translated original on every call, stack included (`WP_VERIFY_STACK=1`):
  - `input/shake.cpp` fn_80042490: how much a controller is shaken (sideways-tilt change).
  - `race/shake_power.cpp` fn_8002F09C: shake power (x GACHA_LEVER_POWER) and the pose gauges.
  - `input/pad.cpp` fn_8021FA1C: button masks and key repeat.
  - `race/course.cpp` fn_8003C204/8003BA48/8003C280: course segments, loop laps, wall crossing.
  - `race/race_update.cpp` fn_80030EB8: the whole per-frame race update (wall check, crashes,
    lives, gauge cheers, combo crash, feints, the five runners' wall animations).
- **Stack fidelity:** the game reads stale stack slots, so lifts keep the original's register
  values at every call and leave the same frame bytes (`lift::OriginalFrame`, `gcall_from`).
- **Deterministic mode:** interrupts, IPC and GX processing happen only at idle polls, so lifting
  code cannot move them.
- **Test tools:**
  - `WP_AUTOPLAY=1` (game `src/debug/autoplay.cpp`, `tools/smoke.py --autoplay --record X`): holds the pose each wall
    asks for. The new movie `movies/autoplay_story.wpm` clears two stages with 20+ combos and
    covers combo crashes, the special runner, feints and shake effects.
  - Movie format v3 (`tools/movie_pack.py`): runs of identical samples, ~300x smaller.
  - `WP_LIFT_OFF=Name,...`, `WP_VERIFY_ONLY=Name,...`, `WP_VERIFY_SHOW=N`; the verify summary
    prints when a replay ends.
- Not yet covered by any movie: endless mode (locked in a fresh save) and the course end.

## 2026-10-06 — First lifted game code: player motion and poses (readability plan, step 4 begins)
- `port/games/musclemarch/src/input/motion.cpp`: readable C++ for fn_80041800, the per-player motion update
  that turns both accelerometers into arm tilts, a pose and pose meters.
  - Named fields (`PlayerInput` view) and documented thresholds. See `docs/GAME.md` for how WPAD data reaches
    the wall check.
  - Verified: `WP_VERIFY=1 WP_VERIFY_EVERY=1` compared all 5,698 calls of a gameplay run with the translated
    original, 0 mismatches.
  - Lifted and translated replays of the golden movie are RAM-identical.
  - `WP_LIFTED=0` switches lifted game code off.
- Deterministic time now advances only while the game idles. Lifted C++ has no poll sites, so it would
  otherwise shift every timestamp.
  - A spin-wait guard makes time move after 1M busy polls.
  - The golden movie was re-recorded under this model.
- `lift.h` gained `ppc::fadds/fsubs/fmuls/fdivs/fmadds/fnmsubs`: single-precision arithmetic exactly as the
  translated code rounds it.
- `WP_VERIFY_EVERY=N` sets how often calls are verified.

## 2026-10-06 — Deterministic replays and golden tests (readability plan, step 1)
- **Deterministic mode** (`WP_DETERMINISTIC=1`, implied by recording/replay):
  - Guest time follows executed work, not the wall clock: a fixed time step per interrupt poll, and bigger steps while the
    game idles in OSSelectThread. The clock starts at 2026-01-01.
  - The time base, decrementer, VI retraces, IPC, the AI/audio clock and the Wii Remote motion model all use it.
  - GX commands are processed on the CPU thread, so GPU results (copies to RAM, draw-done) land at fixed points.
  - Throttled to real time; `WP_DETERMINISTIC_FAST=1` runs unthrottled with the same result.
  - Four parallel runs of a scripted session were identical frame by frame; windowed real-time and headless fast runs too.
  - The last nondeterminism found was the remote motion model stepping by the wall clock.
- **Input movies:**
  - `make record NAME=x` plays from a fresh save and writes `movies/x.wpm`; `make replay NAME=x` plays it back exactly.
  - Format: every controller sample the game reads (`wp/movie.h`).
- **Golden tests:**
  - `make replay-test [FRAMES=1] [BLESS=1]` replays every movie headless and compares RAM (and frame) hashes every
    60 retraces with `build/golden/` (kept local, since they're derived from game memory).
  - First movie `movies/smoke_warmup.wpm` (scripted: boot, menus, Warmup City race with shakes and poses):
    302 checkpoints, about 100 s per run.
- **Lift toolkit** (`wp/lift.h`): the GX verifier generalized for any lifted function. GX moved onto it, unchanged.
- **Feature registry** (`wp/features.h`): presets pure/enhanced/custom in `settings.ini`, recorded in movies (v2).
  - Game folders `src/` (lifted code) and `features/` are ready. There are no features yet, so the default is the original game.
- **Plans:**
  - `docs/FEATURES.md`: opt-in feature system (all off = the Wii game).
  - Launcher: game file chosen by the user and only read; feature toggles; Wii Remote pairing.

## 2026-10-06 — Graphics option A: the GX library is native C++
- All 168 functions of the SDK's gx.a are hand-written C++ in `port/engine/src/gpu/gx_api.cpp` (about 8,000 lines of PPC ported).
  - The SDK's GX code no longer runs.
  - State stays in the SDK's `__GXData` block, and commands go straight to the renderer queue.
- Verification:
  - `WP_GX_VERIFY=1` runs the translated original (whole call tree) and the port on the same input, and compares
    memory, registers and command bytes.
    - Gameplay, menus and the HOME menu: **0 mismatches** across more than 10 million calls.
  - `WP_GX_SELFTEST=1` calls 54 rarely used functions 100 times each with random arguments.
    - Only difference: `__GXShutdown` stores the current time, which naturally differs between runs.
  - Headless frames are **byte-identical** to the translated library.
- Not verified: calls that wait for the GPU (GXDrawDone, the token/finish/CP interrupt handlers, aborts).
  They ran in normal play.
- What still "interprets" GX: `gx.cpp` decodes the command format, which is also the game's display-list data format,
  and inline vertex writes.
- Fallback: `WP_NATIVE_GX=0`.

## 2026-10-06 — Native audio (roadmap step 3)
- Audio no longer emulates the DSP. `audio_hle.cpp` replaces the SDK's dsp.a, ai.a and `__OSInit/StopAudioSystem` (20 entry points),
  and `ax_hle.cpp` runs AX command lists in C++:
  - It's a port of Dolphin's AXWii HLE, reusing the vendored DSP accelerator for ADPCM/PCM.
  - The microcode is recognized by Dolphin's hash: 4cc52064.
  - A host AI clock plays the DMA buffers at 32 kHz and runs the AI callback like the interrupt.
- Verified:
  - Aligned 0.5 s windows of title music are **bit-identical** to the translated-microcode path (WP_DUMP_AUDIO).
  - A 6-minute gameplay smoke run: up to 11 voices/frame, no crashes or unknown commands.
  - The HOME menu opens and closes; its sound plays and the game audio pauses.
- Only 2 DSP register accesses remain per run (OS interrupt masking), stored and otherwise ignored.
- The mixer costs <1% of the CPU thread. The emulated DSP cost 11–14%.
- Fallback: `WP_NATIVE_AUDIO=0` / `audio.native=0`. It's a per-game opt-in (`Description::native_audio`), so Wii Party is unchanged.

## 2026-10-06 — Graphics step, option B: SDL_GPU renderer (Metal)
- New backend `engine/src/gpu/gx_render_gpu.cpp` on SDL_GPU, now the default on macOS (`-DWP_RENDERER=gl` keeps OpenGL).
  Same GX decoding, TEV uber-shader, EFB copies, texture cache and custom textures as the GL backend.
  - Shaders are generated from the GL GLSL (`port/tools/gen_gpu_shaders.py`) into SPIR-V + MSL (`gpu_shaders.inc`).
  - No dual-source blending in SDL_GPU, so TEV-alpha blends draw in two passes.
  - The GX thread records each frame into one command buffer, and the window (main) thread presents to the swapchain.
- Verified:
  - headless captures (boot logo, strap, title) match OpenGL pixel for pixel. The attract demo differs only in animation timing;
  - 2× scale works;
  - windowed swapchain present works, locked at 60 fps;
  - a 6-game-minute smoke run (Warmup City stages, walls, tutorial scenes) showed no defects.
- CPU-side render cost is about 10 ms/s, down from about 290 ms/s with GL (Apple's GL runs on a translation layer over Metal).
- Not verified yet: Vulkan (Linux/Android) and D3D12 (Windows) builds of this backend; custom texture packs on SDL_GPU.
- Next: option A (GX API-level entry, ROADMAP step 4) when the user asks, or AX-level audio.

## 2026-10-05 — Native input (roadmap step 1) done
- The whole controller stack is native and the default (`input.native=2`): KPADInit no longer starts WPAD/WUD/Bluetooth,
  and 25 WPAD/KPAD entry points are host handlers.
  - The Bluetooth stack (560 functions) and the extension crypto no longer run.
  - Fallbacks: `WP_NATIVE_INPUT=1` (native WPADRead only) and `0` (emulated remote).
- Fixed the native DPD encoding (WPAD flips camera y, size ×4), so the game's own pointer works (mode/character/stage select).
- Verified: menus, poses, HOME menu (cursor, battery, Reset dialog), pointer, 10-game-minute smoke run with no Bluetooth.
- Next roadmap step: storage (CNT/NAND at the API level), then audio (AX mixer), then graphics.

## 2026-10-05 — Storage step, part 1: native content archives
- The 8 CNT entry points (CNTInit/Shutdown/InitHandle/ReleaseHandle/Open/GetLength/Read/Close) are host handlers that read
  `<extracted>/files/NN/<path>` with stdio. Default on; `WP_NATIVE_STORAGE=0` falls back to the emulated ES content path.
- Verified: zero ES content ioctls; title, gameplay and HOME menu (shared contents 5–7) load normally.
- **Saves native too:** 18 NAND entry points are host handlers over the engine's host NAND: Open/Close/Read/Write/Seek/
  GetLength/GetStatus/GetType/Create/CreateDir/ReadDir/Delete/Move/Check/GetAvailableArea and
  SimpleSafeOpen/Close/Cancel (temp copy committed on close).
  - Verified: save created, then loaded on the next boot ("check/load complete"), zero IOS FS requests.
  - save.dat and banner.bin are byte-identical to the emulated path.
  - Only the SDK's own NAND use (SYSCONF, play time) still goes through the FS layer.
- Trace with `WP_LOG_NAND=1` (`WP_LOG_NAND_STACK=1` for call stacks). Fallback: `WP_NATIVE_STORAGE=0`.

## 2026-10-05 — Phase 2, session 2: controls, verification, test tooling
- **Poses:** a raised controller reads −Y (found by experiment). Keys I/K/J/L = up/down/left/right pose,
  Left Shift = shake both. Gamepad: right stick = poses, LT = shake, LB = Z. A scripted run cleared a wall with the matching pose.
- **SDK names:** `tools/apply_dsy.py` matches Dolphin's bundled signature DB (`totaldb.dsy`, opcode-only hash), so
  about 1,260 functions are named (was about 350), including library/object names.
- **Lockstep:**
  - Granules owned only by SDK code are ignored by default (owner = code region that references the symbol).
  - Game-owned data matches Dolphin through about 1,500 frames. The rest is SDK/engine internals (stacks, audio voices, IPC).
  - Fixed a real difference: ES content descriptors now reuse the lowest free number, like IOS.
- **Strap screen: not a bug.** Both Dolphin and the port flip strap pages every 300 retraces and loop several times.
  The "one cycle in Dolphin" observation came from Dolphin's frame dump skipping duplicate frames. Dolphin's boot
  reaches the strap about 100 retraces later (slower IOS boot).
- **Test tooling:**
  - `tools/smoke.py`: unattended play (A through menus, shake + random poses, periodic frame captures, crash detection).
  - `make port-test`: a separate build with instruction patches (`analysis/test_patches.csv`). Currently infinite stamina:
    stamina is field 0x194 of the race object, decremented at 0x8003149C in the race update 0x80030EB8.
  - `tools/ramsearch.py` + `WP_CAPTURE_RAM=1`: RAM search over MEM1/MEM2 dumps taken at frame captures.
  - `WP_WATCH=addr`: logs the guest call stack whenever a word changes.
  - Hook probes: `WP_HOOK_PRINT/ONLY/WORDS/STR/ABS`.
- `tools/callgraph.py` (callers/callees/refs/show over dtk asm), `tools/montage.py`.
- `docs/ROADMAP.md`: what's still emulated and the order to make it native.
- **Native input (roadmap step 1, first part):**
  - `WPADRead` (0x801DD510) is replaced by a host handler that fills the game's `WPADFSStatus` directly:
    buttons; accelerometer centered at 0 (1 g = 104); IR objects; Nunchuk accelerometer (1 g = 204) and stick.
    Channel 0 only. The engine reads controllers only through WPADRead plus a few setup calls, never KPAD.
  - Enable with `WP_NATIVE_INPUT=1` or `input.native=1`. Off by default until playtested.
  - Verified: menus and a cleared wall go through the native path.
  - Recompiler: replaced functions keep their original as `f_X_orig`, so handlers can fall back via `wp::call_original`.
- **Smoke runs (test build: infinite stamina + always-pass walls):**
  - Cleared Warmup, Workout and Max-out City, both "TAKEDOWN" scenes and the stage-clear screen,
    and reached Stage Select with combos up to 36.
  - 10 game-minutes, no crash, no visible rendering defects.
- **HOME menu works** (layout, cursor, the "Reset the software?" dialog).
- **Pointer fix:** the IR pointer was vertically inverted and offset for this game (cursor moved up when the mouse moved down).
  The y mapping is now per game (`Description::pointer_y_scale/offset`; Muscle March uses −238/−88, calibrated on the HOME menu cursor).
  Wii Party's default is unchanged.
- **HOME menu exits:** Wii Menu/power-off quit the app cleanly; Reset restarts it (OSReturnToMenu, OSShutdownSystem,
  OSRestart, OSResetSystem are host handlers).
- **`make app`:** `build/MuscleMarch.app`. Saves and settings go to `~/Library/Application Support/MuscleMarch`; it reads `./extracted`.
- **Gamepads verified** with a virtual SDL pad (`WP_VIRTUAL_AXES`): menus, right-stick poses, LT shake. Headless runs now read gamepads.
- **Linux arm64 (Ubuntu 24.04, GCC, headless, in Docker):**
  - builds and boots to the title data load;
  - translator tests: 4,152/4,152;
  - fixed: x86-only FMA detection (arm64 now uses hardware FMA, on the Mac too) and a duplicate `load_tlut` in the no-SDL build.
  - The runtime PNG test needs SDL.
- **Smoke run t3** reached a bonus stage ("Mission: Chase Mech Muscle G", rainbow road) with no issues.
- **Smoke coverage (test build):**
  - All 3 City and all 3 Village stages, bonus missions, results/rank screens ("WOW! PERFECT!", "Muscle God"),
    stage and character select, back to title.
  - 25+25 game-minutes, no crashes. Station is not reached yet: still locked, or the pointer position (0.47,−0.24) is wrong.
- **Paused here (2026-10-05).** Next:
  - review smoke run t3 (`build/smoke/t3`, stage select with Right);
  - press Yes on HOME→Reset / Wii Menu and make sure the port exits cleanly;
  - check the character-select pointer;
  - native input part 2 (WPAD connection callbacks → drop Bluetooth).

## 2026-10-04 — Phase 2, session 1: boots and plays menus natively on macOS
Approach chosen: (b) static recompilation on the wiiparty-recomp engine (GPL-3).
- `port/`: vendored engine and recompiler. Builds on macOS arm64, and the upstream test suites pass,
  including 4,152 instruction tests.
- Game definition in `port/games/musclemarch`. dtk supplies the function boundaries, and 7 SDK functions were identified by hand.
- WiiWare support in the engine: ES content serving from the TMD and `.app` files, title boot memory, title data directory.
- Emulated Nunchuk, with the SDK's extension encryption.
- Audio: the game's AX DSP microcode is byte-identical to Wii Party's (checksum bc128f9d) and runs recompiled. The sound output has signal.
- **Native result:**
  - Boots at a locked 60 fps on an M4: strap screens, Nunchuk notice, Bandai Namco logo, save creation, **title screen**.
  - With scripted A presses: Start, game mode select, character select (3D models render).
  - With no input, the attract/tutorial demo plays.
- Verification loop: `make lockstep` compares game memory with Dolphin at VIWaitForRetrace each frame. The call sequence matches;
  a steady set of SDK/runtime buffers differs (see docs/PORT.md).

### Known gaps / next
- (Done 2026-10-05: lockstep filters SDK-owned data by library region; the big buffers were Bluetooth stack state.)
- (Resolved 2026-10-05: strap screen matches Dolphin.)
- Nunchuk stick isn't mapped yet. Pose and shake controls need a design: gamepad gyro? keyboard chords?
- Audio: listen for correctness. The output is non-silent, but nobody has compared it with Dolphin yet.
- No C++ exception unwinding through translated code (none hit so far).
- Windows/Linux/Android builds untested with this game.

## 2026-10-04 — Phase 1 (analysis) done
- Repo, `.gitignore`, Makefile: `make nand` → `make extract` → `make split` → `make ref`.
- WAD decrypted by Dolphin's install. All contents are hash-verified, and the DOL, U8 and LZ11 assets are unpacked.
- 7,693 functions: about 2.7K game/engine, the rest SDK/MSL/HBM. See `docs/ANALYSIS.md`.
- Dolphin boots headless to the title screen at about frame 1000, deterministically.
