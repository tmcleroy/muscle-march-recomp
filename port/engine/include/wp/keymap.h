#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace wp::keymap {

constexpr int kWheelUp = 0x100;
constexpr int kWheelDown = 0x101;

enum class Action {
    A,
    B,
    One,
    Two,
    Plus,
    Minus,
    Home,
    Up,
    Down,
    Left,
    Right,
    Shake,
    NunchukShake,
    NunchukC,
    NunchukZ,
    PoseUp,
    PoseDown,
    PoseLeft,
    PoseRight,
    SwingUp,
    SwingDown,
    TiltLeft,
    TiltRight,
    TiltUp,
    TiltDown,
    Grip,
    Screenshot,
    Count,
};

constexpr size_t kActionCount = static_cast<size_t>(Action::Count);

struct ActionInfo {
    const char* name;
    uint32_t buttons;
    const char* defaults;
};

const ActionInfo& action(Action which);
const ActionInfo& action(size_t index);
std::string setting_key(size_t index);

int key_code(const std::string& name);
std::string key_name(int code);
bool reserved(int code);

struct Parsed {
    std::vector<int> codes;
    std::vector<std::string> invalid;
};

Parsed parse(const std::string& list);
std::string format(const std::vector<int>& codes, const char* separator = ",");

struct Bindings {
    std::vector<int> codes[kActionCount];

    const std::vector<int>& of(Action which) const { return codes[static_cast<size_t>(which)]; }
    bool has(Action which, int code) const;
    bool bound(int code) const;
};

Bindings defaults();
Bindings load(const std::function<std::string(const std::string& key)>& value, std::vector<std::string>& invalid);

}
