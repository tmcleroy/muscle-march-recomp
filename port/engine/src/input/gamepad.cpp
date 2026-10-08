#include "wp/gamepad.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <mutex>
#include <thread>

#include "wp/game.h"
#include "wp/input.h"
#include "wp/log.h"
#include "wp/settings.h"

namespace wp::gamepad {

namespace {

constexpr float kGravity = 9.80665f;
constexpr float kStickThreshold = 0.5f;
constexpr float kTriggerThreshold = 0.5f;
constexpr float kPointerPerRadian = 2.3f;
constexpr float kStickPointerSpeed = 1.6f;
constexpr float kStickDeadZone = 0.15f;
constexpr float kPointerLimit = 1.3f;
constexpr float kStillGyro = 0.08f;
constexpr float kStillAccel = 0.4f;
constexpr float kStillSeconds = 1.0f;
constexpr auto kPollInterval = std::chrono::milliseconds(4);
constexpr auto kRumbleRefresh = std::chrono::milliseconds(1000);
constexpr uint32_t kRumbleDuration = 2000;
constexpr uint16_t kRumbleStrength = 0xC000;

struct Snapshot {
    bool connected = false;
    bool buttons[SDL_GAMEPAD_BUTTON_COUNT] = {};
    float left_x = 0.0f;
    float left_y = 0.0f;
    float right_trigger = 0.0f;
    float left_trigger = 0.0f;
    float right_x = 0.0f;
    float right_y = 0.0f;
    bool has_accel = false;
    float accel[3] = {0.0f, 0.0f, 0.0f};
    bool pointer_valid = false;
    float pointer_x = 0.0f;
    float pointer_y = 0.0f;
    bool nintendo_layout = false;
};

struct Pad {
    SDL_Gamepad* handle = nullptr;
    SDL_JoystickID id = 0;
    bool has_gyro = false;
    bool has_accel = false;
    bool nintendo_layout = false;
    float pointer_x = 0.0f;
    float pointer_y = 0.0f;
    bool pointer_active = false;
    bool recenter_held = false;
    float bias[3] = {};
    float still_sum[3] = {};
    float still_accel[3] = {};
    float still_time = 0.0f;
    int still_samples = 0;
    bool rumble = false;
    std::chrono::steady_clock::time_point rumble_refresh{};
    int player = -1;
};

std::mutex g_mutex;
std::array<Snapshot, kSlots> g_snapshots;
std::array<std::atomic<bool>, kSlots> g_rumble{};
std::array<std::atomic<int>, kSlots> g_player{};
std::once_flag g_started;

bool enabled() {
    static const settings::LiveFlag value("input.gamepads", "WP_GAMEPAD");
    return value();
}

float axis(SDL_Gamepad* pad, SDL_GamepadAxis which) {
    return static_cast<float>(SDL_GetGamepadAxis(pad, which)) / 32767.0f;
}

void open(std::array<Pad, kSlots>& pads, SDL_JoystickID id) {
    for (const Pad& pad : pads) {
        if (pad.handle && pad.id == id) {
            return;
        }
    }
    // Tests with virtual pads ignore real ones, so a connected controller can't take their slots.
    static const bool virtual_only = std::getenv("WP_VIRTUAL_GAMEPADS") && std::atoi(std::getenv("WP_VIRTUAL_GAMEPADS")) > 0;
    if (virtual_only && !SDL_IsJoystickVirtual(id)) {
        return;
    }
    for (size_t slot = 0; slot < kSlots; slot++) {
        Pad& pad = pads[slot];
        if (pad.handle) {
            continue;
        }
        SDL_Gamepad* handle = SDL_OpenGamepad(id);
        if (!handle) {
            return;
        }
        pad = Pad{};
        pad.handle = handle;
        pad.id = id;
        pad.has_gyro = SDL_GamepadHasSensor(handle, SDL_SENSOR_GYRO) && SDL_SetGamepadSensorEnabled(handle, SDL_SENSOR_GYRO, true);
        pad.has_accel = SDL_GamepadHasSensor(handle, SDL_SENSOR_ACCEL) && SDL_SetGamepadSensorEnabled(handle, SDL_SENSOR_ACCEL, true);
        pad.pointer_active = pad.has_gyro;
        pad.nintendo_layout = SDL_GetGamepadButtonLabel(handle, SDL_GAMEPAD_BUTTON_SOUTH) == SDL_GAMEPAD_BUTTON_LABEL_B;
        const char* name = SDL_GetGamepadName(handle);
        log::write("input", "gamepad %s connected as Wii Remote %u, gyroscope %s, accelerometer %s, %s layout", name ? name : "?", static_cast<unsigned>(slot + 1),
                   pad.has_gyro ? "yes" : "no", pad.has_accel ? "yes" : "no", pad.nintendo_layout ? "Nintendo" : "Xbox");
        return;
    }
}

void close(std::array<Pad, kSlots>& pads, SDL_JoystickID id) {
    for (size_t slot = 0; slot < kSlots; slot++) {
        Pad& pad = pads[slot];
        if (pad.handle && pad.id == id) {
            SDL_CloseGamepad(pad.handle);
            pad = Pad{};
            log::write("input", "gamepad of Wii Remote %u disconnected", static_cast<unsigned>(slot + 1));
        }
    }
}

void calibrate(Pad& pad, const float gyro[3], const float accel[3], float dt) {
    float accel_change = 0.0f;
    float rate = 0.0f;
    for (int i = 0; i < 3; i++) {
        rate = std::max(rate, std::fabs(gyro[i] - pad.bias[i]));
        if (pad.still_samples > 0) {
            accel_change = std::max(accel_change, std::fabs(accel[i] - pad.still_accel[i]));
        }
    }
    if (rate > kStillGyro || accel_change > kStillAccel) {
        pad.still_time = 0.0f;
        pad.still_samples = 0;
        for (int i = 0; i < 3; i++) {
            pad.still_sum[i] = 0.0f;
        }
        return;
    }
    if (pad.still_samples == 0) {
        for (int i = 0; i < 3; i++) {
            pad.still_accel[i] = accel[i];
        }
    }
    for (int i = 0; i < 3; i++) {
        pad.still_sum[i] += gyro[i];
    }
    pad.still_samples++;
    pad.still_time += dt;
    if (pad.still_time >= kStillSeconds) {
        for (int i = 0; i < 3; i++) {
            pad.bias[i] = pad.still_sum[i] / static_cast<float>(pad.still_samples);
            pad.still_sum[i] = 0.0f;
        }
        pad.still_time = 0.0f;
        pad.still_samples = 0;
    }
}

Snapshot read(Pad& pad, float dt) {
    Snapshot snapshot;
    SDL_Gamepad* handle = pad.handle;
    snapshot.connected = true;
    snapshot.nintendo_layout = pad.nintendo_layout;
    for (int i = 0; i < SDL_GAMEPAD_BUTTON_COUNT; i++) {
        snapshot.buttons[i] = SDL_GetGamepadButton(handle, static_cast<SDL_GamepadButton>(i));
    }
    snapshot.left_x = axis(handle, SDL_GAMEPAD_AXIS_LEFTX);
    snapshot.left_y = axis(handle, SDL_GAMEPAD_AXIS_LEFTY);
    snapshot.right_trigger = axis(handle, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);
    snapshot.left_trigger = axis(handle, SDL_GAMEPAD_AXIS_LEFT_TRIGGER);
    float right_x = axis(handle, SDL_GAMEPAD_AXIS_RIGHTX);
    float right_y = axis(handle, SDL_GAMEPAD_AXIS_RIGHTY);
    snapshot.right_x = right_x;
    snapshot.right_y = right_y;
    if (std::fabs(right_x) > kStickDeadZone || std::fabs(right_y) > kStickDeadZone) {
        pad.pointer_active = true;
        pad.pointer_x += right_x * kStickPointerSpeed * dt;
        pad.pointer_y += right_y * kStickPointerSpeed * dt;
    }
    if (pad.has_accel) {
        snapshot.has_accel = SDL_GetGamepadSensorData(handle, SDL_SENSOR_ACCEL, snapshot.accel, 3);
    }
    float gyro[3] = {};
    if (pad.has_gyro && SDL_GetGamepadSensorData(handle, SDL_SENSOR_GYRO, gyro, 3)) {
        if (snapshot.has_accel) {
            calibrate(pad, gyro, snapshot.accel, dt);
        }
        pad.pointer_x -= (gyro[1] - pad.bias[1]) * dt * kPointerPerRadian;
        pad.pointer_y -= (gyro[0] - pad.bias[0]) * dt * kPointerPerRadian;
    }
    bool recenter = snapshot.buttons[SDL_GAMEPAD_BUTTON_RIGHT_STICK];
    if (recenter && !pad.recenter_held) {
        pad.pointer_x = 0.0f;
        pad.pointer_y = 0.0f;
        pad.pointer_active = true;
    }
    pad.recenter_held = recenter;
    pad.pointer_x = std::clamp(pad.pointer_x, -kPointerLimit, kPointerLimit);
    pad.pointer_y = std::clamp(pad.pointer_y, -kPointerLimit, kPointerLimit);
    snapshot.pointer_valid = pad.pointer_active && std::fabs(pad.pointer_x) < 1.0f && std::fabs(pad.pointer_y) < 1.0f;
    snapshot.pointer_x = pad.pointer_x;
    snapshot.pointer_y = pad.pointer_y;
    return snapshot;
}

void apply_outputs(Pad& pad, size_t slot) {
    bool rumble = g_rumble[slot].load();
    auto now = std::chrono::steady_clock::now();
    if (rumble != pad.rumble || (rumble && now >= pad.rumble_refresh)) {
        pad.rumble = rumble;
        pad.rumble_refresh = now + kRumbleRefresh;
        uint16_t strength = rumble ? kRumbleStrength : 0;
        SDL_RumbleGamepad(pad.handle, strength, strength, rumble ? kRumbleDuration : 0);
    }
    int player = g_player[slot].load();
    if (player != pad.player) {
        pad.player = player;
        SDL_SetGamepadPlayerIndex(pad.handle, player);
    }
}

struct Virtual {
    SDL_JoystickID id = 0;
    SDL_Joystick* joystick = nullptr;
    unsigned number = 0;
};

bool SDLCALL virtual_rumble(void* userdata, Uint16 low, Uint16 high) {
    log::write("input", "virtual gamepad %u rumble %u %u", static_cast<Virtual*>(userdata)->number, low, high);
    return true;
}

void SDLCALL virtual_player(void* userdata, int player) {
    log::write("input", "virtual gamepad %u player index %d", static_cast<Virtual*>(userdata)->number, player);
}

void attach_virtual(std::array<Virtual, kSlots>& virtuals) {
    const char* setting = std::getenv("WP_VIRTUAL_GAMEPADS");
    int count = setting ? std::clamp(std::atoi(setting), 0, static_cast<int>(kSlots)) : 0;
    static const SDL_VirtualJoystickSensorDesc sensors[] = {{SDL_SENSOR_ACCEL, 250.0f}, {SDL_SENSOR_GYRO, 250.0f}};
    for (int i = 0; i < count; i++) {
        Virtual& pad = virtuals[i];
        pad.number = static_cast<unsigned>(i + 1);
        SDL_VirtualJoystickDesc desc;
        SDL_INIT_INTERFACE(&desc);
        desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
        desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
        desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
        desc.nsensors = 2;
        desc.sensors = sensors;
        desc.name = "Virtual test gamepad";
        desc.userdata = &pad;
        desc.Rumble = virtual_rumble;
        desc.SetPlayerIndex = virtual_player;
        pad.id = SDL_AttachVirtualJoystick(&desc);
        pad.joystick = pad.id ? SDL_OpenJoystick(pad.id) : nullptr;
    }
}

void drive_virtual(std::array<Virtual, kSlots>& virtuals) {
    static const auto origin = std::chrono::steady_clock::now();
    static const char* until_setting = std::getenv("WP_VIRTUAL_GAMEPADS_UNTIL");
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - origin).count();
    if (until_setting && elapsed >= std::atoll(until_setting) && virtuals[0].joystick) {
        for (Virtual& pad : virtuals) {
            if (pad.joystick) {
                SDL_CloseJoystick(pad.joystick);
                SDL_DetachVirtualJoystick(pad.id);
                pad.joystick = nullptr;
            }
        }
    }
    bool tap = elapsed % 3000 < 100;
    const float accel[3] = {0.0f, kGravity, 0.0f};
    const float gyro[3] = {0.0f, 0.0f, 0.0f};
    // WP_VIRTUAL_AXES="from_ms,to_ms,axis,value;...": SDL gamepad axis (0 LX, 1 LY, 2 RX, 3 RY,
    // 4 LT, 5 RT) held at value (-1..1) on the first virtual pad during [from, to).
    if (const char* axes = std::getenv("WP_VIRTUAL_AXES"); axes && virtuals[0].joystick) {
        float values[SDL_GAMEPAD_AXIS_COUNT] = {};
        const char* p = axes;
        while (*p) {
            long from = std::strtol(p, const_cast<char**>(&p), 10);
            long to = std::strtol(p + 1, const_cast<char**>(&p), 10);
            long axis = std::strtol(p + 1, const_cast<char**>(&p), 10);
            float value = std::strtof(p + 1, const_cast<char**>(&p));
            if (axis >= 0 && axis < SDL_GAMEPAD_AXIS_COUNT && elapsed >= from && elapsed < to) {
                values[axis] = value;
            }
            while (*p && *p != ';') {
                p++;
            }
            if (*p == ';') {
                p++;
            }
        }
        for (int axis = 0; axis < SDL_GAMEPAD_AXIS_COUNT; axis++) {
            SDL_SetJoystickVirtualAxis(virtuals[0].joystick, axis, static_cast<Sint16>(std::clamp(values[axis], -1.0f, 1.0f) * 32767));
        }
    }
    // WP_VIRTUAL_BUTTONS="from_ms,to_ms,button;...": SDL gamepad button held on the first virtual
    // pad during [from, to) (e.g. 6 = Start).
    if (const char* buttons = std::getenv("WP_VIRTUAL_BUTTONS"); buttons && virtuals[0].joystick) {
        bool held[SDL_GAMEPAD_BUTTON_COUNT] = {};
        const char* p = buttons;
        while (*p) {
            long from = std::strtol(p, const_cast<char**>(&p), 10);
            long to = std::strtol(p + 1, const_cast<char**>(&p), 10);
            long button = std::strtol(p + 1, const_cast<char**>(&p), 10);
            if (button >= 0 && button < SDL_GAMEPAD_BUTTON_COUNT && elapsed >= from && elapsed < to) {
                held[button] = true;
            }
            while (*p && *p != ';') {
                p++;
            }
            if (*p == ';') {
                p++;
            }
        }
        for (int button = 0; button < SDL_GAMEPAD_BUTTON_COUNT; button++) {
            if (button != SDL_GAMEPAD_BUTTON_SOUTH) {
                SDL_SetJoystickVirtualButton(virtuals[0].joystick, button, held[button]);
            }
        }
    }
    for (Virtual& pad : virtuals) {
        if (pad.joystick) {
            SDL_SetJoystickVirtualButton(pad.joystick, SDL_GAMEPAD_BUTTON_SOUTH, tap);
            SDL_SendJoystickVirtualSensorData(pad.joystick, SDL_SENSOR_ACCEL, SDL_GetTicksNS(), accel, 3);
            SDL_SendJoystickVirtualSensorData(pad.joystick, SDL_SENSOR_GYRO, SDL_GetTicksNS(), gyro, 3);
        }
    }
}

void worker() {
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_JOY_CONS, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_COMBINE_JOY_CONS, settings::flag("input.joycon_pairs", "WP_JOYCON_PAIRS") ? "1" : "0");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_VERTICAL_JOY_CONS, "0");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_WII, "1");  // real Wii Remotes paired with the computer (as the launcher)
    if (!SDL_Init(SDL_INIT_GAMEPAD)) {
        log::write("input", "SDL gamepad support unavailable: %s", SDL_GetError());
        return;
    }
    std::array<Pad, kSlots> pads;
    std::array<Virtual, kSlots> virtuals;
    attach_virtual(virtuals);
    auto previous = std::chrono::steady_clock::now();
    while (true) {
        drive_virtual(virtuals);
        SDL_UpdateJoysticks();
        SDL_Event event;
        bool own_queue = !SDL_WasInit(SDL_INIT_VIDEO);
        if (own_queue) {
            SDL_PumpEvents();
        }
        while (SDL_PeepEvents(&event, 1, SDL_GETEVENT, own_queue ? SDL_EVENT_FIRST : SDL_EVENT_JOYSTICK_AXIS_MOTION,
                              own_queue ? SDL_EVENT_LAST : SDL_EVENT_FINGER_DOWN - 1) > 0) {
            if (event.type == SDL_EVENT_GAMEPAD_ADDED) {
                open(pads, event.gdevice.which);
            } else if (event.type == SDL_EVENT_GAMEPAD_REMOVED) {
                close(pads, event.gdevice.which);
            }
        }
        auto now = std::chrono::steady_clock::now();
        float dt = std::chrono::duration<float>(now - previous).count();
        previous = now;
        std::array<Snapshot, kSlots> snapshots;
        for (size_t slot = 0; slot < kSlots; slot++) {
            if (pads[slot].handle) {
                snapshots[slot] = read(pads[slot], dt);
                apply_outputs(pads[slot], slot);
            }
        }
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_snapshots = snapshots;
        }
        std::this_thread::sleep_for(kPollInterval);
    }
}

