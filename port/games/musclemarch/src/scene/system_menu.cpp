// The system menus: the controller warning dialog and the race's pause menu.
//
// Lifted from fn_8002E014 (run by the game update while menu 0x1F is open).
// Controller warning (menu 4, raised by scene/controller_check.cpp; layout +0x883EC):
//   1  the boot notice ("tuusin"): closes by itself after 300 frames (counted in +0x88414)
//   2  "w02_ready", 3  "remocon_ready" / "no_remocon", 4  "nunchack_ok" / "no_nunchack":
//      the screen tracks whether the controller is there; A (0x800) closes it.
//   Closing plays "nunchaku_end" (warning 1) or "warning_end", and turns the pointer back on if
//   it was on (+0x68).
// Pause menu (menu 1, during the race frame; layout +0x883E8): state +0x10:
//   0 open ("pose_start"), 1 drive the layout's menu: the pointer (after 20 frames), or keys
//   'w' 's' 'a' 'd' (d-pad), 'x' (A/1), 'z' (B/2), 'q' (+); 2 act on the choice (+0x18):
//   0 resume, 1 retry, 2 quit to the results.

#include "game_features.h"
#include "input/player_input.h"
#include "race/race.h"

namespace musclemarch {

namespace {

using namespace wp;
using namespace wp::lift;
using namespace wp::lift::ppc;

const lift::Group kLifted{"game.lifted", "WP_LIFTED"};

constexpr uint32_t kGetPad = 0x80020F54;          // (game, channel) -> pad state (+0x10: pressed)
constexpr uint32_t kMenuOpen = 0x80021844;        // (game, menu) -> open
constexpr uint32_t kMenuClosing = 0x8002185C;     // (game, menu) -> closing
constexpr uint32_t kCloseMenu = 0x80021834;       // (game, menu)
constexpr uint32_t kConnected = 0x8004239C;
constexpr uint32_t kHasNunchuk = 0x800423B4;
constexpr uint32_t kPointerOn = 0x80042FB4;       // (input) -> the pointer is on
constexpr uint32_t kPointerValid = 0x80042FCC;    // (input) -> the pointer is on the screen
constexpr uint32_t kPointerX = 0x80042E58;        // (input) -> int
constexpr uint32_t kPointerY = 0x80042E74;        // (input) -> int
constexpr uint32_t kSetPointer = 0x80042E90;      // (input, on)
constexpr uint32_t kFindPane = 0x80134A14;        // (layout, kind, name) -> pane
constexpr uint32_t kPlayAnimation = 0x80135038;   // (layout, pane, animation, 0)
constexpr uint32_t kShowPane = 0x80134DE0;        // (layout, pane, 0)
constexpr uint32_t kSendEvent = 0x80134480;       // (layout, event)
constexpr uint32_t kSceneWarning = 0x8003681C;    // (scene, on)
constexpr uint32_t kNames = 0x8026F98C;

// Pane / animation names (offsets into kNames).
constexpr uint32_t kRoot = 0x10D;
constexpr uint32_t kTuusin = 0x113;
constexpr uint32_t kNunchukOk = 0x11A;
constexpr uint32_t kNoNunchuk = 0x126;
constexpr uint32_t kWarning4 = 0x132;
constexpr uint32_t kReady = 0x138;
constexpr uint32_t kWarning2 = 0x142;
constexpr uint32_t kRemoteReady = 0x148;
constexpr uint32_t kNoRemote = 0x156;
constexpr uint32_t kWarning3 = 0x161;
constexpr uint32_t kNunchukEnd = 0x167;
constexpr uint32_t kWarningEnd = 0x174;
constexpr uint32_t kPoseStart = 0x180;

constexpr uint32_t kButtonA = 0x800;

struct Main : lift::View {
    WP_FIELD(int32_t, sequence, 0x0)
    WP_FIELD(int32_t, phase, 0x4)
    WP_FIELD(int32_t, pause_state, 0x10)
    WP_FIELD(uint32_t, pause_frames, 0x14)
    WP_FIELD(int32_t, pause_choice, 0x18)
    WP_FIELD(int32_t, kind, 0x54)
    WP_FIELD(int32_t, pointer_was_on, 0x68)
    WP_FIELD(int32_t, endless_streak, 0x20C)
    WP_FIELD(int32_t, endless_round, 0x3DC)
    WP_FIELD(int32_t, outcome, 0x444)
    WP_FIELD(int32_t, restart, 0x46C)
    WP_FIELD(uint32_t, pause_backdrop, 0x883D0)   // layout holders (+8: the layout; byte 0: hidden)
    WP_FIELD(uint32_t, pause_layout, 0x883E8)
    WP_FIELD(uint32_t, warning_layout, 0x883EC)
    WP_FIELD(int32_t, warning, 0x88408)
    WP_FIELD(int32_t, boot_frames, 0x88414)
};

struct Menu {
    Cpu& c;
    Main main;
    OriginalFrame& frame;

