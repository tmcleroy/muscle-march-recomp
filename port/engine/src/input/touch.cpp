#include "wp/touch.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <vector>

#include "wp/hud.h"
#include "wp/video.h"

namespace wp::touch {

namespace {

using Clock = std::chrono::steady_clock;

constexpr double kPressDelay = 0.06;   // a finger points first, then presses A: menus see the hover
constexpr double kPressLength = 0.1;   // the shortest press, for quick taps
constexpr double kPointerLinger = 1.0; // the pointer stays where the finger was
constexpr double kBackLength = 0.12;

enum class Role { None, Pointer, LeftStick, RightStick, Button };

struct Finger {
    int64_t id = 0;
    float x = 0.0f;  // window fractions
    float y = 0.0f;
    bool down = true;
    Role role = Role::None;
    size_t button = 0;
    Clock::time_point since{};
    Clock::time_point lifted{};
};

struct ScriptEntry {
    long from = 0;
    long to = 0;
    float x0 = 0.0f;
    float y0 = 0.0f;
    float x1 = 0.0f;
    float y1 = 0.0f;
    bool down = false;
};

std::mutex g_mutex;
std::vector<Finger> g_fingers;
Layout g_layout;
bool g_back = false;
Clock::time_point g_back_since{};
// The last pointing finger, kept after it lifts.
bool g_pointed = false;
float g_point_x = 0.0f;
float g_point_y = 0.0f;
Clock::time_point g_point_time{};

std::vector<ScriptEntry>& script() {
    static std::vector<ScriptEntry> entries = [] {
        std::vector<ScriptEntry> result;
        const char* p = std::getenv("WP_TOUCH_SCRIPT");
        while (p && *p) {
            ScriptEntry entry;
            char* end = nullptr;
            entry.from = std::strtol(p, &end, 10);
            entry.to = *end ? std::strtol(end + 1, &end, 10) : 0;
            float* fields[4] = {&entry.x0, &entry.y0, &entry.x1, &entry.y1};
            for (float* field : fields) {
                *field = *end ? std::strtof(end + 1, &end) : 0.0f;
            }
            result.push_back(entry);
            p = end;
            while (*p && *p != ';') {
                p++;
            }
            if (*p == ';') {
                p++;
            }
        }
        return result;
    }();
    return entries;
}

double seconds(Clock::time_point from, Clock::time_point to) {
    return std::chrono::duration<double>(to - from).count();
}

void set_finger(int64_t id, float x, float y, bool down, Clock::time_point now) {
    for (Finger& finger : g_fingers) {
        if (finger.id == id && finger.down) {
            finger.x = x;
            finger.y = y;
            if (!down) {
                finger.down = false;
                finger.lifted = now;
            }
            return;
        }
    }
    if (down) {
        Finger finger;
        finger.id = id;
        finger.x = x;
        finger.y = y;
        finger.since = now;
        g_fingers.push_back(finger);
    }
}

void run_script(Clock::time_point now) {
    static const Clock::time_point origin = now;
    long elapsed = static_cast<long>(seconds(origin, now) * 1000.0);
    std::vector<ScriptEntry>& entries = script();
    for (size_t i = 0; i < entries.size(); i++) {
        ScriptEntry& entry = entries[i];
        bool down = elapsed >= entry.from && elapsed < entry.to;
        if (!down && !entry.down) {
            continue;
        }
        float t = entry.to > entry.from ? std::clamp(static_cast<float>(elapsed - entry.from) / static_cast<float>(entry.to - entry.from), 0.0f, 1.0f) : 0.0f;
        set_finger(-1 - static_cast<int64_t>(i), entry.x0 + (entry.x1 - entry.x0) * t, entry.y0 + (entry.y1 - entry.y0) * t, down, now);
        entry.down = down;
    }
}

bool picture_rect(float& left, float& top, float& width, float& height) {
    uint32_t picture_width = 0;
    uint32_t picture_height = 0;
    hud::picture_size(picture_width, picture_height);
    // The surface the renderer drew to (it can differ from the window's size in pixels: Android
    // scales a surface made before the system bars hid), else the window.
    uint32_t surface_width = 0;
    uint32_t surface_height = 0;
    hud::window_size(surface_width, surface_height);
    int client_width = static_cast<int>(surface_width);
    int client_height = static_cast<int>(surface_height);
    if (picture_width == 0 || picture_height == 0 || ((client_width <= 0 || client_height <= 0) && !video::client_size(client_width, client_height)) ||
        client_width <= 0 || client_height <= 0) {
        return false;
    }
    width = static_cast<float>(picture_width) / static_cast<float>(client_width);
    height = static_cast<float>(picture_height) / static_cast<float>(client_height);
    left = (1.0f - width) * 0.5f;
    top = (1.0f - height) * 0.5f;
    return true;
}

float picture_aspect() {
    uint32_t width = 0;
    uint32_t height = 0;
    hud::picture_size(width, height);
    return height ? static_cast<float>(width) / static_cast<float>(height) : 4.0f / 3.0f;
}

void read_stick(const Stick& stick, float x, float y, float aspect, float& out_x, float& out_y) {
    if (stick.reach <= 0.0f) {
        return;
    }
    float dx = (x - stick.x) * aspect / stick.reach;
    float dy = (y - stick.y) / stick.reach;
    float length = std::sqrt(dx * dx + dy * dy);
    if (length > 1.0f) {
        dx /= length;
        dy /= length;
    }
    out_x = dx;
    out_y = dy;
}

}  // namespace

bool in_use() {
#ifdef __ANDROID__
    return true;
#else
    static const bool scripted = !script().empty();
    return scripted;
#endif
}

void set_layout(const Layout& layout) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_layout = layout;
}

