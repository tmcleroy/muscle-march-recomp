#pragma once

#include <cstddef>

namespace wp::game {

struct Description {
    const char* title;
    const char* short_name;
    const char* data_directory;
    const char* nand_directory;
    const char* settings_file;
    const char* textures_directory;
    const char* notice_module;
    const char* const* sideways_modules;
    size_t sideways_module_count;
    bool nunchuk = false;
    // IR camera y for a pointer at screen y in [-1, 1]: camera_y = 384 + y * pointer_y_scale + pointer_y_offset.
    float pointer_y_scale = 225.5f;
    float pointer_y_offset = 102.0f;
    // The game replaces dsp.a/ai.a with the native audio handlers (hle_functions.csv).
    bool native_audio = false;
};

const Description& description();

}
