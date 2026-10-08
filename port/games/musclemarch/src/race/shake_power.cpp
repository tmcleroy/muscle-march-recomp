// Shake power and the pose gauges, updated once per race frame.
//
// Lifted from fn_8002F09C (called every race frame from fn_8002C544).
//   1. Shake power (race +0x5D4, 0..1): both controllers' sideways shaking over the last 20
//      frames (input/shake.cpp), times 2 and times GACHA_LEVER_POWER, summed and capped at 1.
//      In auto-dash mode (race +0x198 == 2) it is AUTO_DASH_RATE for the difficulty instead.
//      The race speed code (fn_80032420) reads it.
//   2. Pose gauges: the player's four pose meters (0..1) are copied into the race object and
//      scaled to a frame of each gauge's HUD animation.

#include "input/player_input.h"
#include "wp/lift.h"

namespace musclemarch {

namespace {

using namespace wp;
using namespace wp::lift;
using namespace wp::lift::ppc;

const lift::Group kLifted{"game.lifted", "WP_LIFTED"};

constexpr uint32_t kAverageSampleChange = 0x80042490;  // input/shake.cpp
constexpr uint32_t kLayoutValue = 0x8004D18C;          // (layout, id) -> int

constexpr int32_t kShakeFrames = 20;
constexpr float kShakeScale = 2.0f;
constexpr int32_t kAutoDashMode = 2;

// HUD layout ids of the four pose gauges' animations (pose 0..3).
constexpr uint32_t kGaugeAnimation[4] = {0x2B, 0x27, 0x29, 0x28};

// The race object (tuning parameters from +0x484 are the debug menu's, docs/GAME.md).
struct Race : lift::View {
    WP_FIELD(int32_t, mode, 0x198)
    WP_FIELD(uint32_t, difficulty, 0x1C0)                    // 0 easy, 1 normal, 2 hard
    lift::Field<float> auto_dash_rate(uint32_t difficulty) const { return lift::Field<float>(base + 0x4EC + 4 * difficulty); }
    WP_FIELD(float, gacha_lever_power, 0x5B8)
    WP_FIELD(float, shake_power, 0x5D4)
    WP_FIELD(uint32_t, hud_layout, 0x64074)
    lift::Field<float> pose_meter(uint32_t pose) const { return lift::Field<float>(base + 0x6D890 + 4 * pose); }
    lift::Field<float> gauge_frame(uint32_t pose) const { return lift::Field<float>(base + 0x6D8A0 + 4 * pose); }
};

// The original's calls to the shaking measure return here (the callee saves LR on the stack).
constexpr uint32_t kRemoteShakeReturn = 0x8002F0F0;
constexpr uint32_t kNunchukShakeReturn = 0x8002F120;

float shake(Cpu& c, uint32_t return_address, PlayerInput input, uint32_t sample) {
    gcall_from(c, return_address, kAverageSampleChange, {input.base, sample, kShakeFrames});
    return static_cast<float>(c.f[1]);
}

// lfs: both halves of the paired single.
void load_single(Cpu& c, int reg, float value) {
    c.f[reg] = value;
    c.ps1[reg] = value;
}

// (Race*)
WP_LIFTED(kLifted, update_shake_power, "Race_UpdateShakePower", Ret::None) {
    Race race;
    race.base = c.r[3];

    // The original keeps the race in r31 and the lever power in f31/f30 (saved here first).
    OriginalFrame frame(c, 0x40);
    frame.save_fpr(31, 0x30, 0x38);
    frame.save_fpr(30, 0x20, 0x28);
    frame.save_gpr(31, 0x1C);
    c.r[31] = race.base;
    wr32(frame.at(0x8), 0x43300000);
    wr32(frame.at(0x10), 0x43300000);

    load_single(c, 31, race.gacha_lever_power());
    race.shake_power() = 0.0f;
    float remote = fmuls(kShakeScale, shake(c, kRemoteShakeReturn, game_player_input(c, 0), kRemoteSideTiltSample));
    load_single(c, 30, race.gacha_lever_power());
    race.shake_power() = fmadds(remote, static_cast<float>(c.f[31]), race.shake_power());
    float nunchuk = fmuls(kShakeScale, shake(c, kNunchukShakeReturn, game_player_input(c, 0), kNunchukSideTiltSample));
    race.shake_power() = fmadds(nunchuk, static_cast<float>(c.f[30]), race.shake_power());
    if (race.mode() == kAutoDashMode) {
        race.shake_power() = race.auto_dash_rate(race.difficulty());
    }
    if (!(race.shake_power() <= 1.0f)) {
        race.shake_power() = 1.0f;
    }

    for (uint32_t pose = 0; pose < 4; pose++) {
        race.pose_meter(pose) = game_player_input(c, 0).meter(pose);
    }
    for (uint32_t pose = 0; pose < 4; pose++) {
        if (!(race.pose_meter(pose) <= 1.0f)) {
            race.pose_meter(pose) = 1.0f;
        }
    }
    for (uint32_t pose = 0; pose < 4; pose++) {
        int32_t frames = static_cast<int32_t>(gcall(c, kLayoutValue, {race.hud_layout(), kGaugeAnimation[pose]}));
        float last_frame = fsubs(frame.int_to_float(pose % 2 == 0 ? 0x8 : 0x10, frames), 1.0f);
        float value = fmuls(race.pose_meter(pose), last_frame);
        race.gauge_frame(pose) = value;
        if (!(value >= 0.0f)) {
            race.gauge_frame(pose) = 0.0f;
        }
    }

    frame.restore_fpr(31, 0x30, 0x38);
    frame.restore_fpr(30, 0x20, 0x28);
    frame.restore_gpr(31, 0x1C);
}

}  // namespace

}  // namespace musclemarch
