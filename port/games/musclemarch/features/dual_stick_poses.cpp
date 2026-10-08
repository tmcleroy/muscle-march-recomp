// Dual-stick poses: one stick per arm, pointed the way the arm goes, and the arm only counts while
// its stick points within a slice. The slice narrows through each stage and with the difficulty,
// so posing gets harder as the game goes on.
//
// The game reads arms from the controllers' tilt (input/motion.cpp): each arm is up, down or
// level, and a level arm means no pose. With a gamepad the port normally maps the right stick's
// four directions to the four poses (any direction counts). This feature replaces that:
//   right stick = the Remote (right) arm, left stick = the Nunchuk (left) arm;
//   an arm is up while its stick is pushed at least half way within the up slice, down likewise,
//   and level otherwise. Arms reach outward, so each slice is centred kLean degrees outward of
//   vertical (left stick: to the left; right stick: to the right). Straight up/down and the
//   outward diagonal always count; the inward side and sideways drop out as the slice narrows.
// Poses: both up, both down, right up + left down, right down + left up (docs/GAME.md).
// Slice (full angle): Easy 130° -> 100°, Normal 115° -> 80°, Hard 100° -> 60°, from the start of
// a stage to its end. Keyboard poses (I/J/K/L, W/A/S/D or the arrows) are instant too; a controller with a real motion sensor still poses by
// motion when its sticks are at rest.
// Stick poses are instant: the transform sets kPointInstant, so the emulated arms point at once
// and the game (input/motion.cpp) takes the pose the sticks ask for the same frame (not the one
// read from the accelerometers, which shaking would wobble), shows it at once, and drops it as
// soon as a stick is let go; on the Wii an arm takes time to turn and the pose settles first.

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstdint>

#include "game_features.h"
#include "wp/features.h"
#include "wp/input.h"
#include "wp/memory.h"

WP_FEATURE(dual_stick_poses, {"dual_stick_poses", "Dual-stick poses",
                              "With a gamepad, each stick moves one arm (right stick: right arm). An arm only counts while its stick points "
                              "close to straight up or down, and that window narrows through each stage and on harder difficulties.",
                              "controls", wp::features::kEnhanced});

