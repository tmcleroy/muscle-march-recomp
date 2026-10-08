// Pad controller: per-frame button state with edge masks and key repeat.
//
// Lifted from fn_8021FA1C (the middleware's controller object, updated once per frame for each
// channel). It reads the Wii Remote, then derives:
//   held      (+0x1C)  buttons down now (copied from the WPAD status by fn_8021FCC0)
//   pressed   (+0x14)  went down this frame
//   released  (+0x18)  went up this frame
//   up        (+0x20)  ~held
//   repeat    (+0x24)  key-repeat pulses: on the press, after `repeat_delay` ms of holding, then
//                      every `repeat_interval` ms. Mode 0 repeats the whole button set together
//                      (restarting when the set changes); mode 1 repeats each button on its own.
// Menus read `pressed` and `repeat`, so a "press any button" check is pressed != 0.

#include "wp/lift.h"
#include "wp/threads.h"

namespace musclemarch {

namespace {

using namespace wp;
using namespace wp::lift;

const lift::Group kLifted{"game.lifted", "WP_LIFTED"};

constexpr uint32_t kWPADRead = 0x801DD510;
constexpr uint32_t kWPADGetLatestIndexInBuf = 0x801DD710;
constexpr uint32_t kCopyButtons = 0x8021FCC0;    // (state, status): held from the status
constexpr uint32_t kCopyExtension = 0x8021FD24;  // (channel, state, status): sticks/extension
constexpr uint32_t kBusClock = 0x800000F8;

enum RepeatMode : int32_t { kWholePad = 0, kPerButton = 1 };
enum RepeatState : int32_t { kIdle = 0, kWaitingDelay = 1, kRepeating = 2 };

struct Pad : lift::View {
    WP_FIELD(uint32_t, channel, 0x004)
    WP_FIELD(uint32_t, pressed, 0x014)
    WP_FIELD(uint32_t, released, 0x018)
    WP_FIELD(uint32_t, held, 0x01C)
    WP_FIELD(uint32_t, up, 0x020)
    WP_FIELD(uint32_t, repeat, 0x024)
    WP_FIELD(uint32_t, status, 0x044)                // WPAD status buffer
    WP_FIELD(uint8_t, previous_buffer_index, 0x6DE)
    WP_FIELD(uint8_t, buffer_index, 0x6DF)
    WP_FIELD(uint32_t, previous_held, 0x6E4)
    WP_FIELD(int32_t, repeat_mode, 0x6EC)
    WP_FIELD(uint32_t, repeat_held, 0x6F0)           // the button set mode 0 is repeating
    WP_FIELD(uint32_t, repeat_delay, 0x6F4)          // ms
    WP_FIELD(uint32_t, repeat_interval, 0x6F8)       // ms
    lift::Field<uint32_t> button_since(uint32_t bit) const { return lift::Field<uint32_t>(base + 0x6FC + 4 * bit); }
    WP_FIELD(uint32_t, pad_since, 0x778)
    lift::Field<int32_t> button_state(uint32_t bit) const { return lift::Field<int32_t>(base + 0x77C + 4 * bit); }
    WP_FIELD(int32_t, pad_state, 0x7F8)
};

// One repeat state machine step for a held button (or button set). Returns whether it fires.
// `restart` stores the time of a pulse.
bool repeat_step(Pad pad, lift::Field<int32_t> state, lift::Field<uint32_t> since, uint32_t now) {
    uint32_t elapsed = now - since;
    switch (state.get()) {
    case kIdle:
        since = now;
        state = kWaitingDelay;
        return true;
    case kWaitingDelay:
        if (elapsed < pad.repeat_delay()) {
            return false;
        }
        since = now;
        state = kRepeating;
        return true;
    case kRepeating:
        if (elapsed < pad.repeat_interval()) {
            return false;
        }
        since = now;
        return true;
    default:
        return false;
    }
}

// Milliseconds from OSGetTick, as the original computes them (timer = bus clock / 4).
uint32_t now_ms(Cpu& c) {
    static const uint32_t os_get_tick = symbol_address("OSGetTick");
    uint32_t ticks = gcall_from(c, 0x8021FAAC, os_get_tick);
    uint32_t ticks_per_ms = (rd32(kBusClock) >> 2) / 1000;
    return ticks / ticks_per_ms;
}

// (Pad*)
WP_LIFTED(kLifted, update_pad, "Pad_Update", Ret::None) {
    Pad pad;
    pad.base = c.r[3];
    OriginalFrame frame(c, 0x10);
    frame.save_gpr(31, 0xC);
    c.r[31] = pad.base;

    gcall_from(c, 0x8021FA3C, kWPADRead, {pad.channel(), pad.status()});
    gcall_from(c, 0x8021FA48, kCopyButtons, {pad.base + 4, pad.status()});
    gcall_from(c, 0x8021FA58, kCopyExtension, {pad.channel(), pad.base + 4, pad.status()});

    uint32_t held = pad.held();
    uint32_t changed = held ^ pad.previous_held();
    pad.up() = ~held;
    pad.pressed() = held & changed;
    pad.released() = ~held & changed;

    uint32_t repeat = 0;
    if (pad.repeat_delay() != 0 && pad.repeat_interval() != 0) {
        uint32_t now = now_ms(c);
        if (pad.repeat_mode() == kPerButton) {
            for (uint32_t bit = 0; bit < 32; bit++) {
                repeat >>= 1;
                if ((held >> bit) & 1) {
                    if (repeat_step(pad, pad.button_state(bit), pad.button_since(bit), now)) {
                        repeat |= 0x80000000u;
                    }
                } else {
                    pad.button_state(bit) = kIdle;
                }
            }
        } else if (pad.repeat_mode() == kWholePad) {
            if (held == 0) {
                pad.pad_state() = kIdle;
            } else if (held != pad.repeat_held()) {
                pad.pad_since() = now;
                pad.pad_state() = kWaitingDelay;
                repeat = held;
            } else if (repeat_step(pad, pad.pad_state(), pad.pad_since(), now)) {
                repeat = held;
            }
        }
        pad.repeat_held() = held;
    }
    pad.repeat() = repeat;

    pad.previous_held() = pad.held();
    pad.previous_buffer_index() = pad.buffer_index();
    pad.buffer_index() = static_cast<uint8_t>(gcall_from(c, 0x8021FCA8, kWPADGetLatestIndexInBuf, {pad.channel()}));

    frame.restore_gpr(31, 0xC);
}

}  // namespace

}  // namespace musclemarch
