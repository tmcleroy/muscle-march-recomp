// Touch controls: on a touch screen the on-screen stick guides are the controls.
//
// The two dials move out to where the thumbs are (the window's side margins and the picture's
// pillars) and grow to thumb size; touching a dial puts its stick under the finger, so a pose is
// a touch on the dial's highlighted window and shaking is rubbing (or drumming on) both dials. The
// sticks are a virtual gamepad (wp/touch.h), so dual-stick poses and stick shaking work as with a
// real one. While racing there is also a pause button in the top right corner, and the system's
// back gesture pauses a race (it is B elsewhere). Outside races a touch points and presses A, as
// a mouse does, with or without this feature.
//
// Nothing here acts while a real gamepad is connected, or without a touch screen.

#include <algorithm>

#include "game_features.h"
#include "race/course_query.h"
#include "wp/features.h"
#include "wp/gamepad.h"
#include "wp/hud.h"
#include "wp/input.h"
#include "wp/memory.h"
#include "wp/touch.h"

WP_FEATURE(touch_controls, {"touch_controls", "Touch controls",
                            "On a touch screen, the stick guides at the sides are the controls: touch a dial where its stick should "
                            "point, rub both to run, and tap the corner button to pause. Best with dual-stick poses and stick shaking.",
                            "controls", wp::features::kEnhanced});

namespace musclemarch::features {

namespace {

using namespace wp;

constexpr float kPillarWidth = 0.125f;   // the picture's own side pillars (80 of its 640 pixels)
constexpr float kDialHeight = 0.46f;     // the dial sprite's side at most, in picture heights
constexpr float kDialBandFill = 0.94f;   // ... and of the band it sits in
constexpr float kDialCentre = 0.6f;      // down the picture: under the thumbs, with room for the cue above
constexpr float kStickReach = 0.31f;     // the drawn stick's travel, in dial sprite sides (on_screen_guides.cpp)
constexpr float kPauseSize = 0.13f;      // picture heights
constexpr float kPauseInset = 0.03f;
constexpr float kPauseTouch = 1.5f;      // the touch area, in button sizes

void back_gesture(uint32_t channel, input::Sample& sample) {
    static const bool on = wp::features::enabled("touch_controls");
    if (!on || channel != 0 || !touch::back_held()) {
        return;
    }
    uint32_t race = course_query::race();
    bool racing = race && rd32(race + course_query::kPhase) == 6 && rd32(race + course_query::kState) == 1;
    sample.buttons |= racing ? input::kButtonOne : input::kButtonB;
}

struct Install {
    Install() { input::add_transform(&back_gesture); }
} g_install;

}  // namespace

bool touch_sticks() {
    static const bool on = wp::features::enabled("touch_controls");
    return on && touch::in_use() && !gamepad::connected(0);
}

TouchPlacement touch_placement() {
    TouchPlacement place;
    uint32_t width = 0;
    uint32_t height = 0;
    hud::picture_size(width, height);
    float aspect = height ? static_cast<float>(width) / static_cast<float>(height) : 4.0f / 3.0f;
    float margin = std::max(0.0f, touch::side_margin());
    float band = margin + kPillarWidth;  // from the window's edge to the pillar's inner edge, in picture widths
    place.dial_width = std::min(band * kDialBandFill, kDialHeight / aspect);
    place.dial_height = place.dial_width * aspect;
    place.dial_top = kDialCentre - place.dial_height * 0.5f;
    place.dial_left[0] = (kPillarWidth - margin) * 0.5f - place.dial_width * 0.5f;
    place.dial_left[1] = 1.0f - place.dial_left[0] - place.dial_width;
    place.pause_width = kPauseSize / aspect;
    place.pause_height = kPauseSize;
    place.pause_left = 1.0f + margin - kPauseInset / aspect - place.pause_width;
    place.pause_top = kPauseInset;
    return place;
}

void set_touch_layout(const TouchPlacement* place) {
    touch::Layout layout;
    if (place) {
        constexpr float kFar = 100.0f;
        layout.sticks = true;
        for (int i = 0; i < 2; i++) {
            touch::Stick& stick = i == 0 ? layout.left : layout.right;
            stick.x = place->dial_left[i] + place->dial_width * 0.5f;
            stick.y = place->dial_top + place->dial_height * 0.5f;
            stick.reach = place->dial_height * kStickReach;
        }
        // Each thumb's side of the screen, out from the pillar's inner edge (or the dial's, if wider).
        float inner = std::max(kPillarWidth, place->dial_left[0] + place->dial_width);
        layout.left.zone = {-kFar, -kFar, inner, kFar};
        layout.right.zone = {1.0f - inner, -kFar, kFar, kFar};
        float grow_x = place->pause_width * (kPauseTouch - 1.0f) * 0.5f;
        float grow_y = place->pause_height * (kPauseTouch - 1.0f) * 0.5f;
        layout.buttons[0].area = {place->pause_left - grow_x, place->pause_top - grow_y, place->pause_left + place->pause_width + grow_x,
                                  place->pause_top + place->pause_height + grow_y};
        layout.buttons[0].buttons = input::kButtonOne;  // the game's pause
        layout.button_count = 1;
    }
    touch::set_layout(layout);
}

}  // namespace musclemarch::features
