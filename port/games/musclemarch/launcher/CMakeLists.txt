# The Muscle March launcher: the game file, optional features, controllers; starts the game.
# Built next to the game program (it finds `musclemarch` beside itself). Needs SDL3.
if(NOT TARGET SDL3::SDL3)
    message(STATUS "No SDL3: the launcher is not built")
    return()
endif()

include(FetchContent)
FetchContent_Declare(imgui
    URL https://github.com/ocornut/imgui/archive/refs/tags/v1.92.9b.tar.gz
    URL_HASH SHA256=21d8a0a565e85dce943e375db00812c2f3f0ab21f3f0f7964e364a63422d7f99
    DOWNLOAD_EXTRACT_TIMESTAMP ON)
FetchContent_Declare(inter
    URL https://github.com/rsms/inter/releases/download/v4.1/Inter-4.1.zip
    URL_HASH SHA256=9883fdd4a49d4fb66bd8177ba6625ef9a64aa45899767dde3d36aa425756b11e
    DOWNLOAD_EXTRACT_TIMESTAMP ON)
FetchContent_MakeAvailable(imgui inter)

add_library(launcher_imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdlrenderer3.cpp)
target_include_directories(launcher_imgui PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
target_compile_definitions(launcher_imgui PUBLIC IMGUI_DEFINE_MATH_OPERATORS IMGUI_DISABLE_OBSOLETE_FUNCTIONS IMGUI_DISABLE_DEMO_WINDOWS IMGUI_DISABLE_DEBUG_TOOLS)
target_link_libraries(launcher_imgui PUBLIC SDL3::SDL3)

# The fonts, compiled in (blob("font_regular") / blob("font_semibold") for ui.cpp).
set(WP_LAUNCHER_FONTS "${CMAKE_CURRENT_BINARY_DIR}/fonts.cpp")
add_custom_command(OUTPUT "${WP_LAUNCHER_FONTS}"
    COMMAND "${CMAKE_COMMAND}" -DOUT=${WP_LAUNCHER_FONTS}
            -DREGULAR=${inter_SOURCE_DIR}/extras/ttf/Inter-Regular.ttf
            -DSEMIBOLD=${inter_SOURCE_DIR}/extras/ttf/Inter-SemiBold.ttf
            -P "${CMAKE_CURRENT_SOURCE_DIR}/embed_fonts.cmake"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/embed_fonts.cmake"
    VERBATIM)

set(WP_UPSTREAM_LAUNCHER "${CMAKE_SOURCE_DIR}/launcher/src")
add_executable(musclemarch_launcher
    src/main.cpp
    src/app.cpp
    src/ini.cpp
    src/wad.cpp
    src/crypto.cpp
    src/install.cpp
    src/steam.cpp
    src/game_process.cpp
    ${WP_UPSTREAM_LAUNCHER}/ui.cpp
    "${WP_LAUNCHER_FONTS}")
set_target_properties(musclemarch_launcher PROPERTIES OUTPUT_NAME musclemarch-launcher RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
target_include_directories(musclemarch_launcher PRIVATE src "${WP_UPSTREAM_LAUNCHER}")
target_compile_options(musclemarch_launcher PRIVATE -Wall -Wextra)
file(STRINGS "${CMAKE_SOURCE_DIR}/../VERSION" WP_MM_VERSION LIMIT_COUNT 1)
target_compile_definitions(musclemarch_launcher PRIVATE MUSCLE_MARCH_VERSION="${WP_MM_VERSION}")
target_link_libraries(musclemarch_launcher PRIVATE launcher_imgui)
if(TARGET musclemarch)
    add_dependencies(musclemarch_launcher musclemarch)
endif()
