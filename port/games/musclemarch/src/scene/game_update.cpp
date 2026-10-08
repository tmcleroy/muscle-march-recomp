// The game's main update: which scene runs this frame.
//
// Lifted from fn_8002B2FC (called once per frame). The game object's sequence (+0x0) selects the
// scene; each scene's update decides the next one by writing the sequence. Before that, every
// frame: the "player is in the race" flag is cleared, the controller check runs (its result is
// not used here; it pauses through the menu it opens), and while menu 0x1F (the controller
// warning) is open only its handler runs.

#include "input/player_input.h"
#include "race/race.h"

namespace musclemarch {

namespace {

using namespace wp;
using namespace wp::lift;

const lift::Group kLifted{"game.lifted", "WP_LIFTED"};

constexpr uint32_t kControllerCheck = 0x8002ED1C;  // scene/controller_check.cpp
constexpr uint32_t kMenuOpen = 0x80021844;         // (game, menu) -> open
constexpr uint32_t kWarningMenu = 0x8002E014;      // (main): the controller warning screen
constexpr uint32_t kTitleShown = 0x804223D4;       // global flag

// Sequence -> the scene update that runs it (0 = handled inline or nothing).
struct SequenceEntry {
    uint32_t site;
    uint32_t function;
};
constexpr SequenceEntry kSequences[0x12] = {
    {0x8002B36C, 0x8002B528},  // 0  boot screens
    {0x8002B378, 0x8002B844},  // 1
    {0, 0},                    // 2  (inline: on to 3 or 4)
    {0x8002B3BC, 0x8002B9C0},  // 3  title
    {0x8002B3C8, 0x8002BE38},  // 4
    {0, 0},                    // 5  (inline: a layout, then back to 3)
    {0x8002B418, 0x8002C0D0},  // 6
    {0x8002B424, 0x8002C3E0},  // 7
    {0x8002B430, 0x8002C544},  // 8  the race (race/race_scene.cpp)
    {0x8002B454, 0x8002D990},  // 9
    {0x8002B460, 0x8002DB98},  // 10
    {0x8002B46C, 0x8002DD50},  // 11
    {0x8002B478, 0x8002DEB8},  // 12
    {0x8002B43C, 0x8002D688},  // 13
    {0x8002B448, 0x8002D810},  // 14
    {0x8002B484, 0x8002E9B0},  // 15
    {0, 0},                    // 16 nothing
    {0x8002B490, 0x8002465C},  // 17 the debug parameter menu
};

struct Main : lift::View {
    WP_FIELD(uint32_t, sequence, 0x0)
    WP_FIELD(int32_t, field_28, 0x28)
    WP_FIELD(int32_t, layout_step, 0x2C)
    WP_FIELD(int32_t, skip_to_4, 0x1D4)
    WP_FIELD(uint32_t, layout_holder, 0x883D4)
    WP_FIELD(uint8_t, player_in_race, 0x8A100)
};

// (Main*). Never verified call by call: it contains whole scenes, some of which wait for loading
// and saving; it is checked by comparing lifted and translated replays.
WP_LIFTED_WHEN(kLifted, game_update, "Game_Update", Ret::None, false) {
    Main main;
    main.base = c.r[3];
    OriginalFrame frame(c, 0x10);
    frame.save_gpr(31, 0xC);
    c.r[31] = 0;
    frame.save_gpr(30, 0x8);
    c.r[30] = main.base;
    main.player_in_race() = 0;

    call(c, 0x8002B320, kControllerCheck, {main.base});
    uint32_t game = call(c, 0x8002B324, kGetGame);
    if (call(c, 0x8002B32C, kMenuOpen, {game, 0x1F}) != 0) {
        call(c, 0x8002B33C, kWarningMenu, {main.base});
    } else if (main.sequence() <= 0x11) {
        uint32_t sequence = main.sequence();
        if (sequence == 2) {
            main.sequence() = main.skip_to_4() != 0 ? 4 : 3;
            if (rd32(kTitleShown) == 0) {
                wr32(kTitleShown, 1);
            }
        } else if (sequence == 5) {
            if (main.layout_step() == 0) {
                main.field_28() = static_cast<int32_t>(c.r[31]);
                wr8(rd32(main.layout_holder() + 8), 1);
                main.layout_step() = 1;
            } else if (main.layout_step() == 1) {
                main.sequence() = 3;
            }
        } else if (kSequences[sequence].function != 0) {
            call(c, kSequences[sequence].site, kSequences[sequence].function, {main.base});
        }
    }

    frame.restore_gpr(31, 0xC);
    frame.restore_gpr(30, 0x8);
}

}  // namespace

}  // namespace musclemarch
