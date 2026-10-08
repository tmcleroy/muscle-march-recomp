// Scene transitions: back to the title, and on to the next stage.
//
// Lifted from fn_8002B844 (sequence 1) and fn_8002C3E0 (sequence 7) of the game update
// (scene/game_update.cpp).
//   Sequence 1, to the title (step +0x20): 0 reset the game (difficulty, counters, endless
//   records), 1 when loading is done, set up the title screens and go to sequence 2.
//   Sequence 7, the next stage (step +0x34): 0 leave the race, 1 when loading is done, set up
//   the next stage and its card (layout +0x883B4), 2 wait for A, B, 1 or 2 on the card.

#include "input/player_input.h"
#include "race/race.h"

namespace musclemarch {

namespace {

using namespace wp;
using namespace wp::lift;

const lift::Group kLifted{"game.lifted", "WP_LIFTED"};

constexpr uint32_t kGetPad = 0x80020F54;
constexpr uint32_t kLayoutHide = 0x8004DAA8;
constexpr uint32_t kLayoutShow = 0x8004DACC;
constexpr uint32_t kSendEvent = 0x80134480;
constexpr uint32_t kCurrentRace = 0x804223D0;

struct Main : lift::View {
    WP_FIELD(int32_t, sequence, 0x0)
    WP_FIELD(int32_t, title_step, 0x20)
    WP_FIELD(int32_t, stage_step, 0x34)
    WP_FIELD(int32_t, kind, 0x54)
    WP_FIELD(int32_t, difficulty, 0x1C0)
    WP_FIELD(uint32_t, stage_card, 0x883B4)
    WP_FIELD(int32_t, field_883f4, 0x883F4)
    WP_FIELD(int32_t, field_8a0e0, 0x8A0E0)
    WP_FIELD(int32_t, field_8a104, 0x8A104)
    lift::Field<int32_t> at(uint32_t offset) const { return lift::Field<int32_t>(base + offset); }
};

uint32_t game_member(Cpu& c, uint32_t site, uint32_t offset) {
    return rd32(call(c, site, kGetGame) + offset);
}

// (Main*)
WP_LIFTED(kLifted, to_title, "Scene_ToTitle", Ret::None) {
    Main main;
    main.base = c.r[3];
    OriginalFrame frame(c, 0x10);
    frame.save_gpr(31, 0xC);
    frame.save_gpr(30, 0x8);
    c.r[30] = main.base;

    if (main.title_step() == 0) {
        call(c, 0x8002B874, 0x8002AFE4, {main.base});
        c.r[31] = 0;
        main.difficulty() = 0;
        main.field_883f4() = 0;
        call(c, 0x8002B88C, 0x8002AA5C, {main.base});
        main.at(0x408) = 0;
        if (!(0 <= main.at(0x410))) {
            main.at(0x410) = 0;
        }
        for (uint32_t offset : {0x40Cu, 0x20Cu, 0x410u, 0x418u, 0x41Cu, 0x420u, 0x44Cu, 0x450u, 0x42Cu, 0x3DCu}) {
            main.at(offset) = 0;
        }
        for (uint32_t offset = 0x3E4; offset <= 0x400; offset += 4) {
            main.at(offset) = 0;  // endless records
        }
        main.field_8a104() = 0;
        call(c, 0x8002B8FC, 0x80037D74, {main.base});
        main.title_step() = 1;
    } else if (main.title_step() == 1) {
        call(c, 0x8002B90C, 0x80144470, {main.base});
        if (call(c, 0x8002B910, 0x80144690) == 0) {
            call(c, 0x8002B924, 0x80023018, {game_member(c, 0x8002B91C, 0x54)});
            call(c, 0x8002B930, 0x80023148, {game_member(c, 0x8002B928, 0x58)});
            call(c, 0x8002B93C, 0x80022F68, {game_member(c, 0x8002B934, 0x54)});
            call(c, 0x8002B948, kLayoutHide, {game_member(c, 0x8002B940, 0x20)});
            call(c, 0x8002B954, kLayoutHide, {game_member(c, 0x8002B94C, 0x24)});
            call(c, 0x8002B960, kLayoutShow, {game_member(c, 0x8002B958, 0x20)});
            call(c, 0x8002B96C, kLayoutShow, {game_member(c, 0x8002B964, 0x24)});
            call(c, 0x8002B974, 0x800377A0, {main.base});
            call(c, 0x8002B97C, 0x80036CA4, {main.base});
            wr32(call(c, 0x8002B980, kGetGame) + 0x5C, 1);
            main.field_8a0e0() = 2;
            main.at(0x3D8) = 0;
            main.sequence() = 2;
            main.title_step() = 0;
        }
    }

    frame.restore_gpr(31, 0xC);
    frame.restore_gpr(30, 0x8);
}

// (Main*)
WP_LIFTED(kLifted, next_stage, "Scene_NextStage", Ret::None) {
    Main main;
    main.base = c.r[3];
    OriginalFrame frame(c, 0x20);
    frame.save_gpr(31, 0x1C);
    frame.save_gpr(30, 0x18);
    c.r[30] = main.base;

    switch (main.stage_step()) {
    case 0:
        call(c, 0x8002C420, 0x80022F68, {game_member(c, 0x8002C418, 0x54)});
        call(c, 0x8002C42C, 0x80023148, {game_member(c, 0x8002C424, 0x58)});
        call(c, 0x8002C434, 0x80037D74, {main.base});
        call(c, 0x8002C43C, 0x80037AD0, {main.base});
        call(c, 0x8002C448, kLayoutShow, {game_member(c, 0x8002C440, 0x20)});
        call(c, 0x8002C454, kLayoutShow, {game_member(c, 0x8002C44C, 0x24)});
        call(c, 0x8002C45C, 0x80036414, {rd32(kCurrentRace)});
        main.stage_step() = 1;
        break;
    case 1:
        if (call(c, 0x8002C46C, 0x80036358, {main.base}) == 0) {
            break;
        }
        call(c, 0x8002C478, 0x80144470);
        if (call(c, 0x8002C47C, 0x80144690) != 0) {
            break;
        }
        c.r[31] = 0;
        main.kind() = 0;
        call(c, 0x8002C494, 0x80030A44, {main.base});
        call(c, 0x8002C49C, 0x800377FC, {main.base});
        call(c, 0x8002C4A4, 0x80036EF4, {main.base});
        call(c, 0x8002C4B4, 0x80134348, {rd32(main.stage_card() + 8)});
        wr8(rd32(main.stage_card() + 8), static_cast<uint8_t>(c.r[31]));
        call(c, 0x8002C4CC, 0x80034B68, {main.base});
        if (main.difficulty() > 0) {
            call(c, 0x8002C4E0, 0x80034C98, {main.base});
        }
        main.stage_step() = 2;
        break;
    case 2: {
        uint32_t game = call(c, 0x8002C4F0, kGetGame);
        if (rd32(call(c, 0x8002C4F8, kGetPad, {game, 0}) + 0x10) & 0x6C00) {
            wr32(frame.at(0x8), 5);
            wr8(frame.at(0xC), 0x78);  // 'x'
            call(c, 0x8002C528, kSendEvent, {rd32(main.stage_card() + 8), frame.at(0x8)});
        }
        break;
    }
    default:
        break;
    }

    frame.restore_gpr(31, 0x1C);
    frame.restore_gpr(30, 0x18);
}

}  // namespace

}  // namespace musclemarch