    uint32_t input(uint32_t game_site, uint32_t input_site) {
        uint32_t game = call(c, game_site, kGetGame);
        return call(c, input_site, kGetPlayerInput, {game, 0});
    }
    uint32_t pressed(uint32_t game_site, uint32_t pad_site) {
        uint32_t game = call(c, game_site, kGetGame);
        return rd32(call(c, pad_site, kGetPad, {game, 0}) + 0x10);
    }
    uint32_t layout(uint32_t holder) const { return rd32(holder + 8); }

    // Find the pane (kind 3) and animation (kind 0), play it and show the pane. `pane_reg`/`anim_reg`
    // name the registers the original keeps them in.
    void animate(uint32_t holder, uint32_t pane_name, uint32_t animation_name, uint32_t site, int pane_reg) {
        c.r[pane_reg] = call(c, site, kFindPane, {layout(holder), 3, kNames + pane_name});
        uint32_t animation = call(c, site + 0x1C, kFindPane, {layout(holder), 0, animation_name});
        call(c, site + 0x38, kPlayAnimation, {layout(holder), c.r[pane_reg], animation, 0});
        call(c, site + 0x50, kShowPane, {layout(holder), c.r[pane_reg], 0});
    }
    // The pointer was turned on while the dialog shows: remember it and turn it off.
    void pointer_check(uint32_t site) {
        if (call(c, site + 0xC, kPointerOn, {input(site, site + 8)}) != 0) {
            main.pointer_was_on() = 1;
            call(c, site + 0x30, kSetPointer, {input(site + 0x20, site + 0x28), 0});
        }
    }
    void key(uint32_t site, uint32_t data, uint8_t code) {
        wr32(frame.at(data), 5);
        wr8(frame.at(data + 4), code);
        call(c, site, kSendEvent, {layout(main.pause_layout()), frame.at(data)});
    }

