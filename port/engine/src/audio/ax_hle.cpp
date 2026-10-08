// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Native AX mixer: what the Wii AX DSP microcode does, in C++. The SDK's AX
// library (translated PPC) builds a command list every 3 ms frame; this runs
// it: decode each voice (ADPCM/PCM through the accelerator), resample, apply
// the volume envelope and filters, mix into main/aux buses, hand the aux buses
// to the CPU effects and mix them back, and write the final 32 kHz stereo frame
// for the AI DMA. Ported from Dolphin's AX HLE (AXWii.cpp, AXVoice.h, AX.cpp).
//
// Only the newer AXWii microcodes are handled (no per-millisecond PB updates).

#include "wp/ax_hle.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "Core/DSP/DSPAccelerator.h"
#include "wp/dsp_roms.h"
#include "wp/memory.h"

namespace wp::ax {

namespace {

using u16 = uint16_t;
using s16 = int16_t;
using u32 = uint32_t;
using s32 = int32_t;
using s64 = int64_t;

#define HILO_TO_32(name) ((u32(name##_hi) << 16) | name##_lo)

// --- parameter block (AXStructs.h) ------------------------------------------

struct VolumeData {
    u16 volume;
    u16 volume_delta;
};

struct PBMixerWii {
    VolumeData main_left, main_right;
    VolumeData auxA_left, auxA_right;
    VolumeData auxB_left, auxB_right;
    VolumeData auxC_left, auxC_right;
    VolumeData main_surround;
    VolumeData auxA_surround;
    VolumeData auxB_surround;
    VolumeData auxC_surround;
};

struct PBMixerWM {
    VolumeData main0, aux0;
    VolumeData main1, aux1;
    VolumeData main2, aux2;
    VolumeData main3, aux3;
};

struct PBInitialTimeDelay {
    u16 on;
    u16 addrMemHigh;
    u16 addrMemLow;
    u16 offsetLeft;
    u16 offsetRight;
    u16 targetLeft;
    u16 targetRight;
};

struct PBUpdatesWii {
    u16 num_updates[3];
    u16 data_hi;
    u16 data_lo;
};

struct PBDpopWii {
    s16 main_left, auxA_left, auxB_left, auxC_left;
    s16 main_right, auxA_right, auxB_right, auxC_right;
    s16 main_surround, auxA_surround, auxB_surround, auxC_surround;
};

struct PBDpopWM {
    s16 main0, main1, main2, main3;
    s16 aux0, aux1, aux2, aux3;
};

struct PBVolumeEnvelope {
    s16 cur_volume;
    s16 cur_volume_delta;
};

struct PBAudioAddr {
    u16 looping;
    u16 sample_format;
    u16 loop_addr_hi;
    u16 loop_addr_lo;
    u16 end_addr_hi;
    u16 end_addr_lo;
    u16 cur_addr_hi;
    u16 cur_addr_lo;
};

struct PBADPCMInfo {
    s16 coefs[16];
    u16 gain;
    u16 pred_scale;
    s16 yn1;
    s16 yn2;
};

struct PBSampleRateConverter {
    u16 ratio_hi;
    u16 ratio_lo;
    u16 cur_addr_frac;
    s16 last_samples[4];
};

struct PBSampleRateConverterWM {
    u16 cur_addr_frac;
    s16 last_samples[4];
};

struct PBADPCMLoopInfo {
    u16 pred_scale;
    u16 yn1;
    u16 yn2;
};

struct PBLowPassFilter {
    u16 on;
    s16 yn1;
    u16 a0;
    s16 b0;
};

struct PBHighPassFilter {
    u16 on;
    u16 unk[3];
};

struct PBBiquadFilter {
    u16 on;
    s16 xn1;
    s16 xn2;
    s16 yn1;
    s16 yn2;
    s16 b0;
    s16 b1;
    s16 b2;
    s16 a1;
    s16 a2;
};

union PBInfImpulseResponseWM {
    u16 on;  // 0: off, 2: biquad, other: low-pass
    PBLowPassFilter lpf;
    PBBiquadFilter biquad;
};

struct AXPBWii {
    u16 next_pb_hi;
    u16 next_pb_lo;
    u16 this_pb_hi;
    u16 this_pb_lo;
    u16 src_type;
    u16 coef_select;
    u16 mixer_control_hi;
    u16 mixer_control_lo;
    u16 running;
    u16 is_stream;
    PBMixerWii mixer;
    PBInitialTimeDelay initial_time_delay;
    PBDpopWii dpop;
    PBUpdatesWii updates;  // not in the guest structure of the newer microcodes
    PBVolumeEnvelope vol_env;
    PBAudioAddr audio_addr;
    PBADPCMInfo adpcm;
    PBSampleRateConverter src;
    PBADPCMLoopInfo adpcm_loop_info;
    PBLowPassFilter lpf;
    union {
        PBHighPassFilter hpf;
        PBBiquadFilter biquad;
    };
    u16 remote;
    u16 remote_mixer_control;
    PBMixerWM remote_mixer;
    PBDpopWM remote_dpop;
    PBSampleRateConverterWM remote_src;
    PBInfImpulseResponseWM remote_iir;
    u16 pad[2];
};

static_assert(sizeof(AXPBWii) % 2 == 0);

enum { SRCTYPE_POLYPHASE = 0, SRCTYPE_LINEAR = 1, SRCTYPE_NEAREST = 2 };

enum AXMixControl : u32 {
    MIX_MAIN_L = 0x000001,
    MIX_MAIN_L_RAMP = 0x000002,
    MIX_MAIN_R = 0x000004,
    MIX_MAIN_R_RAMP = 0x000008,
    MIX_MAIN_S = 0x000010,
    MIX_MAIN_S_RAMP = 0x000020,
    MIX_AUXA_L = 0x000040,
    MIX_AUXA_L_RAMP = 0x000080,
    MIX_AUXA_R = 0x000100,
    MIX_AUXA_R_RAMP = 0x000200,
    MIX_AUXA_S = 0x000400,
    MIX_AUXA_S_RAMP = 0x000800,
    MIX_AUXB_L = 0x001000,
    MIX_AUXB_L_RAMP = 0x002000,
    MIX_AUXB_R = 0x004000,
    MIX_AUXB_R_RAMP = 0x008000,
    MIX_AUXB_S = 0x010000,
    MIX_AUXB_S_RAMP = 0x020000,
    MIX_AUXC_L = 0x040000,
    MIX_AUXC_L_RAMP = 0x080000,
    MIX_AUXC_R = 0x100000,
    MIX_AUXC_R_RAMP = 0x200000,
    MIX_AUXC_S = 0x400000,
    MIX_AUXC_S_RAMP = 0x800000,
};

enum Command : u16 {
    CMD_SETUP = 0x00,
    CMD_ADD_TO_LR = 0x01,
    CMD_SUB_TO_LR = 0x02,
    CMD_ADD_SUB_TO_LR = 0x03,
    CMD_PROCESS = 0x04,
    CMD_MIX_AUXA = 0x05,
    CMD_MIX_AUXB = 0x06,
    CMD_MIX_AUXC = 0x07,
    CMD_UPL_AUXA_MIX_LRSC = 0x08,
    CMD_UPL_AUXB_MIX_LRSC = 0x09,
    CMD_COMPRESSOR = 0x0A,
    CMD_OUTPUT = 0x0B,
    CMD_OUTPUT_DPL2 = 0x0C,
    CMD_WM_OUTPUT = 0x0D,
    CMD_END = 0x0E,
};

constexpr int kSamples = 96;  // 3 ms at 32 kHz
constexpr int kRemoteSamples = 18;

// --- guest memory -------------------------------------------------------------

// The DSP's view of memory on Wii: bit 28 selects MEM2, otherwise MEM1.
uint8_t* sample_memory(u32 address) {
    u32 physical = (address & 0x10000000) ? 0x10000000 | (address & 0x03FFFFFF) : (address & 0x01FFFFFF);
    return host(physical);
}

u16 read_word(u32 address) {
    return rd16(address);
}

void read_words(u16* out, u32 address, size_t bytes) {
    for (size_t i = 0; i < bytes / 2; i++) {
        out[i] = rd16(address + 2 * static_cast<u32>(i));
    }
}

void write_words(u32 address, const u16* in, size_t bytes) {
    for (size_t i = 0; i < bytes / 2; i++) {
        wr16(address + 2 * static_cast<u32>(i), in[i]);
    }
}

void write_ints(u32 address, const int* in, size_t count) {
    for (size_t i = 0; i < count; i++) {
        wr32(address + 4 * static_cast<u32>(i), static_cast<u32>(in[i]));
    }
}

void read_ints(int* out, u32 address, size_t count) {
    for (size_t i = 0; i < count; i++) {
        out[i] = static_cast<int>(rd32(address + 4 * static_cast<u32>(i)));
    }
}

s16 clamp16(s64 sample) {
    return static_cast<s16>(std::clamp<s64>(sample, -0x8000, 0x7FFF));
}

// --- accelerator ----------------------------------------------------------------

class Accelerator final : public DSP::Accelerator {
public:
    AXPBWii* pb = nullptr;

protected:
    void OnRawReadEndException() override {}
    void OnRawWriteEndException() override {}
    void OnSampleReadEndException() override {
        if (pb->audio_addr.looping) {
            SetPredScale(pb->adpcm_loop_info.pred_scale);
            if (pb->is_stream != 1) {
                SetYn1(static_cast<s16>(pb->adpcm_loop_info.yn1));
                SetYn2(static_cast<s16>(pb->adpcm_loop_info.yn2));
            } else {
                SetYn1(GetYn1());
                SetYn2(GetYn2());
            }
        } else {
            pb->running = 0;
        }
    }
    uint8_t ReadMemory(u32 address) override {
        return *sample_memory(address);
    }
    void WriteMemory(u32 address, uint8_t value) override {
        *sample_memory(address) = value;
    }
};

// --- mixer state ------------------------------------------------------------------

struct Mixer {
    int main_left[kSamples], main_right[kSamples], main_surround[kSamples];
    int auxA_left[kSamples], auxA_right[kSamples], auxA_surround[kSamples];
    int auxB_left[kSamples], auxB_right[kSamples], auxB_surround[kSamples];
    int auxC_left[kSamples], auxC_right[kSamples], auxC_surround[kSamples];
    int wm0[kRemoteSamples], aux0[kRemoteSamples], wm1[kRemoteSamples], aux1[kRemoteSamples];
    int wm2[kRemoteSamples], aux2[kRemoteSamples], wm3[kRemoteSamples], aux3[kRemoteSamples];
};

Mixer g_mix;
Accelerator g_accelerator;
std::array<s16, 0x800> g_coeffs{};
bool g_have_coeffs = false;
u16 g_cmdlist[512];
u16 g_last_main_volume = 0x8000;
u16 g_last_aux_volumes[3] = {0x8000, 0x8000, 0x8000};
u16 g_compressor_pos = 0;
u32 g_variant = 0;
uint64_t g_frames = 0;
uint64_t g_voices = 0;

struct Buffers {
    int* regular[12];
    int* remote[8];
};

Buffers fresh_buffers() {
    Mixer& m = g_mix;
    return {{m.main_left, m.main_right, m.main_surround, m.auxA_left, m.auxA_right, m.auxA_surround, m.auxB_left, m.auxB_right,
             m.auxB_surround, m.auxC_left, m.auxC_right, m.auxC_surround},
            {m.wm0, m.aux0, m.wm1, m.aux1, m.wm2, m.aux2, m.wm3, m.aux3}};
}

void load_coefficients() {
    dsp::RomBlob rom = dsp::coefficient_rom();
    if (rom.size < g_coeffs.size() * 2) {
        return;
    }
    for (size_t i = 0; i < g_coeffs.size(); i++) {
        g_coeffs[i] = static_cast<s16>((rom.data[2 * i] << 8) | rom.data[2 * i + 1]);
    }
    g_have_coeffs = true;
}

// --- voice processing (AXVoice.h) -----------------------------------------------

template <typename Input>
u32 resample(Input input, s16* output, u32 count, s16* last_samples, u32 curr_pos, u32 ratio, int srctype, const s16* coeffs) {
    int read = 0;
    if (coeffs && srctype == SRCTYPE_POLYPHASE) {
        s16 temp[4];
        u32 idx = 0;
        temp[idx++ & 3] = last_samples[0];
        temp[idx++ & 3] = last_samples[1];
        temp[idx++ & 3] = last_samples[2];
        temp[idx++ & 3] = last_samples[3];
        for (u32 i = 0; i < count; ++i) {
            curr_pos += ratio;
            while (curr_pos >= 0x10000) {
                temp[idx++ & 3] = input(read++);
                curr_pos -= 0x10000;
            }
            u16 frac = static_cast<u16>(((curr_pos & 0xFFFF) >> 9) << 2);
            const s16* c = &coeffs[frac];
            s64 t0 = temp[idx++ & 3];
            s64 t1 = temp[idx++ & 3];
            s64 t2 = temp[idx++ & 3];
            s64 t3 = temp[idx++ & 3];
            output[i] = clamp16((t0 * c[0] + t1 * c[1] + t2 * c[2] + t3 * c[3]) >> 15);
        }
        last_samples[3] = temp[--idx & 3];
        last_samples[2] = temp[--idx & 3];
        last_samples[1] = temp[--idx & 3];
        last_samples[0] = temp[--idx & 3];
    } else if (srctype == SRCTYPE_LINEAR || srctype == SRCTYPE_POLYPHASE) {
        s16 temp[4];
        u32 idx = 0;
        temp[idx++ & 3] = last_samples[0];
        temp[idx++ & 3] = last_samples[1];
        temp[idx++ & 3] = last_samples[2];
        temp[idx++ & 3] = last_samples[3];
        for (u32 i = 0; i < count; ++i) {
            curr_pos += ratio;
            while (curr_pos >= 0x10000) {
                temp[idx++ & 3] = input(read++);
                curr_pos -= 0x10000;
            }
            u16 frac = curr_pos & 0xFFFF;
            u16 inverse = static_cast<u16>(-frac);
            s16 sample;
            if (frac) {
                s32 s0 = temp[idx++ & 3];
                s32 s1 = temp[idx++ & 3];
                sample = static_cast<s16>(((s0 * inverse) + (s1 * frac)) >> 16);
                idx += 2;
            } else {
                sample = temp[idx++ & 3];
                idx += 3;
            }
            output[i] = sample;
        }
        last_samples[3] = temp[--idx & 3];
        last_samples[2] = temp[--idx & 3];
        last_samples[1] = temp[--idx & 3];
        last_samples[0] = temp[--idx & 3];
    } else {
        for (u32 i = 0; i < count; ++i) {
            output[i] = input(i);
        }
        std::memcpy(last_samples, output + count - 4, 4 * sizeof(u16));
    }
    return curr_pos;
}

void get_input_samples(AXPBWii& pb, s16* samples, u16 count, const s16* coeffs) {
    Accelerator& acc = g_accelerator;
    acc.pb = &pb;
    acc.SetStartAddress(HILO_TO_32(pb.audio_addr.loop_addr));
    acc.SetEndAddress(HILO_TO_32(pb.audio_addr.end_addr));
    acc.SetCurrentAddress(HILO_TO_32(pb.audio_addr.cur_addr));
    acc.SetSampleFormat(pb.audio_addr.sample_format);
    acc.SetYn1(pb.adpcm.yn1);
    acc.SetYn2(pb.adpcm.yn2);
    acc.SetGain(static_cast<s16>(pb.adpcm.gain));
    acc.SetPredScale(pb.adpcm.pred_scale);
    if (coeffs) {
        coeffs += pb.coef_select * 0x200;
    }
    u32 curr_pos = resample([&acc, &pb](u32) { return static_cast<s16>(acc.ReadSample(pb.adpcm.coefs)); }, samples, count, pb.src.last_samples,
                            pb.src.cur_addr_frac, HILO_TO_32(pb.src.ratio), pb.src_type, coeffs);
    pb.src.cur_addr_frac = curr_pos & 0xFFFF;
    pb.audio_addr.cur_addr_hi = static_cast<u16>(acc.GetCurrentAddress() >> 16);
    pb.audio_addr.cur_addr_lo = static_cast<u16>(acc.GetCurrentAddress());
    pb.adpcm.yn1 = acc.GetYn1();
    pb.adpcm.yn2 = acc.GetYn2();
    pb.adpcm.pred_scale = acc.GetPredScale();
}

void mix_add(int* out, const s16* input, u32 count, VolumeData* vd, s16* dpop, bool ramp) {
    u16& volume = vd->volume;
    u16 delta = ramp ? vd->volume_delta : 0;
    for (u32 i = 0; i < count; ++i) {
        s64 sample = input[i];
        sample *= volume;
        sample >>= 15;
        s16 sample16 = clamp16(static_cast<s32>(sample));
        out[i] += sample16;
        volume += delta;
        *dpop = sample16;
    }
}

void low_pass(s16* samples, u32 count, PBLowPassFilter& f) {
    for (u32 i = 0; i < count; ++i) {
        f.yn1 = samples[i] = clamp16((f.a0 * static_cast<s32>(samples[i]) + f.b0 * static_cast<s32>(f.yn1)) >> 15);
    }
}

void biquad(s16* samples, u32 count, PBBiquadFilter& f) {
    for (u32 i = 0; i < count; ++i) {
        s16 xn0 = samples[i];
        s64 tmp = 0;
        tmp += f.b0 * s32(xn0);
        tmp += f.b1 * s32(f.xn1);
        tmp += f.b2 * s32(f.xn2);
        tmp += f.a1 * s32(f.yn1);
        tmp += f.a2 * s32(f.yn2);
        tmp <<= 2;
        if (tmp & 0x10000) {
            tmp += 0x8000;
        } else {
            tmp += 0x7FFF;
        }
        tmp >>= 16;
        s16 yn0 = clamp16(tmp);
        f.xn2 = f.xn1;
        f.yn2 = f.yn1;
        f.xn1 = xn0;
        f.yn1 = yn0;
        samples[i] = yn0;
    }
}

u32 convert_mixer_control(u32 mixer_control) {
    u32 ret = 0;
    if (mixer_control & 0x00000001) ret |= MIX_MAIN_L;
    if (mixer_control & 0x00000002) ret |= MIX_MAIN_R;
    if (mixer_control & 0x00000004) ret |= MIX_MAIN_L | MIX_MAIN_R | MIX_MAIN_L_RAMP | MIX_MAIN_R_RAMP;
    if (mixer_control & 0x00000008) ret |= MIX_MAIN_S;
    if (mixer_control & 0x00000010) ret |= MIX_MAIN_S | MIX_MAIN_S_RAMP;
    if (mixer_control & 0x00010000) ret |= MIX_AUXA_L;
    if (mixer_control & 0x00020000) ret |= MIX_AUXA_R;
    if (mixer_control & 0x00040000) ret |= MIX_AUXA_L | MIX_AUXA_R | MIX_AUXA_L_RAMP | MIX_AUXA_R_RAMP;
    if (mixer_control & 0x00080000) ret |= MIX_AUXA_S;
    if (mixer_control & 0x00100000) ret |= MIX_AUXA_S | MIX_AUXA_S_RAMP;
    if (mixer_control & 0x00200000) ret |= MIX_AUXB_L;
    if (mixer_control & 0x00400000) ret |= MIX_AUXB_R;
    if (mixer_control & 0x00800000) ret |= MIX_AUXB_L | MIX_AUXB_R | MIX_AUXB_L_RAMP | MIX_AUXB_R_RAMP;
    if (mixer_control & 0x01000000) ret |= MIX_AUXB_S;
    if (mixer_control & 0x02000000) ret |= MIX_AUXB_S | MIX_AUXB_S_RAMP;
    if (mixer_control & 0x04000000) ret |= MIX_AUXC_L;
    if (mixer_control & 0x08000000) ret |= MIX_AUXC_R;
    if (mixer_control & 0x10000000) ret |= MIX_AUXC_L | MIX_AUXC_R | MIX_AUXC_L_RAMP | MIX_AUXC_R_RAMP;
    if (mixer_control & 0x20000000) ret |= MIX_AUXC_S;
    if (mixer_control & 0x40000000) ret |= MIX_AUXC_S | MIX_AUXC_S_RAMP;
    return ret;
}

void process_voice(AXPBWii& pb, const Buffers& b, u16 count, u32 mctrl, const s16* coeffs) {
    if (pb.running != 1) {
        return;
    }
    g_voices++;
    s16 samples[kSamples];
    get_input_samples(pb, samples, count, coeffs);
    for (u32 i = 0; i < count; ++i) {
        const s32 volume = static_cast<u16>(pb.vol_env.cur_volume);  // unsigned on Wii
        samples[i] = clamp16((static_cast<s32>(samples[i]) * volume) >> 15);
        pb.vol_env.cur_volume = static_cast<s16>(pb.vol_env.cur_volume + pb.vol_env.cur_volume_delta);
    }
    if (pb.lpf.on != 0) {
        low_pass(samples, count, pb.lpf);
    }
    if (pb.biquad.on != 0) {
        biquad(samples, count, pb.biquad);
    }
    struct Route {
        u32 on, ramp;
        int* out;
        VolumeData* volume;
        s16* dpop;
    };
    const Route routes[] = {
        {MIX_MAIN_L, MIX_MAIN_L_RAMP, b.regular[0], &pb.mixer.main_left, &pb.dpop.main_left},
        {MIX_MAIN_R, MIX_MAIN_R_RAMP, b.regular[1], &pb.mixer.main_right, &pb.dpop.main_right},
        {MIX_MAIN_S, MIX_MAIN_S_RAMP, b.regular[2], &pb.mixer.main_surround, &pb.dpop.main_surround},
        {MIX_AUXA_L, MIX_AUXA_L_RAMP, b.regular[3], &pb.mixer.auxA_left, &pb.dpop.auxA_left},
        {MIX_AUXA_R, MIX_AUXA_R_RAMP, b.regular[4], &pb.mixer.auxA_right, &pb.dpop.auxA_right},
        {MIX_AUXA_S, MIX_AUXA_S_RAMP, b.regular[5], &pb.mixer.auxA_surround, &pb.dpop.auxA_surround},
        {MIX_AUXB_L, MIX_AUXB_L_RAMP, b.regular[6], &pb.mixer.auxB_left, &pb.dpop.auxB_left},
        {MIX_AUXB_R, MIX_AUXB_R_RAMP, b.regular[7], &pb.mixer.auxB_right, &pb.dpop.auxB_right},
        {MIX_AUXB_S, MIX_AUXB_S_RAMP, b.regular[8], &pb.mixer.auxB_surround, &pb.dpop.auxB_surround},
        {MIX_AUXC_L, MIX_AUXC_L_RAMP, b.regular[9], &pb.mixer.auxC_left, &pb.dpop.auxC_left},
        {MIX_AUXC_R, MIX_AUXC_R_RAMP, b.regular[10], &pb.mixer.auxC_right, &pb.dpop.auxC_right},
        {MIX_AUXC_S, MIX_AUXC_S_RAMP, b.regular[11], &pb.mixer.auxC_surround, &pb.dpop.auxC_surround},
    };
    for (const Route& route : routes) {
        if (mctrl & route.on) {
            mix_add(route.out, samples, count, route.volume, route.dpop, (mctrl & route.ramp) != 0);
        }
    }
    if (pb.remote) {
        if (pb.remote_iir.on != 0) {
            if (pb.remote_iir.on == 2) {
                biquad(samples, count, pb.remote_iir.biquad);
            } else {
                low_pass(samples, count, pb.remote_iir.lpf);
            }
        }
        s16 wm_samples[kRemoteSamples];
        u32 curr_pos = resample([&samples](u32 i) { return samples[i]; }, wm_samples, kRemoteSamples, pb.remote_src.last_samples,
                                pb.remote_src.cur_addr_frac, 0x55555, SRCTYPE_POLYPHASE, coeffs);
        pb.remote_src.cur_addr_frac = curr_pos & 0xFFFF;
        VolumeData* volumes[8] = {&pb.remote_mixer.main0, &pb.remote_mixer.aux0, &pb.remote_mixer.main1, &pb.remote_mixer.aux1,
                                  &pb.remote_mixer.main2, &pb.remote_mixer.aux2, &pb.remote_mixer.main3, &pb.remote_mixer.aux3};
        s16* dpops[8] = {&pb.remote_dpop.main0, &pb.remote_dpop.aux0, &pb.remote_dpop.main1, &pb.remote_dpop.aux1,
                         &pb.remote_dpop.main2, &pb.remote_dpop.aux2, &pb.remote_dpop.main3, &pb.remote_dpop.aux3};
        for (int n = 0; n < 8; n++) {
            u32 bits = (pb.remote_mixer_control >> (2 * n)) & 3;
            if (bits) {
                mix_add(b.remote[n], wm_samples, kRemoteSamples, volumes[n], dpops[n], (bits & 2) != 0);
            }
        }
    }
}

// --- parameter block copy (AXWii.cpp ReadPB/WritePB) ------------------------------

constexpr size_t kUpdatesBegin = offsetof(AXPBWii, updates);
constexpr size_t kUpdatesEnd = kUpdatesBegin + sizeof(PBUpdatesWii);
constexpr size_t kGapBegin = offsetof(AXPBWii, hpf) + sizeof(PBHighPassFilter);
constexpr size_t kGapEnd = offsetof(AXPBWii, biquad) + sizeof(PBBiquadFilter);

bool skips_gap() {
    return g_variant == 0xd9c4bf34 || g_variant == 0xadbc06bd;
}

void read_pb(u32 address, AXPBWii& pb) {
    char* dst = reinterpret_cast<char*>(&pb);
    read_words(reinterpret_cast<u16*>(dst), address, kUpdatesBegin);
    std::memset(dst + kUpdatesBegin, 0, sizeof(PBUpdatesWii));
    if (skips_gap()) {
        read_words(reinterpret_cast<u16*>(dst + kUpdatesEnd), address + kUpdatesBegin, kGapBegin - kUpdatesEnd);
        std::memset(dst + kGapBegin, 0, kGapEnd - kGapBegin);
        read_words(reinterpret_cast<u16*>(dst + kGapEnd), address + kGapBegin, sizeof(pb) - kGapEnd);
    } else {
        read_words(reinterpret_cast<u16*>(dst + kUpdatesEnd), address + kUpdatesBegin, sizeof(pb) - kUpdatesEnd);
    }
}

void write_pb(u32 address, const AXPBWii& pb) {
    const char* src = reinterpret_cast<const char*>(&pb);
    write_words(address, reinterpret_cast<const u16*>(src), kUpdatesBegin);
    if (skips_gap()) {
        write_words(address + kUpdatesBegin, reinterpret_cast<const u16*>(src + kUpdatesEnd), kGapBegin - kUpdatesEnd);
        write_words(address + kGapBegin, reinterpret_cast<const u16*>(src + kGapEnd), sizeof(pb) - kGapEnd);
    } else {
        write_words(address + kUpdatesBegin, reinterpret_cast<const u16*>(src + kUpdatesEnd), sizeof(pb) - kUpdatesEnd);
    }
}

// --- commands (AXWii.cpp) ---------------------------------------------------------

void setup_processing(u32 init_addr) {
    Buffers b = fresh_buffers();
    u16 init[3 * 20];
    read_words(init, init_addr, sizeof init);
    for (int i = 0; i < 20; i++) {
        int* buffer = i < 12 ? b.regular[i] : b.remote[i - 12];
        int samples = i < 12 ? kSamples : kRemoteSamples;
        s32 value = static_cast<s32>((u32(init[3 * i]) << 16) | init[3 * i + 1]);
        s16 delta = static_cast<s16>(init[3 * i + 2]);
        for (int j = 0; j < samples; ++j) {
            buffer[j] = value == 0 ? 0 : value + j * delta;
        }
    }
}

void add_to_lr(u32 address, bool negate) {
    for (int i = 0; i < kSamples; ++i) {
        int value = static_cast<int>(rd32(address + 4 * i));
        if (negate) {
            value = -value;
        }
        g_mix.main_left[i] += value;
        g_mix.main_right[i] += value;
    }
}

void add_sub_to_lr(u32 address) {
    for (int i = 0; i < kSamples; ++i) {
        g_mix.main_left[i] += static_cast<int>(rd32(address + 4 * i));
    }
    for (int i = 0; i < kSamples; ++i) {
        g_mix.main_right[i] -= static_cast<int>(rd32(address + 4 * (kSamples + i)));
    }
}

void process_pb_list(u32 pb_addr) {
    const s16* coeffs = g_have_coeffs ? g_coeffs.data() : nullptr;
    AXPBWii pb;
    for (int guard = 0; pb_addr && guard < 512; guard++) {
        read_pb(pb_addr, pb);
        process_voice(pb, fresh_buffers(), kSamples, convert_mixer_control(HILO_TO_32(pb.mixer_control)), coeffs);
        write_pb(pb_addr, pb);
        pb_addr = HILO_TO_32(pb.next_pb);
    }
}

void volume_ramp(u16* output, u16 from, u16 to, size_t count) {
    float current = from;
    for (size_t i = 0; i < count; ++i) {
        current += (to - from) / static_cast<float>(count);
        output[i] = static_cast<u16>(current);
    }
}

void mix_aux(int aux_id, u32 write_addr, u32 read_addr, u16 volume) {
    u16 ramp[kSamples];
    volume_ramp(ramp, g_last_aux_volumes[aux_id], volume, kSamples);
    g_last_aux_volumes[aux_id] = volume;
    int* main_buffers[3] = {g_mix.main_left, g_mix.main_right, g_mix.main_surround};
    int* aux[3][3] = {{g_mix.auxA_left, g_mix.auxA_right, g_mix.auxA_surround},
                      {g_mix.auxB_left, g_mix.auxB_right, g_mix.auxB_surround},
                      {g_mix.auxC_left, g_mix.auxC_right, g_mix.auxC_surround}};
    if (write_addr) {
        for (int* buffer : aux[aux_id]) {
            write_ints(write_addr, buffer, kSamples);
            write_addr += kSamples * 4;
        }
    }
    for (int* main_buffer : main_buffers) {
        for (int j = 0; j < kSamples; ++j) {
            s64 sample = static_cast<s32>(rd32(read_addr));
            read_addr += 4;
            main_buffer[j] += static_cast<s32>((sample * ramp[j]) >> 15);
        }
    }
}

void upload_aux_mix_lrsc(int aux_id, const u32* addresses, u16 volume) {
    int* aux_left = aux_id ? g_mix.auxB_left : g_mix.auxA_left;
    int* aux_right = aux_id ? g_mix.auxB_right : g_mix.auxA_right;
    int* aux_surround = aux_id ? g_mix.auxB_surround : g_mix.auxA_surround;
    int* auxc = aux_id ? g_mix.auxC_surround : g_mix.auxC_right;
    write_ints(addresses[0], aux_left, kSamples);
    write_ints(addresses[0] + kSamples * 4, aux_right, kSamples);
    write_ints(addresses[0] + 2 * kSamples * 4, aux_surround, kSamples);
    write_ints(addresses[1], auxc, kSamples);
    u16 ramp[kSamples];
    volume_ramp(ramp, g_last_aux_volumes[aux_id], volume, kSamples);
    g_last_aux_volumes[aux_id] = volume;
    int* destinations[4] = {g_mix.main_left, g_mix.main_right, g_mix.main_surround, g_mix.auxC_left};
    for (int n = 0; n < 4; ++n) {
        read_ints(aux_left, addresses[2 + n], kSamples);
        for (int i = 0; i < kSamples; ++i) {
            destinations[n][i] += static_cast<s32>((static_cast<s64>(aux_left[i]) * ramp[i]) >> 15);
        }
    }
}

void run_compressor(u16 threshold, u16 release_frames, u32 table_addr) {
    bool triggered = false;
    for (int i = 0; i < kSamples; ++i) {
        if (std::abs(g_mix.main_left[i]) > int(threshold) || std::abs(g_mix.main_right[i]) > int(threshold)) {
            triggered = true;
            break;
        }
    }
    constexpr u32 frame_bytes = kSamples * 2;
    u32 offset = 0;
    if (triggered) {
        offset = g_compressor_pos * frame_bytes;
        g_compressor_pos = release_frames;
    } else if (g_compressor_pos) {
        --g_compressor_pos;
        constexpr u32 kAttackEntries = 11;
        offset = (kAttackEntries + g_compressor_pos) * frame_bytes;
    } else {
        return;
    }
    for (int i = 0; i < kSamples; ++i) {
        u16 coef = read_word(table_addr + offset + 2 * i);
        g_mix.main_left[i] = static_cast<int>((static_cast<s64>(g_mix.main_left[i]) * coef) >> 15);
        g_mix.main_right[i] = static_cast<int>((static_cast<s64>(g_mix.main_right[i]) * coef) >> 15);
    }
}

void output_samples(u32 lr_addr, u32 surround_addr, u16 volume, bool upload_auxc) {
    u16 ramp[kSamples];
    volume_ramp(ramp, g_last_main_volume, volume, kSamples);
    g_last_main_volume = volume;
    write_ints(surround_addr, g_mix.main_surround, kSamples);
    if (upload_auxc) {
        write_ints(surround_addr + kSamples * 4, g_mix.auxC_left, kSamples);
    }
    for (int i = 0; i < kSamples; ++i) {
        s64 left = (static_cast<s64>(g_mix.main_left[i]) * ramp[i]) >> 15;
        s64 right = (static_cast<s64>(g_mix.main_right[i]) * ramp[i]) >> 15;
        g_mix.main_left[i] = clamp16(left);
        g_mix.main_right[i] = clamp16(right);
    }
    for (int i = 0; i < kSamples; ++i) {
        wr16(lr_addr + 4 * i, static_cast<u16>(g_mix.main_right[i]));
        wr16(lr_addr + 4 * i + 2, static_cast<u16>(g_mix.main_left[i]));
    }
}

void output_remote_samples(const u32* addresses) {
    int* buffers[] = {g_mix.wm0, g_mix.wm1, g_mix.wm2, g_mix.wm3};
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < kRemoteSamples; ++j) {
            wr16(addresses[i] + 2 * j, static_cast<u16>(clamp16(buffers[i][j])));
        }
    }
}

}  // namespace

