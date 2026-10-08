cmake_minimum_required(VERSION 3.24)
project(wii_recomp_launcher C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(WP_LAUNCHER_VERSION "0.1.2-alpha" CACHE STRING "Version shown in the launcher")
set(WP_LAUNCHER_BUILD "2609-003" CACHE STRING "Build number shown in the launcher")

include(FetchContent)

set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_DISABLE_INSTALL ON CACHE BOOL "" FORCE)
foreach(WP_SDL_OFF SDL_AUDIO SDL_CAMERA SDL_HAPTIC SDL_SENSOR SDL_POWER SDL_TRAY SDL_GPU SDL_RENDER_GPU SDL_VULKAN SDL_RENDER_VULKAN SDL_RENDER_D3D SDL_RENDER_D3D12)
    set(${WP_SDL_OFF} OFF CACHE BOOL "" FORCE)
endforeach()

FetchContent_Declare(sdl3
    URL https://github.com/libsdl-org/SDL/releases/download/release-3.4.16/SDL3-3.4.16.tar.gz
    URL_HASH SHA256=7322236cd12090c3eb40b9728be4d49c76f66ad17d04369584d4ecad5cf77c68
    DOWNLOAD_EXTRACT_TIMESTAMP ON)
FetchContent_Declare(imgui
    URL https://github.com/ocornut/imgui/archive/refs/tags/v1.92.9b.tar.gz
    URL_HASH SHA256=21d8a0a565e85dce943e375db00812c2f3f0ab21f3f0f7964e364a63422d7f99
    DOWNLOAD_EXTRACT_TIMESTAMP ON)
FetchContent_Declare(inter
    URL https://github.com/rsms/inter/releases/download/v4.1/Inter-4.1.zip
    URL_HASH SHA256=9883fdd4a49d4fb66bd8177ba6625ef9a64aa45899767dde3d36aa425756b11e
    DOWNLOAD_EXTRACT_TIMESTAMP ON)
FetchContent_MakeAvailable(sdl3 imgui inter)

add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdlrenderer3.cpp)
target_include_directories(imgui PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
target_compile_definitions(imgui PUBLIC IMGUI_DEFINE_MATH_OPERATORS IMGUI_DISABLE_OBSOLETE_FUNCTIONS IMGUI_DISABLE_DEMO_WINDOWS IMGUI_DISABLE_DEBUG_TOOLS)
target_link_libraries(imgui PUBLIC SDL3::SDL3-static)

set(WP_ENGINE "${CMAKE_CURRENT_SOURCE_DIR}/../engine")
add_executable(launcher WIN32
    src/main.cpp
    src/app.cpp
    src/ui.cpp
    src/keys.cpp
    src/payload.cpp
    src/prefs.cpp
    src/subprocess.cpp
    src/project.cpp
    src/runner.cpp
    src/sha256.cpp
    src/texts.cpp
    src/toolchain.cpp
    src/updater.cpp
    src/shortcut.cpp
    ${WP_ENGINE}/src/platform/settings.cpp
    ${WP_ENGINE}/src/platform/options.cpp
    ${WP_ENGINE}/src/platform/ui_text.cpp
    ${WP_ENGINE}/src/platform/save_backup.cpp
    ${WP_ENGINE}/src/input/keymap.cpp)

include(cmake/embed.cmake)

set_target_properties(launcher PROPERTIES OUTPUT_NAME wiipartyrecomp-launcher)
target_include_directories(launcher PRIVATE ${WP_ENGINE}/include src)
target_compile_definitions(launcher PRIVATE WP_LAUNCHER_VERSION="${WP_LAUNCHER_VERSION}" WP_LAUNCHER_BUILD="${WP_LAUNCHER_BUILD}")
target_compile_options(launcher PRIVATE -Wall -Wextra)
target_link_libraries(launcher PRIVATE imgui)

if(WIN32)
    target_sources(launcher PRIVATE res/launcher.rc)
    target_link_libraries(launcher PRIVATE dwmapi winhttp ole32 uuid shell32)
    target_link_options(launcher PRIVATE -static $<$<CONFIG:Release>:-s>)
else()
    target_link_options(launcher PRIVATE -static-libstdc++ -static-libgcc $<$<CONFIG:Release>:-s>)
endif()
