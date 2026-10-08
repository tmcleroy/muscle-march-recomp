// The race update: everything that happens to the runners each frame of a race.
//
// Lifted from fn_80030EB8 (called every race frame from fn_8002C544). In order:
//   1. move the runners (fn_8003F6D4/F720/F304) and update speeds (fn_80032420);
//   2. course end: once the player is in the last segment far past its mark, or the special
//      runner has reached the course end, the race state becomes 3 (the special runner is put 5
//      ahead of the player);
//      with no lives left it becomes 4 (game over);
//   3. effects: in shake modes (race mode 1/2) the shake power drives the running effect;
//      otherwise the four pose meters drive the pose effects and the pose-change pop;
//   4. the wall check: the wall just ahead of the player (distance - 1) asks for a pose; when the
//      player reaches it with the wrong pose, the player crashes through it and loses a life;
//      in the endless mode (+0x54 == 1) new walls are dealt as walls are passed;
//   5. gauge sounds: a cheer when a pose gauge fills past 45 x its maximum;
//   6. combo crash: after CRASH_COMBO_COUNT_MAX walls in a row, the target runner crashes;
//   7. the runners loop (5 runners, 0x49B0 bytes each from race +0x5E0): each one's animation
//      for the wall ahead (pose 1..4, dash 0x16..0x19), the special runner's feint and the
//      end-of-course marker.
// Unknown fields are named by offset. Guest registers r23-r31/f19-f31 hold the same values as in
// the original at every call (callees save them on the stack, and the game reads stale stack).

#include "input/player_input.h"
#include "race/race.h"
#include "wp/lift.h"
#include "wp/threads.h"

