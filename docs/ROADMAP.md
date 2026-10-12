# Roadmap: from "native code on emulated hardware" to a native port

## Where we are (2026-10-05)

| Layer | Today | Emulated? |
| --- | --- | --- |
| CPU (game, NU engine, SDK, MSL) | Translated ahead of time to C++ and compiled for arm64 | **No** (native code). Guest memory is still a big-endian array with byte swaps, and MMIO is caught on load/store. |
| OS (threads, alarms, interrupts) | Guest OS code runs translated. Threads are host fibers; interrupts are polled | Partly (decrementer, VI and PI interrupts are synthesized) |
| Graphics | The SDK's GX library is native C++ (`gx_api.cpp`, all 168 gx.a functions, byte-identical command stream). It feeds the renderer's GX state directly; display lists and inline vertex data (game data and inlined writes) are decoded by `gx.cpp`. Rendering on SDL_GPU (Metal on macOS) | **No** at the API level since 2026-10-06 (`WP_NATIVE_GX=0` runs the translated library). The renderer still interprets the GX command format, which is also the game's display-list data format |
| Audio | AX (CPU side) builds command lists; a native C++ AX mixer runs them (`audio_hle.cpp`, `ax_hle.cpp`). dsp.a, ai.a and the OS audio bring-up are host functions: no DSP code, mailboxes or AI registers | **No** since 2026-10-06 (emulated DSP kept as `audio.native=0`) |
| Input | Native at the WPAD/KPAD API: 25 entry points answered from SDL; WPAD/WUD/Bluetooth never start (`input.native=2`, default) | **No** (emulated remote kept as `input.native=0`) |
| Storage | CNT archives and game NAND saves at the API level (host files); only SDK-internal SYSCONF/play time use emulated IOS FS | **No** for game files (2026-10-05) |

The Bonkers plan in miniature: milestone A (native CPU, emulated hardware) is reached. Milestone B is native renderer, sound and input with no hardware emulation.

## Milestone B, in order of value per effort