bool to_picture(float window_x, float window_y, float& x, float& y) {
    float left, top, width, height;
    if (!picture_rect(left, top, width, height)) {
        return false;
    }
    x = (window_x - left) / width;
    y = (window_y - top) / height;
    return true;
}

float side_margin() {
    float left, top, width, height;
    return picture_rect(left, top, width, height) ? left / width : 0.0f;
}

void finger(int64_t id, float window_x, float window_y, bool down) {
    std::lock_guard<std::mutex> lock(g_mutex);
    set_finger(id, window_x, window_y, down, Clock::now());
}

void cancel() {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_fingers.clear();
}

void set_back(bool held) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (held && !g_back) {
        g_back_since = Clock::now();
    }
    g_back = held;
}

bool back_held() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_back || (g_back_since != Clock::time_point{} && seconds(g_back_since, Clock::now()) < kBackLength);
}

Pointer apply(gamepad::State& pad) {
    Pointer pointer;
    if (!in_use()) {
        return pointer;
    }
    Clock::time_point now = Clock::now();
    float aspect = picture_aspect();
    std::lock_guard<std::mutex> lock(g_mutex);
    run_script(now);
    bool controls = !pad.connected;  // a real gamepad plays instead
    bool left_taken = false;
    bool right_taken = false;
    for (const Finger& finger : g_fingers) {
        left_taken = left_taken || (finger.down && finger.role == Role::LeftStick);
        right_taken = right_taken || (finger.down && finger.role == Role::RightStick);
    }
    if (controls && g_layout.sticks) {
        pad.connected = true;
    }
    for (size_t i = 0; i < g_fingers.size();) {
        Finger& finger = g_fingers[i];
        float x = 0.0f;
        float y = 0.0f;
        bool placed = to_picture(finger.x, finger.y, x, y);
        if (finger.role == Role::None) {
            finger.role = Role::Pointer;
            if (controls && placed) {
                for (size_t b = 0; b < g_layout.button_count; b++) {
                    if (g_layout.buttons[b].area.contains(x, y)) {
                        finger.role = Role::Button;
                        finger.button = b;
                    }
                }
                if (finger.role == Role::Pointer && g_layout.sticks) {
                    if (!left_taken && g_layout.left.zone.contains(x, y)) {
                        finger.role = Role::LeftStick;
                        left_taken = true;
                    } else if (!right_taken && g_layout.right.zone.contains(x, y)) {
                        finger.role = Role::RightStick;
                        right_taken = true;
                    }
                }
            }
        }
        bool keep = finger.down;
        switch (finger.role) {
        case Role::LeftStick:
            if (finger.down && g_layout.sticks) {
                read_stick(g_layout.left, x, y, aspect, pad.left_x, pad.left_y);
            }
            break;
        case Role::RightStick:
            if (finger.down && g_layout.sticks) {
                read_stick(g_layout.right, x, y, aspect, pad.right_x, pad.right_y);
            }
            break;
        case Role::Button: {
            // Held at least a moment, so a game polling once a frame sees the briefest tap.
            bool brief = seconds(finger.since, now) < kPressLength;
            if ((finger.down || brief) && finger.button < g_layout.button_count) {
                pad.buttons |= g_layout.buttons[finger.button].buttons;
            }
            keep = finger.down || brief;
            break;
        }
        default: {
            // The pointer: A from a moment after the touch until it lifts, and never too short.
            if (finger.down && placed) {
                g_pointed = true;
                g_point_x = x * 2.0f - 1.0f;
                g_point_y = y * 2.0f - 1.0f;
                g_point_time = now;
            }
            double press_end = finger.down ? 1e9 : std::max(seconds(finger.since, finger.lifted), kPressDelay + kPressLength);
            double age = seconds(finger.since, now);
            if (age >= kPressDelay && age < press_end) {
                pointer.pressed = true;
            }
            keep = finger.down || age < press_end;
            break;
        }
        }
        if (keep) {
            i++;
        } else {
            g_fingers.erase(g_fingers.begin() + static_cast<std::ptrdiff_t>(i));
        }
    }
    static const bool debug = std::getenv("WP_TOUCH_DEBUG") != nullptr;
    if (debug && !g_fingers.empty()) {
        float x = 0.0f;
        float y = 0.0f;
        to_picture(g_fingers[0].x, g_fingers[0].y, x, y);
        std::fprintf(stderr, "touch: %zu fingers (first at %.3f %.3f of the picture), left %.2f %.2f, right %.2f %.2f, buttons %x, A %d; sticks at %.3f %.3f and %.3f %.3f, reach %.3f\n",
                     g_fingers.size(), x, y, pad.left_x, pad.left_y, pad.right_x, pad.right_y, pad.buttons, pointer.pressed ? 1 : 0, g_layout.left.x, g_layout.left.y,
                     g_layout.right.x, g_layout.right.y, g_layout.left.reach);
    }
    if (g_pointed && (pointer.pressed || seconds(g_point_time, now) < kPointerLinger)) {
        pointer.active = true;
        pointer.x = g_point_x;
        pointer.y = g_point_y;
        pointer.on_picture = std::fabs(g_point_x) < 1.0f && std::fabs(g_point_y) < 1.0f;
    } else if (pointer.pressed) {
        pointer.active = true;
    }
    return pointer;
}

}  // namespace wp::touch
