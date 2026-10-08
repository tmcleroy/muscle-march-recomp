// The race scene: its phases, and the frame that drives the race.
//
// Lifted from fn_8002C544 (the race scene's per-frame update). The scene phase (race +0x4):
//   0  set up (stage objects, fade in), 1  wait for loading, 2  start (HUD, menus by game kind),
//   4  race start (counters reset, the running sound), 5  wait for the intro to finish,
//   6  the race frame (below), 7/8  race end (results set up), 9  results: next stage, retry,
//      or back to the menu.
// The race frame: count frames by game kind (+0x54: 0 story, 1 endless, 2 attract demo, which
// fades out after +0x1FC frames); the countdown (race state 0, 300 frames), pause (state 1,
// button 0x200), the rival-crash slow motion (state 2), the course-end / game-over results
// (states 3, 4; A/B restart, + skips, after 180 frames the next screen); then shake power,
// the race update and the other per-frame systems; finally the wall bookkeeping when the
// player enters a new segment (wall cleared: combo +1, the "pass" sound), the race mode for the
// segment (+0x198: 1 = the special runner's dash zone, 3 = course end), and the running effect.
// Guest registers r31/f30/f31 hold the original's values at every call (see race.h).

#include "input/player_input.h"
#include "race/race.h"
#include "wp/threads.h"

namespace musclemarch {

namespace {

using namespace wp;
using namespace wp::lift;
using namespace wp::lift::ppc;

const lift::Group kLifted{"game.lifted", "WP_LIFTED"};

constexpr uint32_t kCurrentRace = 0x804223D0;
constexpr uint32_t kSoundManager = 0x80422568;
constexpr uint32_t kZero = 0x80421184;            // .sdata 0.0f (std::max/min operands)
constexpr uint32_t kOne = 0x80421188;             // .sdata 1.0f

// Game kinds (race +0x54).
constexpr int32_t kStory = 0;
constexpr int32_t kEndless = 1;
constexpr int32_t kDemo = 2;

// Guest functions (names describe what the caller uses them for).
constexpr uint32_t kGetPad = 0x80020F54;          // (game, channel) -> pad state (+0x10: pressed buttons)
constexpr uint32_t kTimeScale = 0x80039554;       // (timer, f1)
constexpr uint32_t kTimeScaleTarget = 0x8003959C; // (timer, f1)
constexpr uint32_t kTimeScaleRate = 0x800395A4;   // (timer, f1)
constexpr uint32_t kTimerStep = 0x800395AC;       // (timer, f1) -> f1
constexpr uint32_t kScreenFade = 0x8002315C;      // (fader, 0, 0, f1)
constexpr uint32_t kScreenFadeReset = 0x80023148; // (fader)
constexpr uint32_t kRaceFade = 0x80036458;        // (race, f1)
constexpr uint32_t kRaceEnd = 0x80036414;         // (race)
constexpr uint32_t kPlaySound = 0x8003652C;       // (race, bank, sound)
constexpr uint32_t kSegmentAt = 0x8003BA48;
constexpr uint32_t kCrossesSegment = 0x8003C280;
constexpr uint32_t kSegmentLength = 0x8003B640;
constexpr uint32_t kCourseEnd = 0x8003B614;
constexpr uint32_t kTiltedNow = 0x80042584;       // (input, 0 Remote / 1 Nunchuk) -> just tilted sideways

struct Scene {
    Cpu& c;
    Race race;
    OriginalFrame& frame;

    uint32_t timer() const { return race.base + 0x70; }
    int32_t kind() const { return static_cast<int32_t>(rd32(race.base + 0x54)); }
    Field<int32_t> at(uint32_t offset) const { return Field<int32_t>(race.base + offset); }
    Field<float> f_at(uint32_t offset) const { return Field<float>(race.base + offset); }

