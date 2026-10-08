// The title screen.
//
// Lifted from fn_8002B9C0 (sequence 3 of the game update). Step (+0x24):
//   0  set up: the title layout (+0x883A8) and the first-time or returning banner
//      (+0x883AC / +0x883D4, alternating by +0x208)
//   1  the menu: the pointer, or 'w'/'s' (d-pad up/down) and 'x' (A or 1, plus B and 2 except in
//      menu mode 2) go to the title layout's menu; in mode 2, 600 frames without input start
//      the attract demo (sequence 4, game kind 2)
//   2, 3  leaving (mode 2 keeps confirming)
// Each frame the menu mode (+0x58) is compared with last frame's (global 0x80421180) and the
// screen for a new mode is set up.

#include "input/player_input.h"
#include "race/race.h"

namespace musclemarch {

namespace {

using namespace wp;
using namespace wp::lift;

const lift::Group kLifted{"game.lifted", "WP_LIFTED"};

constexpr uint32_t kGetPad = 0x80020F54;
constexpr uint32_t kPointerOn = 0x80042FB4;
constexpr uint32_t kPointerValid = 0x80042FCC;
constexpr uint32_t kPointerX = 0x80042E58;
constexpr uint32_t kPointerY = 0x80042E74;
constexpr uint32_t kSetPointer = 0x80042E90;
constexpr uint32_t kResetLayout = 0x80134348;     // (layout)
constexpr uint32_t kSendEvent = 0x80134480;
constexpr uint32_t kLastMode = 0x80421180;        // global

constexpr int32_t kIdleFrames = 600;

struct Main : lift::View {
    WP_FIELD(int32_t, sequence, 0x0)
    WP_FIELD(int32_t, step, 0x24)
    WP_FIELD(int32_t, field_2c, 0x2C)
    WP_FIELD(int32_t, kind, 0x54)
    WP_FIELD(int32_t, mode, 0x58)
    WP_FIELD(int32_t, pointer_mode, 0x60)
    WP_FIELD(int32_t, idle_frames, 0x1BC)
    WP_FIELD(int32_t, returning, 0x208)
    WP_FIELD(int32_t, field_414, 0x414)
    WP_FIELD(uint32_t, title_layout, 0x883A8)     // layout holders (+8: the layout; byte 0: hidden)
    WP_FIELD(uint32_t, first_banner, 0x883AC)
    WP_FIELD(uint32_t, backdrop, 0x883B0)
    WP_FIELD(uint32_t, returning_banner, 0x883D4)
    WP_FIELD(uint32_t, demo_layout, 0x883F0)
    WP_FIELD(int32_t, demo_skipped, 0x88400)
};

struct Title {
    Cpu& c;
    Main main;
    OriginalFrame& frame;

    uint32_t layout(uint32_t holder) const { return rd32(holder + 8); }
    uint32_t game_member(uint32_t site, uint32_t offset) { return rd32(call(c, site, kGetGame) + offset); }
    uint32_t input(uint32_t game_site, uint32_t input_site) {
        uint32_t game = call(c, game_site, kGetGame);
        return call(c, input_site, kGetPlayerInput, {game, 0});
    }
    uint32_t pressed(uint32_t game_site, uint32_t pad_site) {
        uint32_t game = call(c, game_site, kGetGame);
        return rd32(call(c, pad_site, kGetPad, {game, 0}) + 0x10);
    }
    void key(uint32_t site, uint32_t data, uint8_t code) {
        wr32(frame.at(data), 5);
        wr8(frame.at(data + 4), code);
        call(c, site, kSendEvent, {layout(main.title_layout()), frame.at(data)});
    }
    void demo_tick(uint32_t game_site, uint32_t call_site) {
        if (main.mode() == 2) {
            call(c, call_site, 0x80022A3C, {game_member(game_site, 0x54)});
        }
    }

