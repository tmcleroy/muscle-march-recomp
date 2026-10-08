# Linux port plan

State on 2026-09-29, measured with GCC 13.3, CMake 3.28 and Ninja 1.11 on Ubuntu 24.04 (WSL 2 on the development PC).

## What already works on Linux

- **Recompiler:** Python only; no Windows dependency.
- **Translated code:** a DOL chunk (`dol_003.cpp`) and a module chunk (`mg100_000.cpp`) compile unchanged with Linux GCC (`g++ -std=c++20 -O1 -Iengine/include`). The generated code only uses `wp/cpu.h`, `wp/hle.h`, `wp/threads.h` and the C++ standard library.
- **Engine sources that pass a Linux syntax check (24 of 36):** all of `core/` except `threads.cpp` (`boot`, `dol`, `format`, `fpu`, `hle`, `interrupts`, `log`, `modules`, `runtime`), `gx_lighting.cpp`, `gx_state.cpp`, `bluetooth.cpp`, `keymap.cpp`, `wiimote.cpp`, `disc.cpp`, `ios.cpp`, `ipc.cpp`, `options.cpp`, `save_backup.cpp`, `screenshot.cpp`, `settings.cpp`, `ui_text.cpp`, and the no-op variants. `gamepad.cpp` only needs SDL3 headers.
- **Launcher:** SDL3 and Dear ImGui; `subprocess.cpp` already has a POSIX branch (`fork`, process group, `setpriority`).

## Windows-only code and its Linux replacement

| File | Lines | Windows use | Linux replacement |
| --- | --- | --- | --- |
| `gpu/gx_render.cpp` | 1,985 | Direct3D 11 renderer, HLSL compiled at run time | Done: `gx_render_gl.cpp` (OpenGL 4.1 core, GLSL) |
| `platform/video.cpp` | 589 | Win32 window on its own thread, DXGI swap chain, `SystemParametersInfo` | Done: `video_sdl.cpp` (SDL3 window, OpenGL context) |
| `audio/audio.cpp` | 307 | WASAPI output with windowed-sinc resampling | Done: shared `Mixer` feeding an `SDL_AudioStream` |
| `input/input.cpp` | 342 | `GetAsyncKeyState`, `GetCursorPos`, focus through `GetForegroundWindow` | Done: queries moved to the video layer, answered by SDL3 |
| `platform/options_window.cpp` | 301 | F10 menu drawn with GDI into an overlay texture | Done: shared menu logic, `options_render_stb.cpp` (stb_truetype) |
| `core/threads.cpp` | 391 | Win32 fibers for guest threads, `OSSaveContext` and `setjmp` resumption | Done: `wp::fiber` (`ucontext`) |
| `gpu/custom_textures.cpp` | 272 | PNG decoding with WIC | Done: `SDL_LoadPNG` |
| `gpu/gx.cpp` | 1,284 | `CreateDirectoryA`, `GetThreadTimes` for the thread load statistics | Done |
| `audio/dsp.cpp` | 916 | `CreateThread` with a 64 MB stack for the DSP self-test | Done: `wp::run_with_stack` |
| `ios/nand.cpp` | 525 | `GetUserDefaultUILanguage` for the console language | Done: `LC_ALL`/`LANG` |
| `app/main.cpp` | 96 | `SetUnhandledExceptionFilter` crash report | Done: signal handlers |

## Order of work

1. **Build system (done 2026-09-29):** split `engine/CMakeLists.txt` into common and per-platform source lists; build the game on Linux with no-op video, audio and input (headless) to boot the game to the main menu. This proves the translated code, fibers, IOS, disc and NAND on Linux.
2. **Fibers (done 2026-09-29):** introduce `wp::fiber` and move `threads.cpp` onto it; run the Windows build on it first to check nothing changes.
3. **Window, input and audio on SDL3 (done on Linux 2026-09-29):** `video_sdl.cpp` (window thread, OpenGL context handed to the GPU thread, keyboard and mouse through the existing virtual-key codes) and an `SDL_AudioStream` output fed by the resampler shared with WASAPI. Windows keeps its Win32 window and WASAPI output for now.
4. **Renderer (done on Linux 2026-09-29):** `gx_render_gl.cpp`, OpenGL 4.1 core with GLSL ports of the Direct3D shaders, instead of the SDL_GPU plan: OpenGL needs no shader cross-compiler at build or run time and 4.1 also runs on macOS. Frames match the Direct3D renderer on the title screen, menus, Mii lineup, ¡Puños fuera! and Cinturón de asteroides. SDL_GPU (Vulkan) stays an option if OpenGL drivers turn out to be a problem.
5. **F10 options menu and custom textures on Linux (done 2026-09-29):** the menu logic is shared and only the drawing is per system; PNGs load through SDL.
6. **Launcher on Linux (done 2026-09-29):** the distribution's GCC, CMake, Ninja and Python instead of the Windows tool chain; `nodtool` from its Linux release; packaged as an AppImage by `tools/make_appimage.py`, which updates itself from the release's AppImage. A full install from the PAL disc and a fast update through the AppImage were checked in WSL.
7. **Testers:** the README asks for Linux and macOS testers; macOS needs an arm64 check of the fiber layer.

## Building on Linux

- Packages: the lists in the README (compiler, CMake, Ninja, Python, curl and the X11, Wayland, OpenGL, audio, udev and D-Bus development files SDL3 is built with, since Ubuntu 24.04 has no SDL3 3.4 package). The build stops with those lists when SDL3 found no window or audio system.
- The AppImage runs on glibc 2.38 or newer (Ubuntu 24.04, Debian 13, Fedora 39 and later), since it is built on Ubuntu 24.04.
- `cmake -S . -B ~/wp-build -G Ninja -DCMAKE_BUILD_TYPE=Release` and `cmake --build ~/wp-build`; without SDL3 the game builds headless.
- In WSL, Mesa picks its software renderer by default; `GALLIUM_DRIVER=d3d12` uses the PC's GPU.

## Checks for each step

- `ctest` passes on both systems (the translator tests are platform independent).
- Scripted runs reach the same screens with identical saved frames at 1x on both systems.
- The Windows build keeps 60 fps and the same guest idle share.
