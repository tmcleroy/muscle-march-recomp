// Stick shaking: with a gamepad, run by wiggling both sticks.
//
// The race measures shaking from how much the controllers' tilt changes frame to frame
// (input/shake.cpp, race/shake_power.cpp). With a gamepad the port normally shakes both
// controllers while the left trigger is held. This feature replaces that with wiggling: both
// controllers shake while both sticks keep changing direction (back and forth, or round and
// round). A stick counts a reversal when it turns back by at least kTurn on either axis; it is
// wiggling while it has made kReversals of them in the last kWindow seconds. Striking or letting
// go of a pose is a single movement, so it never shakes (which would wobble the arms through
// other poses). The left trigger no longer shakes. Keyboard shaking is unchanged.

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>

#include "game_features.h"
#include "wp/features.h"
#include "wp/input.h"

WP_FEATURE(stick_shaking, {"stick_shaking", "Shake by wiggling the sticks",
                           "With a gamepad, run by wiggling both sticks back and forth (or round and round) instead of holding the left "
                           "trigger.",
                           "controls", wp::features::kEnhanced});

namespace musclemarch::features {

namespace {

using namespace wp;
using Clock = std::chrono::steady_clock;

constexpr float kTurn = 0.4f;        // how far a stick must come back to count a reversal
constexpr double kWindow = 0.5;      // seconds
constexpr size_t kReversals = 2;     // reversals in the window, on each stick

constexpr uint32_t kShakeBits = input::kMotionShake | input::kNunchukShake;
constexpr float kTriggerShake = 0.5f;  // the gamepad mapping's left trigger threshold

// One axis: which way it's going and the farthest point since it last turned.
struct Axis {
    int direction = 0;  // +1, -1, or 0 before it has moved
    float extreme = 0.0f;

    // True when the axis turns back.
    bool update(float value) {
        if (direction == 0) {
            if (value - extreme >= kTurn) {
                direction = 1;
                extreme = value;
            } else if (extreme - value >= kTurn) {
                direction = -1;
                extreme = value;
            }
            return false;
        }
        if ((value - extreme) * static_cast<float>(direction) > 0.0f) {
            extreme = value;  // still going the same way
            return false;
        }
        if ((extreme - value) * static_cast<float>(direction) >= kTurn) {
            direction = -direction;
            extreme = value;
            return true;
        }
        return false;
    }
};

struct Stick {
    Axis x;
    Axis y;
    std::deque<Clock::time_point> reversals;

    void update(float new_x, float new_y, Clock::time_point now) {
        bool turned_x = x.update(new_x);
        bool turned_y = y.update(new_y);
        if (turned_x || turned_y) {
            reversals.push_back(now);
        }
        while (!reversals.empty() && std::chrono::duration<double>(now - reversals.front()).count() > kWindow) {
            reversals.pop_front();
        }
    }
    bool wiggling() const { return reversals.size() >= kReversals; }
};

struct Channel {
    Stick left;
    Stick right;
    bool shaking = false;
};

Channel g_channels[4];

void transform(uint32_t channel, input::Sample& sample) {
    static const bool on = wp::features::enabled("stick_shaking");
    if (!on || channel >= 4) {
        return;
    }
    const gamepad::State& pad = input::last_gamepad(channel);
    Channel& state = g_channels[channel];
    if (!pad.connected) {
        state = Channel{};
        return;
    }
    Clock::time_point now = Clock::now();
    state.left.update(pad.left_x, pad.left_y, now);
    state.right.update(pad.right_x, pad.right_y, now);

    bool was = state.shaking;
    state.shaking = state.left.wiggling() && state.right.wiggling();
    static const bool debug = std::getenv("WP_STICK_SHAKE_DEBUG") != nullptr;
    if (debug && state.shaking != was) {
        std::fprintf(stderr, "stick shaking: %s (left %zu, right %zu reversals)\n", state.shaking ? "on" : "off", state.left.reversals.size(),
                     state.right.reversals.size());
    }

    if (pad.left_trigger > kTriggerShake) {
        sample.buttons &= ~kShakeBits;  // the trigger's shake (keyboard shaking stays)
    }
    if (state.shaking) {
        sample.buttons |= kShakeBits;
    }
}

}  // namespace

bool stick_shaking() {
    static const bool on = wp::features::enabled("stick_shaking");
    return on;
}

namespace {

struct Install {
    Install() { input::add_transform(&transform); }
} g_install;

}  // namespace

}  // namespace musclemarch::features
