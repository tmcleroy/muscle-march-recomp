#include "wp/keymap.h"

#include <algorithm>
#include <cctype>

#include "wp/input.h"

namespace wp::keymap {

namespace {

constexpr ActionInfo kActions[kActionCount] = {
    {"a", input::kButtonA, "Enter,Space,MouseLeft"},
    {"b", input::kButtonB, "Backspace,MouseRight"},
    {"one", input::kButtonOne, "1"},
    {"two", input::kButtonTwo, "2"},
    {"plus", input::kButtonPlus, "Plus,NumPlus"},
    {"minus", input::kButtonMinus, "Minus,NumMinus"},
    {"home", input::kButtonHome, "H"},
    {"up", input::kButtonUp, "Up"},
    {"down", input::kButtonDown, "Down"},
    {"left", input::kButtonLeft, "Left"},
    {"right", input::kButtonRight, "Right"},
    {"shake", input::kMotionShake, "MouseMiddle,LeftShift"},
    {"nunchuk_shake", input::kNunchukShake, "X,LeftShift"},
    {"nunchuk_c", input::kNunchukC, "C"},
    {"nunchuk_z", input::kNunchukZ, "Z"},
    // Muscle March poses (viewed from behind; Wii Remote = right arm, Nunchuk = left arm).
    // Whole poses: I/J/K/L, W/A/S/D or the arrows (which also move through menus).
    {"pose_up", input::kRemotePointUp | input::kNunchukPointUp, "I,W,Up"},
    {"pose_down", input::kRemotePointDown | input::kNunchukPointDown, "K,S,Down"},
    {"pose_left", input::kNunchukPointUp | input::kRemotePointDown, "J,A,Left"},
    {"pose_right", input::kRemotePointUp | input::kNunchukPointDown, "L,D,Right"},
    {"swing_up", input::kMotionSwingUp, "WheelUp,T"},
    {"swing_down", input::kMotionSwingDown, "WheelDown,G"},
    {"tilt_left", input::kMotionTiltLeft, "Q"},
    {"tilt_right", input::kMotionTiltRight, "E"},
    {"tilt_up", input::kMotionTiltUp, "R"},
    {"tilt_down", input::kMotionTiltDown, "F"},
    {"grip", 0, "Tab"},
    {"screenshot", 0, "F9"},
};

struct Key {
    const char* name;
    int code;
};

constexpr Key kKeys[] = {
    {"MouseLeft", 0x01},  {"MouseRight", 0x02},  {"MouseMiddle", 0x04}, {"MouseX1", 0x05},     {"MouseX2", 0x06},
    {"WheelUp", kWheelUp}, {"WheelDown", kWheelDown},
    {"Backspace", 0x08},  {"Tab", 0x09},         {"Enter", 0x0D},       {"Shift", 0x10},       {"Ctrl", 0x11},
    {"Alt", 0x12},        {"Pause", 0x13},       {"CapsLock", 0x14},    {"Escape", 0x1B},      {"Space", 0x20},
    {"PageUp", 0x21},     {"PageDown", 0x22},    {"End", 0x23},         {"Home", 0x24},        {"Left", 0x25},
    {"Up", 0x26},         {"Right", 0x27},       {"Down", 0x28},        {"Insert", 0x2D},      {"Delete", 0x2E},
    {"Num0", 0x60},       {"Num1", 0x61},        {"Num2", 0x62},        {"Num3", 0x63},        {"Num4", 0x64},
    {"Num5", 0x65},       {"Num6", 0x66},        {"Num7", 0x67},        {"Num8", 0x68},        {"Num9", 0x69},
    {"NumMultiply", 0x6A}, {"NumPlus", 0x6B},    {"NumMinus", 0x6D},    {"NumPeriod", 0x6E},   {"NumDivide", 0x6F},
    {"LeftShift", 0xA0},  {"RightShift", 0xA1},  {"LeftCtrl", 0xA2},    {"RightCtrl", 0xA3},   {"LeftAlt", 0xA4},
    {"RightAlt", 0xA5},   {"Semicolon", 0xBA},   {"Plus", 0xBB},        {"Comma", 0xBC},       {"Minus", 0xBD},
    {"Period", 0xBE},     {"Slash", 0xBF},       {"Backquote", 0xC0},   {"LeftBracket", 0xDB}, {"Backslash", 0xDC},
    {"RightBracket", 0xDD}, {"Quote", 0xDE},
};

constexpr int kF1 = 0x70;
constexpr int kF10 = 0x79;
constexpr int kF11 = 0x7A;
constexpr int kF12 = 0x7B;
constexpr int kF24 = 0x87;

std::string lower(const std::string& text) {
    std::string result = text;
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return result;
}

std::string trimmed(const std::string& text) {
    size_t first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return "";
    }
    size_t last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}

}