namespace musclemarch {

namespace {

using namespace wp;
using namespace wp::lift;
using namespace wp::lift::ppc;

const lift::Group kLifted{"game.lifted", "WP_LIFTED"};

// Guest functions.
constexpr uint32_t kRunnerMove = 0x8003F6D4;          // (runner)
constexpr uint32_t kRunnerMove2 = 0x8003F720;         // (runner)
constexpr uint32_t kRunnerAdvance = 0x8003F304;       // (runner, f1 speed setting)
constexpr uint32_t kUpdateSpeed = 0x80032420;         // (race)
constexpr uint32_t kSegmentAt = 0x8003BA48;           // course.cpp
constexpr uint32_t kCrossesSegment = 0x8003C280;      // course.cpp
constexpr uint32_t kCourseIndexAt = 0x8003C008;       // (course, f1 distance) -> index
constexpr uint32_t kCourseDistanceOf = 0x8003BEC4;    // (course, index) -> f1
constexpr uint32_t kCourseEnd = 0x8003B614;           // (course) -> f1 loop end
constexpr uint32_t kSegmentTouch = 0x8003B640;        // (course, segment)
constexpr uint32_t kWallStart = 0x8003BD7C;           // (course, f1 distance, segment) -> f1
constexpr uint32_t kDealWall = 0x8003B470;            // (course, segment, 1)
constexpr uint32_t kSetBlend = 0x800402C0;            // (runner, id, 2, f1, f2)
constexpr uint32_t kSetAnimation = 0x800408D4;        // (runner, animation)
constexpr uint32_t kAnimationTime = 0x8003F6BC;       // (runner, index, 0) -> f1
constexpr uint32_t kRunnerCrashed = 0x8003F478;       // (runner)
constexpr uint32_t kRunnersEnd = 0x800401D0;          // (runner)
constexpr uint32_t kPlaySound = 0x8003652C;           // (race, bank, sound)
constexpr uint32_t kVoiceBank = 0x80036660;           // (race) -> bank
constexpr uint32_t kCheerVoice = 0x8003669C;          // (race) -> sound
constexpr uint32_t kCrashVoice = 0x800366A4;          // (race) -> sound
constexpr uint32_t kShakeCamera = 0x8002FB94;         // (race, 3)
constexpr uint32_t kSoundCommand = 0x8014DD14;        // (manager, 3)
constexpr uint32_t kRand = 0x802160C4;
constexpr uint32_t kFpClassify = 0x80155570;          // (f1) -> 2 or less: zero/subnormal/inf/NaN
constexpr uint32_t kDebugPrint = 0x800323B8;          // (text)

constexpr uint32_t kSoundManager = 0x80422568;        // global pointer
constexpr uint32_t kLastSegment = 0x804223F0;         // global: the player's segment last frame
constexpr uint32_t kFeintRunner = 0x8042119C;         // global: the runner that feints next
constexpr uint32_t kDebugFormats = 0x8026FFEC;        // 4 formats, 0x1C apart

constexpr float kRandMax = 2147483600.0f;
constexpr uint32_t kNoTarget = 4;

constexpr int32_t kStateCourseEnd = 3;
constexpr int32_t kStateGameOver = 4;
constexpr int32_t kStateRivalCrashed = 2;
constexpr int32_t kStateSpecialFinish = 6;

constexpr uint32_t kCrashAnimation = 0xB;
constexpr uint32_t kGaugeIds[4] = {0x2B, 0x27, 0x29, 0x28};

// Wall requirements (course wall table): 2 = any pose, 3..6 = pose 0..3.
constexpr int32_t kWallAny = 2;
constexpr int32_t kWallBroken = 2;
constexpr int32_t kWallDealt = 1;



// The player's input object, through the game object (both leaf getters).
PlayerInput input_at(Cpu& c, uint32_t game_site, uint32_t input_site) {
    uint32_t game = call(c, game_site, kGetGame);
    PlayerInput input;
    input.base = call(c, input_site, kGetPlayerInput, {game, 0});
    return input;
}

bool crosses(Cpu& c, uint32_t site, Race race, float distance, float step) {
    return call(c, site, kCrossesSegment, {race.course()}, {distance, step}) != 0;
}

// rand() scaled to [0, scale): int(scale * rand / RAND_MAX) through the frame's slots.
int32_t random_below(Cpu& c, uint32_t site, OriginalFrame& frame, uint32_t slot, float scale) {
    int32_t r = static_cast<int32_t>(call(c, site, kRand));
    float value = fmuls(scale, frame.int_to_float(slot, r));
    return frame.float_to_int(0x130, fdivs(value, kRandMax));
}

// Animation state (runner +0x4984, copied to the frame at 0x8) allows a wall reaction: idle or
// states 5, 6.
bool can_react(int32_t state) {
    return state == 0 || static_cast<uint32_t>(state - 5) <= 1;
}

// The pose a runner shows for the wall ahead (wall kind 0..3 -> animation base + 0..3).
void wall_animation(Cpu& c, uint32_t site_base, Runner runner, int32_t kind, uint32_t first_animation) {
    if (kind >= 0 && kind <= 3) {
        call(c, site_base + 0x10 * static_cast<uint32_t>(kind), kSetAnimation, {runner.base, first_animation + static_cast<uint32_t>(kind)});
    }
}

// The animation speed for reaching the wall: 45 / (distance to the wall start / speed), or 45 when
// that is not a normal number. `clamp` keeps the distance from going negative (one copy does).
float reach_speed(Cpu& c, uint32_t wall_site, uint32_t classify_site, Race race, Runner runner, int32_t segment, bool clamp) {
    set_single(c, 19, runner.speed());
    float wall = call_float(c, wall_site, kWallStart, {race.course(), static_cast<uint32_t>(segment)}, {c.f[22]});
    float speed = static_cast<float>(c.f[19]);
    float to_wall;
    if (clamp) {
        to_wall = fsubs(wall, static_cast<float>(c.f[23]));
        if (!(to_wall >= static_cast<float>(c.f[24]))) {
            to_wall = static_cast<float>(c.f[24]);
        }
        if (!(speed >= static_cast<float>(c.f[25]))) {
            set_single(c, 19, 0.2f);
        }
    } else {
        if (!(speed >= static_cast<float>(c.f[25]))) {
            set_single(c, 19, 0.2f);
        }
        to_wall = fsubs(wall, static_cast<float>(c.f[23]));
    }
    float time = fdivs(to_wall, static_cast<float>(c.f[19]));
    set_single(c, 19, fdivs(static_cast<float>(c.f[26]), time));
    if (static_cast<int32_t>(call(c, classify_site, kFpClassify, {}, {c.f[19]})) <= 2) {
        set_single(c, 19, 45.0f);
    }
    return static_cast<float>(c.f[19]);
}

// The target runner is told to feint at the wall ahead (random offset) and the special state set.
void start_feint(Cpu& c, uint32_t animation_site, uint32_t rand_site, uint32_t sound_site, OriginalFrame& frame, uint32_t slot, Race race) {
    call(c, animation_site, kSetAnimation, {race.runner(static_cast<uint32_t>(race.target_runner())), 0x27});
    race.state() = c.r[29];
    race.feinting() = static_cast<int32_t>(c.r[30]);
    int32_t r = static_cast<int32_t>(call(c, rand_site, kRand));
    float value = frame.int_to_float(slot, r);  // lfd - f28 (f28 holds the magic)
    race.field_17c() = static_cast<float>(c.f[31]);
    race.state() = c.r[29];
    int32_t offset = frame.float_to_int(0x130, fdivs(fmuls(static_cast<float>(c.f[29]), value), static_cast<float>(c.f[30])));
    race.feint_offset() = offset * 0x300 - 0x600;
    call(c, sound_site, kPlaySound, {race.base, 1, 0x46});
    race.rival_crash_shown() = static_cast<int32_t>(c.r[31]);
    race.field_8a0f8() = static_cast<int32_t>(c.r[31]);
}

// (Race*)
WP_LIFTED(kLifted, race_update, "Race_Update", Ret::None) {
    Race race;
    race.base = c.r[3];

    OriginalFrame frame(c, 0x230);
    for (int reg = 31; reg >= 19; reg--) {
        uint32_t offset = 0x160 + 0x10 * static_cast<uint32_t>(reg - 19);
        frame.save_fpr(reg, offset, offset + 8);
    }
    for (int reg = 23; reg <= 31; reg++) {  // _savegpr_23
        frame.save_gpr(reg, 0x13C + 4 * static_cast<uint32_t>(reg - 23));
    }
    wr32(frame.at(0x120), 0x43300000);
    c.r[23] = race.base;
    wr32(frame.at(0x128), 0x43300000);

    // 1. Movement and speed.
    uint32_t player = race.runner(0);
    call(c, 0x80030F48, kRunnerMove, {player});
    call(c, 0x80030F50, kRunnerMove2, {player});
    call(c, 0x80030F5C, kRunnerAdvance, {player}, {race.speed_setting()});
    set_single(c, 20, Runner{{player}}.distance());  // f20: the player's distance
    c.r[31] = static_cast<uint32_t>(race.difficulty());
    call(c, 0x80030F6C, kUpdateSpeed, {race.base});

    if (race.state() <= 2) {
        float distance = static_cast<float>(c.f[20]);
        // r30: the player's segment.
        c.r[30] = call(c, 0x80030F88, kSegmentAt, {race.course()}, {distance});
        uint32_t index = c.r[30] == 0 ? 0xFFFFFFFFu : call(c, 0x80030FAC, kCourseIndexAt, {race.course()}, {distance});
        float mark = call_float(c, 0x80030FBC, kCourseDistanceOf, {race.course(), index});

        // 2. The course end: the player well into the last segment, or the special runner within 1
        // of the end.
        if (race.endless() != 1) {
            bool at_end = false;
            if (!(fsubs(distance, mark) <= 200.0f)) {
                c.r[24] = static_cast<uint32_t>(race.course_segments());
                int32_t segment = static_cast<int32_t>(call(c, 0x80030FEC, kSegmentAt, {race.course()}, {distance}));
                at_end = segment == static_cast<int32_t>(c.r[24]) - 1;
            }
            if (!at_end) {
                set_single(c, 19, race.special_distance());
                float end = call_float(c, 0x8003100C, kCourseEnd, {race.course()});
                at_end = !(fsubs(end, static_cast<float>(c.f[19])) >= 1.0f);
            }
            if (at_end) {
                if (race.state() != kStateCourseEnd) {
                    race.special_distance() = fadds(5.0f, distance);
                }
                race.state() = kStateCourseEnd;
            }
        }
        if (race.endless() == 1 && race.lives() == 0) {
            race.state() = kStateGameOver;
        }

        if (race.crash_state() == 0 && race.intro_running() == 0) {
            // 3. Effects.
            if (static_cast<uint32_t>(race.mode() - 1) <= 1) {
                if (!(race.shake_power() <= 0.0f)) {
                    call(c, 0x800310B0, kSetBlend, {player, 0x32, 2}, {race.effect_rate(), race.shake_power()});
                    race.effect_scale() = fmadds(0.7f, race.shake_power(), 1.4f);
                }
            } else {
                float sum = fadds(race.pose_meter(1), race.pose_meter(0));
                sum = fadds(race.pose_meter(2), sum);
                set_single(c, 23, fadds(race.pose_meter(3), sum));  // f23: total of the pose meters
                float total = static_cast<float>(c.f[23]);
                static constexpr uint32_t kBlendSites[4] = {0x80031118, 0x80031140, 0x80031168, 0x80031190};
                if (!(total <= 0.1f)) {
                    for (uint32_t i = 0; i < 4; i++) {
                        float share = fmuls(0.9f, fdivs(race.pose_meter(i), total));
                        call(c, kBlendSites[i], kSetBlend, {player, kGaugeIds[i], 2}, {race.gauge_frame(i), share});
                    }
                }
                // The pose-change pop.
                if (input_at(c, 0x80031194, 0x8003119C).pose_changed() != 0) {
                    race.posing() = input_at(c, 0x800311AC, 0x800311B4).pose() != 4 ? 1 : 0;
                    race.pose_pop_time() = 0;
                }
                if (race.debug_print() != 0 && !(total <= 0.1f)) {
                    static constexpr uint32_t kSprintfSites[4] = {0x8003123C, 0x80031284, 0x800312CC, 0x80031314};
                    for (uint32_t i = 0; i < 4; i++) {
                        if (race.pose_meter(i) <= 0.0f) {
                            continue;
                        }
                        float share = fmuls(0.9f, fdivs(race.pose_meter(i), total));
                        cr_set(c, 6, true);
                        static const uint32_t sprintf_address = symbol_address("sprintf");
                        call(c, kSprintfSites[i], sprintf_address, {frame.at(0x20), kDebugFormats + 0x1C * i}, {race.gauge_frame(i), share});
                        cr_set(c, 6, false);
                        call(c, kSprintfSites[i] + 0xC, kDebugPrint, {frame.at(0x20)});
                    }
                }
            }

            // 4. The wall check.
            set_single(c, 19, fadds(Runner{{player}}.distance(), -1.0f));  // f19: just behind the player
            int32_t segment = static_cast<int32_t>(call(c, 0x8003133C, kSegmentAt, {race.course()}, {c.f[19]}));
            c.r[24] = race.base + 0x60000 + 4 * static_cast<uint32_t>(segment);
            c.r[26] = 0;
            c.r[25] = static_cast<uint32_t>(race.wall(segment).get());
            uint32_t pose = input_at(c, 0x80031354, 0x8003135C).pose();
            int32_t wall = static_cast<int32_t>(c.r[25]);
            if ((wall >= 3 && wall <= 6 && pose == static_cast<uint32_t>(wall - 3)) || wall == kWallAny) {
                c.r[26] = 1;  // the right pose
            }
            if (crosses(c, 0x800313E4, race, static_cast<float>(c.f[19]), 1.0f)) {
                if (c.r[26] == 0 && race.crash_state() != static_cast<int32_t>(kCrashAnimation)) {
                    call(c, 0x8003140C, kSetAnimation, {player, kCrashAnimation});
                    race.wall(segment) = kWallBroken;
                    race.crash_effect() = 15.0f;
                    race.crash_effect_time() = 0.0f;
                    race.flag_74(static_cast<uint32_t>(race.player_runner())) = 1;
                    c.r[24] = call(c, 0x80031444, kCrashVoice, {race.base});
                    uint32_t bank = call(c, 0x80031450, kVoiceBank, {race.base});
                    call(c, 0x80031460, kPlaySound, {race.base, bank, c.r[24]});
                    race.field_408() = 0;
                    if (!(0 <= race.field_410())) {
                        race.field_410() = 0;
                    }
                    c.r[24] = static_cast<uint32_t>(race.lives());
                    race.combo() = 0;
                    race.crashes() = race.crashes() + 1;
                    race.hits() = race.hits() + 1;
                    race.lives() = static_cast<int32_t>(c.r[24]) - 1;
                    call(c, 0x800314B0, kShakeCamera, {race.base, 3});
                    if (static_cast<int32_t>(c.r[24]) - 1 == 0) {
                        race.state() = kStateGameOver;
                    }
                    race.field_430() = 2;
                }
                // Endless mode: deal the wall two segments ahead (wrapping 60.. into 50..59).
                if (race.endless() == 1) {
                    int32_t ahead = static_cast<int32_t>(c.r[30]) + 2;
                    if (ahead >= 60) {
                        ahead = (ahead - 60) % 10 + 50;
                    }
                    c.r[26] = static_cast<uint32_t>(ahead);
                    if (ahead >= 50) {
                        c.r[25] = static_cast<uint32_t>(ahead) * 4;
                        c.r[24] = race.base + 0x60000 + c.r[25];
                        if (race.wall(ahead) != kWallDealt) {
                            race.wall(ahead) = kWallDealt;
                            race.wall_kind(ahead) = 1;
                            race.wall_kind(ahead) = random_below(c, 0x80031538, frame, 0x120, 4.0f);
                            call(c, 0x80031580, kDealWall, {race.course(), static_cast<uint32_t>(ahead), 1});
                            race.flag_74(static_cast<uint32_t>(ahead)) = 0;
                        }
                    }
                }
            }

            // 5. Gauge streak and cheers.
            auto past_max = [&](uint32_t i) { return race.gauge_frame(i) > fmuls(45.0f, race.gauge_max(i)); };
            if (past_max(0) || past_max(3) || past_max(2) || !(race.gauge_frame(1) <= fmuls(45.0f, race.gauge_max(1)))) {
                race.gauge_streak() = race.gauge_streak() + 1;
            } else {
                race.gauge_streak() = 0;
            }
            if (race.mode() == 0) {
                static constexpr uint32_t kOrder[4] = {0, 2, 3, 1};
                static constexpr uint32_t kSites[4] = {0x80031638, 0x800316A4, 0x80031710, 0x8003177C};
                for (uint32_t n = 0; n < 4; n++) {
                    uint32_t i = kOrder[n];
                    if (!(race.gauge_frame(i) <= fmuls(45.0f, race.gauge_max(i))) && race.gauge_cheered(i) == 0) {
                        c.r[24] = call(c, kSites[n], kCheerVoice, {race.base});
                        uint32_t bank = call(c, kSites[n] + 0xC, kVoiceBank, {race.base});
                        call(c, kSites[n] + 0x1C, kPlaySound, {race.base, bank, c.r[24]});
                        call(c, kSites[n] + 0x2C, kPlaySound, {race.base, 1, 0x43});
                        race.gauge_cheered(i) = 1;
                        break;
                    }
                }
                for (uint32_t i : kOrder) {
                    if (!(race.gauge_frame(i) >= fmuls(45.0f, race.gauge_max(i)))) {
                        race.gauge_cheered(i) = 0;
                    }
                }
            }
        }

        // 6. Combo crash: the target runner crashes once the combo passes the maximum.
        if (race.combo() > race.crash_combo_max(race.difficulty())) {
            race.field_1e0() = race.target_runner();
        }
        int32_t target = race.target_runner();
        race.field_1a0() = target;
        if (race.combo() > race.crash_combo_max(race.difficulty()) && target != static_cast<int32_t>(kNoTarget)) {
            Runner rival{{race.runner(static_cast<uint32_t>(target))}};
            set_single(c, 19, rival.distance());
            c.r[29] = call(c, 0x800318B8, kSegmentAt, {race.course()}, {c.f[19]});
            crosses(c, 0x800318D0, race, static_cast<float>(c.f[19]), 4.0f);
            if (crosses(c, 0x800318E4, race, static_cast<float>(c.f[19]), 1.0f)) {
                race.combo() = 0;
                int32_t pick = random_below(c, 0x800318F8, frame, 0x128, 3.0f);
                static constexpr uint32_t kSites[3] = {0x80031960, 0x80031980, 0x800319A0};
                if (pick >= 0 && pick <= 2) {
                    call(c, kSites[pick], kSetAnimation, {race.runner(static_cast<uint32_t>(race.target_runner())), 8 + static_cast<uint32_t>(pick)});
                }
                race.field_17c() = race.target_runner() == 1 ? 65.0f : 55.0f;
                c.r[24] = 1;
                race.state() = kStateRivalCrashed;
                race.rival_crash_shown() = 1;
                call(c, 0x800319E0, kRunnerCrashed, {player});
                race.rival_crashes() = race.rival_crashes() + 1;
                call(c, 0x800319FC, kPlaySound, {race.base, 1, 7});
                if (race.target_runner() == static_cast<int32_t>(kNoTarget)) {
                    race.field_434() = 1;
                    switch (race.special_kind()) {
                    case 0xB: race.special_score() = 0x38; break;
                    case 0xA: race.special_score() = 0x2D; break;
                    case 0x9: race.special_score() = 0x10; break;
                    default: break;
                    }
                }
                race.wall(static_cast<int32_t>(c.r[29])) = kWallBroken;
                call(c, 0x80031A74, kSoundCommand, {rd32(kSoundManager), 3});
            }
        }

        // The special runner reaches the finish.
        if (race.target_runner() == static_cast<int32_t>(kNoTarget)) {
            float time = call_float(c, 0x80031A90, kAnimationTime, {player, kNoTarget, 0});
            if (!(time >= 1.2f) && race.mode() == 1 && race.crash_state() == 0) {
                c.r[24] = race.base + 0x10000;
                bool ready = race.special_ready() == 0;
                c.r[24] = race.base + 0x12CA0;
                Runner special{{c.r[24]}};
                if (ready || special.animation() == 5) {
                    call(c, 0x80031AE0, kSetAnimation, {player, 0x1A});
                    call(c, 0x80031AEC, kSetAnimation, {special.base, 0x1B});
                    int32_t at = frame.float_to_int(0x130, Runner{{player}}.distance());
                    race.field_4f88() = 0;
                    special.field_49a8() = 0;
                    race.finish_distance() = at;
                    race.special_finishes() = race.special_finishes() + 1;
                    race.rival_crashes() = race.rival_crashes() + 1;
                    race.state() = kStateSpecialFinish;
                    race.speed_setting() = 0.3f;
                }
            }
        }

        // Pick the runner that feints at the next wall, once per new player segment.
        if (static_cast<int32_t>(c.r[30]) != static_cast<int32_t>(rd32(kLastSegment))) {
            set_single(c, 19, frame.int_to_float(0x120, 4 - race.target_runner()));
            int32_t r = static_cast<int32_t>(call(c, 0x80031B64, kRand));
            float value = fmuls(static_cast<float>(c.f[19]), frame.int_to_float(0x128, r));
            int32_t pick = frame.float_to_int(0x130, fdivs(value, kRandMax));
            wr32(kFeintRunner, static_cast<uint32_t>(pick + race.target_runner()));
        }

        // 7. The runners.
        wr32(kLastSegment, c.r[30]);
        set_single(c, 24, 0.0f);
        c.r[28] = race.runner(0);
        set_single(c, 23, 7.0f);
        c.r[27] = race.base + 4 * c.r[31];
        set_single(c, 25, 0.0001f);
        c.r[25] = 0;
        set_single(c, 26, 45.0f);
        c.r[29] = kStateRivalCrashed;
        set_single(c, 27, 3.0f);
        c.r[30] = 1;
        c.f[28] = rdf64(0x804234C8);  // lfd: the int -> float magic
        c.r[31] = 0;
        set_single(c, 29, 4.0f);
        set_single(c, 30, kRandMax);
        set_single(c, 31, 120.0f);
        set_single(c, 21, 1.0f);
        set_single(c, 20, 4.5f);
        for (; static_cast<int32_t>(c.r[25]) <= 4; c.r[25]++, c.r[28] += kRunnerStride) {
            int32_t index = static_cast<int32_t>(c.r[25]);
            Runner runner{{c.r[28]}};
            for (uint32_t i = 0; i < 5; i++) {
                wr32(frame.at(0x8 + 4 * i), rd32(runner.base + 0x4984 + 4 * i));
            }
            int32_t state = static_cast<int32_t>(rd32(frame.at(0x8)));
            if (index == 0 && race.intro_running() == 0) {
                continue;
            }
            set_single(c, 22, runner.distance());  // f22: this runner's distance
            float distance = static_cast<float>(c.f[22]);
            c.r[24] = call(c, 0x80031C40, kSegmentAt, {race.course()}, {distance});
            int32_t segment = static_cast<int32_t>(c.r[24]);
            call(c, 0x80031C54, kSegmentTouch, {race.course(), c.r[24]});

            if (index == 4 && race.target_runner() > static_cast<int32_t>(rd32(c.r[27] + 0x4A0))) {
                // The special runner: pose walls (dash value 0) at 10.5 ahead, dash walls at 17.5.
                c.r[26] = race.base + 4 * c.r[24];
                bool dash = race.special_dash(segment) != 0;
                if (crosses(c, dash ? 0x80031D90 : 0x80031C9C, race, distance, dash ? 17.5f : 10.5f) && can_react(state)) {
                    float speed = reach_speed(c, dash ? 0x80031DC8 : 0x80031CD4, dash ? 0x80031DF4 : 0x80031D00, race, runner, segment, true);
                    runner.animation_speed() = dash ? fmuls(static_cast<float>(c.f[27]), speed) : speed;
                    wall_animation(c, dash ? 0x80031E40 : 0x80031D48, runner, race.wall_kind(segment), dash ? 0x16 : 1);
                }
            } else {
                bool feint_wall = false;
                if (index == static_cast<int32_t>(rd32(kFeintRunner))) {
                    c.r[26] = race.base + 4 * c.r[24];
                    feint_wall = race.dash(segment) != 0;
                }
                bool is_target_feint = index == race.target_runner() && race.combo() > race.crash_combo_max(race.difficulty());
                if (!feint_wall) {
                    if (is_target_feint) {
                        if (can_react(state) && race.rival_crash_shown() == -1 && crosses(c, 0x80031F00, race, distance, 8.5f) &&
                            static_cast<int32_t>(rd32(frame.at(0x8))) != 0x27) {
                            start_feint(c, 0x80031F30, 0x80031F40, 0x80031F8C, frame, 0x120, race);
                        }
                    } else if (can_react(state) && crosses(c, 0x80031FC8, race, distance, 10.5f)) {
                        runner.animation_speed() = reach_speed(c, 0x80031FE8, 0x80032008, race, runner, segment, false);
                        wall_animation(c, 0x80032058, runner, race.wall_kind(segment), 1);
                    }
                } else {
                    if (is_target_feint) {
                        if (state != 0x27 && race.rival_crash_shown() == -1 && crosses(c, 0x800320E8, race, distance, 8.5f) &&
                            static_cast<int32_t>(rd32(frame.at(0x8))) != 0x27) {
                            start_feint(c, 0x80032118, 0x80032128, 0x80032174, frame, 0x128, race);
                        }
                    } else if (can_react(state) && crosses(c, 0x800321B0, race, distance, 17.5f)) {
                        float speed = reach_speed(c, 0x800321D0, 0x800321F0, race, runner, segment, false);
                        runner.animation_speed() = fmuls(static_cast<float>(c.f[27]), speed);
                        wall_animation(c, 0x8003223C, runner, race.wall_kind(segment), 0x16);
                    }
                }
            }

            // The target runner idles out of a shake mode before its next wall.
            if (index == race.target_runner() && static_cast<uint32_t>(race.mode() - 1) <= 1 && !crosses(c, 0x800322A0, race, distance, 30.0f)) {
                uint32_t target_index = static_cast<uint32_t>(race.target_runner());
                float time = call_float(c, 0x800322BC, kAnimationTime, {player, target_index, 0});
                if (!(time >= static_cast<float>(c.f[20]))) {
                    time = call_float(c, 0x800322D8, kAnimationTime, {player, static_cast<uint32_t>(race.target_runner()), 0});
                    if (!(time <= static_cast<float>(c.f[21])) && runner.animation() == 0) {
                        call(c, 0x800322F8, kSetAnimation, {runner.base, 5});
                    }
                }
            }
            // Past the course end: marked.
            if (race.endless() != 1) {
                float end = call_float(c, 0x80032310, kCourseEnd, {race.course()});
                if (!(static_cast<float>(c.f[22]) <= end)) {
                    runner.marker() = static_cast<int32_t>(c.r[29]);
                }
            }
        }
    }

    call(c, 0x80032334, kRunnersEnd, {player});
    for (int reg = 31; reg >= 19; reg--) {
        uint32_t offset = 0x160 + 0x10 * static_cast<uint32_t>(reg - 19);
        frame.restore_fpr(reg, offset, offset + 8);
    }
    for (int reg = 23; reg <= 31; reg++) {  // _restgpr_23
        frame.restore_gpr(reg, 0x13C + 4 * static_cast<uint32_t>(reg - 23));
    }
}

}  // namespace

}  // namespace musclemarch
