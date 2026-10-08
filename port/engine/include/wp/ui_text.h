#pragma once

#include <cstddef>

namespace wp::ui {

enum class Language { English, Spanish };

struct Label {
    const char* key;
    const char* text;
};

struct Text {
    const char* fps;
    const char* dsp;
    const char* dsp_native;
    const char* dsp_interpreted;
    const char* dsp_stopped;
    const char* menu_title;
    const char* menu_hint;
    const char* menu_restart;
    const char* menu_on;
    const char* menu_off;
    const char* menu_native;
    const char* menu_system;
    const Label* labels;
    size_t label_count;
};

void set_language(Language language);
const Text& text();
const char* label(const char* key);

}
