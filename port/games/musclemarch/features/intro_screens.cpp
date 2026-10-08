// Intro screens: skip them with any button, and drop the controller notice.
//
// The boot shows the wrist-strap notice (any button already ends it), the Bandai Namco logo and
// the "A Wii Remote and Nunchuk are required" notice, then the opening movie. On the Wii the logo
// and the movie end only on A, B, 1 or 2, and the controller notice waits 300 frames.
// Hooks: scene/boot.cpp (the logo), scene/opening_select.cpp (the movie),
// scene/system_menu.cpp (the controller notice), scene/controller_check.cpp (raising it).

#include "game_features.h"
#include "wp/features.h"

WP_FEATURE(skip_intros, {"skip_intros", "Skip intro screens with any button",
                         "The logos, the controller notice and the opening movie end on any button, not only A, B, 1 or 2.",
                         "quality of life", wp::features::kEnhanced});

WP_FEATURE(no_controller_notice, {"no_controller_notice", "No controller notice at start",
                                  "Leave out the \"A Wii Remote and Nunchuk are required\" screen that shows for five seconds at start-up.",
                                  "quality of life", wp::features::kEnhanced});

WP_FEATURE(skip_strap_screen, {"skip_strap_screen", "Skip the wrist-strap screen",
                               "Leave out the Wii Remote safety notice (\"Put on and tighten the wrist strap\") at start-up.",
                               "quality of life", wp::features::kEnhanced});

namespace musclemarch::features {

bool skip_strap_screen() {
    static const bool on = wp::features::enabled("skip_strap_screen");
    return on;
}


bool skip_intros() {
    static const bool on = wp::features::enabled("skip_intros");
    return on;
}

bool no_controller_notice() {
    static const bool on = wp::features::enabled("no_controller_notice");
    return on;
}

}  // namespace musclemarch::features
