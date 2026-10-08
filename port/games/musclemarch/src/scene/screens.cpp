// The remaining scenes: ending, final results, endless rounds, saving, result screens and the
// info screen.
//
// Lifted from the game update's sequences (scene/game_update.cpp):
//   9   fn_8002D990  the ending (step +0x3C): two endings alternate (+0x200); after it, release
//                    the race data and on to sequence 10
//   10  fn_8002DB98  final results (step +0x40): counters capped at 999
//   11  fn_8002DD50  endless round transition (step +0x4C)
//   12  fn_8002DEB8  saving (step +0x50): the 10 save slots and a value written, a save
//                    request, then the "saved" screen
//   13  fn_8002D688  a result screen (step +0x44) with retry on A (mode +0x64 = 0)
//   14  fn_8002D810  a result screen (step +0x48)
//   15  fn_8002E9B0  the info screen (step +0xC): one of six pages at random (never the same
//                    twice in a row), two layouts, then back to the title
// Most steps wait for A, B, 1 or 2 (0x6C00), sent to the screen's layout as 'x'.

#include "input/player_input.h"
#include "race/race.h"
#include "wp/threads.h"

namespace musclemarch {

namespace {

using namespace wp;
using namespace wp::lift;
using namespace wp::lift::ppc;

const lift::Group kLifted{"game.lifted", "WP_LIFTED"};

constexpr uint32_t kGetPad = 0x80020F54;
constexpr uint32_t kResetLayout = 0x80134348;
constexpr uint32_t kSendEvent = 0x80134480;
constexpr uint32_t kFindPane = 0x80134A14;
constexpr uint32_t kPlayAnimation = 0x80135038;
constexpr uint32_t kShowPane = 0x80134DE0;
constexpr uint32_t kCurrentRace = 0x804223D0;
constexpr uint32_t kNames = 0x8026F98C;
constexpr uint32_t kInfoPages = 0x8026F970;       // 7 ints
constexpr uint32_t kLastInfoPage = 0x80421194;    // global
constexpr uint32_t kLastMode13 = 0x8042118C;      // global
constexpr uint32_t kLastMode14 = 0x80421190;      // global
constexpr uint32_t kSkipButtons = 0x6C00;

// Layout holders in the game object (+8: the layout; byte 0: hidden).
constexpr uint32_t kTitleLayout = 0x883A8;
constexpr uint32_t kEndingLayout = 0x883B8;
constexpr uint32_t kResultLayout = 0x883BC;
constexpr uint32_t kResultScreen = 0x883C0;
constexpr uint32_t kRoundLayout = 0x883C4;
constexpr uint32_t kSavedLayout = 0x883C8;
constexpr uint32_t kEndingCredits = 0x883CC;
constexpr uint32_t kInfoLayout = 0x883E0;
constexpr uint32_t kInfoLayout2 = 0x883E4;

struct Screens {
    Cpu& c;
    uint32_t main;
    OriginalFrame& frame;

