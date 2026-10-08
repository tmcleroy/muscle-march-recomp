// Native audio at the SDK level: the DSP (dsp.a), audio interface (ai.a) and
// the OS audio bring-up are host functions, and the AX library's command lists
// run in the native mixer (ax_hle.cpp). No DSP code runs and no DSP/AI
// registers are touched. audio.native=1 (default) / WP_NATIVE_AUDIO=0 keeps
// the emulated DSP with the translated microcode.
//
// The AX library talks to its DSP task through a handful of calls:
//   DSPAddTask(task)          boot the microcode; the task's init callback says it is up
//   DSPSendMailToDSP(mail)    0xBABE0000|size, then the command list address
//   task resume callback      "frame done": AX may build the next frame
//   DSPAssertTask(task)       "hurry up" when the AI ran out before the frame was done
// Here a command list is mixed as soon as its address arrives, and the resume
// callback follows at the next interrupt point, as if the DSP were very fast.
//
// The AI side plays 32-byte blocks of the current DMA buffer at 32 kHz into the
// host mixer. When a buffer is latched, the registered DMA callback runs (like
// the AI interrupt) so AX can queue the next one with AIInitDMA.

#include "wp/audio_hle.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <string>

#include "wp/audio.h"
#include "wp/ax_hle.h"
#include "wp/cpu.h"
#include "wp/deterministic.h"
#include "wp/game.h"
#include "wp/hle.h"
#include "wp/log.h"
#include "wp/memory.h"
#include "wp/settings.h"

namespace wp::audio_hle {

namespace {

using Clock = std::chrono::steady_clock;

constexpr uint32_t kMailCommandList = 0xBABE0000;
constexpr uint32_t kRate = 32000;
constexpr uint32_t kFramesPerBlock = 8;  // a 32-byte DMA block of 16-bit stereo
constexpr double kMaxStep = 0.0005;
constexpr int kCatchUpRate = 4;

// DSPTaskInfo (RVL SDK dsp.h)
constexpr uint32_t kTaskIramAddress = 0x0C;
constexpr uint32_t kTaskIramLength = 0x10;
constexpr uint32_t kTaskInitCallback = 0x28;
constexpr uint32_t kTaskResumeCallback = 0x2C;

struct Callback {
    uint32_t function;
    uint32_t argument;
};

struct Dsp {
    uint32_t task = 0;
    bool expecting_address = false;
    uint16_t list_words = 0;
    std::deque<Callback> callbacks;
};

struct Ai {
    bool initialized = false;
    uint32_t callback = 0;
    uint32_t source = 0;  // programmed by AIInitDMA (physical)
    uint32_t blocks = 0;
    bool enabled = false;
    bool source_written = false;
    uint32_t current = 0;  // buffer being played
    uint32_t remaining = 0;
    bool interrupt_pending = false;
    bool started = false;
    Clock::time_point next_block{};
    Clock::time_point clock = det::now();
    Clock::time_point last_call = det::now();
};

Dsp g_dsp;
Ai g_ai;
double g_mix_seconds = 0.0;
double g_load_percent = 0.0;
Clock::time_point g_report = det::now();
uint64_t g_buffers = 0;

void queue(uint32_t function, uint32_t argument) {
    if (function) {
        g_dsp.callbacks.push_back({function, argument});
    }
}

void push_block(uint32_t source, bool playing) {
    int16_t frames[kFramesPerBlock * 2];
    for (uint32_t i = 0; i < kFramesPerBlock; i++) {
        frames[2 * i] = playing ? static_cast<int16_t>(rd16(source + 4 * i + 2)) : 0;
        frames[2 * i + 1] = playing ? static_cast<int16_t>(rd16(source + 4 * i)) : 0;
    }
    audio::push(frames, kFramesPerBlock, kRate);
}

void latch() {
    g_ai.current = g_ai.source;
    g_ai.remaining = g_ai.blocks;
    g_ai.source_written = false;
    g_ai.started = false;
    g_ai.interrupt_pending = true;
}

// The AI's playback clock: wall time, catching up at most kCatchUpRate times real time.
void pump(Clock::time_point wall) {
    const auto step = std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(kMaxStep));
    Clock::duration elapsed = std::min(wall - g_ai.last_call, step);
    g_ai.last_call = wall;
    g_ai.clock = std::min(wall, g_ai.clock + elapsed * kCatchUpRate);
    Clock::time_point now = g_ai.clock;
    const auto block = std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(double(kFramesPerBlock) / kRate));
    while (true) {
        uint32_t source = 0;
        if (!g_ai.enabled) {
            g_ai.next_block = std::max(g_ai.next_block, now - block);
            if (g_ai.next_block + block > now) {
                return;
            }
            g_ai.next_block += block;
        } else {
            if (g_ai.remaining == 0) {
                if (!g_ai.source_written) {
                    return;
                }
                latch();
            }
            if (!g_ai.started) {
                if (g_ai.interrupt_pending) {
                    return;  // the callback must see the new buffer first
                }
                g_ai.started = true;
                g_buffers++;
            }
            if (g_ai.next_block + block > now) {
                return;
            }
            g_ai.next_block += block;
            source = g_ai.current;
            g_ai.remaining--;
            g_ai.current += 32;
        }
        push_block(source, g_ai.enabled);
    }
}