    void warning_dialog();
    void pause_menu();
};

void Menu::warning_dialog() {
    c.r[31] = 0;  // close the dialog
    switch (main.warning()) {
    case 1: {
        int32_t frames = main.boot_frames();
        main.boot_frames() = frames + 1;
        if (frames >= 300) {
            c.r[31] = 1;
        }
        // Feature skip_intros: any button closes it too.
        if (features::skip_intros() && (pressed(0x8002E0A4, 0x8002E0A4) & features::kAnyButton)) {
            c.r[31] = 1;
        }
        pointer_check(0x8002E0A4);
        break;
    }
    case 4:
        if (call(c, 0x8002E0E8, kConnected, {input(0x8002E0DC, 0x8002E0E4)}) == 0) {
            main.warning() = 3;
            c.r[29] = kNames;
            animate(main.warning_layout(), kRoot, kNames + kTuusin, 0x8002E118, 28);
            wr8(layout(main.warning_layout()), 0);
            break;
        }
        if (call(c, 0x8002E190, kHasNunchuk, {input(0x8002E184, 0x8002E18C)}) != 0) {
            c.r[28] = kNames + kNunchukOk;
            if (pressed(0x8002E1A8, 0x8002E1B0) & kButtonA) {
                c.r[31] = 1;
            }
        } else {
            c.r[28] = kNames + kNoNunchuk;
        }
        animate(main.warning_layout(), kWarning4, c.r[28], 0x8002E1F8, 29);
        pointer_check(0x8002E24C);
        break;
    case 2:
        if (pressed(0x8002E284, 0x8002E28C) & kButtonA) {
            c.r[31] = 1;
        }
        c.r[28] = kNames + kReady;
        animate(main.warning_layout(), kWarning2, c.r[28], 0x8002E2C4, 29);
        pointer_check(0x8002E318);
        break;
    case 3:
        if (call(c, 0x8002E35C, kConnected, {input(0x8002E350, 0x8002E358)}) != 0) {
            c.r[28] = kNames + kRemoteReady;
            if (pressed(0x8002E374, 0x8002E37C) & kButtonA) {
                c.r[31] = 1;
            }
        } else {
            c.r[28] = kNames + kNoRemote;
        }
        animate(main.warning_layout(), kWarning3, c.r[28], 0x8002E3C4, 29);
        pointer_check(0x8002E418);
        break;
    default:
        break;
    }
    if (c.r[31] != 0) {
        c.r[29] = kNames + (main.warning() == 1 ? kNunchukEnd : kWarningEnd);
        animate(main.warning_layout(), kRoot, c.r[29], 0x8002E490, 28);
        c.r[29] = 0;
        main.warning() = 0;
        if (main.pointer_was_on() != 0) {
            call(c, 0x8002E50C, kSetPointer, {input(0x8002E4FC, 0x8002E504), 1});
            main.pointer_was_on() = static_cast<int32_t>(c.r[29]);
        }
    }
}

void Menu::pause_menu() {
    switch (main.pause_state()) {
    case 0: {
        c.r[31] = kNames;
        c.r[29] = 1;
        wr8(layout(main.pause_backdrop()), static_cast<uint8_t>(c.r[29]));
        animate(main.pause_layout(), kRoot, kNames + kPoseStart, 0x8002E5B0, 28);
        wr8(layout(main.pause_layout()), 0);
        main.pause_state() = static_cast<int32_t>(c.r[29]);
        main.pause_frames() = 0;
        break;
    }
    case 1: {
        uint32_t frames = main.pause_frames() + 1;
        main.pause_frames() = frames;
        if (frames >= 0x14) {
            call(c, 0x8002E648, kSetPointer, {input(0x8002E638, 0x8002E640), 1});
        }
        if (call(c, 0x8002E658, kPointerValid, {input(0x8002E64C, 0x8002E654)}) != 0) {
            c.r[31] = call(c, 0x8002E670, kPointerX, {input(0x8002E664, 0x8002E66C)});
            int32_t y = static_cast<int32_t>(call(c, 0x8002E684, kPointerY, {input(0x8002E678, 0x8002E680)}));
            float fy = frame.int_to_float(0x50, y);
            float fx = frame.int_to_float(0x58, static_cast<int32_t>(c.r[31]));
            wr32(frame.at(0x40), 1);
            stf(frame.at(0x48), fy);
            stf(frame.at(0x44), fx);
            wr8(frame.at(0x4C), 0);
            call(c, 0x8002E6E0, kSendEvent, {layout(main.pause_layout()), frame.at(0x40)});
        } else {
            if (pressed(0x8002E6E8, 0x8002E6F0) & 0x8) {
                key(0x8002E720, 0x38, 'w');
            }
            if (pressed(0x8002E724, 0x8002E72C) & 0x4) {
                key(0x8002E75C, 0x30, 's');
            }
            if (pressed(0x8002E760, 0x8002E768) & 0x1) {
                key(0x8002E798, 0x28, 'a');
            }
            if (pressed(0x8002E79C, 0x8002E7A4) & 0x2) {
                key(0x8002E7D4, 0x20, 'd');
            }
        }
        if (pressed(0x8002E7D8, 0x8002E7E0) & 0x4800) {
            key(0x8002E810, 0x18, 'x');
        }
        if (pressed(0x8002E814, 0x8002E81C) & 0x2400) {
            key(0x8002E84C, 0x10, 'z');
        }
        if (pressed(0x8002E850, 0x8002E858) & 0x200) {
            key(0x8002E888, 0x8, 'q');
        }
        break;
    }
    case 2: {
        wr8(layout(main.pause_layout()), 1);
        c.r[31] = 0;
        int32_t choice = main.pause_choice();
        main.pause_state() = 0;
        if (choice == 0) {
            call(c, 0x8002E8D0, 0x80024444, {main.base});
            call(c, 0x8002E8D8, 0x8002FF38, {main.base});
            call(c, 0x8002E8E0, 0x80033854, {main.base});
            call(c, 0x8002E8E8, 0x80028D20, {main.base});
            wr8(layout(main.pause_backdrop()), static_cast<uint8_t>(c.r[31]));
        } else if (choice == 1) {
            call(c, 0x8002E904, 0x800365EC, {main.base});
            call(c, 0x8002E90C, 0x80036414, {main.base});
            if (main.kind() == 1) {
                main.endless_streak() = static_cast<int32_t>(c.r[31]);
                wr32(main.base + 0x3E4 + 4 * static_cast<uint32_t>(main.endless_round().get()), c.r[31]);
            }
            main.restart() = 1;
            main.phase() = 4;
        } else if (choice == 2) {
            call(c, 0x8002E948, 0x800365EC, {main.base});
            call(c, 0x8002E950, 0x80036414, {main.base});
            main.outcome() = 4;
            main.phase() = 7;
        }
        call(c, 0x8002E96C, kSceneWarning, {main.base, 0});
        uint32_t game = call(c, 0x8002E970, kGetGame);
        call(c, 0x8002E978, kCloseMenu, {game, 1});
        call(c, 0x8002E98C, kSetPointer, {input(0x8002E97C, 0x8002E984), 0});
        break;
    }
    default:
        break;
    }
}

// (Main*)
WP_LIFTED(kLifted, system_menu, "Scene_SystemMenu", Ret::None) {
    Main main;
    main.base = c.r[3];
    OriginalFrame frame(c, 0x70);
    frame.save_gpr(31, 0x6C);
    frame.save_gpr(30, 0x68);
    c.r[30] = main.base;
    frame.save_gpr(29, 0x64);
    frame.save_gpr(28, 0x60);
    Menu menu{c, main, frame};

    uint32_t game = call(c, 0x8002E034, kGetGame);
    if (call(c, 0x8002E03C, kMenuClosing, {game, 4}) == 0) {
        game = call(c, 0x8002E048, kGetGame);
        if (call(c, 0x8002E050, kMenuOpen, {game, 4}) != 0) {
            menu.warning_dialog();
        } else if (rd8(menu.layout(main.warning_layout())) == 0) {
            wr8(menu.layout(main.warning_layout()), 1);
        }
        game = call(c, 0x8002E538, kGetGame);
        if (call(c, 0x8002E540, kMenuClosing, {game, 1}) == 0 && main.sequence() == 8 && main.phase() == 6) {
            menu.pause_menu();
        }
    }

    frame.restore_gpr(31, 0x6C);
    frame.restore_gpr(30, 0x68);
    frame.restore_gpr(29, 0x64);
    frame.restore_gpr(28, 0x60);
}

}  // namespace

}  // namespace musclemarch