namespace musclemarch::features {

namespace {

using namespace wp;

constexpr uint32_t kCurrentRace = 0x804223D0;   // the race object while racing
constexpr uint32_t kDifficulty = 0x1C0;         // 0 easy, 1 normal, 2 hard
constexpr uint32_t kPlayerSegment = 0x168;
constexpr uint32_t kSegmentCount = 0x59CF0 + 0x11A8;  // the course's

constexpr float kStartSlice[3] = {130.0f, 115.0f, 100.0f};
constexpr float kEndSlice[3] = {100.0f, 80.0f, 60.0f};
constexpr float kLean = kPoseLean;
constexpr float kDeadZone = kStickDeadZone;  // how far a stick must be pushed

constexpr uint32_t kPoseBits = input::kRemotePointUp | input::kRemotePointDown | input::kNunchukPointUp | input::kNunchukPointDown;

bool valid(uint32_t address) {
    return (address >= 0x80000000 && address < 0x81800000) || (address >= 0x90000000 && address < 0x94000000);
}

// The slice's full angle in degrees for the current race (the start of Easy outside races).
float slice_degrees() {
    uint32_t race = rd32(kCurrentRace);
    if (!valid(race)) {
        return kStartSlice[0];
    }
    int32_t difficulty = static_cast<int32_t>(rd32(race + kDifficulty));
    difficulty = difficulty < 0 ? 0 : difficulty > 2 ? 2 : difficulty;
    int32_t segment = static_cast<int32_t>(rd32(race + kPlayerSegment));
    int32_t count = static_cast<int32_t>(rd32(race + kSegmentCount));
    float progress = count > 0 ? static_cast<float>(segment) / static_cast<float>(count) : 0.0f;
    progress = progress < 0.0f ? 0.0f : progress > 1.0f ? 1.0f : progress;
    return kStartSlice[difficulty] + (kEndSlice[difficulty] - kStartSlice[difficulty]) * progress;
}

enum class Arm { Level, Up, Down };

// outward: +1 for the right stick (outward = +x), -1 for the left stick.
Arm arm_of(float x, float y, float outward, float half_slice) {
    if (std::hypot(x, y) < kDeadZone) {
        return Arm::Level;
    }
    float out = x * outward;
    float from_up = std::atan2(out, -y) * 57.29578f;   // degrees from straight up, outward positive (stick y is down)
    float from_down = std::atan2(out, y) * 57.29578f;
    if (std::fabs(from_up - kLean) <= half_slice) {
        return Arm::Up;
    }
    if (std::fabs(from_down - kLean) <= half_slice) {
        return Arm::Down;
    }
    return Arm::Level;
}

void transform(uint32_t channel, input::Sample& sample) {
    static const bool on = wp::features::enabled("dual_stick_poses");
    const gamepad::State& pad = input::last_gamepad(channel);
    if (!on || channel != 0) {
        return;
    }
    // Who decides the arms, at once:
    //   - sticks pushed: the sticks (below);
    //   - pose keys held (I/J/K/L, W/A/S/D, the arrows): the keys;
    //   - nothing held: both arms level, unless the controller has a real motion sensor (a Wii
    //     Remote, or a gyro pad), whose motion then moves the arms as on the Wii.
    bool resting = !pad.connected || (std::hypot(pad.left_x, pad.left_y) < kDeadZone && std::hypot(pad.right_x, pad.right_y) < kDeadZone);
    if (resting) {
        if ((sample.buttons & kPoseBits) || !pad.motion_valid) {
            sample.buttons |= input::kPointInstant;
        }
        return;
    }
    sample.buttons |= input::kPointInstant;
    float half = slice_degrees() * 0.5f;
    sample.buttons &= ~kPoseBits;
    static const bool debug = std::getenv("WP_DUAL_STICK_DEBUG") != nullptr;
    if (debug) {
        static int last = -1;
        int key = static_cast<int>(arm_of(pad.right_x, pad.right_y, 1.0f, half)) * 3 + static_cast<int>(arm_of(pad.left_x, pad.left_y, -1.0f, half)) + 9 * static_cast<int>(half);
        if (key != last) {
            last = key;
            static const char* names[] = {"level", "up", "down"};
            std::fprintf(stderr, "dual stick: slice %.0f°, right arm %s, left arm %s\n", half * 2, names[static_cast<int>(arm_of(pad.right_x, pad.right_y, 1.0f, half))],
                         names[static_cast<int>(arm_of(pad.left_x, pad.left_y, -1.0f, half))]);
        }
    }
    switch (arm_of(pad.right_x, pad.right_y, 1.0f, half)) {
    case Arm::Up: sample.buttons |= input::kRemotePointUp; break;
    case Arm::Down: sample.buttons |= input::kRemotePointDown; break;
    case Arm::Level: break;
    }
    switch (arm_of(pad.left_x, pad.left_y, -1.0f, half)) {
    case Arm::Up: sample.buttons |= input::kNunchukPointUp; break;
    case Arm::Down: sample.buttons |= input::kNunchukPointDown; break;
    case Arm::Level: break;
    }
}

}  // namespace

bool dual_stick_poses() {
    static const bool on = wp::features::enabled("dual_stick_poses");
    return on;
}

float pose_slice_degrees() {
    return slice_degrees();
}

bool instant_poses() {
    return (input::last_sample(0).buttons & input::kPointInstant) != 0;
}

void stick_arms(int& remote, int& nunchuk) {
    uint32_t buttons = input::last_sample(0).buttons;
    remote = (buttons & input::kRemotePointUp) ? 1 : (buttons & input::kRemotePointDown) ? -1 : 0;
    nunchuk = (buttons & input::kNunchukPointUp) ? 1 : (buttons & input::kNunchukPointDown) ? -1 : 0;
}

namespace {

struct Install {
    Install() { input::add_transform(&transform); }
} g_install;

}  // namespace

}  // namespace musclemarch::features
