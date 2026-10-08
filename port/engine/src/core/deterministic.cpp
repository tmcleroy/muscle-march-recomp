#include "wp/deterministic.h"

#include "wp/lift.h"

#include <cstdio>
#include <cstring>
#include <vector>

#include "wp/gx_render.h"
#include "wp/memory.h"
#include "wp/movie.h"
#include <cstdlib>
#include <thread>

namespace wp::det {

namespace {

using Clock = std::chrono::steady_clock;

constexpr uint64_t kTimeBaseHz = 60750000;
// 2026-01-01 00:00:00, seconds since 2000-01-01.
constexpr uint64_t kStartSeconds = 820454400;
// Guest time advances only while the game idles (waiting in OSSelectThread), 0.1 ms per poll, and
// events (interrupts, IPC, GX processing) are delivered only at those polls, so neither depends on
// how much code runs or how it is implemented: replacing a translated function with lifted C++
// (which has no poll sites) changes no timestamp and moves no interrupt. (An interrupt taken
// inside busy code runs its handler on the stack below the interrupted function, and the game
// reads stale stack slots, so the delivery point is observable.) A guest that spins without ever
// idling would stall, so after kSpinPolls busy polls in a row every poll is treated as idle.
constexpr uint64_t kTicksPerIdlePoll = 6075;  // 0.1 ms
constexpr uint64_t kSpinPolls = 1000000;

uint64_t g_ticks = kStartSeconds * kTimeBaseHz;
const uint64_t g_start_ticks = kStartSeconds * kTimeBaseHz;
uint32_t g_polls_since_throttle = 0;
uint64_t g_frame_busy_polls = 0;
uint64_t g_busy_run = 0;
uint64_t g_frame_polls = 0;

Clock::time_point origin() {
    static const Clock::time_point value = Clock::now();
    return value;
}

bool fast() {
    static const bool value = std::getenv("WP_DETERMINISTIC_FAST") != nullptr;
    return value;
}

Clock::duration guest_elapsed() {
    uint64_t ticks = g_ticks - g_start_ticks;
    return std::chrono::duration_cast<Clock::duration>(std::chrono::nanoseconds(ticks / kTimeBaseHz * 1000000000ull + ticks % kTimeBaseHz * 1000000000ull / kTimeBaseHz));
}

}  // namespace

bool enabled() {
    static const bool value = [] {
        const char* flag = std::getenv("WP_DETERMINISTIC");
        return (flag && std::atoi(flag) != 0) || std::getenv("WP_RECORD") || std::getenv("WP_REPLAY");
    }();
    return value;
}

uint64_t ticks() {
    return g_ticks;
}

bool advance(bool idle) {
    bool spinning = false;
    if (idle) {
        g_busy_run = 0;
        g_ticks += kTicksPerIdlePoll;
    } else if (++g_busy_run > kSpinPolls) {
        g_ticks += kTicksPerIdlePoll;
        spinning = true;
    }
    g_frame_polls++;
    if (!idle) {
        g_frame_busy_polls++;
    }
    if (fast() || ++g_polls_since_throttle < 256) {
        return idle || spinning;
    }
    g_polls_since_throttle = 0;
    Clock::duration ahead = guest_elapsed() - (Clock::now() - origin());
    if (ahead > std::chrono::milliseconds(2)) {
        std::this_thread::sleep_for(ahead);
    }
    return idle || spinning;
}

uint64_t hash_range(uint64_t hash, uint32_t start, uint32_t size) {
    const uint8_t* bytes = host(start);
    for (uint32_t i = 0; i < size; i += 8) {
        uint64_t word;
        std::memcpy(&word, bytes + i, 8);
        hash = (hash ^ word) * 0x100000001B3ull;
        hash ^= hash >> 29;
    }
    return hash;
}

uint64_t memory_hash() {
    uint64_t hash = 0xCBF29CE484222325ull;
    hash = hash_range(hash, 0x00000000, 0x01800000);  // MEM1
    hash = hash_range(hash, 0x10000000, 0x04000000);  // MEM2
    return hash;
}

void retrace() {
    static uint64_t retraces = 0;
    retraces++;
    static FILE* hashes = [] {
        const char* path = std::getenv("WP_DET_HASHES");
        return path ? std::fopen(path, "w") : nullptr;
    }();
    static const uint64_t every = std::getenv("WP_DET_HASH_EVERY") ? std::strtoull(std::getenv("WP_DET_HASH_EVERY"), nullptr, 10) : 60;
    // WP_REPLAY_EXIT: stop this many retraces after the movie runs out (default 120).
    static const char* exit_setting = std::getenv("WP_REPLAY_EXIT");
    static uint64_t ended_at = 0;
    bool finishing = false;
    if (exit_setting && movie::ended()) {
        if (!ended_at) {
            ended_at = retraces;
        }
        uint64_t linger = std::strtoull(exit_setting, nullptr, 10);
        finishing = retraces >= ended_at + (linger ? linger : 120);
    }
    if (hashes && (retraces % every == 0 || finishing)) {
        // Line: retrace, RAM hash, guest time, and (WP_DET_FRAMES) a hash of the last finished frame.
        std::fprintf(hashes, "%llu %016llx %016llx", static_cast<unsigned long long>(retraces), static_cast<unsigned long long>(memory_hash()),
                     static_cast<unsigned long long>(g_ticks));
        static const bool frames = std::getenv("WP_DET_FRAMES") != nullptr;
        std::vector<uint32_t> pixels;
        uint32_t width = 0;
        uint32_t height = 0;
        if (frames && gx::render::read_frame(pixels, width, height)) {
            uint64_t hash = 0xCBF29CE484222325ull;
            for (uint32_t pixel : pixels) {
                hash = (hash ^ pixel) * 0x100000001B3ull;
            }
            std::fprintf(hashes, " %016llx", static_cast<unsigned long long>(hash));
        }
        std::fputc('\n', hashes);
        std::fflush(hashes);
    }
    if (finishing) {
        std::fprintf(stderr, "replay finished after %llu retraces\n", static_cast<unsigned long long>(retraces));
        lift::print_report();
        std::_Exit(0);
    }
    static const uint64_t dump_at = std::getenv("WP_DET_DUMP") ? std::strtoull(std::getenv("WP_DET_DUMP"), nullptr, 10) : 0;
    if (retraces == dump_at) {  // raw MEM1 + MEM2 for diffing two runs
        if (FILE* file = std::fopen("det_dump.bin", "wb")) {
            std::fwrite(host(0x00000000), 1, 0x01800000, file);
            std::fwrite(host(0x10000000), 1, 0x04000000, file);
            std::fclose(file);
        }
    }
    static const bool log = std::getenv("WP_LOG_POLLS") != nullptr;
    static uint64_t frames = 0;
    static uint64_t busy_total = 0;
    static uint64_t busy_max = 0;
    static uint64_t polls_total = 0;
    frames++;
    busy_total += g_frame_busy_polls;
    polls_total += g_frame_polls;
    if (g_frame_busy_polls > busy_max) {
        busy_max = g_frame_busy_polls;
    }
    g_frame_busy_polls = 0;
    g_frame_polls = 0;
    if (log && frames % 60 == 0) {
        std::fprintf(stderr, "det: busy polls/frame avg %llu max %llu, polls/frame %llu\n", static_cast<unsigned long long>(busy_total / 60),
                     static_cast<unsigned long long>(busy_max), static_cast<unsigned long long>(polls_total / 60));
        busy_total = 0;
        busy_max = 0;
        polls_total = 0;
    }
}

Clock::time_point now() {
    if (!enabled()) {
        return Clock::now();
    }
    return origin() + guest_elapsed();
}

}  // namespace wp::det
