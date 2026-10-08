#pragma once
// Reading the current race from host code (test tools, optional features): plain host reads of
// game memory, never a call into the game, so it can't change what the game does.
// See docs/GAME.md and race/course.cpp for the layout.

#include <cstdint>

#include "wp/lift.h"
#include "wp/memory.h"

namespace musclemarch::course_query {

constexpr uint32_t kCurrentRace = 0x804223D0;  // the race object while racing
constexpr uint32_t kPhase = 0x4;               // the race scene's phase: 6 = racing
constexpr uint32_t kState = 0x38;               // 0 countdown, 1 racing, 2 a rival's crash, 3+ the end
constexpr uint32_t kMode = 0x198;              // 1, 2: shake modes
constexpr uint32_t kCrashState = 0x4F64;
constexpr uint32_t kIntroRunning = 0x53444;
constexpr uint32_t kCourse = 0x59CF0;
constexpr uint32_t kPlayerDistance = 0x5E0;
constexpr uint32_t kWalls = 0x5A8F4;           // per segment: 2 = any pose, 3..6 = pose 0..3

inline bool valid(uint32_t address) {
    return (address >= 0x80000000 && address < 0x81800000) || (address >= 0x90000000 && address < 0x94000000);
}

// The race, or 0 when there is none.
inline uint32_t race() {
    uint32_t race = wp::rd32(kCurrentRace);
    return valid(race) && valid(race + kWalls + 0x400) ? race : 0;
}

// The course segment at a distance, walking the course table the way fn_8003BA48 does, in plain
// host floats.
inline int segment_at(uint32_t course, float distance) {
    int32_t loop_end = static_cast<int32_t>(wp::rd32(course + 0x11C0));
    float loop_begin = wp::lift::ldf(course + 0xA2E0);
    float start = 0.0f;
    if (distance > static_cast<float>(loop_end)) {
        float span = static_cast<float>(loop_end) - loop_begin;
        int lap = span > 0.0f ? static_cast<int>(1.0f + (distance - static_cast<float>(loop_end)) / span) : 0;
        start = span * static_cast<float>(lap - 1) + static_cast<float>(loop_end) - loop_begin;
    }
    int count = static_cast<int32_t>(wp::rd32(course + 0x11A8));
    for (int segment = 0; segment <= count && segment < 512; segment++) {
        int32_t type = static_cast<int32_t>(wp::rd32(course + 0xED4 + 4 * static_cast<uint32_t>(segment)));
        float length = wp::lift::ldf(course + 0x21D4 + 4 * static_cast<uint32_t>(type));
        if (distance <= start + length) {
            return segment;
        }
        start += length;
    }
    return count;
}

// The pose (0..3, docs/GAME.md) the next pose wall asks for, looking `ahead` segments past the
// one the wall check looks at; -1 if none ("any" walls are skipped).
inline int next_pose(uint32_t race, int ahead) {
    float distance = wp::lift::ldf(race + kPlayerDistance);
    if (!(distance >= 0.0f && distance < 1.0e6f)) {
        return -1;
    }
    int segment = segment_at(race + kCourse, distance - 1.0f);
    for (int i = 0; i < ahead; i++) {
        int32_t wall = static_cast<int32_t>(wp::rd32(race + kWalls + 4 * static_cast<uint32_t>(segment + i)));
        if (wall >= 3 && wall <= 6) {
            return wall - 3;
        }
    }
    return -1;
}

}  // namespace musclemarch::course_query
