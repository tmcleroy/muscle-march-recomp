#pragma once
// The per-player input object (docs/GAME.md, "Player input object"), shared by the lifted input
// code. One per player; the game reaches it through game_player_input().

#include <cstdint>

#include "wp/lift.h"

namespace musclemarch {

// The controller-sample history: entries of 0x44 bytes from the object itself. Entry 1 holds this
// frame's samples, entry 2 last frame's, and so on up to entry 32; the motion update shifts them
// every frame. Each entry has 14 signed 16-bit samples at +0x28.
constexpr uint32_t kHistoryStride = 0x44;
constexpr uint32_t kHistorySamples = 0x28;
constexpr int kHistoryEntries = 32;

// Sample indexes (into the 16-bit samples of an entry).
constexpr uint32_t kNunchukSideTiltSample = 11;  // -256 * Nunchuk y
constexpr uint32_t kRemoteSideTiltSample = 13;   // -256 * Remote y

struct PlayerInput : wp::lift::View {
    WP_FIELD(uint32_t, channel, 0x000)
    WP_FIELD(int32_t, read_state, 0x008)       // nonzero: controllers are not read; -1: no controller
    WP_FIELD(uint32_t, extension, 0x010)       // WPAD extension type (2 = Nunchuk)
    WP_FIELD(uint32_t, status, 0x044)          // WPADFSStatus* of this frame
    WP_FIELD(int16_t, nunchuk_side_tilt, 0x082)
    WP_FIELD(int16_t, remote_side_tilt, 0x086)
    wp::lift::Field<float> meter(uint32_t pose) const { return wp::lift::Field<float>(base + 0x8C8 + 4 * pose); }
    WP_FIELD(uint32_t, remote_tilted, 0x8D8)
    WP_FIELD(uint32_t, nunchuk_tilted, 0x8DC)
    WP_FIELD(uint32_t, remote_tilted_before, 0x8E0)
    WP_FIELD(uint32_t, nunchuk_tilted_before, 0x8E4)
    WP_FIELD(uint32_t, last_tick, 0x908)
    WP_FIELD(uint8_t, meters_moving, 0x90C)    // a meter is between 0 and 1: pose changes wait
    WP_FIELD(uint8_t, pose_changed, 0x90D)
    WP_FIELD(float, remote_tilt, 0x910)
    WP_FIELD(float, remote_speed, 0x914)
    WP_FIELD(uint32_t, remote_class, 0x918)
    WP_FIELD(uint8_t, remote_moving, 0x91C)
    WP_FIELD(float, remote_tilt_before, 0x920)
    WP_FIELD(float, remote_peak_speed, 0x924)
    WP_FIELD(float, nunchuk_tilt, 0x928)
    WP_FIELD(float, nunchuk_speed, 0x92C)
    WP_FIELD(uint32_t, nunchuk_class, 0x930)
    WP_FIELD(uint8_t, nunchuk_moving, 0x934)
    WP_FIELD(float, nunchuk_tilt_before, 0x938)
    WP_FIELD(float, nunchuk_peak_speed, 0x93C)
    WP_FIELD(uint32_t, pose, 0x940)
    WP_FIELD(float, hold_time, 0x944)

    // A 16-bit sample of history entry `entry` (1 = this frame).
    wp::lift::Field<int16_t> sample(int entry, uint32_t index) const {
        return wp::lift::Field<int16_t>(base + kHistoryStride * static_cast<uint32_t>(entry) + kHistorySamples + 2 * index);
    }
};

// Guest functions: the game object (fn_80020F4C) and its player input objects (fn_80020F68).
constexpr uint32_t kGetGame = 0x80020F4C;
constexpr uint32_t kGetPlayerInput = 0x80020F68;  // (game, player)

inline PlayerInput game_player_input(wp::Cpu& c, uint32_t player) {
    uint32_t game = wp::lift::gcall(c, kGetGame);
    PlayerInput input;
    input.base = wp::lift::gcall(c, kGetPlayerInput, {game, player});
    return input;
}

}  // namespace musclemarch