void report(Clock::time_point now) {
    double seconds = std::chrono::duration<double>(now - g_report).count();
    if (seconds < 1.0) {
        return;
    }
    uint64_t frames = 0;
    uint64_t voices = 0;
    ax::take_statistics(frames, voices);
    g_load_percent = 100.0 * g_mix_seconds / seconds;
    if (log::enabled()) {
        log::write("audio", "native AX: %.0f frames/s, %.0f voices/frame, %.1f AI buffers/s, mixer %.2f%% of the CPU thread", frames / seconds,
                   frames ? double(voices) / double(frames) : 0.0, g_buffers / seconds, g_load_percent);
    }
    g_mix_seconds = 0.0;
    g_buffers = 0;
    g_report = now;
}

#define FALLBACK(name)          \
    if (!enabled()) {           \
        call_original(c, name); \
        return;                 \
    }

// --- OS ---

void os_init_audio_system(Cpu& c) {
    FALLBACK("__OSInitAudioSystem");
}

void os_stop_audio_system(Cpu& c) {
    FALLBACK("__OSStopAudioSystem");
}

// --- dsp.a ---

void dsp_init(Cpu& c) {
    FALLBACK("DSPInit");
}

void dsp_check_init(Cpu& c) {
    FALLBACK("DSPCheckInit");
    c.r[3] = 1;
}

void dsp_check_mail_to_dsp(Cpu& c) {
    FALLBACK("DSPCheckMailToDSP");
    c.r[3] = 0;  // always taken
}

void dsp_check_mail_from_dsp(Cpu& c) {
    FALLBACK("DSPCheckMailFromDSP");
    c.r[3] = 0;
}

void dsp_read_mail_from_dsp(Cpu& c) {
    FALLBACK("DSPReadMailFromDSP");
    c.r[3] = 0;
}

void dsp_add_task(Cpu& c) {
    FALLBACK("DSPAddTask");
    uint32_t task = c.r[3];
    uint32_t hash = ax::ucode_hash(rd32(task + kTaskIramAddress), rd32(task + kTaskIramLength));
    if (!ax::supports(hash)) {
        std::fprintf(stderr, "native audio: unsupported DSP microcode %08x; run with WP_NATIVE_AUDIO=0\n", hash);
        std::abort();
    }
    log::write("audio", "native AX for microcode %08x", hash);
    ax::start(hash);
    g_dsp.task = task;
    queue(rd32(task + kTaskInitCallback), task);
}

void dsp_assert_task(Cpu& c) {
    FALLBACK("DSPAssertTask");
    // The resume callback for the frame in flight is already queued.
}

void dsp_send_mail_to_dsp(Cpu& c) {
    FALLBACK("DSPSendMailToDSP");
    uint32_t mail = c.r[3];
    if (!g_dsp.expecting_address) {
        if ((mail & 0xFFFF0000) == kMailCommandList) {
            g_dsp.list_words = static_cast<uint16_t>(mail & 0xFFFF);
            g_dsp.expecting_address = true;
        } else {
            log::write("audio", "native AX: ignored mail %08x", mail);
        }
        return;
    }
    g_dsp.expecting_address = false;
    auto started = det::now();
    ax::run_command_list(mail, g_dsp.list_words);
    g_mix_seconds += std::chrono::duration<double>(det::now() - started).count();
    if (g_dsp.task) {
        queue(rd32(g_dsp.task + kTaskResumeCallback), g_dsp.task);
    }
}

