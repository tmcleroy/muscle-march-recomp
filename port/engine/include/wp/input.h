#pragma once

#include <cstdint>

#include "wp/gamepad.h"
#include "wp/keymap.h"

namespace wp::input {

constexpr uint32_t kButtonLeft = 0x0001;
constexpr uint32_t kButtonRight = 0x0002;
constexpr uint32_t kButtonDown = 0x0004;
constexpr uint32_t kButtonUp = 0x0008;
constexpr uint32_t kButtonPlus = 0x0010;
constexpr uint32_t kButtonTwo = 0x0100;
constexpr uint32_t kButtonOne = 0x0200;
constexpr uint32_t kButtonB = 0x0400;
constexpr uint32_t kButtonA = 0x0800;
constexpr uint32_t kButtonMinus = 0x1000;
constexpr uint32_t kButtonHome = 0x8000;
constexpr uint32_t kMotionShake = 0x10000;
constexpr uint32_t kMotionSwingUp = 0x20000;
constexpr uint32_t kMotionSwingDown = 0x40000;
constexpr uint32_t kMotionTiltLeft = 0x80000;
constexpr uint32_t kMotionTiltRight = 0x100000;
constexpr uint32_t kMotionTiltUp = 0x200000;
constexpr uint32_t kMotionTiltDown = 0x400000;
constexpr uint32_t kMotionSideways = 0x800000;
constexpr uint32_t kNunchukC = 0x1000000;
constexpr uint32_t kNunchukZ = 0x2000000;
constexpr uint32_t kNunchukShake = 0x4000000;
// Point a controller straight up or down (Muscle March's poses).
constexpr uint32_t kRemotePointUp = 0x8000000;
constexpr uint32_t kRemotePointDown = 0x10000000;
constexpr uint32_t kNunchukPointUp = 0x20000000;
constexpr uint32_t kNunchukPointDown = 0x40000000;
// Point at once, without the time a turning arm takes (and games may skip their settling time).
constexpr uint32_t kPointInstant = 0x80000000;

struct Sample {
    uint32_t buttons = 0;
    bool pointer_valid = false;
    float pointer_x = 0.0f;
    float pointer_y = 0.0f;
    bool motion_valid = false;
    float accel[3] = {0.0f, 0.0f, 1.0f};
    float stick_x = 0.0f;
    float stick_y = 0.0f;
};

bool connected(uint32_t channel);
Sample sample(uint32_t channel);
// Transforms adjust live input before the game reads it and before it is recorded: optional
// features (e.g. other control schemes) and test tools (a game's autoplay). They run in the
// order added and never while replaying a movie (the movie has the transformed input).
using Transform = void (*)(uint32_t channel, Sample& sample);
void add_transform(Transform transform);
// The gamepad driving `channel` as last read for live input (connected = false if none).
const gamepad::State& last_gamepad(uint32_t channel);
// The last sample() result for a channel (live, after transforms, or replayed).
const Sample& last_sample(uint32_t channel);
bool wakes_remote(uint32_t channel);
void note_wheel(int delta);
const keymap::Bindings& bindings();
void set_blocked(bool blocked);

}
