# Third-party notices

Muscle March Recomp is licensed under the GNU General Public License v3.0 (see `LICENSE`). It is
built on, and includes code from, the projects below. Their licenses are compatible with the
GPL-3.0; their notices are kept here and in the files themselves.

## Vendored in this repository

### wiiparty-recomp (GPL-3.0)

`port/` (the recompiler, `port/recompiler/`, and the runtime, `port/engine/`) comes from
[wiiparty-recomp](https://github.com/arelkair/wiiparty-recomp) by arelkair, with this project's
changes on top (listed in `docs/PORT.md`). Its own provenance record,
[`port/THIRD_PARTY_NOTICES.md`](port/THIRD_PARTY_NOTICES.md), lists the Dolphin code it includes:
the GX lighting pipeline, the emulated Bluetooth stack and Wii Remote, the DSP core with
Dolphin's free replacement DSP ROMs (`port/third_party/dolphin/`, no Nintendo code), the Gekko
floating-point estimates, and stb_truetype.

### Dolphin (GPL-2.0-or-later)

Added by this project, from the [Dolphin Emulator Project](https://github.com/dolphin-emu/dolphin)
(Copyright Dolphin Emulator Project, `SPDX-License-Identifier: GPL-2.0-or-later`):

- **Nunchuk extension encryption:** `port/third_party/dolphin/Source/Core/Core/HW/WiimoteEmu/`
  (`Encryption.cpp`/`.h`), used by `port/engine/src/input/wiimote.cpp`.
- **AX audio microcode (HLE):** `port/engine/src/audio/ax_hle.cpp` is a port of Dolphin's AXWii
  high-level emulation of the AX audio microcode.

## Downloaded at build time and included in release builds

| Component | License | Used for |
| --- | --- | --- |
| [SDL 3](https://libsdl.org) | zlib | Windows, audio, input, GPU (game and launcher) |
| [Dear ImGui](https://github.com/ocornut/imgui) | MIT | The launcher's interface |
| [Inter](https://rsms.me/inter/) | SIL Open Font License 1.1 | The launcher's font (embedded) |

CMake downloads each one at configure time and checks its SHA-256. Release archives include their
license texts in a `licenses/` folder.

## Tools used to build (not included in builds)

- [decomp-toolkit](https://github.com/encounter/decomp-toolkit) (MIT or Apache-2.0): finds the
  game's function boundaries on the builder's machine.
- Python, CMake, Ninja and a C++ compiler.

## What is not included

Nothing from *Muscle March* or the Wii itself is in this repository: no game code, data, graphics,
sound or text, no Nintendo SDK files, and no keys. Release builds contain code translated from the
game by the recompiler; the game's data is built by each player's launcher from their own WAD.
