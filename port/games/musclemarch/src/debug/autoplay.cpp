// Autoplay (WP_AUTOPLAY=1): a test tool, not a feature. During a race it holds the pose the
// next wall asks for and shakes both controllers, so scripted runs (tools/smoke.py) play well
// and reach the parts of the race code a random player never does: combos, rival crashes,
// the special runner, the course end. Record such a run with WP_RECORD to get a movie for
// the golden tests; replays never consult it.
//
// It reads the race through the game's current-race pointer and walks the course table the way
// fn_8003BA48 does (see docs/GAME.md, race/course.cpp), in plain host floats.

#include <cstdio>
#include <cstdlib>

#include "race/course_query.h"
#include "wp/input.h"
#include "wp/lift.h"

namespace musclemarch {

namespace {

using namespace wp;
using namespace course_query;

// Buttons for pose 0..3 (docs/GAME.md: 0 both up, 1 both down, 2 Remote up + Nunchuk down,
// 3 Remote down + Nunchuk up).
constexpr uint32_t kPoseButtons[4] = {
    input::kRemotePointUp | input::kNunchukPointUp,
    input::kRemotePointDown | input::kNunchukPointDown,
    input::kRemotePointUp | input::kNunchukPointDown,
    input::kRemotePointDown | input::kNunchukPointUp,
};

void autoplay(uint32_t channel, input::Sample& sample) {
    if (channel != 0) {
        return;
    }
    uint32_t race = rd32(kCurrentRace);
    if (!valid(race) || !valid(race + kWalls + 0x400)) {
        return;
    }
    uint32_t course = race + kCourse;
    float distance = lift::ldf(race + kPlayerDistance);
    if (!(distance >= 0.0f && distance < 1.0e6f)) {
        return;
    }
    // The wall at the end of the segment the wall check looks at (distance - 1); "any" (2) walls
    // let the bot get ready for the one after.
    int segment = segment_at(course, distance - 1.0f);
    static const bool debug = std::getenv("WP_AUTOPLAY") && std::atoi(std::getenv("WP_AUTOPLAY")) == 2;
    static uint32_t last_hash = 0;
    if (debug) {
        uint32_t hash = static_cast<uint32_t>(segment);
        for (uint32_t i = 0; i < 10; i++) {
            hash = hash * 31 + rd32(race + kWalls + 4 * i);
        }
        if (hash != last_hash) {
            last_hash = hash;
            std::fprintf(stderr, "autoplay: distance %.1f segment %d walls", distance, segment);
            for (uint32_t i = 0; i < 10; i++) {
                std::fprintf(stderr, " %d", static_cast<int32_t>(rd32(race + kWalls + 4 * i)));
            }
            std::fprintf(stderr, "\n");
        }
    }
    bool posing = false;
    for (int ahead = 0; ahead < 3; ahead++) {
        int32_t wall = static_cast<int32_t>(rd32(race + kWalls + 4 * static_cast<uint32_t>(segment + ahead)));
        if (wall >= 3 && wall <= 6) {
            sample.buttons |= kPoseButtons[wall - 3];
            static const bool instant = std::getenv("WP_AUTOPLAY_INSTANT") != nullptr;  // pose like the sticks do
            if (instant) {
                sample.buttons |= input::kPointInstant;
            }
            posing = true;
            break;
        }
    }
    // Shaking replaces the accelerometer readings, so it would undo the pose: shake only between
    // walls, or in the shake modes (race +0x198 = 1, 2).
    int32_t mode = static_cast<int32_t>(rd32(race + 0x198));
    if (!posing || mode == 1 || mode == 2) {
        sample.buttons |= input::kMotionShake | input::kNunchukShake;
    }
}

struct Install {
    Install() {
        if (std::getenv("WP_AUTOPLAY")) {
            input::add_transform(&autoplay);
        }
    }
} g_install;

}  // namespace

}  // namespace musclemarch
