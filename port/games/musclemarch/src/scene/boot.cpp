// The boot sequence: the notice screens and loading the save before the title.
//
// Lifted from fn_8002B528 (sequence 0 of the game update, scene/game_update.cpp). Sub-state
// (+0x8):
//   0     start
//   1     load the HUD layouts, show the first notice (layout +0x883D8)
//   2     the first notice: any button (0x7F1F) fades it out; every 10 frames the rumble is
//         ticked. Its animation moves on to 3 when it ends.
//   3, 4  hide it, show the second notice (layout +0x883DC)
//   5     the second notice: A, B, 1 or 2 fades it out (its animation moves on to 6)
//   6     hide it, start loading (fn_8003809C)
//   7..9  check, then load the save (10 slots into +0x21C, a value into +0x244)
//   0xA   the controller check (scene/controller_check.cpp) raises its first warning here:
//         the "Wii Remote / Nunchuk required" screen
//   0xB   on to sequence 1
// Feature hooks (features/intro_screens.cpp): skip_intros ends the logo (sub-state 5) on any
// button; no_controller_notice keeps the controller check from raising warning 1.

#include "game_features.h"
#include "input/player_input.h"
#include "race/race.h"
#include "wp/threads.h"

namespace musclemarch {

namespace {

using namespace wp;
using namespace wp::lift;

const lift::Group kLifted{"game.lifted", "WP_LIFTED"};

constexpr uint32_t kGetPad = 0x80020F54;          // (game, channel) -> pad state (+0x10: pressed)
constexpr uint32_t kLayoutHide = 0x8004DAA8;      // (layout)
constexpr uint32_t kLayoutShow = 0x8004DACC;      // (layout)
constexpr uint32_t kFadeOut = 0x80134480;         // (layout, {5, 0x78})
constexpr uint32_t kRumbleState = 0x80042460;     // (input) -> state
constexpr uint32_t kRumbleTick = 0x800423C8;      // (input)
constexpr uint32_t kSaveSystem = 0x80148738;      // () -> save system
constexpr uint32_t kSaveRequest = 0x80149838;     // (save, 1 check / 2 load)
constexpr uint32_t kSaveDone = 0x80149804;        // (save) -> done
constexpr uint32_t kSaveData = 0x80149BDC;        // () -> save data
constexpr uint32_t kSaveSlot = 0x8014A0D8;        // (data, slot) -> value
constexpr uint32_t kSaveValue = 0x8014A11C;       // (data) -> value
constexpr uint32_t kTexts = 0x8026F98C;           // Shift-JIS reports at +0x19, +0x39, +0x53, +0x71

constexpr uint32_t kAnyButton = 0x7F1F;
constexpr uint32_t kConfirmButtons = 0x6C00;      // A, B, 1, 2

struct Main : lift::View {
    WP_FIELD(int32_t, sequence, 0x0)
    WP_FIELD(uint32_t, sub_state, 0x8)
    lift::Field<int32_t> save_slot(uint32_t i) const { return lift::Field<int32_t>(base + 0x21C + 4 * i); }
    WP_FIELD(int32_t, save_value, 0x244)
    WP_FIELD(uint32_t, first_notice, 0x883D8)     // layout holders (+8: the layout; byte 0: visible)
    WP_FIELD(uint32_t, second_notice, 0x883DC)
    WP_FIELD(int32_t, frames, 0x88410)
    WP_FIELD(int32_t, rumble_ticks, 0x8840C)
};

void report(Cpu& c, uint32_t site, uint32_t text) {
    static const uint32_t os_report = symbol_address("OSReport");
    cr_set(c, 6, false);
    call(c, site, os_report, {text});
}

// (Main*)
WP_LIFTED(kLifted, boot_update, "Boot_Update", Ret::None) {
    Main main;
    main.base = c.r[3];
    OriginalFrame frame(c, 0x30);
    frame.save_gpr(31, 0x2C);
    c.r[31] = main.base;
    frame.save_gpr(30, 0x28);
    frame.save_gpr(29, 0x24);

    auto pressed = [&](uint32_t game_site, uint32_t pad_site) {
        uint32_t game = call(c, game_site, kGetGame);
        return rd32(call(c, pad_site, kGetPad, {game, 0}) + 0x10);
    };
    auto fade_out = [&](uint32_t site, uint32_t holder, uint32_t data) {
        wr32(frame.at(data), 5);
        wr8(frame.at(data + 4), 0x78);
        call(c, site, kFadeOut, {rd32(holder + 8), frame.at(data)});
    };
    auto layout_member = [&](uint32_t site, uint32_t offset) { return rd32(call(c, site, kGetGame) + offset); };

    switch (main.sub_state()) {
    case 0:
        main.sub_state() = 1;
        break;
    case 1:
        call(c, 0x8002B57C, kLayoutHide, {layout_member(0x8002B574, 0x20)});
        call(c, 0x8002B588, kLayoutHide, {layout_member(0x8002B580, 0x24)});
        call(c, 0x8002B594, kLayoutShow, {layout_member(0x8002B58C, 0x20)});
        call(c, 0x8002B5A0, kLayoutShow, {layout_member(0x8002B598, 0x24)});
        call(c, 0x8002B5A8, 0x80037948, {main.base});
        call(c, 0x8002B5B0, 0x80037474, {main.base});
        wr8(rd32(main.first_notice() + 8), 0);
        main.sub_state() = 2;
        break;
    case 2: {
        // The wrist-strap notice. Feature skip_strap_screen: a button "press" every 10 frames until
        // the notice takes it (it ignores buttons for its first seconds).
        bool skip = features::skip_strap_screen() && main.frames() % 10 == 0;
        if ((pressed(0x8002B5D4, 0x8002B5DC) & kAnyButton) || skip) {
            fade_out(0x8002B60C, main.first_notice(), 0x10);
        }
        int32_t frames = main.frames();
        main.frames() = frames + 1;
        if (frames % 10 == 0) {
            uint32_t game = call(c, 0x8002B644, kGetGame);
            uint32_t input = call(c, 0x8002B64C, kGetPlayerInput, {game, 0});
            if (call(c, 0x8002B650, kRumbleState, {input}) <= 1) {
                main.rumble_ticks() = main.rumble_ticks() + 1;
            }
            game = call(c, 0x8002B66C, kGetGame);
            input = call(c, 0x8002B674, kGetPlayerInput, {game, 0});
            call(c, 0x8002B678, kRumbleTick, {input});
        }
        break;
    }
    case 3:
        c.r[30] = 1;
        wr8(rd32(main.first_notice() + 8), 1);
        main.sub_state() = 4;
        wr8(call(c, 0x8002B69C, 0x80146A20, {main.base}) + 0x29, static_cast<uint8_t>(c.r[30]));
        break;
    case 4:
        wr8(rd32(main.second_notice() + 8), 0);
        main.sub_state() = 5;
        break;
    case 5:
        // The logo. Feature skip_intros: any button, not only A, B, 1 or 2.
        if (pressed(0x8002B6C8, 0x8002B6D0) & (features::skip_intros() ? features::kAnyButton : kConfirmButtons)) {
            fade_out(0x8002B700, main.second_notice(), 0x8);
        }
        break;
    case 6:
        wr8(rd32(main.second_notice() + 8), 1);
        call(c, 0x8002B71C, 0x8003809C, {main.base});
        main.sub_state() = 7;
        break;
    case 7:
        call(c, 0x8002B72C, 0x80144470, {main.base});
        if (call(c, 0x8002B730, 0x80144690) == 0) {
            call(c, 0x8002B744, kSaveRequest, {call(c, 0x8002B73C, kSaveSystem), 1});
            report(c, 0x8002B758, kTexts + 0x19);  // save data check request
            main.sub_state() = 8;
        }
        break;
    case 8:
        if (call(c, 0x8002B76C, kSaveDone, {call(c, 0x8002B768, kSaveSystem)}) != 0) {
            c.r[30] = kTexts;
            report(c, 0x8002B788, kTexts + 0x39);  // save data check done
            call(c, 0x8002B794, kSaveRequest, {call(c, 0x8002B78C, kSaveSystem), 2});
            report(c, 0x8002B7A0, kTexts + 0x53);  // save data load request
            main.sub_state() = 9;
        }
        break;
    case 9:
        if (call(c, 0x8002B7B4, kSaveDone, {call(c, 0x8002B7B0, kSaveSystem)}) != 0) {
            report(c, 0x8002B7D0, kTexts + 0x71);  // save data load done
            c.r[30] = main.base;
            for (c.r[29] = 0; static_cast<int32_t>(c.r[29]) < 10; c.r[30] += 4) {
                uint32_t data = call(c, 0x8002B7DC, kSaveData);
                uint32_t value = call(c, 0x8002B7E4, kSaveSlot, {data, c.r[29]});
                c.r[29]++;
                wr32(c.r[30] + 0x21C, value);
            }
            uint32_t data = call(c, 0x8002B7FC, kSaveData);
            main.save_value() = static_cast<int32_t>(call(c, 0x8002B800, kSaveValue, {data}));
            main.sub_state() = 0xA;
        }
        break;
    case 0xA:
        main.sub_state() = 0xB;
        break;
    case 0xB:
        main.sequence() = 1;
        break;
    default:
        break;
    }

    frame.restore_gpr(31, 0x2C);
    frame.restore_gpr(30, 0x28);
    frame.restore_gpr(29, 0x24);
}

}  // namespace

}  // namespace musclemarch