bool supports(uint32_t ucode_hash) {
    switch (ucode_hash) {
    case 0x347112ba:
    case 0x4cc52064:
    case 0xd9c4bf34:
    case 0xadbc06bd:
        return true;
    default:
        return false;
    }
}

uint32_t ucode_hash(uint32_t address, uint32_t length) {
    uint32_t crc = 0;
    for (uint32_t i = 0; i < length; i++) {
        crc ^= rd8(address + i);
        crc = (crc << 3) | (crc >> 29);
    }
    return crc;
}

void start(uint32_t variant) {
    g_variant = variant;
    load_coefficients();
    g_last_main_volume = 0x8000;
    std::fill(std::begin(g_last_aux_volumes), std::end(g_last_aux_volumes), u16(0x8000));
    g_compressor_pos = 0;
}

void run_command_list(uint32_t address, uint16_t words) {
    if (words > std::size(g_cmdlist)) {
        std::fprintf(stderr, "AX: command list at %08x is too large (%u words)\n", address, words);
        return;
    }
    read_words(g_cmdlist, address, words * 2u);
    g_frames++;
    u32 i = 0;
    auto next = [&]() { return i < std::size(g_cmdlist) ? g_cmdlist[i++] : u16(CMD_END); };
    auto next32 = [&]() {
        u32 hi = next();
        return (hi << 16) | next();
    };
    while (true) {
        u16 cmd = next();
        switch (cmd) {
        case CMD_SETUP:
            setup_processing(next32());
            break;
        case CMD_ADD_TO_LR:
        case CMD_SUB_TO_LR:
            add_to_lr(next32(), cmd == CMD_SUB_TO_LR);
            break;
        case CMD_ADD_SUB_TO_LR:
            add_sub_to_lr(next32());
            break;
        case CMD_PROCESS:
            process_pb_list(next32());
            break;
        case CMD_MIX_AUXA:
        case CMD_MIX_AUXB:
        case CMD_MIX_AUXC: {
            u16 volume = next();
            u32 write_addr = next32();
            u32 read_addr = next32();
            mix_aux(cmd - CMD_MIX_AUXA, write_addr, read_addr, volume);
            break;
        }
        case CMD_UPL_AUXA_MIX_LRSC:
        case CMD_UPL_AUXB_MIX_LRSC: {
            u16 volume = next();
            u32 addresses[6];
            for (u32& a : addresses) {
                a = next32();
            }
            upload_aux_mix_lrsc(cmd == CMD_UPL_AUXB_MIX_LRSC, addresses, volume);
            break;
        }
        case CMD_COMPRESSOR: {
            u16 threshold = next();
            u16 frames = next();
            run_compressor(threshold, frames, next32());
            break;
        }
        case CMD_OUTPUT:
        case CMD_OUTPUT_DPL2: {
            u16 volume = g_variant == 0xd9c4bf34 ? 0x8000 : next();
            u32 surround = next32();
            u32 lr = next32();
            output_samples(lr, surround, volume, cmd == CMD_OUTPUT_DPL2);
            break;
        }
        case CMD_WM_OUTPUT: {
            u32 addresses[4];
            for (u32& a : addresses) {
                a = next32();
            }
            output_remote_samples(addresses);
            break;
        }
        case CMD_END:
            return;
        default:
            std::fprintf(stderr, "AX: unknown command %04x in the command list at %08x\n", cmd, address);
            return;
        }
    }
}

void take_statistics(uint64_t& frames, uint64_t& voices) {
    frames = g_frames;
    voices = g_voices;
    g_frames = 0;
    g_voices = 0;
}

}  // namespace wp::ax
