#include <chrono>
#include <cstdio>
#include <cstdlib>

#include "wp/audio.h"
#include "wp/audio_hle.h"
#include "wp/deterministic.h"
#include "wp/cpu.h"
#include "wp/dsp.h"
#include "wp/gx.h"
#include "wp/log.h"
#include "wp/hle.h"
#include "wp/ipc.h"
#include "wp/memory.h"
#include "wp/threads.h"
#include "wp/video.h"

namespace wp {

uint32_t g_poll_counter = 1u << 10;
bool g_polling_blocked = false;

namespace {

constexpr uint32_t kMsrExternalInterrupt = 0x8000;
constexpr uint32_t kInterruptTable = 0x80003040;
constexpr uint32_t kVideoInterrupt = 24;
constexpr uint32_t kFinishInterrupt = 19;
constexpr uint32_t kVideoInterruptRegisters[] = {0xCC002030, 0xCC002034};
constexpr uint16_t kVideoInterruptEnable = 0x1000;
constexpr uint16_t kVideoInterruptFlag = 0x8000;
constexpr uint32_t kCurrentContext = 0x800000D4;
constexpr uint32_t kExceptionTable = 0x80003000;
constexpr uint32_t kDecrementerException = 8;
constexpr uint32_t kDisplayConfig = 0xCC002002;
constexpr uint16_t kFormatMask = 0x0300;
constexpr uint16_t kFormatPal = 0x0100;
constexpr std::chrono::microseconds kPalPeriod(20000);
constexpr std::chrono::microseconds kNtscPeriod(16683);

using Clock = std::chrono::steady_clock;

std::chrono::microseconds retrace_period() {
    return (rd16(kDisplayConfig) & kFormatMask) == kFormatPal ? kPalPeriod : kNtscPeriod;
}

Clock::time_point g_next_retrace{};
bool g_in_interrupt = false;

bool arm_video_interrupt() {
    bool armed = false;
    for (uint32_t address : kVideoInterruptRegisters) {
        uint16_t value = rd16(address);
        if (value & kVideoInterruptEnable) {
            wr16(address, value | kVideoInterruptFlag);
            armed = true;
        }
    }
    return armed;
}

void deliver_decrementer(Cpu& c) {
    decrementer_fired();
    static const bool log_interrupts = std::getenv("WP_LOG_IRQ") != nullptr;
    if (log_interrupts) {
        std::fprintf(stderr, "IRQ decrementer at tick %llu", static_cast<unsigned long long>(time_base()));
        std::fputc(10, stderr);
    }
    uint32_t handler = rd32(kExceptionTable + 4 * kDecrementerException);
    uint32_t context = rd32(kCurrentContext);
    if (handler == 0 || context == 0) {
        return;
    }
    Cpu interrupted = c;
#ifdef WP_TRACE
    size_t trace_depth = g_call_depth;
#endif
    std::jmp_buf point;
    c.r[3] = context;
    save_context(c, &point);
    if (setjmp(point) == 0) {
        c = interrupted;
        c.spr[26] = interrupted.lr;
        c.spr[27] = interrupted.msr;
        c.msr &= ~kMsrExternalInterrupt;
        c.r[3] = kDecrementerException;
        c.r[4] = context;
        g_in_interrupt = true;
        call(c, handler);
    }
    c = interrupted;
#ifdef WP_TRACE
    g_call_depth = trace_depth;
#endif
    g_in_interrupt = false;
}

void deliver_external_interrupt(Cpu& c, uint32_t index) {
    uint32_t handler = rd32(kInterruptTable + 4 * index);
    if (handler == 0) {
        return;
    }
    Cpu saved = c;
    g_in_interrupt = true;
    c.msr &= ~kMsrExternalInterrupt;
    c.r[3] = index;
    c.r[4] = rd32(kCurrentContext);
    call(c, handler);
    c = saved;
    g_in_interrupt = false;
    c.r[3] = 0;
    call(c, symbol_address("OSSelectThread"));
    c = saved;
}

// A host-side interrupt source (native audio): run the guest callback like an interrupt handler.
void deliver_callback(Cpu& c, uint32_t function, uint32_t argument) {
    Cpu saved = c;
    g_in_interrupt = true;
    c.msr &= ~kMsrExternalInterrupt;
    c.r[3] = argument;
    call(c, function);
    c = saved;
    g_in_interrupt = false;
    c.r[3] = 0;
    call(c, symbol_address("OSSelectThread"));
    c = saved;
}

void deliver_video_interrupt(Cpu& c) {
    uint32_t handler = rd32(kInterruptTable + 4 * kVideoInterrupt);
    if (handler == 0 || !arm_video_interrupt()) {
        return;
    }
    Cpu saved = c;
    g_in_interrupt = true;
    c.msr &= ~kMsrExternalInterrupt;
    c.r[3] = kVideoInterrupt;
    c.r[4] = rd32(kCurrentContext);
    call(c, handler);
    c = saved;
    video::present();
    g_in_interrupt = false;
    c.r[3] = 0;
    call(c, symbol_address("OSSelectThread"));
    c = saved;
}

}

void interrupt_left() {
    g_in_interrupt = false;
}

void poll_interrupts(Cpu& c) {
    static const uint32_t interval = [] {
        const char* setting = std::getenv("WP_POLL_INTERVAL");
        uint32_t value = setting ? static_cast<uint32_t>(std::strtoul(setting, nullptr, 10)) : 0;
        return value ? value : 1u << 10;
    }();
    g_poll_counter = interval;
    if (g_polling_blocked) {
        return;
    }
    if (det::enabled()) {
        static const uint32_t idle_function = symbol_address("OSSelectThread");
        size_t depth = g_call_depth;
        if (!det::advance(depth > 0 && depth <= kCallStackSize && g_call_stack[depth - 1] == idle_function)) {
            return;  // busy code: events wait for the next idle poll (deterministic.cpp)
        }
    }
    static const bool log_polls = std::getenv("WP_LOG_POLLS") != nullptr;
    if (log_polls) {
        static uint64_t polls = 0;
        static Clock::time_point since = Clock::now();
        polls++;
        if (Clock::now() - since >= std::chrono::seconds(5)) {
            std::fprintf(stderr, "polls: %.0f/s\n", polls / std::chrono::duration<double>(Clock::now() - since).count());
            polls = 0;
            since = Clock::now();
        }
    }
    audio::update();
    dsp::update();
    audio_hle::update();
    log::watch_modules();
    ipc::update();
    if (g_in_interrupt || !(c.msr & kMsrExternalInterrupt)) {
        gx::process();
        return;
    }
    if (decrementer_due()) {
        deliver_decrementer(c);
    }
    if (ipc::interrupt_pending()) {
        deliver_external_interrupt(c, ipc::kInterrupt);
    }
    for (int delivered = 0; delivered < 3; delivered++) {
        uint32_t index = dsp::pending_interrupt();
        if (index == 0) {
            break;
        }
        deliver_external_interrupt(c, index);
    }
    uint32_t function = 0;
    uint32_t argument = 0;
    for (int delivered = 0; delivered < 4 && audio_hle::take_interrupt(function, argument); delivered++) {
        deliver_callback(c, function, argument);
    }
    if (gx::take_finish_interrupt()) {
        deliver_external_interrupt(c, kFinishInterrupt);
    }
    gx::process();
    Clock::time_point now = det::now();
    if (g_next_retrace == Clock::time_point{}) {
        g_next_retrace = now + kPalPeriod;
    }
    if (now < g_next_retrace) {
        return;
    }
    std::chrono::microseconds period = retrace_period();
    g_next_retrace += period;
    if (g_next_retrace < now) {
        g_next_retrace = now + period;
    }
    if (det::enabled()) {
        det::retrace();
    }
    deliver_video_interrupt(c);
}

}