1. **Input at the API level.** *(Done 2026-10-05: fully native and the default. See the section below.)* Replace the WPAD/KPAD entry points the NU engine calls (`WPADRead`/`WPADProbe`/`WPADGetStatus`… around 0x801DB000–0x801E0000, and the engine's input manager at 0x8021F000–0x80229000) with host functions that fill the status structs straight from SDL. That removes Bluetooth/HID/IOS-USB and the extension crypto. Poses then become "set accelerometer vector" calls.
   Needs: naming the WPAD functions (signature-match against a public RVL SDK decomp, e.g. ogws or MKW's), plus the status struct layout.
2. **Storage/OS at the API level.** *(Done 2026-10-05: CNT archives and game NAND saves are native; only SDK-internal SYSCONF/play-time use the FS layer.)* Replace CNT/ARC (`contentOpen*`, `CNTRead`) and NAND (`NANDOpen`/`Read`/`Write`) with host file I/O. IOS emulation goes away for files.
3. **Audio at the AX level.** *(Done 2026-10-06: native mixer, bit-exact with the translated microcode on aligned music. See dev/PROGRESS.md.)* Replace AX voice processing (ADPCM decode, SRC, volume/pan, AUX) with a host mixer driven by the AX parameter blocks. The NU sound engine (`CnuSound2*`) stays translated. That removes the DSP, its mailboxes and ARAM. Verify sample-by-sample against the current DSP path, which already matches Dolphin on Wii Party.
4. **Graphics at the GX API level.** *(Done 2026-10-06: gx.a ported to C++ and verified byte-exact, see dev/PROGRESS.md.)* The SDK's GX functions and the engine's `nuWiiRenderDevice` call into a small native GX layer (Metal on Apple, Vulkan/GL elsewhere), the way Aurora does for Prime. Inline vertex writes (`GXPosition3f32` stored straight to 0xCC008000) remain, so keep a slim vertex-stream decoder for those. EFB copies become render-to-texture.
5. **Readable code.** Lift game functions (about 2,700 in 0x8001FF40–0x8014F4DC) to hand-written C++ one subsystem at a time: pose/input logic, stage flow, rival AI, UI glue. Each replacement is checked with `make lockstep` against Dolphin. The recompiler already supports replacing a function with a host implementation (`hle_functions.csv`), so mixed translated/handwritten code works from day one.

## Native input: dropping Bluetooth (step 1, done 2026-10-05)

Implemented in `engine/src/input/input_hle.cpp`. Verified: no `/dev/usb/oh1`, menus, pointer (mode select), poses/walls, HOME menu cursor and Reset dialog, gamepad path, and a 10-game-minute smoke run. Original plan:

To remove the Bluetooth stack (bte.a, about 560 functions) and WUD entirely, every WPAD/KPAD entry point that something calls
needs a native version:
- **Init and connection:**
  - KPADInit 0x801B71D0 → no-op (skips WPAD/WUD/BTE init). WPAD allocator 0x801DB820 → no-op.
  - KPADSetConnectCallback 0x801B7860 (engine handler 0x8021F230) → store, then call `cb(0, WPAD_ERR_NONE)` once at start.
  - WPADProbe 0x801DCB70 → channel 0: type 1 (Nunchuk), result 0. Others: -1.
- **Data:** WPADRead (done). WPADGetLatestIndexInBuf 0x801DD710 / auto-sampling buffer (0x801DD5B0 from 0x8021F8B8).
  KPADRead 0x801B6960, used by the HOME-menu cursor path 0x80228F30, needs KPAD-style `pos` from the pointer.
- **Control, which can return success:** data format, DPD on/off (0x801DCDD0/0x801DF520 from 0x8021F8B8),
  motor (WPADControlMotor/EnableMotor/IsMotorEnabled: route to SDL rumble), speaker (0x801DD990/0x801DD9E0,
  WPADCanSendStreamData, 0x801DF240: accept and drop, or mix into host audio), WPADGetInfoAsync 0x801DCF00
  (battery for the HOME menu; must fire its callback), radio sensitivity, WPADSaveConfig.
- **HOME-menu callers** (0x80248000+): 0x801DB7F0/800/810, 0x801DCC90, 0x801DCD20, 0x801DEFE0, 0x801DF020, 0x801E2690.
  Name each one before replacing it.
- **Verification:**
  - boot to title with Bluetooth never opened (no `/dev/usb/oh1` in the IOS log);
  - HOME menu cursor, battery and the Reset dialog;
  - the pose wall test (`/tmp/posetest.sh`-style run in tools/smoke.py);
  - gamepad virtual pad (`WP_VIRTUAL_AXES`).

## Prerequisites that help everything

- **SDK symbol names.** dtk names only about 350 functions. Signature-matching the RVL SDK (Feb 2009, CW 4.3 build 145), MSL and the HBM library against an existing decomp would name most of the 4,900 library functions. That makes 1–4 tractable and shrinks the lockstep ignore list to exact objects.
- **Deterministic mode.** A virtual clock instead of wall time (about 50 call sites use `steady_clock`), with synchronous GPU/finish interrupts. Then scripted runs replay exactly: golden-image regression tests, brute-forcing walls for automated full-game runs, and tighter lockstep.
- **Input movies.** Dolphin `.dtm` recordings of real play, plus a matching port input script, for lockstep beyond the attract demo.

## Known differences to Dolphin (lockstep, see docs/PORT.md)

- Strap screen: resolved, same behavior as Dolphin (see dev/PROGRESS.md 2026-10-05). Dolphin boots about 100 retraces slower.
- SDK and engine thread stacks and audio voice state differ (host timing). Game-owned data matched Dolphin through boot (frame 300). The CNT content descriptor difference (1 vs 7) was fixed by allocating ES descriptors like IOS.

## Android

Target: Android phones, distributed as an APK on the releases page (not the Play Store: it
contains translated game code). Controls: gamepads (as on desktop) and touch controls.

Done (docs/BUILDING.md, "Android"):

- The game runs on arm64 Android 8+ with the SDL_GPU renderer on Vulkan, SDL audio and gamepads;
  guest threads switch with a small ARM64 context switch (Android has no `swapcontext`); the app
  pauses in the background and resumes.
- Touch controls (feature `touch_controls`, docs/FEATURES.md): the stick guides are the sticks,
  plus a pause button, the back gesture and tap-to-point in menus.
- First start: the app asks for the player's WAD and Wii common key (Android's file picker, a
  paste, or typed) and builds the game files in its folder with the launcher's install code.
- `tools/release/android.sh` builds the APK; a development build can pack the builder's own game
  files instead. `tools/release/all.sh` builds it with the other downloads, signed with the
  release key.

Still to do:

1. **Settings.** There are no launcher pages on Android: a first start gets the Enhanced preset,
   and `settings.ini` in the app's folder is the only way to change features. Changing the game
   file afterwards means clearing the app's data.
2. **Testing on real phones.** So far it has run on the Android emulator (arm64, on Apple
   Silicon) and one phone, where it plays below full speed; the range of phone Vulkan drivers
   needs more devices. The golden replays have not been run on Android.
3. Smaller things: a HOME button and the options menu (F10 on desktop) on touch, 32-bit ARM and
   x86_64 builds if anyone needs them.

