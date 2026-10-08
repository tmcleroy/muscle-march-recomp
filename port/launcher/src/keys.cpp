#include "keys.h"

#include "wp/keymap.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

struct Named {
    SDL_Scancode scancode;
    const char* name;
};

constexpr Named kNamed[] = {
    {SDL_SCANCODE_RETURN, "Enter"},
    {SDL_SCANCODE_KP_ENTER, "Enter"},
    {SDL_SCANCODE_SPACE, "Space"},
    {SDL_SCANCODE_BACKSPACE, "Backspace"},
    {SDL_SCANCODE_TAB, "Tab"},
    {SDL_SCANCODE_LSHIFT, "LeftShift"},
    {SDL_SCANCODE_RSHIFT, "RightShift"},
    {SDL_SCANCODE_LCTRL, "Ctrl"},
    {SDL_SCANCODE_RCTRL, "Ctrl"},
    {SDL_SCANCODE_LALT, "Alt"},
    {SDL_SCANCODE_RALT, "RightAlt"},
    {SDL_SCANCODE_UP, "Up"},
    {SDL_SCANCODE_DOWN, "Down"},
    {SDL_SCANCODE_LEFT, "Left"},
    {SDL_SCANCODE_RIGHT, "Right"},
    {SDL_SCANCODE_EQUALS, "Plus"},
    {SDL_SCANCODE_MINUS, "Minus"},
    {SDL_SCANCODE_KP_PLUS, "NumPlus"},
    {SDL_SCANCODE_KP_MINUS, "NumMinus"},
    {SDL_SCANCODE_KP_MULTIPLY, "NumMultiply"},
    {SDL_SCANCODE_KP_DIVIDE, "NumDivide"},
    {SDL_SCANCODE_KP_PERIOD, "NumPeriod"},
    {SDL_SCANCODE_COMMA, "Comma"},
    {SDL_SCANCODE_PERIOD, "Period"},
    {SDL_SCANCODE_SLASH, "Slash"},
    {SDL_SCANCODE_SEMICOLON, "Semicolon"},
    {SDL_SCANCODE_APOSTROPHE, "Quote"},
    {SDL_SCANCODE_GRAVE, "Backquote"},
    {SDL_SCANCODE_LEFTBRACKET, "LeftBracket"},
    {SDL_SCANCODE_RIGHTBRACKET, "RightBracket"},
    {SDL_SCANCODE_BACKSLASH, "Backslash"},
    {SDL_SCANCODE_PAGEUP, "PageUp"},
    {SDL_SCANCODE_PAGEDOWN, "PageDown"},
    {SDL_SCANCODE_HOME, "Home"},
    {SDL_SCANCODE_END, "End"},
    {SDL_SCANCODE_INSERT, "Insert"},
    {SDL_SCANCODE_DELETE, "Delete"},
    {SDL_SCANCODE_PAUSE, "Pause"},
    {SDL_SCANCODE_CAPSLOCK, "CapsLock"},
};

std::string portable_name(const SDL_KeyboardEvent& event) {
    if (event.key >= SDLK_A && event.key <= SDLK_Z) {
        return std::string(1, static_cast<char>('A' + (event.key - SDLK_A)));
    }
    if (event.key >= SDLK_0 && event.key <= SDLK_9) {
        return std::string(1, static_cast<char>('0' + (event.key - SDLK_0)));
    }
    if (event.scancode >= SDL_SCANCODE_KP_1 && event.scancode <= SDL_SCANCODE_KP_9) {
        return "Num" + std::to_string(event.scancode - SDL_SCANCODE_KP_1 + 1);
    }
    if (event.scancode == SDL_SCANCODE_KP_0) {
        return "Num0";
    }
    if (event.scancode >= SDL_SCANCODE_F1 && event.scancode <= SDL_SCANCODE_F12) {
        return "F" + std::to_string(event.scancode - SDL_SCANCODE_F1 + 1);
    }
    if (event.scancode >= SDL_SCANCODE_F13 && event.scancode <= SDL_SCANCODE_F24) {
        return "F" + std::to_string(event.scancode - SDL_SCANCODE_F13 + 13);
    }
    for (const Named& named : kNamed) {
        if (named.scancode == event.scancode) {
            return named.name;
        }
    }
    return {};
}

}

std::string key_event_name(const SDL_KeyboardEvent& event) {
#ifdef _WIN32
    UINT virtual_key = MapVirtualKeyW(event.raw, MAPVK_VSC_TO_VK_EX);
    if (virtual_key == VK_LCONTROL || virtual_key == VK_RCONTROL) {
        virtual_key = VK_CONTROL;
    } else if (virtual_key == VK_LMENU) {
        virtual_key = VK_MENU;
    }
    if (virtual_key != 0 && event.scancode != SDL_SCANCODE_KP_ENTER) {
        std::string name = wp::keymap::key_name(static_cast<int>(virtual_key));
        if (!name.empty()) {
            return name;
        }
    }
#endif
    std::string name = portable_name(event);
    return wp::keymap::key_code(name) >= 0 ? name : std::string();
}

std::string mouse_button_name(uint8_t button) {
    switch (button) {
    case SDL_BUTTON_LEFT:
        return "MouseLeft";
    case SDL_BUTTON_RIGHT:
        return "MouseRight";
    case SDL_BUTTON_MIDDLE:
        return "MouseMiddle";
    case SDL_BUTTON_X1:
        return "MouseX1";
    case SDL_BUTTON_X2:
        return "MouseX2";
    default:
        return {};
    }
}
