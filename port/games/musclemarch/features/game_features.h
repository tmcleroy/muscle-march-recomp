#pragma once
// The optional features the game code asks about (each defined in its own file here). All are
// off in the Pure preset: with every one off the game runs exactly as on the Wii.

#include <cstdint>

namespace musclemarch::features {

// intro_screens.cpp
bool skip_intros();            // logos, notices and the opening end on any button
bool no_controller_notice();   // no "A Wii Remote and Nunchuk are required" screen
bool skip_strap_screen();      // the wrist-strap safety notice fades out at once

// dual_stick_poses.cpp
bool instant_poses();          // the sticks are posing: poses count at once (from the input sample,
                               // so replays agree)
// The arms the sticks ask for while instant_poses(): +1 up, -1 down, 0 level.
void stick_arms(int& remote, int& nunchuk);
bool dual_stick_poses();       // the feature is on
float pose_slice_degrees();    // each pose window's full angle now (narrows through a stage)
constexpr float kPoseLean = 22.5f;  // the windows' lean outward of vertical, in degrees
constexpr float kStickDeadZone = 0.5f;

// stick_shaking.cpp
bool stick_shaking();          // the feature is on

// touch_controls.cpp
bool touch_sticks();           // the stick guides are touch controls now (a touch screen, no gamepad)
// Where the guides go then, in the game picture's fractions (outside 0..1: the window's margins).
struct TouchPlacement {
    float dial_left[2];        // left stick, right stick
    float dial_top;
    float dial_width;
    float dial_height;
    float pause_left;
    float pause_top;
    float pause_width;
    float pause_height;
};
TouchPlacement touch_placement();
// The sticks and the pause button take touches there; nullptr: none (not racing).
void set_touch_layout(const TouchPlacement* place);

// Wii Remote buttons that count as "any button": everything but HOME (d-pad, +, -, 1, 2, A, B,
// and the Nunchuk's C and Z).
constexpr uint32_t kAnyButton = 0x7F1F;

}  // namespace musclemarch::features
