// Shaking: how much a controller sample changes from frame to frame.
//
// Lifted from fn_80042490. The race measures shaking with it: the average absolute change of a
// 16-bit history sample (in practice a controller's sideways tilt, -256 * y) over the last
// `frames` frames, scaled by 1/512. Wiggling a controller side to side makes it large; holding
// it still makes it 0. See race/shake_power.cpp for how it becomes running speed.

#include "input/player_input.h"
#include "wp/lift.h"

namespace musclemarch {

namespace {

using namespace wp;
using namespace wp::lift;
using namespace wp::lift::ppc;

const lift::Group kLifted{"game.lifted", "WP_LIFTED"};

constexpr float kSampleScale = 0.001953125f;  // 1/512
constexpr int32_t kMaxFrames = 30;             // the history has 32 entries

// (PlayerInput*, sample index, frames) -> f1
WP_LIFTED(kLifted, average_sample_change, "PlayerInput_AverageSampleChange", Ret::Float) {
    PlayerInput input;
    input.base = c.r[3];
    uint32_t index = c.r[4];
    int32_t frames = static_cast<int32_t>(c.r[5]);

    OriginalFrame frame(c, 0x50);
    frame.save_fpr(31, 0x40, 0x48);
    frame.save_fpr(30, 0x30, 0x38);
    frame.save_fpr(29, 0x20, 0x28);
    for (int reg = 28; reg <= 31; reg++) {
        frame.save_gpr(reg, 0x10 + 4 * static_cast<uint32_t>(reg - 28));
    }

    float total = 0.0f;
    if (frames <= kMaxFrames) {
        for (int32_t i = 0; i < frames; i++) {
            int32_t change = input.sample(1 + i, index) - input.sample(2 + i, index);
            float step = frame.int_to_float(0x8, change);
            if (!(step >= 0.0f)) {
                step = -step;
            }
            total = fmadds(step, kSampleScale, total);
        }
        total = fdivs(total, frame.int_to_float(0x8, frames));
    }

    frame.restore_fpr(31, 0x40, 0x48);
    frame.restore_fpr(30, 0x30, 0x38);
    frame.restore_fpr(29, 0x20, 0x28);
    c.f[1] = total;
}

}  // namespace

}  // namespace musclemarch
