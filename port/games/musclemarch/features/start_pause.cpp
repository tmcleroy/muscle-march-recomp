// Start pauses: the gamepad's Start button opens the race's pause menu.
//
// The race pauses on the Wii Remote's 1 button (race/race_scene.cpp), which a gamepad sends from
// its west face button (X on Xbox layouts) and the keyboard from the 1 key; Start sends +, which
// does nothing in a race. With this feature Start also sends 1 while racing.

#include "game_features.h"
#include "race/course_query.h"
#include "wp/features.h"
#include "wp/input.h"

WP_FEATURE(start_pause, {"start_pause", "Pause with Start", "Start pauses a race (as X, the Wii Remote's 1 button, does).",
                         "controls", wp::features::kEnhanced});

namespace musclemarch::features {

namespace {

constexpr uint32_t kStartBit = 1u << 6;  // SDL_GAMEPAD_BUTTON_START in gamepad::State::raw_buttons

void start_pauses(uint32_t channel, wp::input::Sample& sample) {
    static const bool on = wp::features::enabled("start_pause");
    if (!on || channel != 0) {
        return;
    }
    const wp::gamepad::State& pad = wp::input::last_gamepad(0);
    uint32_t race = course_query::race();
    if (pad.connected && (pad.raw_buttons & kStartBit) && race && wp::rd32(race + course_query::kPhase) == 6 &&
        wp::rd32(race + course_query::kState) == 1) {
        sample.buttons |= wp::input::kButtonOne;
    }
}

struct Install {
    Install() { wp::input::add_transform(&start_pauses); }
} g_install;

}  // namespace

}  // namespace musclemarch::features