// --- ai.a ---

void ai_init(Cpu& c) {
    FALLBACK("AIInit");
    static bool output_started = false;
    if (!output_started) {  // the emulated DSP starts the host output itself; nothing else would here
        output_started = true;
        audio::start_output();
    }
    g_ai.initialized = true;
    g_ai.clock = g_ai.last_call = det::now();
}

void ai_check_init(Cpu& c) {
    FALLBACK("AICheckInit");
    c.r[3] = g_ai.initialized ? 1 : 0;
}

void ai_reset(Cpu& c) {
    FALLBACK("AIReset");
    g_ai.initialized = false;
}

void ai_register_dma_callback(Cpu& c) {
    FALLBACK("AIRegisterDMACallback");
    uint32_t previous = g_ai.callback;
    g_ai.callback = c.r[3];
    c.r[3] = previous;
}

void ai_init_dma(Cpu& c) {
    FALLBACK("AIInitDMA");
    g_ai.source = c.r[3] & 0x1FFFFFE0;
    g_ai.blocks = (c.r[4] >> 5) & 0x7FFF;
    g_ai.source_written = true;
}

void ai_start_dma(Cpu& c) {
    FALLBACK("AIStartDMA");
    if (!g_ai.enabled) {
        g_ai.enabled = true;
        latch();
        g_ai.next_block = det::now();
    }
}

void ai_stop_dma(Cpu& c) {
    FALLBACK("AIStopDMA");
    g_ai.enabled = false;
}

void ai_get_dma_bytes_left(Cpu& c) {
    FALLBACK("AIGetDMABytesLeft");
    c.r[3] = g_ai.remaining << 5;
}

void ai_get_dma_start_address(Cpu& c) {
    FALLBACK("AIGetDMAStartAddr");
    c.r[3] = g_ai.source;
}

void ai_get_dma_length(Cpu& c) {
    FALLBACK("AIGetDMALength");
    c.r[3] = g_ai.blocks << 5;
}

}  // namespace

bool enabled() {
    static const bool value = [] {
        std::string text = settings::text("audio.native", "WP_NATIVE_AUDIO");
        return game::description().native_audio && (text.empty() || std::atoi(text.c_str()) != 0);
    }();
    return value;
}

bool active() {
    return enabled() && g_dsp.task != 0;
}

double load_percent() {
    return g_load_percent;
}

void update() {
    if (!enabled() || !g_ai.initialized) {
        return;
    }
    Clock::time_point now = det::now();
    pump(now);
    report(now);
}

bool take_interrupt(uint32_t& function, uint32_t& argument) {
    if (!enabled()) {
        return false;
    }
    if (!g_dsp.callbacks.empty()) {
        function = g_dsp.callbacks.front().function;
        argument = g_dsp.callbacks.front().argument;
        g_dsp.callbacks.pop_front();
        return true;
    }
    if (g_ai.interrupt_pending) {
        g_ai.interrupt_pending = false;
        if (g_ai.callback) {
            function = g_ai.callback;
            argument = 0;
            return true;
        }
    }
    return false;
}

const input_hle::Handler kHandlers[] = {
    {"__OSInitAudioSystem", os_init_audio_system},
    {"__OSStopAudioSystem", os_stop_audio_system},
    {"DSPInit", dsp_init},
    {"DSPCheckInit", dsp_check_init},
    {"DSPCheckMailToDSP", dsp_check_mail_to_dsp},
    {"DSPCheckMailFromDSP", dsp_check_mail_from_dsp},
    {"DSPReadMailFromDSP", dsp_read_mail_from_dsp},
    {"DSPSendMailToDSP", dsp_send_mail_to_dsp},
    {"DSPAddTask", dsp_add_task},
    {"DSPAssertTask", dsp_assert_task},
    {"AIInit", ai_init},
    {"AICheckInit", ai_check_init},
    {"AIReset", ai_reset},
    {"AIRegisterDMACallback", ai_register_dma_callback},
    {"AIInitDMA", ai_init_dma},
    {"AIStartDMA", ai_start_dma},
    {"AIStopDMA", ai_stop_dma},
    {"AIGetDMABytesLeft", ai_get_dma_bytes_left},
    {"AIGetDMAStartAddr", ai_get_dma_start_address},
    {"AIGetDMALength", ai_get_dma_length},
    {nullptr, nullptr},
};

}  // namespace wp::audio_hle