    void setup();
    void menu();
};

void Title::setup() {
    c.r[31] = 1;
    wr8(layout(main.backdrop()), static_cast<uint8_t>(c.r[31]));
    call(c, 0x8002BA20, kResetLayout, {layout(main.title_layout())});
    c.r[29] = 0;
    wr8(layout(main.title_layout()), static_cast<uint8_t>(c.r[29]));
    wr32(call(c, 0x8002BA38, kGetGame) + 0x5C, c.r[31]);
    int32_t returning = main.returning();
    main.step() = static_cast<int32_t>(c.r[31]);
    wr32(kLastMode, 0xFFFFFFFFu);
    if (returning == 0) {
        call(c, 0x8002BA64, kResetLayout, {layout(main.first_banner())});
        wr8(layout(main.first_banner()), static_cast<uint8_t>(c.r[31]));
        main.returning() = static_cast<int32_t>(c.r[31]);
    } else {
        call(c, 0x8002BA8C, kResetLayout, {layout(main.returning_banner())});
        wr8(layout(main.returning_banner()), static_cast<uint8_t>(c.r[31]));
        main.returning() = static_cast<int32_t>(c.r[29]);
    }
    call(c, 0x8002BAA8, 0x8003665C, {main.base});
    main.field_2c() = 0;
    main.field_414() = 0;
}

void Title::menu() {
    demo_tick(0x8002BAC8, 0x8002BAD0);
    c.r[29] = 0;  // any input this frame
    if (call(c, 0x8002BAE4, kPointerValid, {input(0x8002BAD8, 0x8002BAE0)}) != 0) {
        c.r[31] = call(c, 0x8002BAFC, kPointerX, {input(0x8002BAF0, 0x8002BAF8)});
        int32_t y = static_cast<int32_t>(call(c, 0x8002BB10, kPointerY, {input(0x8002BB04, 0x8002BB0C)}));
        float fy = frame.int_to_float(0x38, y);
        float fx = frame.int_to_float(0x40, static_cast<int32_t>(c.r[31]));
        wr32(frame.at(0x28), 1);
        stf(frame.at(0x30), fy);
        stf(frame.at(0x2C), fx);
        wr8(frame.at(0x34), 0);
        call(c, 0x8002BB6C, kSendEvent, {layout(main.title_layout()), frame.at(0x28)});
        c.r[29] = 1;
    } else {
        if (pressed(0x8002BB78, 0x8002BB80) & 0x8) {
            key(0x8002BBB0, 0x20, 'w');
            c.r[29] = 1;
        }
        if (pressed(0x8002BBB8, 0x8002BBC0) & 0x4) {
            key(0x8002BBF0, 0x18, 's');
            c.r[29] = 1;
        }
    }
    c.r[31] = 0x4800;
    if (static_cast<uint32_t>(main.mode()) <= 1) {
        c.r[31] |= 0x2400;
    }
    if (pressed(0x8002BC0C, 0x8002BC14) & c.r[31]) {
        key(0x8002BC44, 0x10, 'x');
        c.r[29] = 1;
    }
    if (c.r[29] != 0) {
        main.idle_frames() = 0;
    }
    if (main.mode() != 2) {
        return;
    }
    main.idle_frames() = main.idle_frames() + 1;
    if (main.pointer_mode() == 2 && call(c, 0x8002BC8C, kPointerOn, {input(0x8002BC80, 0x8002BC88)}) == 0) {
        call(c, 0x8002BCA8, kSetPointer, {input(0x8002BC98, 0x8002BCA0), 1});
    }
    if (main.pointer_mode() == 3 && call(c, 0x8002BCC4, kPointerOn, {input(0x8002BCB8, 0x8002BCC0)}) != 0) {
        call(c, 0x8002BCE0, kSetPointer, {input(0x8002BCD0, 0x8002BCD8), 0});
    }
    if (main.idle_frames() > kIdleFrames) {
        main.idle_frames() = 0;
        main.demo_skipped() = 0;
        main.sequence() = 4;
        main.kind() = 2;  // the attract demo
        call(c, 0x8002BD20, kSetPointer, {input(0x8002BD10, 0x8002BD18), 0});
    }
}

// (Main*)
WP_LIFTED(kLifted, title_update, "Scene_Title", Ret::None) {
    Main main;
    main.base = c.r[3];
    OriginalFrame frame(c, 0x60);
    frame.save_gpr(31, 0x5C);
    frame.save_gpr(30, 0x58);
    c.r[30] = main.base;
    frame.save_gpr(29, 0x54);
    Title title{c, main, frame};

    switch (main.step()) {
    case 0:
        title.setup();
        break;
    case 1:
        title.menu();
        break;
    case 2:
        title.demo_tick(0x8002BD34, 0x8002BD3C);
        main.step() = 3;
        break;
    case 3:
        main.idle_frames() = main.idle_frames() + 1;
        title.demo_tick(0x8002BD64, 0x8002BD6C);
        if (main.mode() == 2) {
            title.key(0x8002BD9C, 0x8, 'x');
        }
        break;
    default:
        break;
    }

    // A new menu mode: set up its screen.
    int32_t mode = main.mode();
    if (static_cast<int32_t>(rd32(kLastMode)) != mode) {
        if (mode == 0) {
            call(c, 0x8002BDD4, 0x80022A58, {title.game_member(0x8002BDCC, 0x54)});
        } else if (mode == 1) {
            call(c, 0x8002BDE4, 0x80022A6C, {title.game_member(0x8002BDDC, 0x54)});
        } else if (mode == 2) {
            uint32_t holder = main.demo_layout();
            if (holder != 0) {
                wr8(rd32(holder + 8), 0);
            }
            call(c, 0x8002BE10, 0x80022A80, {title.game_member(0x8002BE08, 0x54)});
        }
    }
    wr32(kLastMode, static_cast<uint32_t>(main.mode()));

    frame.restore_gpr(31, 0x5C);
    frame.restore_gpr(30, 0x58);
    frame.restore_gpr(29, 0x54);
}

}  // namespace

}  // namespace musclemarch