void start() {
    std::call_once(g_started, [] {
        for (size_t slot = 0; slot < kSlots; slot++) {
            g_player[slot] = -1;
        }
        std::thread(worker).detach();
    });
}

Snapshot snapshot(uint32_t slot) {
    if (slot >= kSlots || !enabled()) {
        return Snapshot{};
    }
    start();
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_snapshots[slot];
}

}

bool connected(uint32_t slot) {
    return snapshot(slot).connected;
}

void set_outputs(uint32_t slot, bool rumble, uint8_t leds) {
    if (slot >= kSlots || !enabled()) {
        return;
    }
    int player = -1;
    for (int i = 0; i < 4; i++) {
        if (leds == (1u << i)) {
            player = i;
        }
    }
    g_rumble[slot] = rumble;
    g_player[slot] = player;
}

uint32_t menu_buttons() {
    uint32_t result = 0;
    for (uint32_t slot = 0; slot < kSlots; slot++) {
        Snapshot current = snapshot(slot);
        if (!current.connected) {
            continue;
        }
        auto held = [&](SDL_GamepadButton button) { return current.buttons[button]; };
        SDL_GamepadButton accept = current.nintendo_layout ? SDL_GAMEPAD_BUTTON_EAST : SDL_GAMEPAD_BUTTON_SOUTH;
        SDL_GamepadButton cancel = current.nintendo_layout ? SDL_GAMEPAD_BUTTON_SOUTH : SDL_GAMEPAD_BUTTON_EAST;
        if (held(SDL_GAMEPAD_BUTTON_DPAD_UP) || current.left_y < -kStickThreshold) {
            result |= kMenuUp;
        }
        if (held(SDL_GAMEPAD_BUTTON_DPAD_DOWN) || current.left_y > kStickThreshold) {
            result |= kMenuDown;
        }
        if (held(SDL_GAMEPAD_BUTTON_DPAD_LEFT) || current.left_x < -kStickThreshold) {
            result |= kMenuLeft;
        }
        if (held(SDL_GAMEPAD_BUTTON_DPAD_RIGHT) || current.left_x > kStickThreshold) {
            result |= kMenuRight;
        }
        if (held(accept)) {
            result |= kMenuAccept;
        }
        if (held(cancel)) {
            result |= kMenuCancel;
        }
        if (held(SDL_GAMEPAD_BUTTON_START) && (held(SDL_GAMEPAD_BUTTON_GUIDE) || held(SDL_GAMEPAD_BUTTON_BACK))) {
            result |= kMenuToggle;
        }
    }
    return result;
}

