# Phase 1 analysis (2026-10-04)

## Title and contents

Title `00010001-574D4D45` (WMME), IOS53, title version 0, boot content 8.
All nine contents verify against the TMD SHA-1s after Dolphin's WAD install.

| # | Size | What |
| --- | --- | --- |
| 0 | 362,880 | banner, prefixed with a build stamp `muscle.0911201433` / `@PC-SHUJIE2` (built 2009-11-20 14:33) |
| 1 | 1,291,432 | **main DOL**, LZ11 → 2,938,336 bytes |
| 2 | 799,360 | U8: save icon/banner (`.tpl`), bitmap font, canvas (`.nut`) |
| 3 | 9,010 | U8: HomeButton3 CSVs |
| 4 | 34,304,512 | U8: all game data (59 files, most LZ11) |
| 5–7 | | shared contents: HOME Menu (HomeButton2/3 layouts, sounds, nwm) |
| 8 | 312,576 | Nintendo's "NAND BOOT PROGRAM" loader DOL (loads content 1) |

No RELs. Everything is in one statically linked DOL.

Content 4 formats are all Namco in-house, not NintendoWare: `mml1.1` (stage/level packs, 5–7 MB unpacked),
`NDWI` (character models), `NTWI` (textures, the Wii flavour of Namco's NUT), `SWG` (2D UI/layout scenes,
one per screen), `nusc` (sound config), `sound_*` banks, `motion`, `list.bin`.

## main.dol

| Section | Address | Size |
| --- | --- | --- |
| .init | 80004000 | 0x26C8 |
| extab / extabindex | 800066E0 | 0x14C04 / 0x4C40 |
| .text | 8001FF40 | 0x24E4F8 (2.3 MB) |
| .ctors/.dtors | 8026E440 | 0x8C / 0xC |
| .rodata | 8026E500 | 0xF75C |
| .data | 8027DC60 | 0x50AB0 |
| .bss | 802CE780 | 0x152990 |
| .sdata/.sbss/.sdata2 | 80421120… | small |

dtk 1.8.4 finds **7,693 functions** (~607K instructions):

| Region | Range | Functions | Instructions |
| --- | --- | --- | --- |
| Game | 8001FF40–8014F4DC | 2,721 | 313K |
| CW runtime + MSL C/C++ (+TRK) | 8014F4DC–80165000 | | |
| RVL SDK: AI/AX (sound) | 80165000–8016DD00 | | |
| RVL SDK: Bluetooth stack (bte.a, 558 functions) | 8016DD40–8019AFE4 | | |
| RVL SDK: DSP/DVD/EXI/FS/GX/IPC/MEM/MTX/NAND/OS/PAD/SC/SI/USB/VI/WPAD/WUD | 8019B000–801F1400 | | |
| Namco NU engine (render, input, sound) | 801F1400–80248000 | | |
| RVL SDK: CNT, HBM (nw4hbm) | 80248000–8026E440 | | |

(Region boundaries were refined in phase 2 from the `<< RVL_SDK >>` version-string anchors and the call graph.
The original phase-1 guess lumped the NU engine in with the SDK.)

Paired-single instructions: 460 in game code, ~2,700 in the SDK (mostly MTX/GX).

## Toolchain

- RVL SDK libs all say `release build: Feb 27 2009 (0x4302_145)`, except KPAD (May 18 2009). Libraries linked:
  AI AX CNT DSP DVD ENC EXI GX HBM KPAD NAND OS PAD SC SI VI WPAD.
  `0x4302_145` is the CodeWarrior version the SDK was built with: mwcceppc 4.3 build 145 ("Wii 1.0" on decomp.me).
- The game code's compiler isn't confirmed yet. It's most likely CW for Wii 1.0–1.3, given the 2009 SDK
  and the C99 `__msl_runtime_constraint_violation_s` in MSL. To pin it down, compile test functions in objdiff.
- "MetroTRK for Revolution v0.4" is linked into the release build.
- Heavy C++: exception tables (84 KB extab), RTTI, `std::tr1`, `boost::shared_ptr`, templates.

## Symbols

No map, no DWARF. What survives:
- **RTTI class names, 239 of them**: `application::USWGCallbackImpl_*` (one per screen),
  `System::CReserver<…>`, `Game::CManager`, `NU::Draw::*`, `NU::Debug::Font::*`, `CnuSound2*`, `homebutton::*`, `nw4hbm::lyt::*`.
- Source file names in asserts: `main.cpp`, `Memory.cpp`, `DvdFile.cpp`, `FileDataManager.cpp`,
  `nuTextureContextWii.cpp`, `nuWiiRenderDevice.cpp`, `youMapManager.cpp`.
- Game-state and tuning-parameter strings (`YOUE_GAMEMAIN_STATE_*`, `DASH_ZONE_*`, `FEINT_*`).
- The SDK, MSL and HBM can be named by signature against existing decomps.

## Middleware

- **Namco "NU" engine** (`nu*`, `NU::`, `CnuSound2`): renderer over GX, its own sound system over AX, and NUD/NUT/SWG formats.
- **Siren14 3.02** (Polycom codec, probably voice/speaker audio).
- **HBM** (HOME Button Menu) with its embedded nw4hbm. There is no NW4R and no CRI.

## Dolphin reference

Dolphin 2609 (Homebrew cask). `make nand` installs the WAD into a private user folder.
`tools/dolphin_run.sh` boots it headless (`-b -n`) at unthrottled speed and dumps frames as PNG.
- With no input, it reaches the title screen ("Press A") at about frame 1000. The order is strap warning ×2,
  Nunchuk-required notice, Bandai Namco logo, then title. That takes about 40 s of wall time, faster than real time.
- **Deterministic boot:** two runs gave 1,100 frame dumps with identical hashes.
- Available scripting: `-m` movie (.dtm) playback, `-s` savestate, `-C` config overrides, frame dumps,
  and the GDB stub. Stock Dolphin has no Python/Lua. For per-frame RAM/PC traces, build Dolphin from source with a trace hook
  (the same idea as the LakeSnes patch in bonkers), or drive the GDB stub.

## Prior work (checked 2026-10-04)

There is no Muscle March decomp, recomp or port on GitHub, decomp.dev, decompbuddy or the recomp lists.
New since the last check:
- **arelkair/wiiparty-recomp** (GPL-3, active 2026-09-29): a complete Wii static recomp. It has a Python PPC→C++
  recompiler and a runtime that emulates the hardware *below* the SDK: IOS, NAND, Bluetooth Wiimote,
  GX FIFO → D3D11 / **OpenGL 4.1**, and AX audio with recompiled DSP microcode. It has SDL3, `ucontext` fibers and a
  Linux port, and its README asks for macOS testers.
- AH64-dll/DolRecomp (GPL-3): a CPU-only PPC→C recompiler. Luigi's Mansion reaches its title screen.
- BlackLineInteractive/NWiiRecomp: recompiler plus HLE runtime, under a custom non-commercial MIT-like license.
- Mario Kart Wiicompiled: announced, closed.