    // r3 = race for the callees that take it.
    void race_call(uint32_t site, uint32_t function) { call(c, site, function, {race.base}); }
    uint32_t game(uint32_t site) { return call(c, site, kGetGame); }
    uint32_t game_member(uint32_t site, uint32_t offset) { return rd32(game(site) + offset); }
    uint32_t pressed(uint32_t game_site, uint32_t pad_site) {
        uint32_t g = game(game_site);
        return rd32(call(c, pad_site, kGetPad, {g, 0}) + 0x10);
    }
    void fade(uint32_t game_site, uint32_t fade_site, double amount) {
        uint32_t fader = game_member(game_site, 0x58);
        call(c, fade_site, kScreenFade, {fader, 0, 0}, {amount});
    }

    void setup();
    void wait_loading();
    void start();
    void race_start();
    void wait_intro();
    void frame_update();
    void countdown_and_states();
    void results_screen(int32_t state);
    void segment_bookkeeping();
    void race_ended();
    void results_setup();
    void results();
};

// Phase 0.
void Scene::setup() {
    race_call(0x8002C598, 0x80037AD0);
    race_call(0x8002C5A0, 0x80037C7C);
    race_call(0x8002C5A8, 0x80037D74);
    race_call(0x8002C5B0, 0x80037EDC);
    call(c, 0x8002C5BC, kScreenFadeReset, {game_member(0x8002C5B4, 0x58)});
    fade(0x8002C5C0, 0x8002C5D4, 1.0);
    if (kind() == kStory) {
        race_call(0x8002C5E8, 0x80035088);
        if (race.difficulty() > 0) {
            race_call(0x8002C5FC, 0x80035158);
        }
    }
    at(0x4) = 1;
}

// Phase 1.
void Scene::wait_loading() {
    call(c, 0x8002C60C, 0x80144470);
    if (call(c, 0x8002C610, 0x80144690) != 0) {
        return;
    }
    race_call(0x8002C620, 0x800360E8);
    race_call(0x8002C628, 0x800361C4);
    at(0x4) = 2;
}

// Phase 2.
void Scene::start() {
    int32_t k = kind();
    if (k == kStory) {
        race_call(0x8002C644, 0x80034B68);
        race_call(0x8002C64C, 0x8003029C);
    } else if (k == kDemo) {
        race_call(0x8002C65C, 0x80034B68);
        race_call(0x8002C664, 0x8003029C);
    } else if (k == kEndless) {
        race_call(0x8002C674, 0x8003029C);
    }
    if (static_cast<uint32_t>(kind()) <= 1) {
        call(c, 0x8002C68C, 0x80022F54, {game_member(0x8002C684, 0x54)});
        call(c, 0x8002C698, 0x8004DACC, {game_member(0x8002C690, 0x20)});
        call(c, 0x8002C6A4, 0x8004DACC, {game_member(0x8002C69C, 0x24)});
        if (kind() == kEndless) {
            race.table_value() = 0x32;
        }
        race_call(0x8002C6C0, 0x800378E0);
        race_call(0x8002C6C8, 0x80037384);
    } else if (kind() == kDemo) {
        call(c, 0x8002C6E0, 0x80022D90, {game_member(0x8002C6D8, 0x54)});
    }
    at(0x4) = 4;
}

// Phase 4.
void Scene::race_start() {
    if (static_cast<uint32_t>(kind()) <= 1) {
        race_call(0x8002C6FC, 0x80037730);
    }
    call(c, 0x8002C708, 0x80022F68, {game_member(0x8002C700, 0x54)});
    race_call(0x8002C710, 0x800365EC);
    race_call(0x8002C718, 0x8003089C);
    race_call(0x8002C720, 0x8002AA5C);
    race.field_408() = 0;
    if (!(0 <= race.field_410())) {
        race.field_410() = 0;
    }
    race.combo() = 0;
    at(0x20C) = 0;
    race.field_410() = 0;
    race.rival_crashes() = 0;
    race.crashes() = 0;
    race.finish_distance() = 0;
    at(0x44C) = 0;
    at(0x450) = 0;
    at(0x42C) = 0;
    call(c, 0x8002C770, kPlaySound, {race.base, 1, 2});
    at(0x4) = 5;
    call(c, 0x8002C780, 0x80036278, {rd32(kCurrentRace)});
}

// Phase 5.
void Scene::wait_intro() {
    if (call(c, 0x8002C788, 0x80036348, {race.base}) == 0) {
        return;
    }
    race_call(0x8002C798, 0x800363B0);
    call(c, 0x8002C7A0, 0x8013B498, {race.base + 0x1E4});
    switch (static_cast<int32_t>(rd32(race.base + 0x88330))) {
    case 0: f_at(0x1EC) = 0.355029f; break;
    case 1: f_at(0x1EC) = 0.428571f; break;
    case 2: f_at(0x1EC) = 0.47619f; break;
    case 3: f_at(0x1EC) = 0.33f; break;
    default: break;
    }
    call(c, 0x8002C804, kScreenFadeReset, {game_member(0x8002C7FC, 0x58)});
    at(0x4) = 6;
}

// Race states 3 and 4: the results overlay over the stopped race.
void Scene::results_screen(int32_t state) {
    if (state == 3) {
        race.field_444() = 1;
    } else if (state == 4 && kind() == kStory) {
        race.field_444() = 0;
    }
    if (at(0x204) != 0) {
        if (pressed(0x8002CC1C, 0x8002CC24) & 0x4800) {
            at(0x454) = 1;
            at(0x4) = 4;  // retry
        }
        if (pressed(0x8002CC44, 0x8002CC4C) & 0x2400) {
            wr32(frame.at(0x10), 5);
            wr8(frame.at(0x14), 0x78);
            uint32_t menu = rd32(rd32(race.base + 0x883D0) + 8);
            call(c, 0x8002CC7C, 0x80134480, {menu, frame.at(0x10)});
        }
        if (kind() == kStory) {
            call(c, 0x8002CC94, 0x80022E98, {game_member(0x8002CC8C, 0x54)});
        }
        call(c, 0x8002CCA0, 0x80022A3C, {game_member(0x8002CC98, 0x54)});
        return;
    }
    int32_t time = frame.float_to_int(0x28, f_at(0x18C).get());
    if (time == 0) {
        call(c, 0x8002CCC8, kRaceFade, {rd32(kCurrentRace)}, {92.0f});
    } else {
        time = frame.float_to_int(0x28, f_at(0x18C).get());
        if (!(frame.int_to_float(0x18, time) <= 180.0f)) {
            race_call(0x8002CCFC, 0x800365EC);
            if (kind() == kStory) {
                for (uint32_t i = 1; i < 5; i++) {
                    Runner{{race.runner(i)}}.marker() = 2;
                }
                at(0x204) = 1;
                call(c, 0x8002CD38, 0x80023018, {game_member(0x8002CD30, 0x54)});
            } else {
                at(0x4) = 8;
                fade(0x8002CD48, 0x8002CD5C, 1.0);
            }
        }
    }
    if (kind() == kEndless && !(f_at(0x18C) <= 150.0f)) {
        set_single(c, 31, fdivs(fsubs(f_at(0x18C), 150.0f), 30.0f));
        fade(0x8002CD88, 0x8002CD9C, c.f[31]);
    }
    f_at(0x18C) = fadds(f_at(0x18C), 1.0f);
}

// The race states the scene handles itself.
void Scene::countdown_and_states() {
    call(c, 0x8002C954, 0x8013B4B4, {race.base + 0x1E4});
    int32_t state = static_cast<int32_t>(race.state());
    race.field_430() = 0;
    if (static_cast<uint32_t>(state - 3) <= 1) {
        results_screen(state);
        return;
    }
    if (state == 0) {
        // The countdown: 300 frames (the demo starts at once).
        race.countdown() = race.countdown() + 1;
        call(c, 0x8002C9A0, kTimeScale, {timer()}, {1.0f});
        if (frame.int_to_float(0x20, race.countdown()) > 300.0f || kind() == kDemo) {
            f_at(0x178) = fsubs(Runner{{race.runner(0)}}.distance(), 2.5f);
            race_call(0x8002C9E8, 0x80030B94);
            race.state() = 1;
            race.countdown() = 1;
        }
    } else if (state == 1) {
        call(c, 0x8002CA04, kTimeScale, {timer()}, {1.0f});
        if (kind() != kDemo && (pressed(0x8002CA14, 0x8002CA1C) & 0x200)) {
            uint32_t g = game(0x8002CA2C);
            call(c, 0x8002CA34, 0x80021824, {g, 1});
            call(c, 0x8002CA40, 0x8003681C, {race.base, 1});  // pause
        }
    } else if (state == 2) {
        if (race.rival_crash_shown() == 0) {
            // A rival crashed: ease time down and back up over 70 frames.
            int32_t n = race.field_8a0f8() + 1;
            race.field_8a0f8() = n;
            float v;
            if (n < 10) {
                float x = fsubs(fdivs(frame.int_to_float(0x18, n), 10.0f), 1.0f);
                v = fmadds(-x, x, 1.0f);
            } else if (n < 30) {
                v = 1.0f;
            } else {
                v = fsubs(1.0f, fdivs(frame.int_to_float(0x20, n - 0x1E), 40.0f));
            }
            stf(frame.at(0x8), v);
            uint32_t p = !(ldf(kZero) >= v) ? frame.at(0x8) : kZero;  // std::max(v, 0)
            if (!(ldf(kOne) >= ldf(p))) {
                p = kOne;  // std::min(.., 1)
            }
            float clamped = ldf(p);
            float rate = fmadds(0.05f, clamped, fsubs(1.0f, clamped));
            stf(frame.at(0x8), clamped);
            call(c, 0x8002CB2C, kTimeScaleTarget, {timer()}, {rate});
            f_at(0x8A0FC) = fmuls(0.5f, ldf(frame.at(0x8)));
            if (race.field_8a0f8() > 0x46) {
                race.state() = 1;
                race.rival_crash_shown() = -1;
            }
        } else {
            uint32_t rival = static_cast<uint32_t>(race.target_runner() - 1);
            if (static_cast<uint32_t>(Runner{{race.runner(rival)}}.animation() - 8) <= 2) {
                call(c, 0x8002CB90, kTimeScale, {timer()}, {0.11f});
            } else {
                call(c, 0x8002CBA0, kTimeScale, {timer()}, {0.08f});
                call(c, 0x8002CBAC, kTimeScaleRate, {timer()}, {0.03f});
            }
            float left = fsubs(race.field_17c(), 1.0f);
            race.field_17c() = left;
            if (!(left >= 0.0f)) {
                race.state() = 1;
                race.rival_crash_shown() = -1;
            }
        }
    }
}

// Entering a new segment: the wall just passed is counted, and the next segment sets the race
// mode (+0x198) and the running effect (+0x57C74).
void Scene::segment_bookkeeping() {
    set_single(c, 30, fadds(Runner{{race.runner(0)}}.distance(), -1.0f));  // f30: just behind the player
    at(0x16C) = at(0x168).get();
    int32_t segment = static_cast<int32_t>(call(c, 0x8002CE60, kSegmentAt, {race.course()}, {c.f[30]}));
    int32_t previous = at(0x16C);
    at(0x168) = segment;
    if (segment != previous && race.flag_74(static_cast<uint32_t>(previous)) == 0) {
        switch (race.wall_kind(previous)) {
        case 0: at(0x164) = 0; break;
        case 1: at(0x164) = 1; break;
        case 2: at(0x164) = 2; break;
        case 3: at(0x164) = 3; break;
        default: break;
        }
        if (race.wall(previous) != 2) {  // the wall was passed with the right pose
            f_at(0x190) = 20.0f;
            call(c, 0x8002CF18, kPlaySound, {race.base, 1, 9});
            wr32(rd32(kSoundManager) + 0x10, 1);
        }
        if (static_cast<uint32_t>(kind()) <= 1) {
            int32_t streak = race.field_408() + 1;
            race.field_408() = streak;
            if (streak > race.field_410()) {
                race.field_410() = streak;
            }
            race.combo() = race.combo() + 1;
            at(0x460) = at(0x460) + 1;
        }
        if (kind() == kEndless) {
            if (at(0x20C) < 999) {
                at(0x20C) = at(0x20C) + 1;
            }
            at(0x3E4 + 4 * static_cast<uint32_t>(at(0x3DC).get())) = at(0x20C).get();
        }
        race.field_430() = race.gauge_streak() < 10 ? 3 : 1;
    }

    c.f[31] = call_float(c, 0x8002CFC8, kSegmentLength, {race.course(), static_cast<uint32_t>(at(0x168).get())});  // fmr
    float length = static_cast<float>(c.f[31]);
    if (at(0x168) > 2) {
        if (!(length <= 100.0f) && call(c, 0x8002CFF8, kCrossesSegment, {race.course()}, {c.f[30], 40.0f}) == 0) {
            if (race.target_runner() == 4) {
                race.mode() = 1;  // the special runner's dash zone
            } else {
                race.mode() = 0;
                at(0x53448) = 0x35;
            }
            if (race.state() == 3) {
                race.mode() = 3;
            }
        } else {
            race.mode() = 0;
        }
    }

    // The running effect.
    int32_t k = kind();
    if (k == kStory) {
        if (race.mode() == 0) {
            bool fast = !(length <= 47.0f) && call(c, 0x8002D084, kCrossesSegment, {race.course()}, {c.f[30], 18.0f}) == 0 && at(0x168) > 0;
            static constexpr float kFast[2][3] = {{1.8f, 1.9f, 2.0f}, {2.2f, 2.3f, 2.4f}};
            static constexpr float kNormal[2][3] = {{1.7f, 1.8f, 1.9f}, {2.0f, 2.1f, 2.3f}};
            int special = race.target_runner() == 4 ? 1 : 0;
            int32_t d = race.difficulty();
            if (d >= 0 && d <= 2) {
                race.effect_scale() = fast ? kFast[special][d] : kNormal[special][d];
            }
            at(0x53448) = fast ? 0x30 : 0x35;
        }
    } else if (k == kEndless) {
        race.effect_scale() = 2.0f;
    } else if (k == kDemo) {
        race.effect_scale() = 1.7f;
    }

    if (kind() == kStory) {
        set_single(c, 31, Runner{{race.runner(0)}}.distance());
        float end = call_float(c, 0x8002D228, kCourseEnd, {race.course()});
        at(0x424) = frame.float_to_int(0x28, fsubs(end, static_cast<float>(c.f[31])));
    } else {
        at(0x424) = 0;
    }
    // The demo: A, B, 1 or 2 skips to its last 30 frames.
    if (kind() == kDemo && (pressed(0x8002D258, 0x8002D260) & 0x6C00)) {
        int32_t last = at(0x1FC) - 0x1E;
        if (at(0x1D0) < last) {
            at(0x1D0) = last;
            wr32(race.base + 0x88400, 1);
            call(c, 0x8002D29C, 0x800364F4, {race.base, 0x12});
        }
    }
}

// Phase 6.
void Scene::frame_update() {
    int32_t k = kind();
    at(0x44C) = at(0x44C) + 1;
    if (k == kStory) {
        at(0x1D8) = at(0x1D8) + 1;
    } else if (k == kEndless) {
        at(0x1DC) = at(0x1DC) + 1;
    } else if (k == kDemo) {
        at(0x1D0) = at(0x1D0) + 1;
        call(c, 0x8002C870, 0x80022A3C, {game_member(0x8002C868, 0x54)});
        call(c, 0x8002C87C, 0x80022CD4, {game_member(0x8002C874, 0x54)});
        int32_t limit = at(0x1FC);
        int32_t frames = at(0x1D0);
        if (frames > limit) {
            call(c, 0x8002C894, kRaceEnd, {rd32(kCurrentRace)});
            fade(0x8002C898, 0x8002C8AC, 1.0);
            at(0x5C) = 2;
            at(0x4) = 8;
            at(0x1D0) = 0;
        } else if (frames > limit - 0x1E) {
            set_single(c, 31, fdivs(frame.int_to_float(0x18, frames - (limit - 0x1E)), 30.0f));
            fade(0x8002C8F8, 0x8002C90C, c.f[31]);
            call(c, 0x8002C918, kRaceFade, {rd32(kCurrentRace)}, {45.0f});
        } else {
            uint32_t g = game(0x8002C920);
            uint32_t input = call(c, 0x8002C928, kGetPlayerInput, {g, 0});
            if (call(c, 0x8002C930, kTiltedNow, {input, 1}) == 0) {
                g = game(0x8002C93C);
                input = call(c, 0x8002C944, kGetPlayerInput, {g, 0});
                call(c, 0x8002C94C, kTiltedNow, {input, 0});
            }
        }
    }

    countdown_and_states();

    // The race systems.
    static constexpr uint32_t kSystems[9][2] = {
        {0x8002CDB4, 0x8002F09C}, {0x8002CDBC, 0x80030EB8}, {0x8002CDC4, 0x800287A4}, {0x8002CDCC, 0x800353C0}, {0x8002CDD4, 0x8002FBDC},
        {0x8002CDDC, 0x80023704}, {0x8002CDE4, 0x8002FF38}, {0x8002CDEC, 0x80033854}, {0x8002CDF4, 0x80028D20},
    };
    for (const auto& system : kSystems) {
        race_call(system[0], system[1]);
    }
    wr8(race.base + 0x8A100, 1);
    float step = call_float(c, 0x8002CE0C, kTimerStep, {timer()}, {race.speed_setting()});
    f_at(0x174) = fadds(f_at(0x174), step);
    if (race.state() != 0) {
        step = call_float(c, 0x8002CE30, kTimerStep, {timer()}, {Runner{{race.runner(0)}}.speed()});
        f_at(0x178) = fadds(f_at(0x178), step);
    }
    segment_bookkeeping();
}

// Phase 7.
void Scene::race_ended() {
    race_call(0x8002D2A4, kRaceEnd);
    race_call(0x8002D2AC, 0x800365EC);
    at(0x4) = 8;
}

// Phase 8.
void Scene::results_setup() {
    race_call(0x8002D2BC, 0x80030930);
    race_call(0x8002D2C4, 0x8003671C);
    race_call(0x8002D2CC, 0x8003678C);
    race_call(0x8002D2D4, 0x800365EC);
    int32_t k = kind();
    if (k == kStory) {
        int32_t outcome = race.field_444();
        if (outcome >= 0 && outcome <= 2) {
            at(0x448) = outcome;
        }
        call(c, 0x8002D32C, 0x8004DACC, {game_member(0x8002D324, 0x20)});
        call(c, 0x8002D338, 0x8004DACC, {game_member(0x8002D330, 0x24)});
        race_call(0x8002D340, 0x8003777C);
        race_call(0x8002D348, 0x80037FF0);
        outcome = race.field_444();
        if (static_cast<uint32_t>(outcome - 4) <= 1) {
            race_call(0x8002D3AC, 0x80035088);
        } else if (outcome == 0) {
            race_call(0x8002D37C, 0x80035088);
        } else if (outcome == 1) {
            race_call(0x8002D388, 0x80035088);
        } else if (outcome == 2 && static_cast<uint32_t>(race.difficulty()) <= 1) {
            race_call(0x8002D3A0, 0x80035088);
        }
    } else if (k == kEndless) {
        call(c, 0x8002D3C4, 0x8004DACC, {game_member(0x8002D3BC, 0x20)});
        call(c, 0x8002D3D0, 0x8004DACC, {game_member(0x8002D3C8, 0x24)});
        race_call(0x8002D3D8, 0x8003777C);
        race_call(0x8002D3E0, 0x80037FF0);
    } else if (k == kDemo) {
        race_call(0x8002D3F4, 0x80035088);
    }
    at(0x4) = 9;
}

// Phase 9: where to go after the results (race +0x0 is the next scene).
void Scene::results() {
    call(c, 0x8002D404, 0x80144470);
    if (call(c, 0x8002D408, 0x80144690) != 0) {
        return;
    }
    int32_t k = kind();
    if (k == kStory) {
        int32_t outcome = race.field_444();
        if (static_cast<uint32_t>(outcome) <= 1) {
            race_call(0x8002D44C, 0x80037848);
            race_call(0x8002D454, 0x80036FF4);
            at(0x0) = 0xA;
        } else if (outcome == 2) {
            if (race.field_410() > at(0x414)) {
                at(0x414) = race.field_410();
            }
            switch (race.difficulty()) {
            case 0: at(0x0) = 7; race.difficulty() = 1; break;
            case 1: at(0x0) = 7; race.difficulty() = 2; break;
            case 2:
                race_call(0x8002D4C4, 0x80037848);
                race_call(0x8002D4CC, 0x80036FF4);
                race_call(0x8002D4D4, 0x80034DB8);
                at(0x0) = 9;
                break;
            default: break;
            }
        } else if (outcome == 4) {
            at(0x5C) = 2;
            at(0x0) = 1;
        } else if (outcome == 5) {
            at(0x46C) = 1;
        }
        wr32(race.base + 0x8A0E0, 0);
    } else if (k == kEndless) {
        int32_t outcome = race.field_444();
        if (static_cast<uint32_t>(outcome) <= 2) {
            if (at(0x3DC) + 1 == at(0x3E0)) {
                race_call(0x8002D550, 0x80037848);
                race_call(0x8002D558, 0x80036FF4);
                race_call(0x8002D560, 0x8002F3B4);
                wr32(race.base + 0x8A0E0, 0);
                at(0x0) = 0xC;
                race.field_444() = 3;
            } else {
                race_call(0x8002D588, 0x80037894);
                race_call(0x8002D590, 0x80037234);
                race_call(0x8002D598, 0x80030B04);
                at(0x0) = 0xB;
                at(0x3DC) = at(0x3DC) + 1;
            }
        } else if (outcome == 4) {
            at(0x5C) = 2;
            at(0x0) = 1;
        } else if (outcome == 5) {
            at(0x20C) = 0;
            at(0x3E4 + 4 * static_cast<uint32_t>(at(0x3DC).get())) = 0;
            at(0x46C) = 1;
        }
    } else if (k == kDemo) {
        int32_t shown = at(0x1F4);
        at(0x0) = 1;
        if (shown == 0) {
            int32_t next = at(0x1C4);
            if (next == 0) {
                at(0x1C4) = 1;
            } else if (next == 1) {
                at(0x1C4) = 2;
            } else if (next == 2) {
                at(0x1C4) = 0;
            }
            at(0x1F4) = 1;
        } else if (shown == 1) {
            at(0x1F4) = 0;
        }
    }
    race_call(0x8002D660, 0x8002AFE4);
}

// (Race*). Only the race frame (phase 6) is verified: the other phases wait for loading and
// saving, which the verifier holds back.
WP_LIFTED_WHEN(kLifted, race_scene, "Race_SceneUpdate", Ret::None, rd32(c.r[3] + 0x4) == 6) {
    Race race;
    race.base = c.r[3];
    OriginalFrame frame(c, 0x60);
    frame.save_fpr(31, 0x50, 0x58);
    frame.save_fpr(30, 0x40, 0x48);
    frame.save_gpr(31, 0x3C);
    c.r[31] = race.base;
    wr32(frame.at(0x18), 0x43300000);
    wr32(frame.at(0x20), 0x43300000);

    Scene scene{c, race, frame};
    switch (rd32(race.base + 0x4)) {
    case 0: scene.setup(); break;
    case 1: scene.wait_loading(); break;
    case 2: scene.start(); break;
    case 4: scene.race_start(); break;
    case 5: scene.wait_intro(); break;
    case 6: scene.frame_update(); break;
    case 7: scene.race_ended(); break;
    case 8: scene.results_setup(); break;
    case 9: scene.results(); break;
    default: break;
    }

    frame.restore_fpr(31, 0x50, 0x58);
    frame.restore_fpr(30, 0x40, 0x48);
    frame.restore_gpr(31, 0x3C);
}

}  // namespace

}  // namespace musclemarch
