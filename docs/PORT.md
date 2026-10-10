# Port notes

## How it works

1. dtk (`make split`) analyses `extracted/main.dol` and writes `config/WMME01/symbols.txt`.
   It finds 7,693 functions and names about 350 SDK/runtime functions by signature.
2. `port/tools/dtk_functions.py` turns that into the recompiler's `dol_functions.csv` and `symbols.csv`.
   Hand-identified names come from `analysis/symbols_extra.csv`.
3. `port/recompiler/recomp.py` translates every function into C++ (`port/build/musclemarch/generated/dol`, 28 files, ~30 MB).
   Functions listed in `hle_functions.csv` become calls into runtime handlers. Functions in `sync_hooks.csv` get a `wp::sync_hook` call at entry.
4. CMake builds the engine plus the translated code into `port/build/out/musclemarch`. A full build takes about 50 s on an M4.

The game and SDK run translated. Only the hardware is reimplemented: the GX FIFO, VI, DSP/AX, IOS (ES, FS, STM, Bluetooth HID), EXI.

## Changes to the vendored engine (upstream 627abec)

| Area | Change |
| --- | --- |
| macOS build | ucontext needs `_XOPEN_SOURCE` (fiber.cpp). Thread CPU time comes from mach `thread_info` (gx.cpp). Plain linking instead of GNU `LINK_GROUP:RESCAN`. |
| macOS window | Cocoa needs the main thread: `video::run_main_thread()`. On Apple, `main()` runs the game on a 512 MB-stack thread and the SDL window loop on the main thread. |
| WiiWare boot | `title.cpp`: mounts `extracted/title.tmd` and `contents/NN.app`, and serves `/dev/es` (GetTitleId, GetTMDViewSize/View, GetViews, GetTitleDir, Open/Read/Seek/CloseContent, GetConsumption). Low memory comes from the TMD (game ID `WMME`+`AF`, IOS53) instead of a disc header. There's no disc mount, and the title's data directory is created at boot like ES does at install. |
| Title type | Save paths use the mounted title's type (`00010001`) instead of the hard-coded `00010000`. |
| Hard-coded address | The idle sampler's `OSSelectThread` address now comes from the symbol table, not Wii Party's address. |
| Nunchuk | Emulated Nunchuk: I2C 0x52 registers (ID `A4 20`, calibration), extension bit in status reports, extension bytes in data reports, and the SDK's extension encryption. The encryption is Dolphin's `WiimoteEmu/Encryption.cpp` (GPL-2.0+). It's on when `Description::nunchuk` is set. Inputs: `kNunchukC/Z/Shake`, `Sample::stick_x/y`, key actions `nunchuk_c/z/shake`. |
| Lockstep | `sync.cpp`: `WP_SYNC_OUT`, `WP_SYNC_RANGES`, `WP_SYNC_GRANULE`, `WP_SYNC_DUMP`, `WP_SYNC_LIMIT`. `recomp.py --hooks`. |
| Recompiler | `game.toml` may set `extracted` and `dol` (paths outside the game folder). |
| Native audio | `audio_hle.cpp` replaces dsp.a (DSPInit/CheckInit/AddTask/AssertTask/mail functions), ai.a (AIInit/InitDMA/StartDMA/StopDMA/RegisterDMACallback/GetDMA*) and `__OSInit/StopAudioSystem` (per game: `Description::native_audio` + `hle_functions.csv`). A command list sent with `0xBABE0000|size` + address runs at once in `ax_hle.cpp` (port of Dolphin's AXWii HLE for microcodes 4cc52064/347112ba/d9c4bf34/adbc06bd, reusing the vendored DSP accelerator for ADPCM/PCM); the task's resume callback follows at the next interrupt point. A host AI clock plays the DMA buffers at 32 kHz and runs the AI DMA callback when a buffer is latched (`audio_hle::take_interrupt`, delivered by `interrupts.cpp`). DSP registers are inert storage in this mode. `WP_NATIVE_AUDIO=0` restores the emulated DSP. |
| Feature registry | `wp/features.h`: `WP_FEATURE(name, {...})` registers an optional feature; settings `[features] preset=pure|enhanced|custom` (default pure) plus one key per feature, `WP_FEATURES=a,b` for tests; resolved once per run. Input movies (format v2) record the set and replays use it. Game features live in `port/games/musclemarch/features/`, lifted game code in `src/` (linked into the executable; `WP_LIFTED` registers handlers through `HandlerRegistrar`). Features: see docs/FEATURES.md. |
| Lift toolkit | `wp/lift.h` + `lift.cpp`: `WP_LIFT(group, fn, "Name", Ret)` installs a lifted (hand-written) version of a guest function with a per-group switch, and `WP_VERIFY=1` compares it with the translated original (memory, registers, GX bytes; whole reference call tree translated, interrupts paused). Helpers: `gcall`, `GuestFrame`, `rlwinm`/`rlwimi`, `ldf`/`stf`, typed big-endian `Field<T>` and `WP_FIELD` struct views. The native GX library uses it. |
| Native GX | `gx_api.cpp`: hand-written C++ for every function of the SDK's gx.a (GXInit, FIFO, attributes, TEV, textures, lights, copies, transforms, perf counters), installed through `hle_functions.csv`. State stays in the SDK's `__GXData` block (guest memory), so GXGet* and any remaining guest code agree; commands go straight to the renderer queue (`gx::push`). `WP_GX_VERIFY=1` runs the translated original and the port on the same input (whole call tree translated for the reference; interrupt polling paused) and compares memory, registers and command bytes; `WP_GX_SELFTEST=1` adds randomized calls of the rarely used functions. Calls that wait for the GPU (GXDrawDone, interrupt handlers, aborts) are not verified. `WP_NATIVE_GX=0` restores the translated library. |
| Deterministic mode | `deterministic.cpp`: virtual time base for `time_base()` (OSGetTime, decrementer, IPC), VI retraces (`interrupts.cpp`), audio (`audio.cpp`, `audio_hle.cpp`) and the remote motion model (`wiimote.cpp`); `gx.cpp` runs unthreaded. `movie.cpp` records/replays `input::sample`. `WP_DET_HASHES`/`WP_DET_HASH_EVERY`/`WP_DET_FRAMES`/`WP_DET_DUMP` write state hashes or a RAM dump; `WP_LOG_POLLS` prints busy/idle poll statistics. |
| SDL_GPU renderer | `gx_render_gpu.cpp`: the GX renderer on SDL_GPU (Metal on Apple, Vulkan/D3D12 elsewhere), selected with CMake `-DWP_RENDERER=gpu` (default on Apple; `gl` keeps the OpenGL 4.1 backend). Shaders are generated from the GL backend's GLSL by `port/tools/gen_gpu_shaders.py` into `gpu_shaders.inc` (SPIR-V + MSL, committed; needs glslangValidator and spirv-cross only to regenerate). No dual-source blending: "TEV alpha" blend factors draw twice (colour with the TEV alpha, then alpha). The GX thread records a frame into one command buffer (one copy pass + one EFB render pass per segment between copies/clears); the window thread draws the published frame to the swapchain (`video::request_present`). |
| Feature input | `input::add_transform` (features adjust live input before it is recorded; not run on replay), `input::last_gamepad` (raw sticks, triggers, buttons), `input::last_sample`. `kPointInstant` (bit 31): the emulated arms point at once (`wiimote.cpp`). With `WP_VIRTUAL_GAMEPADS`, real controllers are ignored (`gamepad.cpp`). |
| HUD sprites | `wp/hud.h` + `hud.cpp`: small images placed in fractions of the game picture, blended over the finished frame by the SDL_GPU and OpenGL renderers (not yet D3D11), for features' on-screen guides. `hud::picture_size` reports the presented size; `WP_CAPTURE_HUD=1` composites them into captures. |
| Android | `fiber.cpp`: an ARM64 context switch (Android has no `swapcontext`). `platform/android.cpp`: output to the system log, the app's folder as working directory, `env.txt`, game files packed into a development build, a first `settings.ini`. `main()` is SDL's entry there and runs the window loop on SDL's thread, as on macOS. `video_sdl.cpp`: landscape full-screen window, the SDL_GPU device without the Vulkan features the renderer doesn't use, the swapchain made once the surface has settled, the game held in `present()` while the app is in the background. The game is a shared library `libmain.so` (Gradle project in `android/`). |
| Touch | `wp/touch.h` + `touch.cpp`: fingers as a virtual gamepad (two sticks and buttons a feature lays out in picture coordinates) and as the pointer with A; `input.cpp` applies it to channel 0. `hud::window_size`, HUD sprites may lie outside the picture (the window's margins), up to 8 of them. `WP_TOUCH_SCRIPT` for tests. |

## Game definition (`port/games/musclemarch/analysis`)

- `hle_functions.csv`: EXIInit/Lock/Unlock/Probe/GetID, OSRealModeCall, OSLoadContext, OSSwitchFiber, OSPanic, OSReport.
  OSReport is a no-op stub in the retail game, so this makes the game's debug prints visible.
- `symbols_extra.csv`:
  - entry 0x80006310
  - OSSaveContext 0x801C2720
  - OSSwitchFiber 0x801C2890
  - OSRealModeCall 0x801C72F0 (the BATConfig `rfi` trampoline)
  - EXIProbe 0x801A7000
  - OSSelectThread 0x801C95F0
  - VIWaitForRetrace 0x801D5720
- No setjmp/longjmp in this binary.
- `dsp_ucode.csv`: the AX microcode at DOL offset 0x28D7A0 (0x2000 bytes), checksum bc128f9d. It's the same build as Wii Party's and is recompiled by `recompiler/dsp/recomp_dsp.py`. The 4 KB DSP init task (f0c8bf28) stays on the interpreter.

## Lockstep with Dolphin

`tools/lockstep.py dolphin` starts Dolphin with the GDB stub. It needs `Interface.DebugModeEnabled=True`,
otherwise the JIT ignores breakpoints. It breaks at VIWaitForRetrace, which the main thread calls once a frame,
and CRC-hashes each 256-byte granule of the game's static data (`.data`…`.bss2`, 0x8027DC60–0x80424CD0).
That costs about 0.15 s per frame. The port writes the same hashes from `wp::sync_hook`.

`compare` aligns by hit index. It skips granules covered by symbols in `tools/lockstep_ignore.txt`,
or that already differ at a baseline hit (`--baseline 0`). `--trend N` prints the count every N hits,
and `--bytes N` byte-diffs the raw dumps at hit N with dtk names.

The call sequence at the sync point matches Dolphin exactly (same return addresses, frame by frame).
Memory differs from frame 0 in SDK hardware bookkeeping: IPC tables, SYSCONF buffer, thread contexts, SI/EXI.
After about 25 frames, a steady set of about 180 granules differs, mostly large `.bss` buffers used by
runtime/SDK code, mostly the Bluetooth stack (`lbl_80338840`, 0x28AE0 bytes, bte.a). These are now filtered by owner region.
Next step: classify these and grow the ignore list until only game state remains.
