#pragma once
// Touch screens: fingers become a virtual gamepad and the pointer.
//
// A game feature lays out the controls (set_layout): two sticks and a few buttons, placed in the
// game picture's coordinates (fractions of it from its top left; outside 0..1 is the window's
// margin beside the picture). A finger that lands on a button or in a stick's zone stays that
// control until it lifts; a stick reads the finger's offset from the stick's centre, so touching
// a point on the drawn stick puts the stick there. Any other finger is the pointer: it points
// where it touches and presses A, as a mouse click does.
//
// Used on Android, and anywhere under WP_TOUCH_SCRIPT (tests): "from_ms,to_ms,x0,y0,x1,y1;..." is
// one finger per entry, down during [from, to) and moving from (x0, y0) to (x1, y1) in window
// fractions.

#include <cstddef>
#include <cstdint>

#include "wp/gamepad.h"

namespace wp::touch {

struct Rect {
    float left = 0.0f;
    float top = 0.0f;
    float right = 0.0f;
    float bottom = 0.0f;
    bool contains(float x, float y) const { return x >= left && x < right && y >= top && y < bottom; }
};

struct Stick {
    float x = 0.0f;       // centre
    float y = 0.0f;
    float reach = 0.0f;   // the offset of a full push, in picture heights
    Rect zone;            // fingers landing here take the stick
};

struct Button {
    Rect area;
    uint32_t buttons = 0;  // input::kButton... bits held while touched
};

constexpr size_t kMaxButtons = 4;

struct Layout {
    bool sticks = false;
    Stick left;
    Stick right;
    Button buttons[kMaxButtons];
    size_t button_count = 0;
};

struct Pointer {
    bool active = false;   // a finger is pointing, or just did
    bool on_picture = false;
    float x = 0.0f;        // -1..1 across the picture, as a mouse on it
    float y = 0.0f;
    bool pressed = false;  // A
};

// True where touch is how the game is played (Android), or scripted.
bool in_use();
void set_layout(const Layout& layout);
// Window fractions (0..1) to picture fractions; false before the first frame is shown.
bool to_picture(float window_x, float window_y, float& x, float& y);
// The window's margin beside the picture, in picture widths (0 when the picture fills it).
float side_margin();

// From the window thread.
void finger(int64_t id, float window_x, float window_y, bool down);
void cancel();
void set_back(bool held);

// From the input sample of channel 0: adds the sticks and buttons to `pad` (when no real gamepad
// is connected) and returns the pointer.
Pointer apply(gamepad::State& pad);
// The system's back gesture or key (held at least a moment, so a game polling per frame sees it).
bool back_held();

}  // namespace wp::touch