const ActionInfo& action(Action which) {
    return kActions[static_cast<size_t>(which)];
}

const ActionInfo& action(size_t index) {
    return kActions[index];
}

std::string setting_key(size_t index) {
    return std::string("keys.") + kActions[index].name;
}

int key_code(const std::string& name) {
    std::string key = lower(trimmed(name));
    if (key.size() == 1 && std::isalnum(static_cast<unsigned char>(key[0]))) {
        return std::toupper(static_cast<unsigned char>(key[0]));
    }
    if (key.size() >= 2 && key.size() <= 3 && key[0] == 'f' && std::all_of(key.begin() + 1, key.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)); })) {
        int number = std::stoi(key.substr(1));
        if (number >= 1 && number <= kF24 - kF1 + 1 && key[1] != '0') {
            return kF1 + number - 1;
        }
        return -1;
    }
    for (const Key& entry : kKeys) {
        if (lower(entry.name) == key) {
            return entry.code;
        }
    }
    return -1;
}

std::string key_name(int code) {
    if ((code >= '0' && code <= '9') || (code >= 'A' && code <= 'Z')) {
        return std::string(1, static_cast<char>(code));
    }
    if (code >= kF1 && code <= kF24) {
        return "F" + std::to_string(code - kF1 + 1);
    }
    for (const Key& entry : kKeys) {
        if (entry.code == code) {
            return entry.name;
        }
    }
    return "";
}

bool reserved(int code) {
    return code == kF10 || code == kF11 || code == kF12;
}

Parsed parse(const std::string& list) {
    Parsed result;
    size_t start = 0;
    while (start <= list.size()) {
        size_t comma = list.find(',', start);
        std::string token = trimmed(list.substr(start, comma == std::string::npos ? std::string::npos : comma - start));
        if (!token.empty()) {
            int code = key_code(token);
            if (code < 0 || reserved(code)) {
                result.invalid.push_back(token);
            } else if (std::find(result.codes.begin(), result.codes.end(), code) == result.codes.end()) {
                result.codes.push_back(code);
            }
        }
        if (comma == std::string::npos) {
            break;
        }
        start = comma + 1;
    }
    return result;
}

std::string format(const std::vector<int>& codes, const char* separator) {
    std::string result;
    for (int code : codes) {
        std::string name = key_name(code);
        if (name.empty()) {
            continue;
        }
        if (!result.empty()) {
            result += separator;
        }
        result += name;
    }
    return result;
}

bool Bindings::has(Action which, int code) const {
    const std::vector<int>& list = of(which);
    return std::find(list.begin(), list.end(), code) != list.end();
}

bool Bindings::bound(int code) const {
    for (const std::vector<int>& list : codes) {
        if (std::find(list.begin(), list.end(), code) != list.end()) {
            return true;
        }
    }
    return false;
}

Bindings defaults() {
    Bindings result;
    for (size_t i = 0; i < kActionCount; i++) {
        result.codes[i] = parse(kActions[i].defaults).codes;
    }
    return result;
}

namespace {

// Earlier default bindings. settings.ini stores every binding, so a value still equal to an old
// default was never chosen by the player: it gets the current default instead.
struct OldDefault {
    const char* action;
    const char* keys;
};
constexpr OldDefault kOldDefaults[] = {
    {"up", "Up,A"},   {"down", "Down,D"},   {"left", "Left,S"},   {"right", "Right,W"},
    {"pose_up", "I"}, {"pose_down", "K"},   {"pose_left", "J"},   {"pose_right", "L"},
};

}  // namespace

std::string upgraded(size_t index, const std::string& stored) {
    for (const OldDefault& old : kOldDefaults) {
        if (std::string(kActions[index].name) == old.action && lower(trimmed(stored)) == lower(old.keys)) {
            return kActions[index].defaults;
        }
    }
    return stored;
}

Bindings load(const std::function<std::string(const std::string& key)>& value, std::vector<std::string>& invalid) {
    Bindings result;
    for (size_t i = 0; i < kActionCount; i++) {
        std::string key = setting_key(i);
        Parsed parsed = parse(upgraded(i, value(key)));
        for (const std::string& name : parsed.invalid) {
            invalid.push_back(key + ": " + name);
        }
        result.codes[i] = parsed.codes;
    }
    return result;
}

}
