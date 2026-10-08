#include "wp/input_hle.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "wp/gamepad.h"
#include "wp/hle.h"
#include "wp/input.h"
#include "wp/memory.h"
#include "wp/settings.h"
#include "wp/wiimote.h"

// Native controller input at the WPAD/KPAD API level.
//
// input.native=0: the translated SDK talks to the emulated Bluetooth Wii Remote.
// input.native=1: WPADRead is native, the rest as 0.
// input.native=2 (default): the whole controller stack is native. KPADInit no longer
//                 starts WPAD/WUD/Bluetooth; the entry points the game, the NU
//                 engine and the HOME Button menu call are answered here.
//                 WPAD's per-channel control blocks are still pointed at their
//                 static storage (marked "no controller") so the remaining
//                 translated accessors read valid memory and bail out before
//                 they would reach the Bluetooth stack.
//
// Addresses below are Muscle March's (game-specific constants, see
// games/musclemarch/analysis/symbols_extra.csv).

namespace wp::input_hle {

namespace {

constexpr uint32_t kStatusSize = 0x32;          // WPADFSStatus
constexpr uint32_t kControlPointers = 0x8037E4F0;  // __rvl_p_wpadcb[4]
constexpr uint32_t kControlBlocks = 0x8037F520;    // static control blocks
constexpr uint32_t kControlBlockSize = 0xBE0;
constexpr uint32_t kControlStatus = 0x900;         // s32 device status, -1 = none
constexpr int32_t kErrNone = 0;
constexpr int32_t kErrNoController = -1;
constexpr uint8_t kDevFreestyle = 1;               // Wii Remote + Nunchuk
constexpr uint8_t kDevNotFound = 253;
constexpr uint32_t kChannels = 4;

int level() {
    static const int value = [] {
        // Default: fully native (2). 0 = emulated Bluetooth Wii Remote, 1 = native WPADRead only.
        std::string text = settings::text("input.native", "WP_NATIVE_INPUT");
        return text.empty() ? 2 : std::atoi(text.c_str());
    }();
    return value;
}

bool full() {
    return level() >= 2;
}

struct Channel {
    uint32_t connect_callback = 0;
    bool connect_pending = false;
    uint32_t data_format = 0;
    uint32_t dpd_command = 0;
    uint32_t kpad_hold = 0;
};

Channel g_channels[kChannels];
bool g_motor_enabled = true;

// Call a guest function with up to three arguments, preserving the caller's registers.
void invoke(Cpu& c, uint32_t function, uint32_t a, uint32_t b, uint32_t d = 0) {
    if (function == 0) {
        return;
    }
    Cpu saved = c;
    c.r[3] = a;
    c.r[4] = b;
    c.r[5] = d;
    call(c, function);
    c = saved;
}

// Tell the game the Wii Remote on channel 0 is connected, once its callback is registered.
void deliver_connect(Cpu& c) {
    Channel& ch = g_channels[0];
    if (ch.connect_pending && ch.connect_callback) {
        ch.connect_pending = false;
        invoke(c, ch.connect_callback, 0, static_cast<uint32_t>(kErrNone));
    }
}

void put_float(uint32_t address, float value) {
    uint32_t bits;
    std::memcpy(&bits, &value, sizeof bits);
    wr32(address, bits);
}

int16_t be16(const uint8_t* p) {
    return static_cast<int16_t>((p[0] << 8) | p[1]);
}

#define FALLBACK(name)              \
    if (!full()) {                  \
        call_original(c, name);     \
        return;                     \
    }

// --- KPAD ---------------------------------------------------------------

void kpad_init(Cpu& c) {
    FALLBACK("KPADInit");
    for (uint32_t i = 0; i < kChannels; i++) {
        uint32_t block = kControlBlocks + i * kControlBlockSize;
        wr32(kControlPointers + 4 * i, block);
        wr32(block + kControlStatus, static_cast<uint32_t>(kErrNoController));
    }
}

void kpad_set_connect_callback(Cpu& c) {
    FALLBACK("KPADSetConnectCallback");
    uint32_t channel = c.r[3];
    if (channel < kChannels) {
        uint32_t previous = g_channels[channel].connect_callback;
        g_channels[channel].connect_callback = c.r[4];
        g_channels[channel].connect_pending = channel == 0 && c.r[4] != 0;
        c.r[3] = previous;
    }
}

// KPADRead(chan, KPADStatus* samples, u32 length): the HOME Button menu's cursor path.
void kpad_read(Cpu& c) {
    FALLBACK("KPADRead");
    deliver_connect(c);
    uint32_t channel = c.r[3];
    uint32_t out = c.r[4];
    if (channel != 0 || c.r[5] == 0) {
        c.r[3] = 0;
        return;
    }
    uint8_t status[kStatusSize];
    wiimote::native_status(0, status);
    input::Sample sample = input::sample(0);
    std::memset(host(out), 0, 0xF0);
    Channel& ch = g_channels[0];
    uint32_t hold = static_cast<uint32_t>((status[0] << 8) | status[1]);
    wr32(out + 0x00, hold);
    wr32(out + 0x04, hold & ~ch.kpad_hold);
    wr32(out + 0x08, ch.kpad_hold & ~hold);
    ch.kpad_hold = hold;
    for (int i = 0; i < 3; i++) {
        put_float(out + 0x0C + 4 * i, be16(status + 2 + 2 * i) / 104.0f);
    }
    if (sample.pointer_valid) {
        put_float(out + 0x20, sample.pointer_x);
        put_float(out + 0x24, sample.pointer_y);
    }
    put_float(out + 0x34, 1.0f);  // horizon
    put_float(out + 0x48, 1.5f);  // distance (m)
    wr8(out + 0x5C, kDevFreestyle);
    wr8(out + 0x5D, static_cast<uint8_t>(kErrNone));
    wr8(out + 0x5E, sample.pointer_valid ? 2 : 0);
    wr8(out + 0x5F, static_cast<uint8_t>(ch.data_format));
    put_float(out + 0x60, static_cast<int8_t>(status[0x30]) / 96.0f);
    put_float(out + 0x64, static_cast<int8_t>(status[0x31]) / 96.0f);
    for (int i = 0; i < 3; i++) {
        put_float(out + 0x68 + 4 * i, be16(status + 0x2A + 2 * i) / 204.0f);
    }
    c.r[3] = 1;
}

// --- WPAD ---------------------------------------------------------------

void wpad_register_allocator(Cpu& c) {
    FALLBACK("WPADRegisterAllocator");
}

void wpad_probe(Cpu& c) {
    FALLBACK("WPADProbe");
    deliver_connect(c);
    uint32_t channel = c.r[3];
    uint32_t type = c.r[4];
    if (channel == 0) {
        if (type) {
            wr32(type, kDevFreestyle);
        }
        c.r[3] = static_cast<uint32_t>(kErrNone);
    } else {
        if (type) {
            wr32(type, kDevNotFound);
        }
        c.r[3] = static_cast<uint32_t>(kErrNoController);
    }
}

void wpad_get_acc_gravity_unit(Cpu& c) {
    FALLBACK("WPADGetAccGravityUnit");
    uint32_t type = c.r[4];
    uint32_t out = c.r[5];
    int16_t unit = type == 1 ? 204 : 104;  // Nunchuk / Wii Remote, in WPADStatus units
    for (int i = 0; i < 3; i++) {
        wr16(out + 2 * i, static_cast<uint16_t>(unit));
    }
}

void wpad_set_data_format(Cpu& c) {
    FALLBACK("WPADSetDataFormat");
    if (c.r[3] < kChannels) {
        g_channels[c.r[3]].data_format = c.r[4];
    }
    c.r[3] = static_cast<uint32_t>(c.r[3] == 0 ? kErrNone : kErrNoController);
}

void wpad_set_auto_sampling_buf(Cpu& c) {
    FALLBACK("WPADSetAutoSamplingBuf");
}

void wpad_get_latest_index_in_buf(Cpu& c) {
    FALLBACK("WPADGetLatestIndexInBuf");
    c.r[3] = 0;
}

void wpad_control_dpd(Cpu& c) {
    FALLBACK("WPADControlDpd");
    uint32_t channel = c.r[3];
    uint32_t callback = c.r[5];
    int32_t result = channel == 0 ? kErrNone : kErrNoController;
    if (channel < kChannels) {
        g_channels[channel].dpd_command = c.r[4];
    }
    invoke(c, callback, channel, static_cast<uint32_t>(result));
    c.r[3] = static_cast<uint32_t>(result);
}

void wpad_is_dpd_enabled(Cpu& c) {
    FALLBACK("WPADIsDpdEnabled");
    c.r[3] = c.r[3] < kChannels ? g_channels[c.r[3]].dpd_command : 0;
}

// Called by the engine's connect handler (and the HOME menu) after a remote connects.
void wpad_connect_setup(Cpu& c) {
    FALLBACK("WPADiConnectSetup");
    c.r[3] = static_cast<uint32_t>(c.r[3] == 0 ? kErrNone : kErrNoController);
}

// WPADGetInfoAsync(chan, WPADInfo* info, callback): battery etc. for the HOME menu.
void wpad_get_info_async(Cpu& c) {
    FALLBACK("WPADGetInfoAsync");
    uint32_t channel = c.r[3];
    uint32_t info = c.r[4];
    uint32_t callback = c.r[5];
    int32_t result = channel == 0 ? kErrNone : kErrNoController;
    if (channel == 0 && info) {
        std::memset(host(info), 0, 0x18);
        wr32(info + 0x00, g_channels[0].dpd_command != 0);  // dpd
        wr32(info + 0x04, 0);                              // speaker
        wr32(info + 0x08, 1);                              // attach (Nunchuk)
        wr32(info + 0x0C, 0);                              // lowBat
        wr32(info + 0x10, 0);                              // nearempty
        wr8(info + 0x14, 4);                               // battery level (full)
        wr8(info + 0x15, 0x1);                             // LEDs: player 1
    }
    invoke(c, callback, channel, static_cast<uint32_t>(result));
    c.r[3] = static_cast<uint32_t>(result);
}

void wpad_control_motor(Cpu& c) {
    FALLBACK("WPADControlMotor");
    if (c.r[3] < kChannels) {
        gamepad::set_outputs(c.r[3], g_motor_enabled && c.r[4] == 1, 1);
    }
}

void wpad_enable_motor(Cpu& c) {
    FALLBACK("WPADEnableMotor");
    g_motor_enabled = c.r[3] != 0;
}

void wpad_is_motor_enabled(Cpu& c) {
    FALLBACK("WPADIsMotorEnabled");
    c.r[3] = g_motor_enabled ? 1 : 0;
}

// WPADSaveConfig(SCFlushCallback): nothing to save; report success.
void wpad_save_config(Cpu& c) {
    FALLBACK("WPADSaveConfig");
    invoke(c, c.r[3], 0, 0);
    c.r[3] = 1;
}

// Wii Remote speaker: no speaker on the host side (yet); report "no controller".
void wpad_control_speaker(Cpu& c) {
    FALLBACK("WPADControlSpeaker");
    uint32_t channel = c.r[3];
    invoke(c, c.r[5], channel, static_cast<uint32_t>(kErrNoController));
    c.r[3] = static_cast<uint32_t>(kErrNoController);
}

void wpad_can_send_stream_data(Cpu& c) {
    FALLBACK("WPADCanSendStreamData");
    c.r[3] = 0;
}

void wpad_send_stream_data(Cpu& c) {
    FALLBACK("WPADSendStreamData");
    c.r[3] = static_cast<uint32_t>(kErrNoController);
}

// Pairing / sync (HOME menu "Wii Remote Settings"): nothing to pair.
void wpad_sync_801db7f0(Cpu& c) {
    FALLBACK("WPADiSync_801DB7F0");
    c.r[3] = 0;
}

void wpad_sync_801db800(Cpu& c) {
    FALLBACK("WPADiSync_801DB800");
    c.r[3] = 0;
}

void wpad_sync_801db810(Cpu& c) {
    FALLBACK("WPADiSync_801DB810");
    c.r[3] = 0;
}

// Status clamping helpers: native statuses are already in range.
void wpad_clamp_801e9a80(Cpu& c) {
    FALLBACK("WPADiClamp_801E9A80");
}

void wpad_clamp_801e9be0(Cpu& c) {
    FALLBACK("WPADiClamp_801E9BE0");
}

}  // namespace

bool enabled() {
    return level() >= 1;
}

void wpad_read(Cpu& c) {
    const uint32_t channel = c.r[3];
    const uint32_t status = c.r[4];
    static const bool trace = std::getenv("WP_LOG_WPADREAD") != nullptr;
    if (!enabled() || (channel != 0 && !full())) {
        call_original(c, "WPADRead");
        if (trace && channel == 0) {
            std::fprintf(stderr, "WPADRead orig:");
            for (uint32_t i = 0; i < kStatusSize; i++) {
                std::fprintf(stderr, "%s%02x", i % 8 ? "" : " ", rd8(status + i));
            }
            std::fputc('\n', stderr);
        }
        return;
    }
    if (full()) {
        deliver_connect(c);
    }
    if (channel != 0) {
        std::memset(host(status), 0, kStatusSize);
        wr8(status + 0x28, kDevNotFound);
        wr8(status + 0x29, static_cast<uint8_t>(kErrNoController));
        return;
    }
    uint8_t bytes[kStatusSize];
    wiimote::native_status(channel, bytes);
    std::memcpy(host(status), bytes, kStatusSize);
    if (trace) {
        std::fprintf(stderr, "WPADRead native:");
        for (uint32_t i = 0; i < kStatusSize; i++) {
            std::fprintf(stderr, "%s%02x", i % 8 ? "" : " ", bytes[i]);
        }
        std::fputc('\n', stderr);
    }
}

const Handler kHandlers[] = {
    {"KPADInit", kpad_init},
    {"KPADSetConnectCallback", kpad_set_connect_callback},
    {"KPADRead", kpad_read},
    {"WPADRegisterAllocator", wpad_register_allocator},
    {"WPADProbe", wpad_probe},
    {"WPADGetAccGravityUnit", wpad_get_acc_gravity_unit},
    {"WPADSetDataFormat", wpad_set_data_format},
    {"WPADSetAutoSamplingBuf", wpad_set_auto_sampling_buf},
    {"WPADGetLatestIndexInBuf", wpad_get_latest_index_in_buf},
    {"WPADControlDpd", wpad_control_dpd},
    {"WPADIsDpdEnabled", wpad_is_dpd_enabled},
    {"WPADiConnectSetup", wpad_connect_setup},
    {"WPADGetInfoAsync", wpad_get_info_async},
    {"WPADControlMotor", wpad_control_motor},
    {"WPADEnableMotor", wpad_enable_motor},
    {"WPADIsMotorEnabled", wpad_is_motor_enabled},
    {"WPADSaveConfig", wpad_save_config},
    {"WPADControlSpeaker", wpad_control_speaker},
    {"WPADCanSendStreamData", wpad_can_send_stream_data},
    {"WPADSendStreamData", wpad_send_stream_data},
    {"WPADiSync_801DB7F0", wpad_sync_801db7f0},
    {"WPADiSync_801DB800", wpad_sync_801db800},
    {"WPADiSync_801DB810", wpad_sync_801db810},
    {"WPADiClamp_801E9A80", wpad_clamp_801e9a80},
    {"WPADiClamp_801E9BE0", wpad_clamp_801e9be0},
    {nullptr, nullptr},
};

}
