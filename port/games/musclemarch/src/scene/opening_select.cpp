// The opening movie and the select menu.
//
// Lifted from fn_8002BE38 (sequence 4) and fn_8002C0D0 (sequence 6) of the game update.
//   Sequence 4, the opening (step +0x28): 0 hide the screen that led here (title, stage card or
//   results), start the opening (the attract demo's when game kind is 2; layout +0x883D4 or
//   +0x883AC); 1 play it; A, B, 1 or 2 (0x6C00) skips it (+0x883FC) by sending 'x' to its layout.
//   Sequence 6, the select menu (step +0x30): 0 set up (layout +0x883B0, the pointer on);
//   1 the menu: the pointer, or 'a' 'd' 'w' 's' (d-pad), 'x' (A/1), 'z' (B/2).
// Feature hook: skip_intros (features/intro_screens.cpp) skips the opening with any button.

#include "game_features.h"
#include "input/player_input.h"
#include "race/race.h"

namespace musclemarch {

namespace {

using namespace wp;
using namespace wp::lift;

const lift::Group kLifted{"game.lifted", "WP_LIFTED"};

constexpr uint32_t kGetPad = 0x80020F54;
constexpr uint32_t kPointerValid = 0x80042FCC;
constexpr uint32_t kPointerX = 0x80042E58;
constexpr uint32_t kPointerY = 0x80042E74;
constexpr uint32_t kSetPointer = 0x80042E90;
constexpr uint32_t kResetLayout = 0x80134348;
constexpr uint32_t kSendEvent = 0x80134480;

constexpr uint32_t kSkipButtons = 0x6C00;  // A, B, 1, 2

// Buttons that skip the opening (feature skip_intros: any button).
uint32_t skip_buttons() {
    return features::skip_intros() ? features::kAnyButton : kSkipButtons;
}

struct Main : lift::View {
    WP_FIELD(int32_t, opening_step, 0x28)
    WP_FIELD(int32_t, select_step, 0x30)
    WP_FIELD(int32_t, kind, 0x54)
    WP_FIELD(int32_t, next_scene_mode, 0x5C)
    WP_FIELD(int32_t, returning, 0x208)
    WP_FIELD(int32_t, select_x, 0x59C8C)
    WP_FIELD(float, select_y, 0x59C90)
    WP_FIELD(int32_t, select_z, 0x59C94)
    WP_FIELD(uint32_t, title_layout, 0x883A8)     // layout holders (+8: the layout; byte 0: hidden)
    WP_FIELD(uint32_t, first_banner, 0x883AC)
    WP_FIELD(uint32_t, select_layout, 0x883B0)
    WP_FIELD(uint32_t, stage_card, 0x883B4)
    WP_FIELD(uint32_t, results_layout, 0x883C4)
    WP_FIELD(uint32_t, returning_banner, 0x883D4)
    WP_FIELD(int32_t, field_883f4, 0x883F4)
    WP_FIELD(int32_t, field_883f8, 0x883F8)
    WP_FIELD(int32_t, opening_skipped, 0x883FC)
};

uint32_t layout(uint32_t holder) {
    return rd32(holder + 8);
}

uint32_t game_member(Cpu& c, uint32_t site, uint32_t offset) {
    return rd32(call(c, site, kGetGame) + offset);
}

uint32_t pressed(Cpu& c, uint32_t game_site, uint32_t pad_site) {
    uint32_t game = call(c, game_site, kGetGame);
    return rd32(call(c, pad_site, kGetPad, {game, 0}) + 0x10);
}

uint32_t input(Cpu& c, uint32_t game_site, uint32_t input_site) {
    uint32_t game = call(c, game_site, kGetGame);
    return call(c, input_site, kGetPlayerInput, {game, 0});
}

void send_key(Cpu& c, OriginalFrame& frame, uint32_t site, uint32_t holder, uint32_t data, uint8_t code) {
    wr32(frame.at(data), 5);
    wr8(frame.at(data + 4), code);
    call(c, site, kSendEvent, {layout(holder), frame.at(data)});
}

// (Main*)
WP_LIFTED(kLifted, opening_update, "Scene_Opening", Ret::None) {
    Main main;
    main.base = c.r[3];
    OriginalFrame frame(c, 0x30);
    frame.save_gpr(31, 0x2C);
    c.r[31] = main.base;
    bool demo = false;

    switch (main.opening_step()) {
    case 0: {
        wr32(main.base + 0x24, 0);
        int32_t kind = main.kind();
        uint32_t previous = kind == 2 ? main.title_layout() : kind == 0 ? main.stage_card() : main.results_layout();
        wr8(layout(previous), 1);
        demo = main.kind() == 2;
        uint32_t banner = demo && main.returning() == 1 ? main.returning_banner() : main.first_banner();
        uint32_t site = demo && main.returning() == 1 ? 0x8002BEE8 : demo ? 0x8002BF10 : 0x8002BF38;
        call(c, site, kResetLayout, {layout(banner)});
        wr8(layout(banner), 0);
        call(c, 0x8002BF58, 0x80023018, {game_member(c, 0x8002BF50, 0x54)});
        if (main.kind() == 2) {
            if (main.returning() == 1) {
                call(c, 0x8002BF7C, 0x80022B88, {game_member(c, 0x8002BF74, 0x54)});
            } else {
                call(c, 0x8002BF8C, 0x80022C30, {game_member(c, 0x8002BF84, 0x54)});
            }
        } else {
            call(c, 0x8002BF9C, 0x80022D7C, {game_member(c, 0x8002BF94, 0x54)});
        }
        main.opening_skipped() = 0;
        main.opening_step() = 1;
        main.next_scene_mode() = 2;
        break;
    }
    case 1:
        call(c, 0x8002BFC8, 0x80022A3C, {game_member(c, 0x8002BFC0, 0x54)});
        if (main.kind() == 2) {
            if (main.returning() == 1) {
                call(c, 0x8002BFEC, 0x80022B88, {game_member(c, 0x8002BFE4, 0x54)});
            } else {
                call(c, 0x8002BFFC, 0x80022C30, {game_member(c, 0x8002BFF4, 0x54)});
            }
            if (pressed(c, 0x8002C000, 0x8002C008) & skip_buttons()) {
                main.opening_skipped() = 1;
                if (main.returning() == 1) {
                    send_key(c, frame, 0x8002C04C, main.returning_banner(), 0x18, 'x');
                } else {
                    send_key(c, frame, 0x8002C070, main.first_banner(), 0x10, 'x');
                }
            }
        } else if (pressed(c, 0x8002C078, 0x8002C080) & skip_buttons()) {
            main.opening_skipped() = 1;
            send_key(c, frame, 0x8002C0B8, main.first_banner(), 0x8, 'x');
        }
        break;
    default:
        break;
    }

    frame.restore_gpr(31, 0x2C);
}

// (Main*)
WP_LIFTED(kLifted, select_update, "Scene_Select", Ret::None) {
    Main main;
    main.base = c.r[3];
    OriginalFrame frame(c, 0x60);
    frame.save_gpr(31, 0x5C);
    c.r[31] = main.base;
    frame.save_gpr(30, 0x58);

    if (main.select_step() == 0) {
        call(c, 0x8002C110, kSetPointer, {input(c, 0x8002C100, 0x8002C108), 1});
        wr8(layout(main.title_layout()), 1);
        wr8(layout(main.returning() == 1 ? main.returning_banner() : main.first_banner()), 1);
        call(c, 0x8002C15C, kResetLayout, {layout(main.select_layout())});
        c.r[30] = 0;
        wr8(layout(main.select_layout()), 0);
        main.field_883f4() = 0;
        main.select_x() = 0x12;
        main.select_y() = 0.0f;
        main.select_z() = 0;
        call(c, 0x8002C198, 0x80022A44, {game_member(c, 0x8002C190, 0x54)});
        call(c, 0x8002C1A0, 0x800363E0, {main.base});
        main.select_step() = 1;
        main.field_883f8() = 0;
    } else if (main.select_step() == 1) {
        call(c, 0x8002C1C0, 0x80022A3C, {game_member(c, 0x8002C1B8, 0x54)});
        if (call(c, 0x8002C1D0, kPointerValid, {input(c, 0x8002C1C4, 0x8002C1CC)}) != 0) {
            c.r[30] = call(c, 0x8002C1E8, kPointerX, {input(c, 0x8002C1DC, 0x8002C1E4)});
            int32_t y = static_cast<int32_t>(call(c, 0x8002C1FC, kPointerY, {input(c, 0x8002C1F0, 0x8002C1F8)}));
            float fy = frame.int_to_float(0x48, y);
            float fx = frame.int_to_float(0x50, static_cast<int32_t>(c.r[30]));
            wr32(frame.at(0x38), 1);
            stf(frame.at(0x40), fy);
            stf(frame.at(0x3C), fx);
            wr8(frame.at(0x44), 0);
            call(c, 0x8002C258, kSendEvent, {layout(main.select_layout()), frame.at(0x38)});
        } else {
            if (pressed(c, 0x8002C260, 0x8002C268) & 0x1) {
                send_key(c, frame, 0x8002C298, main.select_layout(), 0x30, 'a');
            }
            if (pressed(c, 0x8002C29C, 0x8002C2A4) & 0x2) {
                send_key(c, frame, 0x8002C2D4, main.select_layout(), 0x28, 'd');
            }
            if (pressed(c, 0x8002C2D8, 0x8002C2E0) & 0x8) {
                send_key(c, frame, 0x8002C310, main.select_layout(), 0x20, 'w');
            }
            if (pressed(c, 0x8002C314, 0x8002C31C) & 0x4) {
                send_key(c, frame, 0x8002C34C, main.select_layout(), 0x18, 's');
            }
        }
        if (pressed(c, 0x8002C350, 0x8002C358) & 0x4800) {
            send_key(c, frame, 0x8002C388, main.select_layout(), 0x10, 'x');
        }
        if (pressed(c, 0x8002C38C, 0x8002C394) & 0x2400) {
            send_key(c, frame, 0x8002C3C4, main.select_layout(), 0x8, 'z');
        }
    }

    frame.restore_gpr(31, 0x5C);
    frame.restore_gpr(30, 0x58);
}

}  // namespace

}  // namespace musclemarch
