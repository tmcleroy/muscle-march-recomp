// The controller check: "connect the Wii Remote / Nunchuk" while playing.
//
// Lifted from fn_8002ED1C (called every frame by the game's main update, fn_8002B2FC). It
// returns 1 while a menu is open or a warning is shown (the caller then pauses the game):
//   - on the title (sequence 0, sub-state 10) the first frame raises warning 1; after that a
//     missing Remote raises warning 3, and every 5 ticks warning 2;
//   - in play (other sequences; sequence 3 only in its mode 2; never in sequence 0xF) a missing
//     Remote raises 3 and a missing Nunchuk 4.
// A raised warning plays the "w_start" animation on the warning layout and opens menu 4.

#include "game_features.h"
#include "input/player_input.h"
#include "race/race.h"

namespace musclemarch {

namespace {

using namespace wp;
using namespace wp::lift;

const lift::Group kLifted{"game.lifted", "WP_LIFTED"};

constexpr uint32_t kMenuOpen = 0x80021844;        // (game, menu) -> open
constexpr uint32_t kMenuClosing = 0x8002185C;     // (game, menu) -> closing
constexpr uint32_t kOpenMenu = 0x80021824;        // (game, menu)
constexpr uint32_t kConnected = 0x8004239C;       // (input) -> a Remote is connected
constexpr uint32_t kHasNunchuk = 0x800423B4;      // (input) -> a Nunchuk is attached
constexpr uint32_t kFindPane = 0x80134A14;        // (layout, kind, name) -> pane
constexpr uint32_t kPlayAnimation = 0x80135038;   // (layout, pane, animation, 0)
constexpr uint32_t kShowPane = 0x80134DE0;        // (layout, pane, 0)
constexpr uint32_t kSceneWarning = 0x80036858;    // (scene, 1)
constexpr uint32_t kNames = 0x8026F98C;           // "_root" at +0x10D, "w_start" at +0x1B2

constexpr uint32_t kControllerMenu = 4;

enum Warning : int32_t { kNone = 0, kFirst = 1, kReminder = 2, kNoRemote = 3, kNoNunchuk = 4 };

struct Scene : lift::View {
    WP_FIELD(int32_t, sequence, 0x0)
    WP_FIELD(int32_t, sub_state, 0x8)
    WP_FIELD(int32_t, sequence_mode, 0x58)
    WP_FIELD(uint32_t, warning_layout, 0x883EC)
    WP_FIELD(int32_t, warning, 0x88408)
    WP_FIELD(int32_t, warning_ticks, 0x8840C)
    WP_FIELD(int32_t, title_checked, 0x88414)
};

// (Scene*) -> 1 while the game should wait
WP_LIFTED(kLifted, controller_check, "Scene_ControllerCheck", Ret::Int) {
    Scene scene;
    scene.base = c.r[3];
    OriginalFrame frame(c, 0x20);
    frame.save_gpr(31, 0x1C);
    c.r[31] = scene.base;
    frame.save_gpr(30, 0x18);
    frame.save_gpr(29, 0x14);

    auto input = [&](uint32_t game_site, uint32_t input_site) {
        uint32_t game = call(c, game_site, kGetGame);
        return call(c, input_site, kGetPlayerInput, {game, 0});
    };
    auto check_controllers = [&](uint32_t base_site) {
        if (call(c, base_site + 0xC, kConnected, {input(base_site, base_site + 8)}) == 0) {
            scene.warning() = kNoRemote;
        } else if (call(c, base_site + 0x34, kHasNunchuk, {input(base_site + 0x28, base_site + 0x30)}) == 0) {
            scene.warning() = kNoNunchuk;
        }
    };

    uint32_t result = 1;
    uint32_t game = call(c, 0x8002ED38, kGetGame);
    bool menu = call(c, 0x8002ED40, kMenuOpen, {game, kControllerMenu}) != 0;
    if (!menu) {
        game = call(c, 0x8002ED4C, kGetGame);
        menu = call(c, 0x8002ED54, kMenuClosing, {game, kControllerMenu}) != 0;
    }
    if (!menu) {
        switch (scene.sequence()) {
        case 0:
            if (scene.sub_state() == 0xA) {
                if (scene.title_checked() == 0) {
                    scene.title_checked() = 1;
                    // The boot notice; feature no_controller_notice leaves it out.
                    if (!features::no_controller_notice()) {
                        scene.warning() = kFirst;
                    }
                } else if (call(c, 0x8002EDC4, kConnected, {input(0x8002EDB8, 0x8002EDC0)}) == 0) {
                    scene.warning() = kNoRemote;
                } else if (scene.warning_ticks() >= 5) {
                    scene.warning_ticks() = 0;
                    scene.warning() = kReminder;
                }
            }
            break;
        case 0xF:
            break;
        case 3:
            if (scene.sequence_mode() == 2) {
                check_controllers(0x8002EE10);
            }
            break;
        default:
            check_controllers(0x8002EE10);
            break;
        }

        if (scene.warning() != kNone) {
            c.r[30] = kNames;
            uint32_t layout = rd32(scene.warning_layout() + 8);
            c.r[29] = call(c, 0x8002EE84, kFindPane, {layout, 3, kNames + 0x10D});
            layout = rd32(scene.warning_layout() + 8);
            uint32_t animation = call(c, 0x8002EEA0, kFindPane, {layout, 0, kNames + 0x1B2});
            layout = rd32(scene.warning_layout() + 8);
            call(c, 0x8002EEBC, kPlayAnimation, {layout, c.r[29], animation, 0});
            layout = rd32(scene.warning_layout() + 8);
            call(c, 0x8002EED4, kShowPane, {layout, c.r[29], 0});
            wr8(rd32(scene.warning_layout() + 8), 0);
            game = call(c, 0x8002EEEC, kGetGame);
            call(c, 0x8002EEF4, kOpenMenu, {game, kControllerMenu});
            call(c, 0x8002EF00, kSceneWarning, {scene.base, 1});
        } else {
            result = 0;
        }
    }

    frame.restore_gpr(31, 0x1C);
    frame.restore_gpr(30, 0x18);
    frame.restore_gpr(29, 0x14);
    c.r[3] = result;
}

}  // namespace

}  // namespace musclemarch
