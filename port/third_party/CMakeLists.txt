set(WP_DOLPHIN_DIR "${CMAKE_CURRENT_SOURCE_DIR}/dolphin/Source/Core")
file(GLOB WP_DOLPHIN_DSP_SOURCES CONFIGURE_DEPENDS "${WP_DOLPHIN_DIR}/Core/DSP/*.cpp" "${WP_DOLPHIN_DIR}/Core/DSP/Interpreter/*.cpp")
add_library(wp_dolphin_dsp STATIC ${WP_DOLPHIN_DSP_SOURCES})
target_include_directories(wp_dolphin_dsp SYSTEM PUBLIC "${WP_DOLPHIN_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/dolphin_shim")
target_compile_features(wp_dolphin_dsp PUBLIC cxx_std_20)
set_target_properties(wp_dolphin_dsp PROPERTIES CXX_STANDARD 20)
target_compile_options(wp_dolphin_dsp PRIVATE -O2)

# Wii Remote extension encryption (marcan / Dolphin), used by the emulated Nunchuk.
add_library(wp_dolphin_wiimote STATIC "${WP_DOLPHIN_DIR}/Core/HW/WiimoteEmu/Encryption.cpp")
target_include_directories(wp_dolphin_wiimote SYSTEM PUBLIC "${WP_DOLPHIN_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/dolphin_shim")
set_target_properties(wp_dolphin_wiimote PROPERTIES CXX_STANDARD 20)
