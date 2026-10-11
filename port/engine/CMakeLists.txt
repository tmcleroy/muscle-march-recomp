add_library(wp_engine STATIC
    src/core/runtime.cpp
    src/core/fpu.cpp
    src/core/fiber.cpp
    src/core/big_stack.cpp
    src/core/dol.cpp
    src/core/boot.cpp
    src/core/hle.cpp
    src/core/deterministic.cpp
    src/core/lift.cpp
    src/core/threads.cpp
    src/core/interrupts.cpp
    src/core/modules.cpp
    src/core/format.cpp
    src/core/log.cpp
    src/core/sync.cpp
    src/gpu/gx.cpp
    src/gpu/gx_api.cpp
    src/gpu/gx_lighting.cpp
    src/gpu/custom_textures.cpp
    src/gpu/gx_state.cpp
    src/gpu/gx_render_common.cpp
    src/gpu/tev_specialise.cpp
    src/audio/dsp.cpp
    src/audio/audio.cpp
    src/audio/audio_hle.cpp
    src/audio/ax_hle.cpp
    src/audio/dsp_roms.cpp
    src/ios/ios.cpp
    src/ios/ipc.cpp
    src/ios/nand.cpp
    src/ios/disc.cpp
    src/ios/title.cpp
    src/ios/storage_hle.cpp
    src/input/bluetooth.cpp
    src/input/wiimote.cpp
    src/input/input_hle.cpp
    src/input/input.cpp
    src/input/movie.cpp
    src/input/keymap.cpp
    src/input/touch.cpp
    src/platform/png.cpp
    src/platform/save_backup.cpp
    src/platform/screenshot.cpp
    src/platform/settings.cpp
    src/platform/features.cpp
    src/platform/ui_text.cpp
    src/platform/options.cpp
    src/platform/hud.cpp
    src/platform/options_window.cpp
)
if(ANDROID)
    target_sources(wp_engine PRIVATE src/platform/android.cpp)
    target_link_libraries(wp_engine PUBLIC log)
endif()
if(WIN32)
    target_sources(wp_engine PRIVATE src/gpu/gx_render.cpp src/platform/video.cpp src/platform/options_render_gdi.cpp)
else()
    target_sources(wp_engine PRIVATE src/platform/options_render_stb.cpp)
    target_include_directories(wp_engine SYSTEM PRIVATE "${CMAKE_SOURCE_DIR}/third_party/stb")
endif()
target_include_directories(wp_engine PUBLIC include)
target_compile_definitions(wp_engine PRIVATE WP_BUILD="${WP_BUILD}"
    WP_DSP_IROM_PATH="${CMAKE_SOURCE_DIR}/third_party/dolphin/Data/Sys/GC/dsp_rom.bin"
    WP_DSP_COEF_PATH="${CMAKE_SOURCE_DIR}/third_party/dolphin/Data/Sys/GC/dsp_coef.bin")
set_source_files_properties(src/audio/dsp_roms.cpp PROPERTIES OBJECT_DEPENDS "${CMAKE_SOURCE_DIR}/third_party/dolphin/Data/Sys/GC/dsp_rom.bin;${CMAKE_SOURCE_DIR}/third_party/dolphin/Data/Sys/GC/dsp_coef.bin")
target_link_libraries(wp_engine PUBLIC wp_dolphin_dsp wp_dolphin_wiimote)
if(WIN32)
    target_link_libraries(wp_engine PUBLIC gdi32 user32 d3d11 d3dcompiler dxgi ole32 windowscodecs dwmapi)
else()
    find_package(Threads REQUIRED)
    target_link_libraries(wp_engine PUBLIC Threads::Threads)
endif()
if(WP_TRACE)
    target_compile_definitions(wp_engine PUBLIC WP_TRACE)
endif()
target_compile_options(wp_engine PRIVATE -Wall -Wextra)

if(WIN32)
    set(WP_SDL3_DIR "${CMAKE_SOURCE_DIR}/build/deps/SDL3-3.4.16/x86_64-w64-mingw32" CACHE PATH "SDL3 development files for gamepad support")
    find_package(SDL3 CONFIG QUIET GLOBAL PATHS "${WP_SDL3_DIR}/lib/cmake/SDL3" NO_DEFAULT_PATH)