State poll(uint32_t slot, bool sideways) {
    Snapshot current = snapshot(slot);
    State state;
    if (!current.connected) {
        return state;
    }
    state.connected = true;
    state.left_x = current.left_x;
    state.left_y = current.left_y;
    state.right_x = current.right_x;
    state.right_y = current.right_y;
    state.left_trigger = current.left_trigger;
    state.right_trigger = current.right_trigger;
    for (int i = 0; i < SDL_GAMEPAD_BUTTON_COUNT && i < 32; i++) {
        state.raw_buttons |= current.buttons[i] ? 1u << i : 0u;
    }
    auto held = [&](SDL_GamepadButton button) { return current.buttons[button]; };
    uint32_t& b = state.buttons;
    if (held(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER)) {
        b |= input::kButtonA;
    }
    if (current.right_trigger > kTriggerThreshold) {
        b |= input::kButtonB;
    }
    if (sideways) {
        if (held(SDL_GAMEPAD_BUTTON_SOUTH)) {
            b |= input::kButtonTwo;
        }
        if (held(SDL_GAMEPAD_BUTTON_WEST) || held(SDL_GAMEPAD_BUTTON_EAST)) {
            b |= input::kButtonOne;
        }
        if (held(SDL_GAMEPAD_BUTTON_NORTH)) {
            b |= input::kButtonA;
        }
    } else {
        SDL_GamepadButton a_button = current.nintendo_layout ? SDL_GAMEPAD_BUTTON_EAST : SDL_GAMEPAD_BUTTON_SOUTH;
        SDL_GamepadButton b_button = current.nintendo_layout ? SDL_GAMEPAD_BUTTON_SOUTH : SDL_GAMEPAD_BUTTON_EAST;
        if (held(a_button)) {
            b |= input::kButtonA;
        }
        if (held(b_button)) {
            b |= input::kButtonB;
        }
        if (held(SDL_GAMEPAD_BUTTON_WEST)) {
            b |= input::kButtonOne;
        }
        if (held(SDL_GAMEPAD_BUTTON_NORTH)) {
            b |= input::kButtonTwo;
        }
    }
    if (held(SDL_GAMEPAD_BUTTON_BACK)) {
        b |= input::kButtonMinus;
    }
    if (held(SDL_GAMEPAD_BUTTON_START)) {
        b |= input::kButtonPlus;
    }
    if (held(SDL_GAMEPAD_BUTTON_GUIDE)) {
        b |= input::kButtonHome;
    }
    bool left = held(SDL_GAMEPAD_BUTTON_DPAD_LEFT) || current.left_x < -kStickThreshold;
    bool right = held(SDL_GAMEPAD_BUTTON_DPAD_RIGHT) || current.left_x > kStickThreshold;
    bool up = held(SDL_GAMEPAD_BUTTON_DPAD_UP) || current.left_y < -kStickThreshold;
    bool down = held(SDL_GAMEPAD_BUTTON_DPAD_DOWN) || current.left_y > kStickThreshold;
    if (sideways) {
        b |= (left ? input::kButtonUp : 0) | (right ? input::kButtonDown : 0) | (up ? input::kButtonRight : 0) | (down ? input::kButtonLeft : 0);
    } else {
        b |= (left ? input::kButtonLeft : 0) | (right ? input::kButtonRight : 0) | (up ? input::kButtonUp : 0) | (down ? input::kButtonDown : 0);
    }
    if (game::description().nunchuk) {
        // Nunchuk games (Muscle March): right stick = the four arm poses,
        // left trigger = shake both controllers, left shoulder = Z.
        if (std::fabs(current.right_y) > kStickThreshold && std::fabs(current.right_y) >= std::fabs(current.right_x)) {
            b |= current.right_y < 0 ? input::kRemotePointUp | input::kNunchukPointUp : input::kRemotePointDown | input::kNunchukPointDown;
        } else if (std::fabs(current.right_x) > kStickThreshold) {
            b |= current.right_x < 0 ? input::kNunchukPointUp | input::kRemotePointDown : input::kRemotePointUp | input::kNunchukPointDown;
        }
        if (current.left_trigger > kTriggerThreshold) {
            b |= input::kMotionShake | input::kNunchukShake;
        }
        if (held(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER)) {
            b |= input::kNunchukZ;
        }
    }
    state.pointer_valid = current.pointer_valid;
    state.pointer_x = current.pointer_x;
    state.pointer_y = current.pointer_y;
    if (current.has_accel) {
        float x = current.accel[0] / kGravity;
        float y = current.accel[1] / kGravity;
        float z = current.accel[2] / kGravity;
        state.motion_valid = true;
        if (sideways) {
            state.accel[0] = z;
            state.accel[1] = -x;
        } else {
            state.accel[0] = -x;
            state.accel[1] = -z;
        }
        state.accel[2] = y;
    }
    return state;
}

}
