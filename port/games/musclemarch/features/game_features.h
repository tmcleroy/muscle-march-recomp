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

// Wii Remote buttons that count as "any button": everything but HOME (d-pad, +, -, 1, 2, A, B,
// and the Nunchuk's C and Z).
constexpr uint32_t kAnyButton = 0x7F1F;

}  // namespace musclemarch::features