else()
    find_package(SDL3 3.4 CONFIG QUIET GLOBAL)
    option(WP_FETCH_SDL3 "Download and build SDL3 when the system has none" ON)
    if(NOT SDL3_FOUND AND WP_FETCH_SDL3)
        include(FetchContent)
        if(ANDROID)
            # SDL's Java half loads libSDL3.so by name (android/app/build.gradle).
            set(SDL_SHARED ON CACHE BOOL "" FORCE)
            set(SDL_STATIC OFF CACHE BOOL "" FORCE)
        else()
            set(SDL_SHARED OFF CACHE BOOL "" FORCE)
            set(SDL_STATIC ON CACHE BOOL "" FORCE)
        endif()
        set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
        FetchContent_Declare(sdl3
            URL https://github.com/libsdl-org/SDL/releases/download/release-3.4.16/SDL3-3.4.16.tar.gz
            URL_HASH SHA256=7322236cd12090c3eb40b9728be4d49c76f66ad17d04369584d4ecad5cf77c68
            DOWNLOAD_EXTRACT_TIMESTAMP ON)
        FetchContent_MakeAvailable(sdl3)
        set(SDL3_FOUND TRUE)
        set(WP_SDL_PACKAGES "Debian and Ubuntu: sudo apt install libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev libxss-dev libxkbcommon-dev libwayland-dev wayland-protocols libdecor-0-dev libegl-dev libgl-dev libpulse-dev libasound2-dev libudev-dev libdbus-1-dev\nFedora: sudo dnf install libX11-devel libXext-devel libXrandr-devel libXcursor-devel libXi-devel libXScrnSaver-devel libxkbcommon-devel wayland-devel wayland-protocols-devel libdecor-devel mesa-libEGL-devel mesa-libGL-devel pulseaudio-libs-devel alsa-lib-devel systemd-devel dbus-devel\nArch: sudo pacman -S --needed sdl3")
        foreach(WP_SDL_FEATURE X11 WAYLAND PULSEAUDIO PIPEWIRE ALSA)
            get_directory_property(WP_HAVE_${WP_SDL_FEATURE} DIRECTORY "${sdl3_SOURCE_DIR}" DEFINITION HAVE_${WP_SDL_FEATURE})
        endforeach()
        if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT WP_HAVE_X11 AND NOT WP_HAVE_WAYLAND)
            message(FATAL_ERROR "SDL3 found no X11 or Wayland development files, so the game could not open a window. Install them and build again:\n${WP_SDL_PACKAGES}")
        endif()
        if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT WP_HAVE_PULSEAUDIO AND NOT WP_HAVE_PIPEWIRE AND NOT WP_HAVE_ALSA)
            message(FATAL_ERROR "SDL3 found no PulseAudio, PipeWire or ALSA development files, so the game would have no sound. Install them and build again:\n${WP_SDL_PACKAGES}")
        endif()
    endif()
    option(WP_REQUIRE_SDL3 "Stop when SDL3 is missing instead of building the game without a window" OFF)
    if(NOT SDL3_FOUND AND WP_REQUIRE_SDL3)
        message(FATAL_ERROR "SDL3 was not found. Install the SDL3 development package of your distribution, or allow the download of SDL3 (WP_FETCH_SDL3).")
    endif()
endif()
if(SDL3_FOUND)
    target_sources(wp_engine PRIVATE src/input/gamepad.cpp)
    if(TARGET SDL3::SDL3)
        target_link_libraries(wp_engine PUBLIC SDL3::SDL3)
    else()
        target_link_libraries(wp_engine PUBLIC SDL3::SDL3-static)
    endif()
else()
    target_sources(wp_engine PRIVATE src/input/no_gamepad.cpp)
endif()
if(NOT WIN32)
    if(SDL3_FOUND)
        if(APPLE OR ANDROID)
            set(WP_DEFAULT_RENDERER "gpu")  # Metal; Vulkan on Android
        else()
            set(WP_DEFAULT_RENDERER "gl")  # SDL_GPU needs Vulkan there; untested so far
        endif()
        set(WP_RENDERER "${WP_DEFAULT_RENDERER}" CACHE STRING "Renderer of SDL builds: gl (OpenGL 4.1) or gpu (SDL_GPU: Metal, Vulkan, D3D12)")
        if(WP_RENDERER STREQUAL "gpu")
            target_sources(wp_engine PRIVATE src/gpu/gx_render_gpu.cpp src/platform/video_sdl.cpp)
            target_compile_definitions(wp_engine PRIVATE WP_SDL_PLATFORM WP_RENDERER_GPU)
        else()
            target_sources(wp_engine PRIVATE src/gpu/gx_render_gl.cpp src/platform/video_sdl.cpp)
            target_compile_definitions(wp_engine PRIVATE WP_SDL_PLATFORM)
        endif()
    else()
        target_sources(wp_engine PRIVATE src/gpu/null_render.cpp src/platform/video_headless.cpp)
    endif()
endif()

add_library(wp_app INTERFACE)
target_sources(wp_app INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/src/app/main.cpp")
if(WIN32)
    target_link_options(wp_app INTERFACE -Wl,--stack,536870912)
endif()
