// Race speeds: how fast each runner goes, by race state.
//
// Lifted from fn_80032420 (called first thing in the race update, race_update.cpp). By state
// (race +0x38):
//   0  countdown: runners held, the special runner eased in by the countdown timer;
//   1  racing: the special runner's behavior (follow the player, dash zones in mode 1, chase
//      speed-ups), then for each runner: base speed (SELF/RIVAL/AUTO_DASH rates), shake power
//      in the shake modes, speed zones from the course's zone lists, MIDDLE_LENGTH_MAP_SPEED_UP,
//      the special mode's speed table, and a slow-down when too close to the runner ahead;
//      the running sound follows whether any runner is in a zone;
//   2  a rival crashed: it slows, the player eases off;   3, 4  course end / game over;
//   6  the special runner caught: slow motion, then the celebration line-up;
//   7  the celebration: runners stop in turn and cheer, then the race ends.
// Guest registers r15-r31/f23-f31 hold the original's values at every call (see race.h).

#include "race/race.h"
#include "input/player_input.h"
#include "wp/threads.h"

namespace musclemarch {

namespace {

using namespace wp;
using namespace wp::lift;
using namespace wp::lift::ppc;

const lift::Group kLifted{"game.lifted", "WP_LIFTED"};

constexpr uint32_t kSetSpeed = 0x8003F2DC;        // (runners, runner, f1)
constexpr uint32_t kAddSpeed = 0x8003F2EC;        // (runners, runner, f1)
constexpr uint32_t kGetSpeed = 0x8003F6AC;        // (runners, runner) -> f1
constexpr uint32_t kGap = 0x8003F6BC;             // (runners, a, b) -> f1
constexpr uint32_t kRunnerAdvance = 0x8003F304;   // (runners, f1)
constexpr uint32_t kSetAnimation = 0x800408D4;    // (runner, animation)
constexpr uint32_t kTimerStep = 0x800395AC;       // (timer, f1 rate) -> f1 elapsed
constexpr uint32_t kSetTimeScale = 0x80039554;    // (timer, f1)
constexpr uint32_t kChaseOver = 0x8003D248;       // (course, f1 distance) -> int
constexpr uint32_t kSegmentAt = 0x8003BA48;
constexpr uint32_t kLapAt = 0x8003C204;
constexpr uint32_t kCrossesSegment = 0x8003C280;
constexpr uint32_t kSegmentLength = 0x8003B640;   // (course, segment) -> f1
constexpr uint32_t kZonePosition = 0x8003BB94;    // (course, f1 distance) -> int
constexpr uint32_t kPlaySound = 0x8003652C;       // (race, bank, sound)
constexpr uint32_t kStopSound = 0x8003658C;       // (race, bank, sound)
constexpr uint32_t kSoundPlaying = 0x800364B0;    // (race, bank, sound) -> int
constexpr uint32_t kRaceFade = 0x80036458;        // (race, f1)
constexpr uint32_t kRaceEnd = 0x80036414;         // (race)
constexpr uint32_t kScreenFade = 0x8002315C;      // (fader, f1, 0, 0)
constexpr uint32_t kRand = 0x802160C4;
constexpr uint32_t kDebugPrint = 0x800323B8;
// The special mode's speed table: a sheet in the game's data, read through a local reader.
constexpr uint32_t kSheetFind = 0x8014AF3C;       // (book, name) -> sheet
constexpr uint32_t kSheetColumn = 0x8014AFF8;     // (book, sheet, name) -> column
constexpr uint32_t kSheetRows = 0x8014B424;       // (book, column) -> rows
constexpr uint32_t kSheetData = 0x8014B0D4;       // (book, sheet, column) -> data
constexpr uint32_t kReaderInit = 0x8014B89C;      // (reader, data, rows)
constexpr uint32_t kReaderCell = 0x8014B8F4;      // (reader, column, row, out) -> found
constexpr uint32_t kReaderDestroy = 0x8014B8B4;   // (reader, -1)
constexpr uint32_t kParseInt = 0x80159914;        // (text) -> int
constexpr uint32_t kParseFloat = 0x801593B8;      // (text) -> f1

constexpr uint32_t kCurrentRace = 0x804223D0;
constexpr uint32_t kSpecialEased = 0x804211A0;    // global flag
constexpr uint32_t kSheetName = 0x804211A4;       // global: pointer to the sheet name
constexpr uint32_t kColumnName = 0x804211A8;      // global: pointer to the column name
constexpr uint32_t kTexts = 0x8026FFEC;           // report/debug formats (+0x70, +0xB7)

constexpr float kRandMax = 2147483600.0f;
constexpr uint32_t kSpecial = 4;                  // the special runner's index

// Parameter offsets (one float or int per difficulty, docs/GAME.md).
constexpr uint32_t kSelfDashRate = 0x4AC;
constexpr uint32_t kRivalDashRate = 0x4B8;
constexpr uint32_t kGachaDashRate = 0x4C4;
constexpr uint32_t kAutoDashMulCombo = 0x4F8;
constexpr uint32_t kAutoDashComboSpeedMax = 0x504;
constexpr uint32_t kRivalMulNumSpeed = 0x510;
constexpr uint32_t kDashZoneRandNum = 0x538;
constexpr uint32_t kDashZoneRandParam = 0x544;
constexpr uint32_t kDashZoneNearDownRate = 0x550;
constexpr uint32_t kDashZoneBaseSpeed = 0x55C;
constexpr uint32_t kDashZoneChangeSpeedLife = 0x568;
constexpr uint32_t kDashZoneChangeSpeedRate = 0x574;
constexpr uint32_t kDashZoneExchangeNumRate = 0x580;
constexpr uint32_t kDashZoneExchangeNumRateF = 0x58C;
constexpr uint32_t kDashZoneFirstDashSpeed = 0x598;
constexpr uint32_t kDashZoneBothUpSpeed = 0x5A4;

struct Speed {
    Cpu& c;
    Race race;
    OriginalFrame& frame;
    int32_t difficulty;