    Field<int32_t> at(uint32_t offset) const { return Field<int32_t>(main + offset); }
    // A layout holder's pointer lives in the game object; the layout is at holder +8.
    uint32_t layout(uint32_t holder) const { return rd32(rd32(main + holder) + 8); }
    void hide(uint32_t holder, uint8_t hidden) { wr8(layout(holder), hidden); }
    uint32_t game_member(uint32_t site, uint32_t offset) { return rd32(call(c, site, kGetGame) + offset); }
    uint32_t pressed(uint32_t game_site, uint32_t pad_site) {
        uint32_t game = call(c, game_site, kGetGame);
        return rd32(call(c, pad_site, kGetPad, {game, 0}) + 0x10);
    }
    // A, B, 1 or 2: send 'x' to the screen's layout (event at frame +data).
    void confirm(uint32_t game_site, uint32_t pad_site, uint32_t send_site, uint32_t holder, uint32_t data) {
        if (pressed(game_site, pad_site) & kSkipButtons) {
            wr32(frame.at(data), 5);
            wr8(frame.at(data + 4), 'x');
            call(c, send_site, kSendEvent, {layout(holder), frame.at(data)});
        }
    }
    void report(uint32_t site, uint32_t text) {
        static const uint32_t os_report = symbol_address("OSReport");
        cr_set(c, 6, false);
        call(c, site, os_report, {kNames + text});
    }
    void animate(uint32_t holder, uint32_t pane, uint32_t animation, uint32_t site) {
        c.r[28] = call(c, site, kFindPane, {layout(holder), 3, kNames + pane});
        uint32_t found = call(c, site + 0x1C, kFindPane, {layout(holder), 0, kNames + animation});
        call(c, site + 0x38, kPlayAnimation, {layout(holder), c.r[28], found, 0});
        call(c, site + 0x50, kShowPane, {layout(holder), c.r[28], 0});
    }
    // A change of the result screen's mode (+0x64) sets up its view.
    void mode_change(uint32_t last_mode, const uint32_t (&sites)[6]) {
        int32_t mode = at(0x64);
        if (static_cast<int32_t>(rd32(last_mode)) != mode) {
            static constexpr uint32_t kViews[3] = {0x80022E98, 0x80022F40, 0x80022F68};
            if (mode >= 0 && mode <= 2) {
                call(c, sites[2 * mode + 1], kViews[mode], {game_member(sites[2 * mode], 0x54)});
            }
        }
        wr32(last_mode, static_cast<uint32_t>(at(0x64).get()));
    }
};

// Sequence 9.
WP_LIFTED(kLifted, ending_update, "Scene_Ending", Ret::None) {
    uint32_t main = c.r[3];
    OriginalFrame frame(c, 0x20);
    frame.save_gpr(31, 0x1C);
    c.r[31] = main;
    frame.save_gpr(30, 0x18);
    frame.save_gpr(29, 0x14);
    Screens s{c, main, frame};

    switch (s.at(0x3C)) {
    case 0:
        call(c, 0x8002D9E4, 0x80023148, {s.game_member(0x8002D9DC, 0x58)});
        if (s.at(0x200) == 0) {
            s.at(0x59C8C) = 0x14;
            call(c, 0x8002DA08, 0x80036364, {main, 6});
        } else if (s.at(0x200) == 1) {
            s.at(0x59C8C) = 0x13;
            call(c, 0x8002DA2C, 0x80036364, {main, 7});
        }
        c.r[29] = 1;
        stf(main + 0x59C90, 0.0f);
        s.at(0x59C94) = static_cast<int32_t>(c.r[29]);
        call(c, 0x8002DA50, kResetLayout, {s.layout(kEndingLayout)});
        c.r[30] = 0;
        s.hide(kEndingLayout, 0);
        call(c, 0x8002DA70, kResetLayout, {s.layout(kEndingCredits)});
        s.hide(kEndingCredits, 0);
        s.at(0x3C) = static_cast<int32_t>(c.r[29]);
        break;
    case 1:
        if (call(c, 0x8002DA8C, 0x80036348, {main}) != 0) {
            call(c, 0x8002DA9C, 0x800363B0, {main});
            s.at(0x8A0E0) = 4;
            s.at(0x3C) = 2;
        }
        break;
    case 2:
        s.confirm(0x8002DAB8, 0x8002DAC0, 0x8002DAF0, kEndingCredits, 0x8);
        s.report(0x8002DB04, 0x89);  // "reach ending main"
        break;
    case 3:
        s.report(0x8002DB1C, 0x9C);  // "reach ending data release"
        call(c, 0x8002DB24, 0x80035088, {rd32(kCurrentRace)});
        call(c, 0x8002DB2C, 0x80035234, {rd32(kCurrentRace)});
        s.at(0x3C) = 4;
        break;
    case 4:
        s.report(0x8002DB4C, 0xB7);  // "reach ending data release wait"
        call(c, 0x8002DB50, 0x80144470);
        if (call(c, 0x8002DB54, 0x80144690) == 0) {
            s.hide(kEndingLayout, 1);
            s.at(0x0) = 0xA;
        }
        break;
    default:
        break;
    }

    frame.restore_gpr(31, 0x1C);
    frame.restore_gpr(30, 0x18);
    frame.restore_gpr(29, 0x14);
}

// Sequence 10.
WP_LIFTED(kLifted, final_results_update, "Scene_FinalResults", Ret::None) {
    uint32_t main = c.r[3];
    OriginalFrame frame(c, 0x20);
    frame.save_gpr(31, 0x1C);
    c.r[31] = main;
    frame.save_gpr(30, 0x18);
    frame.save_gpr(29, 0x14);
    Screens s{c, main, frame};

    if (s.at(0x40) == 0) {
        c.r[29] = 1;
        s.hide(kEndingLayout, 1);
        s.hide(kEndingCredits, 1);
        call(c, 0x8002DBF4, 0x80023148, {s.game_member(0x8002DBEC, 0x58)});
        call(c, 0x8002DC04, kResetLayout, {s.layout(kResultLayout)});
        c.r[30] = 0;
        s.hide(kResultLayout, 0);
        call(c, 0x8002DC20, 0x80036414, {rd32(kCurrentRace)});
        if (s.at(0x200) == 0) {
            s.at(0x200) = static_cast<int32_t>(c.r[29]);
        } else if (s.at(0x200) == 1) {
            s.at(0x200) = static_cast<int32_t>(c.r[30]);
        }
        if (s.at(0x408) > 999) {
            s.at(0x408) = 999;
            if (999 > s.at(0x410)) {
                s.at(0x410) = 999;
            }
        }
        for (uint32_t offset : {0x410u, 0x40Cu, 0x41Cu, 0x460u, 0x464u, 0x458u}) {
            if (s.at(offset) > 999) {
                s.at(offset) = 999;
            }
        }
        s.at(0x8A0E0) = 0;
        s.at(0x40) = 1;
    } else if (s.at(0x40) == 1) {
        s.confirm(0x8002DCF8, 0x8002DD00, 0x8002DD30, kResultLayout, 0x8);
    }

    frame.restore_gpr(31, 0x1C);
    frame.restore_gpr(30, 0x18);
    frame.restore_gpr(29, 0x14);
}

// Sequence 11.
WP_LIFTED(kLifted, endless_round_update, "Scene_EndlessRound", Ret::None) {
    uint32_t main = c.r[3];
    OriginalFrame frame(c, 0x20);
    frame.save_gpr(31, 0x1C);
    c.r[31] = main;
    Screens s{c, main, frame};

    switch (s.at(0x4C)) {
    case 0:
        call(c, 0x8002DD94, 0x80022F68, {s.game_member(0x8002DD8C, 0x54)});
        call(c, 0x8002DDA0, 0x80023148, {s.game_member(0x8002DD98, 0x58)});
        s.at(0x883F4) = s.at(0x404).get();
        if (s.at(0x20C) > 999) {
            s.at(0x20C) = 999;
        }
        call(c, 0x8002DDC8, 0x80036414, {rd32(kCurrentRace)});
        s.at(0x4C) = 1;
        break;
    case 1:
        if (call(c, 0x8002DDD8, 0x80036358, {main}) != 0) {
            if (s.at(0x3DC) == 0) {
                call(c, 0x8002DDF4, 0x80037AD0, {main});
            }
            s.at(0x4C) = 2;
        }
        break;
    case 2:
        call(c, 0x8002DE04, 0x80144470, {main});
        if (call(c, 0x8002DE08, 0x80144690) == 0) {
            if (s.at(0x3DC) == 0) {
                call(c, 0x8002DE24, 0x80037894, {main});
                call(c, 0x8002DE2C, 0x80037234, {main});
            }
            call(c, 0x8002DE3C, kResetLayout, {s.layout(kRoundLayout)});
            s.hide(kRoundLayout, 0);
            s.at(0x54) = 1;
            s.at(0x4C) = 3;
        }
        break;
    case 3:
        s.confirm(0x8002DE68, 0x8002DE70, 0x8002DEA0, kRoundLayout, 0x8);
        break;
    default:
        break;
    }

    frame.restore_gpr(31, 0x1C);
}

// Sequence 12. Saving waits for I/O, so it is not verified call by call.
WP_LIFTED_WHEN(kLifted, save_update, "Scene_Save", Ret::None, false) {
    uint32_t main = c.r[3];
    OriginalFrame frame(c, 0x20);
    frame.save_gpr(31, 0x1C);
    c.r[31] = main;
    frame.save_gpr(30, 0x18);
    frame.save_gpr(29, 0x14);
    Screens s{c, main, frame};

    switch (s.at(0x50)) {
    case 0: {
        c.r[30] = main;
        for (c.r[29] = 0; static_cast<int32_t>(c.r[29]) < 10; c.r[30] += 4) {
            uint32_t data = call(c, 0x8002DEFC, 0x80149BDC);
            call(c, 0x8002DF08, 0x8014A0A4, {data, c.r[29], rd32(c.r[30] + 0x21C)});
            c.r[29]++;
        }
        uint32_t data = call(c, 0x8002DF1C, 0x80149BDC);
        call(c, 0x8002DF24, 0x8014A0EC, {data, static_cast<uint32_t>(s.at(0x244).get())});
        call(c, 0x8002DF30, 0x80149838, {call(c, 0x8002DF28, 0x80148738), 3});
        s.report(0x8002DF44, 0xD7);  // save data save request
        s.at(0x50) = 1;
        break;
    }
    case 1:
        if (call(c, 0x8002DF58, 0x80149804, {call(c, 0x8002DF54, 0x80148738)}) != 0) {
            s.report(0x8002DF74, 0xF5);  // save data save done
            call(c, 0x8002DF7C, 0x8002F794, {main});
            call(c, 0x8002DF8C, kResetLayout, {s.layout(kSavedLayout)});
            s.hide(kSavedLayout, 0);
            call(c, 0x8002DFAC, 0x80023148, {s.game_member(0x8002DFA4, 0x58)});
            s.at(0x50) = 2;
        }
        break;
    case 2:
        s.confirm(0x8002DFBC, 0x8002DFC4, 0x8002DFF4, kSavedLayout, 0x8);
        break;
    default:
        break;
    }

    frame.restore_gpr(31, 0x1C);
    frame.restore_gpr(30, 0x18);
    frame.restore_gpr(29, 0x14);
}

// Sequence 13.
WP_LIFTED(kLifted, result_a_update, "Scene_ResultA", Ret::None) {
    uint32_t main = c.r[3];
    OriginalFrame frame(c, 0x20);
    frame.save_gpr(31, 0x1C);
    c.r[31] = main;
    frame.save_gpr(30, 0x18);
    Screens s{c, main, frame};

    if (s.at(0x44) == 0) {
        call(c, 0x8002D6C0, 0x80023018, {s.game_member(0x8002D6B8, 0x54)});
        call(c, 0x8002D6CC, 0x80022F40, {s.game_member(0x8002D6C4, 0x54)});
        c.r[30] = 1;
        s.hide(kResultLayout, 1);
        call(c, 0x8002D6EC, kResetLayout, {s.layout(kResultScreen)});
        wr32(kLastMode13, 0xFFFFFFFFu);
        s.hide(kResultScreen, 0);
        s.at(0x44) = static_cast<int32_t>(c.r[30]);
        s.at(0x5C) = 2;
    } else if (s.at(0x44) == 1) {
        call(c, 0x8002D724, 0x80022A3C, {s.game_member(0x8002D71C, 0x54)});
        s.confirm(0x8002D728, 0x8002D730, 0x8002D760, kResultScreen, 0x8);
        if (s.at(0x64) == 0 && (s.pressed(0x8002D770, 0x8002D778) & kSkipButtons)) {
            s.at(0x454) = 1;
            s.at(0x0) = 7;  // retry
        }
    }
    static constexpr uint32_t kSites[6] = {0x8002D7C4, 0x8002D7CC, 0x8002D7D4, 0x8002D7DC, 0x8002D7E4, 0x8002D7EC};
    s.mode_change(kLastMode13, kSites);

    frame.restore_gpr(31, 0x1C);
    frame.restore_gpr(30, 0x18);
}

// Sequence 14.
WP_LIFTED(kLifted, result_b_update, "Scene_ResultB", Ret::None) {
    uint32_t main = c.r[3];
    OriginalFrame frame(c, 0x20);
    frame.save_gpr(31, 0x1C);
    c.r[31] = main;
    Screens s{c, main, frame};

    if (s.at(0x48) == 0) {
        call(c, 0x8002D844, 0x80023018, {s.game_member(0x8002D83C, 0x54)});
        call(c, 0x8002D850, 0x80022F40, {s.game_member(0x8002D848, 0x54)});
        if (s.at(0x54) == 0) {
            s.hide(kResultLayout, 1);
        } else if (s.at(0x54) == 1) {
            s.hide(kSavedLayout, 1);
        }
        call(c, 0x8002D8A0, kResetLayout, {s.layout(kResultScreen)});
        wr32(kLastMode14, 0xFFFFFFFFu);
        s.hide(kResultScreen, 0);
        s.at(0x48) = 1;
        s.at(0x5C) = 2;
    } else if (s.at(0x48) == 1) {
        call(c, 0x8002D8DC, 0x80022A3C, {s.game_member(0x8002D8D4, 0x54)});
        s.confirm(0x8002D8E0, 0x8002D8E8, 0x8002D918, kResultScreen, 0x8);
    }
    static constexpr uint32_t kSites[6] = {0x8002D948, 0x8002D950, 0x8002D958, 0x8002D960, 0x8002D968, 0x8002D970};
    s.mode_change(kLastMode14, kSites);

    frame.restore_gpr(31, 0x1C);
}

// Sequence 15.
WP_LIFTED(kLifted, info_update, "Scene_Info", Ret::None) {
    uint32_t main = c.r[3];
    OriginalFrame frame(c, 0x40);
    frame.save_gpr(31, 0x3C);
    c.r[31] = main;
    frame.save_gpr(30, 0x38);
    frame.save_gpr(29, 0x34);
    frame.save_gpr(28, 0x30);
    Screens s{c, main, frame};

    switch (s.at(0xC)) {
    case 0: {
        int32_t r = static_cast<int32_t>(call(c, 0x8002EA00, 0x802160C4));
        float scaled = fmuls(6.0f, frame.int_to_float(0x18, r));
        int32_t page = frame.float_to_int(0x20, fdivs(scaled, 2147483600.0f));
        int32_t chosen = static_cast<int32_t>(rd32(kLastInfoPage)) == page ? 6 : page;
        c.r[29] = 0;
        wr32(kLastInfoPage, static_cast<uint32_t>(chosen));
        s.at(0x883F4) = static_cast<int32_t>(rd32(kInfoPages + 4 * static_cast<uint32_t>(chosen)));
        s.at(0x59C8C) = 0x12;
        stf(main + 0x59C90, 0.0f);
        s.at(0x59C94) = static_cast<int32_t>(c.r[29]);
        call(c, 0x8002EA8C, 0x80036414, {main});
        call(c, 0x8002EA94, 0x80036278, {main});
        call(c, 0x8002EA9C, 0x800363B0, {main});
        c.r[30] = kNames;
        s.animate(kInfoLayout, 0x10D, 0x18B, 0x8002EABC);  // "roll_start"
        s.hide(kTitleLayout, 1);
        s.hide(kInfoLayout, static_cast<uint8_t>(c.r[29]));
        s.at(0xC) = 1;
        break;
    }
    case 1:
        s.confirm(0x8002EB38, 0x8002EB40, 0x8002EB70, kInfoLayout, 0x10);
        break;
    case 2:
        c.r[30] = kNames;
        s.animate(kInfoLayout2, 0x10D, 0x196, 0x8002EB94);  // "bng_fadein"
        s.hide(kInfoLayout2, 0);
        s.at(0xC) = 3;
        break;
    case 3:
        s.confirm(0x8002EC08, 0x8002EC10, 0x8002EC40, kInfoLayout2, 0x8);
        break;
    case 4:
        c.r[30] = kNames;
        s.animate(kTitleLayout, 0x1A1, 0x1A6, 0x8002EC64);  // "main" / "title_start"
        s.hide(kInfoLayout, 1);
        s.hide(kInfoLayout2, 1);
        call(c, 0x8002ECDC, 0x8002AFE4, {main});
        c.r[30] = 0;
        s.at(0x1BC) = 0;
        call(c, 0x8002ECEC, 0x80036414, {main});
        s.at(0xC) = 0;
        s.at(0x0) = 3;  // back to the title
        break;
    default:
        break;
    }

    frame.restore_gpr(31, 0x3C);
    frame.restore_gpr(30, 0x38);
    frame.restore_gpr(29, 0x34);
    frame.restore_gpr(28, 0x30);
}

}  // namespace

}  // namespace musclemarch
