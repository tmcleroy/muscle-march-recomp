// Dealing walls: the special runner sets each wall's pose as it passes.
//
// Lifted from fn_8002FBDC (a per-frame race system run after the race update). The special runner
// (runner 4, the one being chased) runs ahead of the player; when its step enters a new segment
// whose wall is still undecided (1), the wall gets its required pose: kind + 3 (race +0x5A804,
// the pose the runners show; docs/GAME.md), or "any" (2) while the player is in one of the
// animations 0xC, 0xE, 0x10, 0x12. A sound plays.
// Then the course objects (zone list types 3, 4, 5, 0x12) between the camera's segment and the
// special runner's are switched on once the special runner is past them (fn_8003C980 per
// segment); when the chase target becomes the special runner, its course part is prepared; and
// each new camera segment counts toward the dash exchanges (+0x1B0).
// Guest registers r20-r31/f31 hold the original's values at every call (see race.h).

#include "race/race.h"

namespace musclemarch {

namespace {

using namespace wp;
using namespace wp::lift;
using namespace wp::lift::ppc;

const lift::Group kLifted{"game.lifted", "WP_LIFTED"};

constexpr uint32_t kSegmentAt = 0x8003BA48;
constexpr uint32_t kCrossesSegment = 0x8003C280;
constexpr uint32_t kCourseDistanceOf = 0x8003BEC4;  // (course, segment) -> f1 start
constexpr uint32_t kSegmentObjects = 0x8003C980;    // (course, segment)
constexpr uint32_t kPrepareChase = 0x8003A6C0;      // (course, segment)
constexpr uint32_t kZoneKind = 0x80039CD4;          // (course, 0, segment) -> 1: a dash zone
constexpr uint32_t kZoneStart = 0x80039D18;         // (course, 0, segment) -> 1: its first segment
constexpr uint32_t kPlaySound = 0x8003652C;
constexpr uint32_t kLastTarget = 0x804223E8;        // global
constexpr uint32_t kLastCameraSegment = 0x804223EC; // global

constexpr int32_t kUndecided = 1;
constexpr int32_t kAnyPose = 2;

bool special_object(int32_t type) {
    return type == 3 || type == 4 || type == 0x12 || type == 5;
}

// (Race*)
WP_LIFTED(kLifted, deal_walls, "Race_DealWalls", Ret::None) {
    Race race;
    race.base = c.r[3];
    OriginalFrame frame(c, 0x50);
    frame.save_fpr(31, 0x40, 0x48);
    for (int reg = 20; reg <= 31; reg++) {  // _savegpr_20
        frame.save_gpr(reg, 0x10 + 4 * static_cast<uint32_t>(reg - 20));
    }

    c.r[26] = race.base + 0x10000;
    c.r[22] = race.base;
    set_single(c, 31, race.special_distance());
    c.r[29] = call(c, 0x8002FC10, kSegmentAt, {race.course()}, {c.f[31]});
    int32_t special_segment = static_cast<int32_t>(c.r[29]);
    if (call(c, 0x8002FC28, kCrossesSegment, {race.course()}, {c.f[31], 2.0f}) != 0 && race.wall(special_segment) == kUndecided) {
        int32_t animation = race.crash_state();
        if (animation == 0xC || animation == 0xE || animation == 0x10 || animation == 0x12) {
            race.wall(special_segment) = kAnyPose;
        } else {
            race.wall(special_segment) = race.wall_kind(special_segment) + 3;
        }
        call(c, 0x8002FC94, kPlaySound, {race.base, 1, 3});
    }

    // Course objects from the camera's segment to the special runner's.
    c.r[25] = call(c, 0x8002FCA4, kSegmentAt, {race.course()}, {race.f_at(0x178).get()});
    int32_t camera_segment = static_cast<int32_t>(c.r[25]);
    c.r[24] = c.r[25];
    c.r[30] = race.base + 0x60000;
    c.r[28] = race.base + 4 * c.r[25];
    c.r[20] = 1;
    for (; static_cast<int32_t>(c.r[24]) <= static_cast<int32_t>(c.r[29]); c.r[28] += 4, c.r[24]++) {
        int32_t segment = static_cast<int32_t>(c.r[24]);
        c.r[23] = 0;
        c.r[21] = 0;
        c.r[27] = 0;
        c.r[31] = c.r[30] + 8 * static_cast<uint32_t>(race.zone_table(segment).get());
        for (; static_cast<int32_t>(c.r[23]) < static_cast<int32_t>(rd32(rd32(c.r[31] - 0x6308) + 0x2C));
             c.r[27] += 8, c.r[23]++, c.r[21] += 0x80) {
            uint32_t list = rd32(c.r[31] - 0x6308);
            uint32_t entry = list + rd32(list + 0x30) + c.r[21];
            if (!special_object(static_cast<int32_t>(rd32(entry)))) {
                continue;
            }
            // How far the special runner is into its segment.
            float into;
            if (c.r[29] == 0) {
                into = race.special_distance();
            } else {
                set_single(c, 31, race.special_distance());
                float start = call_float(c, 0x8002FDC4, kCourseDistanceOf, {race.course(), c.r[29] - 1});
                into = fsubs(static_cast<float>(c.f[31]), start);
            }
            list = rd32(c.r[31] - 0x6308);
            entry = list + rd32(list + 0x30) + c.r[21];
            float at = ldf(entry + 0x10);
            if (at < into || at == into) {
                uint32_t flags = rd32(c.r[28] + 0x60000 - 0x4240);
                wr32(flags + c.r[27] + 4, c.r[20]);
            }
        }
        call(c, 0x8002FE40, kSegmentObjects, {race.course(), c.r[24]});
    }

    // The chase target just became the special runner.
    int32_t target = race.target_runner();
    if (target == 4 && static_cast<int32_t>(rd32(kLastTarget)) != target) {
        wr32(race.base + 0x63FCC, 1);
        if (race.endless() == 0) {
            call(c, 0x8002FE90, kPrepareChase, {race.course(), static_cast<uint32_t>(special_segment + 1)});
        }
    }
    int32_t last_camera_segment = static_cast<int32_t>(rd32(kLastCameraSegment));
    wr32(kLastTarget, static_cast<uint32_t>(race.target_runner()));
    if (last_camera_segment != camera_segment) {
        if (call(c, 0x8002FEBC, kZoneKind, {race.course(), 0, c.r[25]}) == 1) {
            if (call(c, 0x8002FED8, kZoneStart, {race.course(), 0, c.r[25]}) == 1) {
                race.dash_exchanges() = race.dash_exchanges() + 1;
            }
        } else if (camera_segment + 1 == race.course_segments()) {
            race.dash_exchanges() = race.dash_exchanges() + 1;
        }
    }

    frame.restore_fpr(31, 0x40, 0x48);
    wr32(kLastCameraSegment, c.r[25]);
    for (int reg = 20; reg <= 31; reg++) {  // _restgpr_20
        frame.restore_gpr(reg, 0x10 + 4 * static_cast<uint32_t>(reg - 20));
    }
}

}  // namespace

}  // namespace musclemarch