    uint32_t runners() const { return race.runner(0); }
    uint32_t course() const { return race.course(); }
    float param(uint32_t offset) const { return race.param(offset, difficulty); }

    void set_speed(uint32_t site, uint32_t runner, float speed) { call(c, site, kSetSpeed, {runners(), runner}, {speed}); }
    void add_speed(uint32_t site, uint32_t runner, float speed) { call(c, site, kAddSpeed, {runners(), runner}, {speed}); }
    float get_speed(uint32_t site, uint32_t runner) { return call_float(c, site, kGetSpeed, {runners(), runner}); }
    float gap(uint32_t site, uint32_t a, uint32_t b) { return call_float(c, site, kGap, {runners(), a, b}); }
    float timer_step(uint32_t site, float rate) { return call_float(c, site, kTimerStep, {race.base + 0x70}, {rate}); }
    void sound(uint32_t site, uint32_t bank, uint32_t id) { call(c, site, kPlaySound, {race.base, bank, id}); }

    void countdown();
    void racing();
    void special_runner();
    void chase(uint32_t done_site, uint32_t sound_site, uint32_t get_site, uint32_t set_site, uint32_t step_site, uint32_t done2_site,
               uint32_t first_slot, uint32_t second_slot, float effect);
    void runner_speeds();
    void special_table_speed();
    void rival_crashed();
    void course_end();
    void game_over();
    void caught();
    void celebration();
};

// State 0: the countdown. The special runner (index 4) moves at 0.06 until 70 frames, stands
// until 148, then moves at 0.18; the others are held until 190.
void Speed::countdown() {
    if (race.endless() == 1) {
        c.r[4] = race.base + 0x50000;
        call(c, 0x8003251C, kRunnerAdvance, {runners(), c.r[4]}, {0.0f});
        return;
    }
    int32_t timer = race.countdown();
    if (timer >= 0xBE) {
        set_speed(0x80032610, kSpecial, 0.18f);
        race.special_effect() = 1.8f;
        c.r[17] = runners();
        c.r[19] = 0;
        c.r[18] = 0;
        for (; static_cast<int32_t>(c.r[19]) < 4; c.r[19]++, c.r[17] += kRunnerStride) {
            Runner{{c.r[17]}}.marker() = static_cast<int32_t>(c.r[18]);
            set_speed(0x8003263C, c.r[19], 0.18f);
        }
    } else if (static_cast<uint32_t>(timer - 0x46) <= 0x4D) {
        set_speed(0x800325AC, kSpecial, 0.0f);
        if (rd32(race.base + 0x17624) == 5 || rd32(kSpecialEased) == 0) {
            return;
        }
        call(c, 0x800325D4, kSetAnimation, {race.base + 0x12CA0, 5});
        wr32(kSpecialEased, 0);
    } else if (static_cast<uint32_t>(timer - 0x94) <= 0x29) {
        set_speed(0x800325F0, kSpecial, 0.18f);
        race.special_effect() = 1.8f;
    } else {
        set_speed(0x80032554, kSpecial, 0.06f);
        race.special_effect() = 1.0f;
        c.r[17] = runners();
        c.r[19] = 0;
        c.r[18] = 2;
        for (; static_cast<int32_t>(c.r[19]) < 4; c.r[19]++, c.r[17] += kRunnerStride) {
            Runner{{c.r[17]}}.marker() = static_cast<int32_t>(c.r[18]);
            set_speed(0x80032580, c.r[19], 0.0f);
        }
        wr32(kSpecialEased, 1);
    }
}

// The chase after the special runner in the normal modes: wait until the chase starts (a sound),
// then speed it up OOO_ENEMY_SPEED_UP_COUNT times, until the chase is over.
void Speed::chase(uint32_t done_site, uint32_t sound_site, uint32_t get_site, uint32_t set_site, uint32_t step_site, uint32_t done2_site,
                  uint32_t first_slot, uint32_t second_slot, float effect) {
    set_single(c, 24, race.special_distance());
    switch (race.chase_state()) {
    case 0:
        if (call(c, done_site, kChaseOver, {course()}, {c.f[24]}) == 0) {
            sound(sound_site, 1, 0x42);
            race.chase_state() = 1;
        }
        return;
    case 1: {
        float speed = get_speed(get_site, kSpecial);
        set_speed(set_site, kSpecial, fmuls(speed, race.ooo_enemy_speed_up_rate()));
        if (!(frame.int_to_float(first_slot, race.chase_count()) >= race.ooo_enemy_speed_up_count())) {
            float step = timer_step(step_site, 1.0f);
            float count = frame.int_to_float(second_slot, race.chase_count());
            race.chase_count() = frame.float_to_int(0x328, fadds(count, step));
        } else if (call(c, done2_site, kChaseOver, {course()}, {c.f[24]}) == 0) {
            race.chase_state() = 2;
        }
        race.special_effect() = effect;
        return;
    }
    default:
        return;
    }
}

// State 1, the special runner (index 4) and the target runner.
void Speed::special_runner() {
    race.intro_running() = race.endless() == 2 ? 1 : 0;
    if (race.crash_state() == 0xB) {
        set_speed(0x80032694, 0, fsubs(race.speed_setting(), race.crash_down_speed()));
    }
    int32_t mode = static_cast<int32_t>(c.r[24]);
    if (race.target_runner() != static_cast<int32_t>(kSpecial)) {
        // Chasing a rival: the player keeps up with it, the special runner hangs back.
        if (!(gap(0x800326B0, static_cast<uint32_t>(race.target_runner()), 0) <= 2.5f)) {
            if (race.crash_state() == 0) {
                add_speed(0x800326D8, 0, race.player_follow_speed_up());
            }
        } else if (!(gap(0x800326F0, static_cast<uint32_t>(race.target_runner()), 0) >= 2.0f) && race.crash_state() == 0) {
            add_speed(0x80032718, 0, -0.02f);
        }
        if (!(gap(0x80032728, kSpecial, 3) <= 3.5f) && rd32(race.base + 0x17624) == 0) {
            set_speed(0x80032764, kSpecial, fmuls(0.8f, get_speed(0x80032750, kSpecial)));
        }
        return;  // to the runners
    }

    if (mode != 1) {
        if (!(gap(0x8003277C, kSpecial, 0) <= 3.0f)) {
            if (race.crash_state() == 0 && mode != 1) {
                add_speed(0x800327AC, 0, race.player_follow_speed_up());
            }
        } else if (!(gap(0x800327C4, static_cast<uint32_t>(race.target_runner()), 0) >= 2.5f) && rd32(race.base + 0x17624) == 0) {
            set_speed(0x800327F8, kSpecial, fadds(0.02f, race.speed_setting()));
        }
        race.dash_timer() = 0.0f;
        float exchanges = frame.int_to_float(0x320, race.dash_exchanges());
        race.dash_speed() = fnmsubs(exchanges, param(kDashZoneExchangeNumRateF), param(kDashZoneFirstDashSpeed));
        switch (race.special_kind()) {
        case 7: race.special_effect() = 3.0f; break;
        case 0xA: race.special_effect() = 2.0f; break;
        case 0x9: race.special_effect() = 2.0f; break;
        case 0x8: race.special_effect() = 2.3f; break;
        case 0xB: race.special_effect() = 3.0f; break;
        default: race.special_effect() = 2.0f; break;
        }
        if (race.endless() != 1) {
            chase(0x800328E4, 0x800328FC, 0x80032914, 0x80032928, 0x80032958, 0x80032998, 0x318, 0x320, 3.0f);
        }
    } else {
        // Mode 1: dash zones. Every 30 frames the special runner's dash speed is re-rolled.
        if (race.dash_timer() >= 30.0f) {
            race.dash_timer() = 0.0f;
            if (!(gap(0x800329E0, static_cast<uint32_t>(race.target_runner()), 0) >= 4.0f)) {
                int32_t r = static_cast<int32_t>(call(c, 0x800329F0, kRand));
                float random = frame.int_to_float(0x320, r);
                float rand_num = frame.int_to_float(0x318, race.int_param(kDashZoneRandNum, difficulty));
                int32_t zones_so_far = race.dash_zones();
                float zones = frame.int_to_float(0x320, zones_so_far);
                double scaled = static_cast<double>(fmuls(rand_num, random)) / 2147483648.0;
                int32_t pick = frame.float_to_int(0x328, scaled);
                float picked = frame.int_to_float(0x318, pick + 1);
                float exchanges = frame.int_to_float(0x318, race.dash_exchanges());
                race.dash_speed() = fmadds(picked, param(kDashZoneRandParam), param(kDashZoneBaseSpeed));
                float rate = fnmsubs(zones, param(kDashZoneNearDownRate), 1.0f);
                rate = fnmsubs(exchanges, param(kDashZoneExchangeNumRate), rate);
                if (!(race.lives() > race.int_param(kDashZoneChangeSpeedLife, difficulty))) {
                    rate = fsubs(rate, param(kDashZoneChangeSpeedRate));
                }
                if (!(rate >= 0.0f)) {
                    rate = 0.0f;
                }
                race.dash_speed() = fmuls(race.dash_speed(), rate);
                race.dash_zones() = zones_so_far + 1;
            } else {
                race.dash_speed() = 0.9f;
            }
        } else {
            race.dash_timer() = fadds(race.dash_timer(), timer_step(0x80032AF8, 1.0f));
        }
        float dash = race.dash_speed();
        if (!(dash <= 0.9f)) {
            race.dash_speed() = fsubs(dash, 0.01f);
        } else {
            race.dash_speed() = 0.9f;
        }
        if (race.crash_state() == 0) {
            if (!(gap(0x80032B48, static_cast<uint32_t>(race.target_runner()), 0) >= 18.0f)) {
                set_speed(0x80032B74, kSpecial, fmuls(get_speed(0x80032B60, kSpecial), race.dash_speed()));
                race.special_effect() = fadds(1.5f, race.dash_speed());
            } else {
                set_speed(0x80032BAC, kSpecial, fmuls(0.5f, get_speed(0x80032B98, 0)));
                race.special_effect() = 1.2f;
            }
        }
        chase(0x80032BE8, 0x80032C00, 0x80032C18, 0x80032C2C, 0x80032C5C, 0x80032C9C, 0x320, 0x318, 2.0f);
        c.r[18] = race.base + 4 * static_cast<uint32_t>(difficulty);
        add_speed(0x80032CD0, 0, param(kDashZoneBothUpSpeed));
        add_speed(0x80032CE0, kSpecial, param(kDashZoneBothUpSpeed));
    }

    // Shake modes: shaking pushes the player while the target is close.
    if (race.crash_state() == 0 && static_cast<uint32_t>(mode - 1) <= 1 &&
        !(gap(0x80032D0C, static_cast<uint32_t>(race.target_runner()), 0) <= 1.2f)) {
        if (mode == 2) {
            add_speed(0x80032D38, 0, fmuls(0.2f, race.shake_power()));
        } else if (mode == 1) {
            add_speed(0x80032D60, 0, fmuls(race.shake_power(), param(kGachaDashRate)));
        }
    }
}

// The special mode's speed table: row (lap x 10 + segment) / 5 + 5 of a data sheet.
void Speed::special_table_speed() {
    uint32_t game = call(c, 0x80033058, kGetGame);
    c.r[17] = rd32(game + 0x3C);
    c.r[16] = call(c, 0x80033068, kSheetFind, {c.r[17], rd32(kSheetName)});
    c.r[15] = call(c, 0x8003307C, kSheetColumn, {c.r[17], c.r[16], rd32(kColumnName)});
    c.r[18] = call(c, 0x8003308C, kSheetRows, {c.r[17], c.r[15]});
    uint32_t data = call(c, 0x800330A0, kSheetData, {c.r[17], c.r[16], c.r[15]});
    call(c, 0x800330B0, kReaderInit, {frame.at(0x8), data, c.r[18]});

    auto row_at = [&](uint32_t lap_site, uint32_t segment_site) {
        int32_t lap = static_cast<int32_t>(call(c, lap_site, kLapAt, {course()}, {c.f[23]}));
        c.r[18] = static_cast<uint32_t>(lap * 10);
        int32_t segment = static_cast<int32_t>(call(c, segment_site, kSegmentAt, {course()}, {c.f[23]}));
        int32_t row = (segment + static_cast<int32_t>(c.r[18])) / 5 + 5;
        c.r[15] = static_cast<uint32_t>(row > 0x31 ? 0x31 : row);
    };

    set_single(c, 23, race.special_distance());
    row_at(0x800330C4, 0x800330D8);
    if (call(c, 0x80033114, kReaderCell, {frame.at(0x8), 2, c.r[15], frame.at(0x218)}) != 0) {
        race.table_value() = static_cast<int32_t>(call(c, 0x80033124, kParseInt, {frame.at(0x218)}));
    } else {
        static const uint32_t os_report = symbol_address("OSReport");
        cr_set(c, 6, false);
        call(c, 0x8003313C, os_report, {kTexts + 0x70, c.r[15]});
    }

    set_single(c, 23, Runner{{runners()}}.distance());
    row_at(0x80033150, 0x80033164);
    if (call(c, 0x800331A0, kReaderCell, {frame.at(0x8), 3, c.r[15], frame.at(0x118)}) != 0) {
        call(c, 0x800331B0, kParseFloat, {frame.at(0x118)});
        set_single(c, 24, static_cast<float>(c.f[1]));  // frsp
    } else {
        static const uint32_t os_report = symbol_address("OSReport");
        cr_set(c, 6, false);
        call(c, 0x800331C8, os_report, {kTexts + 0x70, c.r[15]});
    }

    if (race.debug_print() != 0) {
        static const uint32_t sprintf_address = symbol_address("sprintf");
        cr_set(c, 6, true);
        call(c, 0x800331F0, sprintf_address, {frame.at(0x18), kTexts + 0xB7, static_cast<uint32_t>(race.table_value())}, {c.f[24]});
        cr_set(c, 6, false);
        call(c, 0x800331FC, kDebugPrint, {frame.at(0x18)});
    }
    call(c, 0x80033208, kReaderDestroy, {frame.at(0x8), 0xFFFFFFFFu});
}

// State 1, every runner's speed.
void Speed::runner_speeds() {
    int32_t mode = static_cast<int32_t>(c.r[24]);
    c.f[28] = rdf64(0x804234C8);  // lfd: the int -> float magic
    c.r[21] = race.base + 4 * static_cast<uint32_t>(difficulty);
    set_single(c, 27, 47.0f);
    set_single(c, 25, 0.01f);
    c.r[22] = race.base;
    set_single(c, 26, 2.5f);
    c.r[23] = runners();
    c.r[20] = race.base + 0x10000;
    c.r[25] = static_cast<uint32_t>(mode - 1);
    c.r[26] = race.base + 0x60000;
    c.r[30] = kTexts;
    c.r[19] = 0;
    c.r[27] = 1;
    c.r[28] = 0;
    c.r[29] = 0x66660000;
    for (; static_cast<int32_t>(c.r[19]) < 5; c.r[19]++, c.r[22] += 4, c.r[23] += kRunnerStride) {
        uint32_t index = c.r[19];
        Runner runner{{c.r[23]}};
        set_single(c, 23, runner.distance());
        c.r[18] = call(c, 0x80032DB4, kSegmentAt, {course()}, {c.f[23]});
        int32_t segment = static_cast<int32_t>(c.r[18]);
        c.f[31] = call_float(c, 0x80032DC8, kSegmentLength, {course(), c.r[18]});  // fmr

        // Base speed.
        float speed = 0.0f;
        if (c.r[25] <= 1) {
            if (index == 0) {
                if (mode == 2) {
                    float combo = frame.int_to_float(0x320, race.field_408());
                    float boost = fmuls(param(kAutoDashMulCombo), combo);
                    float cap = param(kAutoDashComboSpeedMax);
                    speed = !(cap >= boost) ? fadds(speed, fadds(boost, param(kSelfDashRate))) : fadds(speed, cap);
                } else {
                    speed = fadds(speed, param(kSelfDashRate));
                }
            } else {
                speed = fadds(speed, static_cast<float>(c.f[30]));
            }
        }
        speed = fadds(speed, static_cast<float>(c.f[29]));
        if (race.endless() != 1) {
            add_speed(0x80032E54, index, speed);
        }

        // Speed zones from the course: zone lists of {type, from, to, factor}; type 10 scales the
        // speed, type 0x11 marks the runner as in a zone.
        c.f[24] = call_float(c, 0x80032E60, kGetSpeed, {runners(), index});  // fmr
        race.in_zone(index) = static_cast<int32_t>(c.r[28]);
        runner.field_49a8() = static_cast<int32_t>(c.r[27]);
        c.r[17] = static_cast<uint32_t>(race.zone_table(segment).get());
        int32_t position = static_cast<int32_t>(call(c, 0x80032E90, kZonePosition, {course()}, {c.f[23]}));
        uint32_t list = race.zone_list(static_cast<int32_t>(c.r[17]));
        for (int32_t j = 0; j < static_cast<int32_t>(rd32(list + 0x2C)); j++) {
            uint32_t entry = list + rd32(list + 0x30) + 0x80 * static_cast<uint32_t>(j);
            int32_t type = static_cast<int32_t>(rd32(entry));
            if (static_cast<int32_t>(rd32(entry + 4)) > position || static_cast<int32_t>(rd32(entry + 8)) < position) {
                continue;
            }
            if (type == 10) {
                set_single(c, 24, fmuls(static_cast<float>(c.f[24]), ldf(entry + 0xC)));
            } else if (type == 0x11) {
                race.in_zone(index) = static_cast<int32_t>(c.r[27]);
                runner.field_49a8() = static_cast<int32_t>(c.r[28]);
            }
        }

        // Long segments in the normal mode.
        if (race.endless() == 0 && mode != 1 && !(c.f[31] <= c.f[27]) &&
            call(c, 0x80033024, kCrossesSegment, {course()}, {c.f[23], 18.0f}) == 0 && segment > 0) {
            float s = static_cast<float>(c.f[24]);
            set_single(c, 24, fmadds(s, race.middle_length_map_speed_up(), s));
        }
        // The special mode: speed from the table, up to SPECIAL_SPEED_MAX.
        if (race.endless() == 1) {
            if (!(c.f[24] >= race.special_speed_max())) {
                special_table_speed();
            } else {
                c.f[24] = race.special_speed_max();  // fmr
            }
        }
        // Too close to the runner ahead: slow down by 1%.
        if (index != 4 && index != 0 && !(gap(0x80033230, index + 1, index) >= static_cast<float>(c.f[26]))) {
            float s = static_cast<float>(c.f[24]);
            set_single(c, 24, fnmsubs(static_cast<float>(c.f[25]), s, s));
        }
        set_speed(0x8003324C, index, static_cast<float>(c.f[24]));
    }

    // The running sound: on while any runner is in a zone.
    bool any = false;
    for (uint32_t i = 0; i < 5; i++) {
        any = any || race.in_zone(i) != 0;
    }
    c.r[18] = any ? 1 : 0;
    bool playing = call(c, 0x800332D4, kSoundPlaying, {race.base, 1, 2}) != 0;
    if (any && playing) {
        call(c, 0x800332F4, kStopSound, {race.base, 1, 2});
    } else if (!any && !playing) {
        sound(0x80033318, 1, 2);
    }
}

void Speed::racing() {
    special_runner();
    runner_speeds();
}

// State 2: the crashed rival slows; the player eases off.
void Speed::rival_crashed() {
    uint32_t rival = static_cast<uint32_t>(race.target_runner() - 1);
    if (static_cast<uint32_t>(Runner{{race.runner(rival)}}.animation() - 8) <= 2) {
        set_speed(0x80033344, rival, -0.1f);
    }
    set_speed(0x80033364, 0, fmuls(0.9f, get_speed(0x80033350, 0)));
}

// State 3: the course end. The special runner slows, the player stops and poses.
void Speed::course_end() {
    race.special_effect() = 2.0f;
    set_speed(0x80033384, kSpecial, 0.15f);
    if (rd32(race.base + 0x17624) != 5) {
        call(c, 0x800333A0, kSetAnimation, {race.base + 0x12CA0, 5});
    }
    set_speed(0x800333B0, 0, 0.0f);
    if (race.crash_state() != 0x14) {
        call(c, 0x800333C8, kSetAnimation, {runners(), 0x14});
    }
}

// State 4: game over.
void Speed::game_over() {
    set_speed(0x800333DC, 0, 0.0f);
    race.field_4f88() = 0;
    if (race.crash_state() != 0x15) {
        call(c, 0x800333FC, kSetAnimation, {runners(), 0x15});
    }
}

// State 6: the special runner caught. Slow motion, then the line-up for the celebration.
void Speed::caught() {
    call(c, 0x8003340C, kStopSound, {race.base, 1, 2});
    if (!(race.speed_setting() <= 0.0f)) {
        float step = timer_step(0x80033428, 0.0035f);
        race.speed_setting() = fsubs(race.speed_setting(), step);
    } else {
        race.speed_setting() = 0.0f;
    }
    float time = race.celebration_time();
    bool slow = !(time <= 3.0f) && !(time >= 31.0f);
    call(c, slow ? 0x80033464 : 0x80033474, kSetTimeScale, {race.base + 0x70}, {slow ? 0.1f : 1.0f});
    float step = timer_step(0x80033480, 1.0f);
    time = fadds(race.celebration_time(), step);
    race.celebration_time() = time;
    if (time <= 31.0f) {
        return;
    }
    call(c, 0x800334A4, kRaceFade, {rd32(kCurrentRace)}, {210.0f});
    set_single(c, 27, fsubs(static_cast<float>(c.f[31]), 7.0f));  // line up behind the player
    set_single(c, 29, 3.0f);
    c.r[17] = race.runner(1);
    race.crash_effect() = 10.0f;
    c.r[15] = 1;
    c.f[28] = rdf64(0x804234C8);  // lfd
    race.crash_effect_time() = 0.0f;
    c.r[18] = 0;
    set_single(c, 26, 2.0f);
    race.crash_effect_scale() = static_cast<float>(c.f[29]);
    set_single(c, 25, kRandMax);
    race.speed_setting() = 0.0f;
    race.state() = 7;
    do {
        int32_t r = static_cast<int32_t>(call(c, 0x800334F0, kRand));
        float random = frame.int_to_float(0x320, r);
        int32_t place = static_cast<int32_t>(c.r[15]);
        c.r[15]++;
        float spread = fdivs(fmuls(static_cast<float>(c.f[26]), random), static_cast<float>(c.f[25]));
        float behind = fnmsubs(static_cast<float>(c.f[29]), frame.int_to_float(0x318, place), static_cast<float>(c.f[27]));
        Runner runner{{c.r[17]}};
        runner.distance() = fadds(behind, spread);
        runner.marker() = static_cast<int32_t>(c.r[18]);
        c.r[17] += kRunnerStride;
    } while (static_cast<int32_t>(c.r[15]) < 4);
    sound(0x80033548, 1, 0x35);
}

// State 7: the celebration. Rivals stop past the line-up point, flex (animations 0x21..0x25),
// and cheer once (sound 0x35 or 0x36); at 240 the race ends.
void Speed::celebration() {
    call(c, 0x80033558, kSetTimeScale, {race.base + 0x70}, {1.0f});
    float step = timer_step(0x80033564, 1.0f);
    float time = race.celebration_time();
    c.r[17] = race.runner(1);
    set_single(c, 26, 2.0f);
    c.r[21] = race.base + 4;
    time = fadds(time, step);
    set_single(c, 28, 0.0f);
    set_single(c, 27, fsubs(static_cast<float>(c.f[31]), static_cast<float>(c.f[26])));
    set_single(c, 25, 0.1f);
    race.celebration_time() = time;
    c.r[18] = 1;
    c.f[30] = rdf64(0x804234C8);  // lfd
    c.r[20] = 0;
    set_single(c, 29, kRandMax);
    c.r[19] = 1;
    set_single(c, 31, 53.333332f);
    static constexpr uint32_t kFlexSites[5] = {0x80033600, 0x80033634, 0x80033668, 0x8003369C, 0x800336D0};
    for (; static_cast<int32_t>(c.r[18]) < 4; c.r[18]++, c.r[21] += 4, c.r[17] += kRunnerStride) {
        uint32_t index = c.r[18];
        Runner runner{{c.r[17]}};
        runner.speed() = static_cast<float>(c.f[25]);
        if (!(runner.distance() <= static_cast<float>(c.f[27]))) {
            runner.speed() = static_cast<float>(c.f[28]);
            if (index >= 1 && index <= 5) {
                int32_t pose = 0x1B + static_cast<int32_t>(index);
                int32_t flex = 0x20 + static_cast<int32_t>(index);
                if (runner.animation() != pose && runner.animation() != flex) {
                    call(c, kFlexSites[index - 1], kSetAnimation, {runner.base, static_cast<uint32_t>(flex)});
                    runner.field_49a8() = static_cast<int32_t>(c.r[20]);
                    race.cheer_time(index) = race.celebration_time();
                }
            }
        }
        if (!(fadds(static_cast<float>(c.f[31]), race.cheer_time(index)) >= race.celebration_time()) && race.cheered(index) == 0) {
            int32_t r = static_cast<int32_t>(call(c, 0x80033708, kRand));
            float value = fmuls(static_cast<float>(c.f[26]), frame.int_to_float(0x318, r));
            int32_t pick = frame.float_to_int(0x328, fdivs(value, static_cast<float>(c.f[29])));
            if (pick == 0) {
                sound(0x80033750, 1, 0x35);
            } else if (pick == 1) {
                sound(0x80033764, 1, 0x36);
            }
            race.cheered(index) = static_cast<int32_t>(c.r[19]);
        }
    }
    float now = race.celebration_time();
    if (now >= 240.0f) {
        call(c, 0x8003379C, kRaceEnd, {rd32(kCurrentRace)});
        call(c, 0x800337AC, kStopSound, {race.base, 1, 2});
        race.scene_request() = 8;
        race.field_444() = 2;
    } else if (!(now <= 210.0f)) {
        set_single(c, 23, fdivs(fsubs(now, 210.0f), 30.0f));
        uint32_t game = call(c, 0x800337DC, kGetGame);
        call(c, 0x800337F0, kScreenFade, {rd32(game + 0x58), 0, 0}, {c.f[23]});
    }
}

// (Race*)
WP_LIFTED(kLifted, race_speed, "Race_UpdateSpeed", Ret::None) {
    Race race;
    race.base = c.r[3];

    OriginalFrame frame(c, 0x410);
    for (int reg = 31; reg >= 23; reg--) {
        uint32_t offset = 0x380 + 0x10 * static_cast<uint32_t>(reg - 23);
        frame.save_fpr(reg, offset, offset + 8);
    }
    for (int reg = 15; reg <= 31; reg++) {  // _savegpr_15
        frame.save_gpr(reg, 0x33C + 4 * static_cast<uint32_t>(reg - 15));
    }

    c.r[31] = race.base;
    c.r[21] = static_cast<uint32_t>(race.difficulty());
    int32_t difficulty = static_cast<int32_t>(c.r[21]);
    wr32(frame.at(0x318), 0x43300000);
    c.r[20] = c.r[21] * 4;
    float target = frame.int_to_float(0x318, race.target_runner());
    wr32(frame.at(0x320), 0x43300000);
    set_single(c, 31, Runner{{race.runner(0)}}.distance());  // f31: the player's distance
    c.r[24] = static_cast<uint32_t>(race.mode());
    set_single(c, 29, fmuls(race.param(kRivalMulNumSpeed, difficulty), target));
    set_single(c, 30, race.param(kRivalDashRate, difficulty));

    Speed speed{c, race, frame, difficulty};
    switch (race.state()) {
    case 0: speed.countdown(); break;
    case 1: speed.racing(); break;
    case 2: speed.rival_crashed(); break;
    case 3: speed.course_end(); break;
    case 4: speed.game_over(); break;
    case 6: speed.caught(); break;
    case 7: speed.celebration(); break;
    default: break;
    }

    for (int reg = 31; reg >= 23; reg--) {
        uint32_t offset = 0x380 + 0x10 * static_cast<uint32_t>(reg - 23);
        frame.restore_fpr(reg, offset, offset + 8);
    }
    for (int reg = 15; reg <= 31; reg++) {  // _restgpr_15
        frame.restore_gpr(reg, 0x33C + 4 * static_cast<uint32_t>(reg - 15));
    }
}

}  // namespace

}  // namespace musclemarch
