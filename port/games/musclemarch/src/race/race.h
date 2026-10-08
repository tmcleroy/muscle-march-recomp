#pragma once
// The race object and its runners (docs/GAME.md), shared by the lifted race code, and helpers
// for lifted code that keeps guest registers as the original does.

#include <cstdint>
#include <initializer_list>

#include "wp/lift.h"

namespace musclemarch {

constexpr uint32_t kRunnerStride = 0x49B0;

struct Race : wp::lift::View {
    // Fields not named yet, by offset.
    wp::lift::Field<int32_t> at(uint32_t offset) const { return wp::lift::Field<int32_t>(base + offset); }
    wp::lift::Field<float> f_at(uint32_t offset) const { return wp::lift::Field<float>(base + offset); }
    WP_FIELD(uint32_t, state, 0x038)
    WP_FIELD(int32_t, endless, 0x054)               // 1: walls are dealt as they are passed
    wp::lift::Field<int32_t> flag_74(uint32_t i) const { return wp::lift::Field<int32_t>(base + 0x74 + 4 * i); }
    WP_FIELD(int32_t, player_runner, 0x168)
    WP_FIELD(float, speed_setting, 0x170)
    WP_FIELD(float, field_17c, 0x17C)
    WP_FIELD(int32_t, lives, 0x194)
    WP_FIELD(int32_t, mode, 0x198)                  // 1, 2: shake modes
    WP_FIELD(int32_t, field_1a0, 0x1A0)
    WP_FIELD(int32_t, difficulty, 0x1C0)
    WP_FIELD(int32_t, field_1e0, 0x1E0)
    WP_FIELD(int32_t, field_408, 0x408)
    WP_FIELD(int32_t, combo, 0x40C)                 // walls passed in a row
    WP_FIELD(int32_t, field_410, 0x410)
    WP_FIELD(int32_t, rival_crashes, 0x418)
    WP_FIELD(int32_t, crashes, 0x41C)
    WP_FIELD(int32_t, finish_distance, 0x420)
    WP_FIELD(int32_t, field_430, 0x430)
    WP_FIELD(int32_t, field_434, 0x434)
    WP_FIELD(int32_t, gauge_streak, 0x440)          // frames with a gauge past its maximum
    WP_FIELD(int32_t, hits, 0x464)
    WP_FIELD(int32_t, special_finishes, 0x468)
    wp::lift::Field<int32_t> feint_start(int32_t difficulty) const { return wp::lift::Field<int32_t>(base + 0x4A0 + 4 * static_cast<uint32_t>(difficulty)); }
    wp::lift::Field<int32_t> crash_combo_max(int32_t difficulty) const { return wp::lift::Field<int32_t>(base + 0x520 + 4 * static_cast<uint32_t>(difficulty)); }
    WP_FIELD(float, shake_power, 0x5D4)
    uint32_t runner(uint32_t i) const { return base + 0x5E0 + kRunnerStride * i; }
    WP_FIELD(int32_t, crash_state, 0x4F64)
    WP_FIELD(int32_t, field_4f88, 0x4F88)
    WP_FIELD(float, special_distance, 0x12CA0)       // = runner(4).distance(): the special runner
    WP_FIELD(int32_t, special_ready, 0x17624)
    WP_FIELD(int32_t, target_runner, 0x53440)        // 0..3, or 4 = the special runner
    WP_FIELD(int32_t, intro_running, 0x53444)
    WP_FIELD(float, effect_rate, 0x57C70)
    WP_FIELD(float, effect_scale, 0x57C74)
    WP_FIELD(int32_t, special_kind, 0x59CD4)
    WP_FIELD(int32_t, special_score, 0x59C80)
    uint32_t course() const { return base + 0x59CF0; }
    wp::lift::Field<int32_t> wall(int32_t segment) const { return wp::lift::Field<int32_t>(base + 0x5A8F4 + 4 * static_cast<uint32_t>(segment)); }
    // Per segment: the pose animation a runner shows at the wall (0..3), whether the special
    // runner dashes at it, whether the feinting runner dashes at it.
    wp::lift::Field<int32_t> wall_kind(int32_t segment) const { return wp::lift::Field<int32_t>(base + 0x5A804 + 4 * static_cast<uint32_t>(segment)); }
    wp::lift::Field<int32_t> special_dash(int32_t segment) const { return wp::lift::Field<int32_t>(base + 0x5A9E4 + 4 * static_cast<uint32_t>(segment)); }
    wp::lift::Field<int32_t> dash(int32_t segment) const { return wp::lift::Field<int32_t>(base + 0x5AAD4 + 4 * static_cast<uint32_t>(segment)); }
    WP_FIELD(int32_t, course_segments, 0x5AE98)
    wp::lift::Field<float> pose_meter(uint32_t i) const { return wp::lift::Field<float>(base + 0x6D890 + 4 * i); }
    wp::lift::Field<float> gauge_frame(uint32_t i) const { return wp::lift::Field<float>(base + 0x6D8A0 + 4 * i); }
    wp::lift::Field<float> gauge_max(uint32_t i) const { return wp::lift::Field<float>(base + 0x6D8B0 + 4 * i); }
    wp::lift::Field<int32_t> gauge_cheered(uint32_t i) const { return wp::lift::Field<int32_t>(base + 0x6D8C0 + 4 * i); }
    WP_FIELD(float, crash_effect, 0x8854C)
    WP_FIELD(float, crash_effect_time, 0x88550)
    WP_FIELD(uint8_t, posing, 0x88284)
    WP_FIELD(int32_t, pose_pop_time, 0x88280)
    WP_FIELD(int32_t, feinting, 0x882D4)
    WP_FIELD(int32_t, feint_offset, 0x882D8)
    WP_FIELD(int32_t, rival_crash_shown, 0x8A0F4)
    WP_FIELD(int32_t, field_8a0f8, 0x8A0F8)
    WP_FIELD(int32_t, debug_print, 0x8A114)
    // Speed and dash zones (race_speed.cpp).
    WP_FIELD(int32_t, scene_request, 0x004)
    WP_FIELD(float, celebration_time, 0x188)
    WP_FIELD(int32_t, chase_state, 0x1A4)            // the special runner's chase: 0 wait, 1 speeding up, 2 done
    WP_FIELD(int32_t, chase_count, 0x1A8)
    WP_FIELD(int32_t, dash_exchanges, 0x1B0)
    WP_FIELD(int32_t, dash_zones, 0x1B4)
    WP_FIELD(int32_t, countdown, 0x1B8)
    WP_FIELD(float, dash_timer, 0x1C8)
    WP_FIELD(float, dash_speed, 0x1CC)
    WP_FIELD(int32_t, table_value, 0x214)
    WP_FIELD(int32_t, field_444, 0x444)
    // Tuning parameters (docs/GAME.md): one per difficulty from these offsets.
    wp::lift::Field<float> param(uint32_t offset, int32_t difficulty) const { return wp::lift::Field<float>(base + offset + 4 * static_cast<uint32_t>(difficulty)); }
    wp::lift::Field<int32_t> int_param(uint32_t offset, int32_t difficulty) const { return wp::lift::Field<int32_t>(base + offset + 4 * static_cast<uint32_t>(difficulty)); }
    WP_FIELD(float, crash_down_speed, 0x49C)
    WP_FIELD(float, player_follow_speed_up, 0x51C)
    WP_FIELD(float, special_speed_max, 0x52C)
    WP_FIELD(float, ooo_enemy_speed_up_rate, 0x5C8)
    WP_FIELD(float, ooo_enemy_speed_up_count, 0x5CC)
    WP_FIELD(float, middle_length_map_speed_up, 0x5D0)
    WP_FIELD(float, special_effect, 0x59C88)
    wp::lift::Field<int32_t> in_zone(uint32_t runner) const { return wp::lift::Field<int32_t>(base + 0x59CB0 + 4 * runner); }
    wp::lift::Field<int32_t> zone_table(int32_t segment) const { return wp::lift::Field<int32_t>(base + 0x5ABC4 + 4 * static_cast<uint32_t>(segment)); }
    wp::lift::Field<uint32_t> zone_list(int32_t table) const { return wp::lift::Field<uint32_t>(base + 0x59CF8 + 8 * static_cast<uint32_t>(table)); }
    wp::lift::Field<float> cheer_time(uint32_t runner) const { return wp::lift::Field<float>(base + 0x88340 + 4 * runner); }
    wp::lift::Field<int32_t> cheered(uint32_t runner) const { return wp::lift::Field<int32_t>(base + 0x88350 + 4 * runner); }
    WP_FIELD(float, crash_effect_scale, 0x88554)
};

struct Runner : wp::lift::View {
    WP_FIELD(float, distance, 0x0)
    WP_FIELD(float, speed, 0x4)
    WP_FIELD(int32_t, marker, 0x24)
    WP_FIELD(int32_t, animation, 0x4984)
    WP_FIELD(float, animation_speed, 0x4998)
    WP_FIELD(int32_t, field_49a8, 0x49A8)
};

// lfs (and single-precision results): both paired-single halves.
inline void set_single(wp::Cpu& c, int reg, float value) {
    c.f[reg] = value;
    c.ps1[reg] = value;
}

// A guest call from the bl at `site`.
inline uint32_t call(wp::Cpu& c, uint32_t site, uint32_t function, std::initializer_list<uint32_t> ints = {}, std::initializer_list<double> floats = {}) {
    return wp::lift::gcall_from(c, site + 4, function, ints, floats);
}

inline float call_float(wp::Cpu& c, uint32_t site, uint32_t function, std::initializer_list<uint32_t> ints = {}, std::initializer_list<double> floats = {}) {
    call(c, site, function, ints, floats);
    return static_cast<float>(c.f[1]);
}

}  // namespace musclemarch
