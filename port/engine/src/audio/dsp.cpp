// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "wp/dsp.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

#include "Common/ChunkFile.h"

#include "Core/DSP/DSPCore.h"
#include "Core/DSP/DSPHost.h"
#include "Core/DSP/DSPTables.h"
#include "Core/DSP/Interpreter/DSPInterpreter.h"
#include "wp/audio.h"
#include "wp/audio_hle.h"
#include "wp/big_stack.h"
#include "wp/cpu.h"
#include "wp/dsp_roms.h"
#include "wp/dsp_translated.h"
#include "wp/log.h"
#include "wp/memory.h"

namespace wp::dsp {

namespace {

constexpr uint32_t kMailToDspHigh = 0x5000;
constexpr uint32_t kMailToDspLow = 0x5002;
constexpr uint32_t kMailFromDspHigh = 0x5004;
constexpr uint32_t kMailFromDspLow = 0x5006;
constexpr uint32_t kControl = 0x500A;
constexpr uint32_t kAramInfo = 0x5012;
constexpr uint32_t kAramMode = 0x5016;
constexpr uint32_t kAramRefresh = 0x501A;
constexpr uint32_t kAramDmaMainHigh = 0x5020;
constexpr uint32_t kAramDmaMainLow = 0x5022;
constexpr uint32_t kAramDmaAramHigh = 0x5024;
constexpr uint32_t kAramDmaAramLow = 0x5026;
constexpr uint32_t kAramDmaCountHigh = 0x5028;
constexpr uint32_t kAramDmaCountLow = 0x502A;
constexpr uint32_t kAudioDmaStartHigh = 0x5030;
constexpr uint32_t kAudioDmaStartLow = 0x5032;
constexpr uint32_t kAudioDmaControl = 0x5036;
constexpr uint32_t kAudioDmaBlocksLeft = 0x503A;

constexpr uint16_t kControlMask = 0x0C07;
constexpr uint16_t kControlReset = 0x0001;
constexpr uint16_t kControlHalt = 0x0004;
constexpr uint16_t kIntAudioDma = 0x0008;
constexpr uint16_t kIntAram = 0x0020;
constexpr uint16_t kIntDsp = 0x0080;
constexpr uint16_t kIntAll = kIntAudioDma | kIntAram | kIntDsp;
constexpr uint16_t kDmaState = 0x0200;

constexpr uint32_t kAudioControl = 0xCD006C00;
constexpr uint32_t kAudioDmaRate32k = 0x40;
constexpr uint32_t kMem1Mask = 0x01FFFFFF;
constexpr uint32_t kMem2Base = 0x10000000;
constexpr uint32_t kMem2Mask = 0x03FFFFFF;
constexpr double kDspClock = 729000000.0 / 6.0;
constexpr double kMaxLag = 0.02;
constexpr int kSlice = 512;
constexpr int kMailSlice = 512;
constexpr uint32_t kFramesPerBlock = 8;
constexpr uint16_t kIramWords = 0x1000;
constexpr double kMaxPumpStep = 0.0005;
constexpr int kCatchUpRate = 4;

using Clock = std::chrono::steady_clock;

struct AudioDma {
    uint32_t source = 0;
    uint16_t control = 0;
    uint32_t current = 0;
    uint16_t remaining = 0;
    bool source_written = false;
    bool started = false;
    Clock::time_point next_block{};
    Clock::time_point clock = Clock::now();
    Clock::time_point last_call = Clock::now();
};

struct AramDma {
    uint32_t main = 0;
    uint32_t aram = 0;
    uint32_t count = 0;
};

struct Statistics {
    uint32_t buffers = 0;
    uint32_t late_starts = 0;
    uint32_t stale_blocks = 0;
    uint32_t overwritten_blocks = 0;
    double worst_start = 0.0;
    double dsp_seconds = 0.0;
    double dsp_cycles = 0.0;
    Clock::time_point since = Clock::now();
};

DSP::DSPCore* g_core = nullptr;
bool g_started = false;
uint16_t g_control = kControlHalt;
uint16_t g_aram_info = 0;
uint16_t g_aram_mode = 1;
uint16_t g_aram_refresh = 156;
AramDma g_aram_dma;
AudioDma g_audio_dma;
double g_owed = 0.0;
Clock::time_point g_last_update = Clock::now();
Statistics g_statistics;
std::atomic<bool> g_code_loaded{false};
std::atomic<bool> g_native{false};
struct BlockHistory {
    uint32_t writes = 0;
    bool played = false;
};

std::unordered_map<uint32_t, BlockHistory> g_blocks;

void note_dsp_write(uint32_t address, uint32_t size) {
    for (uint32_t block = address & ~31u; block < address + size; block += 32) {
        g_blocks[block].writes++;
    }
}

void note_block_played(uint32_t address) {
    BlockHistory& history = g_blocks[address & ~31u];
    if (history.played && history.writes == 0) {
        g_statistics.stale_blocks++;
    }
    if (history.writes >= 2) {
        g_statistics.overwritten_blocks++;
    }
    history.writes = 0;
    history.played = true;
}
std::atomic<double> g_load_percent{0.0};
bool g_log = std::getenv("WP_LOG_DSP") != nullptr;
bool g_force_interpreter = std::getenv("WP_DSP_INTERPRETER") != nullptr;
bool g_verify = std::getenv("WP_DSP_VERIFY") != nullptr;

struct MemoryWrite {
    uint32_t address;
    uint8_t old_value;
    uint8_t new_value;
};

std::vector<MemoryWrite>* g_write_log = nullptr;

void record_write(uint32_t address, uint8_t value) {
    if (g_write_log) {
        g_write_log->push_back({address, rd8(address), value});
    }
}

void undo_writes(const std::vector<MemoryWrite>& log) {
    for (auto it = log.rbegin(); it != log.rend(); ++it) {
        *host(it->address) = it->old_value;
    }
}

void redo_writes(const std::vector<MemoryWrite>& log) {
    for (const MemoryWrite& write : log) {
        *host(write.address) = write.new_value;
    }
}

bool same_writes(const std::vector<MemoryWrite>& a, const std::vector<MemoryWrite>& b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); i++) {
        if (a[i].address != b[i].address || a[i].new_value != b[i].new_value) {
            return false;
        }
    }
    return true;
}

TranslatedFunction g_translated = nullptr;

bool load_rom(RomBlob rom, uint16_t* words, size_t count) {
    if (rom.size != count * 2) {
        return false;
    }
    const uint8_t* bytes = rom.data;
    for (size_t i = 0; i < count; i++) {
        words[i] = static_cast<uint16_t>((bytes[2 * i] << 8) | bytes[2 * i + 1]);
    }
    return true;
}

void assert_interrupt_line() {
    g_poll_counter = 1;
}

void raise(uint16_t interrupt) {
    g_control = static_cast<uint16_t>(g_control | interrupt);
    assert_interrupt_line();
}

bool halted() {
    return !g_core || (g_core->DSPState().control_reg & kControlHalt);
}

std::vector<uint8_t> save_state() {
    std::vector<uint8_t> buffer;
    PointerWrap writer(buffer, PointerWrap::Mode::Write);
    g_core->DSPState().DoState(writer);
    return buffer;
}

bool g_restoring = false;

void load_state(std::vector<uint8_t>& buffer) {
    TranslatedFunction translated = g_translated;
    PointerWrap reader(buffer, PointerWrap::Mode::Read);
    g_restoring = true;
    g_core->DSPState().DoState(reader);
    g_restoring = false;
    g_translated = translated;
}

size_t step_counter_offset() {
    return sizeof(DSP::DSP_Regs) + 2 + 2 + 8 + 4 + 1 + sizeof(std::atomic<bool>) + 4 * DSP::DSP_STACK_DEPTH * 2;
}

void report_mismatch(uint16_t start_pc, int instructions, const std::vector<uint8_t>& translated, const std::vector<uint8_t>& interpreted) {
    size_t skip = step_counter_offset();
    size_t first = 0;
    while (first < translated.size() && (translated[first] == interpreted[first] || (first >= skip && first < skip + 8))) {
        first++;
    }
    DSP::DSP_Regs a;
    DSP::DSP_Regs b;
    std::memcpy(&a, translated.data(), sizeof a);
    std::memcpy(&b, interpreted.data(), sizeof b);
    uint16_t pc_a;
    uint16_t pc_b;
    std::memcpy(&pc_a, translated.data() + sizeof a, 2);
    std::memcpy(&pc_b, interpreted.data() + sizeof b, 2);
    std::fprintf(stderr, "DSP verify: mismatch after %d instructions from pc %04x, first differing byte %zu of %zu\n", instructions, start_pc,
                 first, translated.size());
    std::fprintf(stderr, "  translated  pc %04x sr %04x ac0 %016llx ac1 %016llx ax0 %08x ax1 %08x prod %016llx ar %04x %04x %04x %04x\n", pc_a, a.sr,
                 static_cast<unsigned long long>(a.ac[0].val), static_cast<unsigned long long>(a.ac[1].val), a.ax[0].val, a.ax[1].val,
                 static_cast<unsigned long long>(a.prod.val), a.ar[0], a.ar[1], a.ar[2], a.ar[3]);
    std::fprintf(stderr, "  interpreter pc %04x sr %04x ac0 %016llx ac1 %016llx ax0 %08x ax1 %08x prod %016llx ar %04x %04x %04x %04x\n", pc_b, b.sr,
                 static_cast<unsigned long long>(b.ac[0].val), static_cast<unsigned long long>(b.ac[1].val), b.ax[0].val, b.ax[1].val,
                 static_cast<unsigned long long>(b.prod.val), b.ar[0], b.ar[1], b.ar[2], b.ar[3]);
}

bool same_state(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
    if (a.size() != b.size()) {
        return false;
    }
    size_t skip = step_counter_offset();
    return std::memcmp(a.data(), b.data(), skip) == 0 && std::memcmp(a.data() + skip + 8, b.data() + skip + 8, a.size() - skip - 8) == 0;
}

uint64_t g_verify_checks = 0;
bool g_self_testing = false;
bool g_iram_written = false;
uint64_t g_verify_skipped = 0;
uint64_t g_verify_failures = 0;
uint8_t g_verified_pc[kIramWords] = {};

int coverage_percent(int& covered, int& total) {
    covered = 0;
    total = 0;
    const DSP::Analyzer& analyzer = g_core->DSPState().GetAnalyzer();
    for (uint16_t address = 0; address < kIramWords; address++) {
        if (analyzer.IsStartOfInstruction(address)) {
            total++;
            covered += g_verified_pc[address] != 0;
        }
    }
    return total ? covered * 100 / total : 0;
}

int run_translated_verified(DSP::SDSP& state, int left, bool& idle) {
    uint64_t& checks = g_verify_checks;
    uint64_t& failures = g_verify_failures;
    std::vector<uint8_t> before = save_state();
    uint16_t start_pc = state.pc;
    std::vector<MemoryWrite> translated_writes;
    std::vector<MemoryWrite> interpreted_writes;
    g_write_log = &translated_writes;
    g_iram_written = false;
    int remaining = g_translated(g_core->GetInterpreter(), state, left, idle);
    g_write_log = nullptr;
    int executed = left - remaining;
    std::vector<uint8_t> translated = save_state();
    undo_writes(translated_writes);
    load_state(before);
    g_write_log = &interpreted_writes;
    for (int i = 0; i < executed; i++) {
        if (state.pc < kIramWords) {
            g_verified_pc[state.pc] = 1;
        }
        g_core->GetInterpreter().Step();
    }
    g_write_log = nullptr;
    std::vector<uint8_t> interpreted = save_state();
    undo_writes(interpreted_writes);
    redo_writes(translated_writes);
    checks++;
    if (g_iram_written) {
        g_verify_skipped++;
        if (!g_self_testing) {
            std::fprintf(stderr, "DSP verify: the microcode overwrote its own instruction memory at pc %04x; not comparable\n", start_pc);
        }
    } else if (!same_writes(translated_writes, interpreted_writes)) {
        failures++;
        if (failures <= 20) {
            std::fprintf(stderr, "DSP verify: memory writes differ after %d instructions from pc %04x (%zu vs %zu bytes)\n", executed, start_pc,
                         translated_writes.size(), interpreted_writes.size());
        }
    } else if (!same_state(translated, interpreted)) {
        failures++;
        if (failures <= 20) {
            report_mismatch(start_pc, executed, translated, interpreted);
            undo_writes(translated_writes);
            for (int k = 1; k <= left; k++) {
                std::vector<MemoryWrite> log_t;
                std::vector<MemoryWrite> log_i;
                load_state(before);
                bool unused = false;
                g_write_log = &log_t;
                int left_k = g_translated(g_core->GetInterpreter(), state, k, unused);
                g_write_log = nullptr;
                uint16_t pc_t = state.pc;
                std::vector<uint8_t> t_k = save_state();
                undo_writes(log_t);
                load_state(before);
                g_write_log = &log_i;
                uint16_t pc_last = state.pc;
                for (int i = 0; i < k - left_k; i++) {
                    pc_last = state.pc;
                    g_core->GetInterpreter().Step();
                }
                g_write_log = nullptr;
                std::vector<uint8_t> i_k = save_state();
                undo_writes(log_i);
                if (!same_state(t_k, i_k)) {
                    std::fprintf(stderr, "  first divergence with budget %d: counted %d, translated stopped at %04x, interpreter last executed %04x and is at %04x, exceptions %02x\n",
                                 k, k - left_k, pc_t, pc_last, state.pc, state.exceptions);
                    break;
                }
            }
            redo_writes(translated_writes);
        }
    }
    if (checks % 20000 == 0) {
        int covered = 0;
        int total = 0;
        int percent = coverage_percent(covered, total);
        std::fprintf(stderr, "DSP verify: %llu runs checked, %llu mismatches, %d of %d instructions covered (%d%%)\n",
                     static_cast<unsigned long long>(checks), static_cast<unsigned long long>(failures), covered, total, percent);
    }
    load_state(translated);
    return remaining;
}

void randomize_state(DSP::SDSP& state, std::mt19937& random, uint16_t pc) {
    auto next = [&random](uint32_t range) { return static_cast<uint32_t>(random() % range); };
    for (int i = 0; i < 4; i++) {
        state.r.ar[i] = static_cast<uint16_t>(next(8) == 0 ? 0x1000 | next(0x800) : next(0x1000));
        state.r.ix[i] = static_cast<uint16_t>(next(4) == 0 ? next(0x10000) : next(9) - 4);
        state.r.wr[i] = static_cast<uint16_t>(next(2) == 0 ? 0xFFFF : next(0x10000));
    }
    for (int i = 0; i < 2; i++) {
        uint64_t value = (static_cast<uint64_t>(random()) << 32) | random();
        if (next(4) == 0) {
            value = next(3) == 0 ? 0 : (next(2) == 0 ? 0x7FFF0000ull : 0xFFFFFF8000000000ull);
        }
        state.r.ac[i].val = static_cast<uint64_t>((static_cast<int64_t>(value) << 24) >> 24);
        state.r.ax[i].val = random();
    }
    state.r.prod.l = static_cast<uint16_t>(random());
    state.r.prod.m = static_cast<uint16_t>(random());
    state.r.prod.h = static_cast<uint16_t>(random() & 0xFF);
    state.r.prod.m2 = static_cast<uint16_t>(random());
    state.r.sr = static_cast<uint16_t>(random() & ~(DSP::SR_100 | DSP::SR_INT_ENABLE | DSP::SR_EXT_INT_ENABLE));
    state.r.cr = static_cast<uint16_t>(next(0xFF));
    if (next(2) == 0) {
        state.r.st[2] = static_cast<uint16_t>(pc + next(4));
        state.r.st[3] = static_cast<uint16_t>(next(3));
    }
    for (uint32_t i = 0; i < DSP::DSP_DRAM_SIZE; i++) {
        state.dram[i] = static_cast<uint16_t>(random());
    }
    state.pc = pc;
    state.exceptions = 0;
}

void run_self_test(int trials) {
    DSP::SDSP& state = g_core->DSPState();
    const DSP::Analyzer& analyzer = state.GetAnalyzer();
    std::vector<uint8_t> original = save_state();
    uint16_t control = g_control;
    std::mt19937 random(0x5eed);
    const int budgets[] = {1, 2, 3, 8};
    g_self_testing = true;
    uint64_t checks_before = g_verify_checks;
    uint64_t failures_before = g_verify_failures;
    uint64_t skipped_before = g_verify_skipped;
    int instructions = 0;
    for (uint16_t address = 0; address < kIramWords; address++) {
        if (!(analyzer.IsStartOfInstruction(address))) {
            continue;
        }
        instructions++;
        for (int trial = 0; trial < trials; trial++) {
            for (int budget : budgets) {
                load_state(original);
                randomize_state(state, random, address);
                bool idle = false;
                run_translated_verified(state, budget, idle);
            }
        }
    }
    load_state(original);
    g_control = control;
    g_self_testing = false;
    uint64_t checks = g_verify_checks - checks_before;
    uint64_t failures = g_verify_failures - failures_before;
    uint64_t skipped = g_verify_skipped - skipped_before;
    std::fprintf(stderr, "DSP self test: %d instructions, %llu random runs, %llu mismatches, %llu runs skipped because they overwrote the instruction memory\n",
                 instructions, static_cast<unsigned long long>(checks), static_cast<unsigned long long>(failures), static_cast<unsigned long long>(skipped));
    log::write("audio", "DSP self test: %d instructions, %llu random runs, %llu mismatches, %llu skipped", instructions, static_cast<unsigned long long>(checks),
               static_cast<unsigned long long>(failures), static_cast<unsigned long long>(skipped));
    std::fflush(stderr);
    std::_Exit(failures == 0 ? 0 : 1);
}

int run_dsp(int cycles) {
    Clock::time_point start = Clock::now();
    DSP::SDSP& state = g_core->DSPState();
    int left = cycles;
    while (left > 0 && !(state.control_reg & kControlHalt)) {
        if (g_translated && state.pc < kIramWords) {
            bool idle = false;
            int before = left;
            left = g_verify ? run_translated_verified(state, left, idle) : g_translated(g_core->GetInterpreter(), state, left, idle);
            if (idle) {
                break;
            }
            if (left == before) {
                g_core->GetInterpreter().Step();
                left--;
            }
        } else {
            g_core->RunCycles(left);
            left = 0;
        }
    }
    int executed = cycles - left;
    g_statistics.dsp_seconds += std::chrono::duration<double>(Clock::now() - start).count();
    g_statistics.dsp_cycles += executed;
    return executed;
}

void push_block(uint32_t source, bool enabled, uint32_t rate) {
    int16_t frames[kFramesPerBlock * 2];
    for (uint32_t i = 0; i < kFramesPerBlock; i++) {
        frames[2 * i] = enabled ? static_cast<int16_t>(rd16(source + 4 * i + 2)) : 0;
        frames[2 * i + 1] = enabled ? static_cast<int16_t>(rd16(source + 4 * i)) : 0;
    }
    audio::push(frames, kFramesPerBlock, rate);
}

void pump_audio_dma(Clock::time_point wall) {
    const auto step = std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(kMaxPumpStep));
    Clock::duration elapsed = std::min(wall - g_audio_dma.last_call, step);
    g_audio_dma.last_call = wall;
    g_audio_dma.clock = std::min(wall, g_audio_dma.clock + elapsed * kCatchUpRate);
    Clock::time_point now = g_audio_dma.clock;
    uint32_t rate = (rd32(kAudioControl) & kAudioDmaRate32k) ? 32000 : 48000;
    const auto block = std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(double(kFramesPerBlock) / rate));
    while (true) {
        bool enabled = (g_audio_dma.control & 0x8000) != 0;
        uint32_t source = 0;
        if (!enabled) {
            g_audio_dma.next_block = std::max(g_audio_dma.next_block, now - block);
            if (g_audio_dma.next_block + block > now) {
                return;
            }
            g_audio_dma.next_block += block;
        } else {
            if (g_audio_dma.remaining == 0) {
                if (!g_audio_dma.source_written) {
                    return;
                }
                g_audio_dma.current = g_audio_dma.source;
                g_audio_dma.remaining = g_audio_dma.control & 0x7FFF;
                g_audio_dma.source_written = false;
                g_audio_dma.started = false;
                raise(kIntAudioDma);
            }
            if (!g_audio_dma.started) {
                if (g_control & kIntAudioDma) {
                    return;
                }
                g_audio_dma.started = true;
                g_statistics.buffers++;
                double lateness = std::chrono::duration<double>(wall - g_audio_dma.next_block).count();
                g_statistics.worst_start = std::max(g_statistics.worst_start, lateness);
                if (lateness > 0.001) {
                    g_statistics.late_starts++;
                }
            }
            if (g_audio_dma.next_block + block > now) {
                return;
            }
            g_audio_dma.next_block += block;
            source = g_audio_dma.current;
            note_block_played(source & kMem1Mask);
            g_audio_dma.remaining--;
            g_audio_dma.current += 32;
        }
        push_block(source, enabled, rate);
    }
}

void report(Clock::time_point now) {
    double seconds = std::chrono::duration<double>(now - g_statistics.since).count();
    if (seconds < 1.0) {
        return;
    }
    g_load_percent = 100.0 * g_statistics.dsp_seconds / seconds;
    if (log::enabled()) {
        log::write("audio", "buffers %.1f/s, late starts %u, %u stale and %u overwritten blocks, worst start %.2f ms, DSP %.1f Mcycles/s using %.1f%% of the CPU thread, DMA %.2f ms behind",
                   g_statistics.buffers / seconds, g_statistics.late_starts, g_statistics.stale_blocks, g_statistics.overwritten_blocks, g_statistics.worst_start * 1000.0,
                   g_statistics.dsp_cycles / seconds / 1e6, 100.0 * g_statistics.dsp_seconds / seconds,
                   std::chrono::duration<double>(now - g_audio_dma.clock).count() * 1000.0);
    }
    g_statistics = Statistics{};
    g_statistics.since = now;
}

void start() {
    if (g_started) {
        return;
    }
    g_started = true;
    DSP::DSPInitOptions options;
    options.core_type = DSP::DSPInitOptions::CoreType::Interpreter;
    if (!load_rom(instruction_rom(), options.irom_contents.data(), options.irom_contents.size()) ||
        !load_rom(coefficient_rom(), options.coef_contents.data(), options.coef_contents.size())) {
        std::fprintf(stderr, "DSP ROMs missing from the executable resources\n");
        return;
    }
    DSP::DSPCore* core = new DSP::DSPCore();
    if (!core->Initialize(options)) {
        std::fprintf(stderr, "DSP core failed to initialize\n");
        return;
    }
    core->Reset();
    DSP::InitInstructionTable();
    g_core = core;
    g_last_update = Clock::now();
    audio::start_output();
}

void do_aram_dma() {
    uint32_t main = g_aram_dma.main & 0x3FFFFFF;
    uint32_t aram = g_aram_dma.aram & 0x3FFFFFF;
    uint32_t count = g_aram_dma.count & 0x7FFFFFFF;
    bool to_main = (g_aram_dma.count & 0x80000000u) != 0;
    if (g_log) {
        std::fprintf(stderr, "DSP aram dma %s main %08x aram %08x count %x\n", to_main ? "in" : "out", main, aram, count);
    }
    for (uint32_t i = 0; i < count; i++) {
        uint8_t* aram_byte = host(kMem2Base | ((aram + i) & kMem2Mask));
        uint8_t* main_byte = host(main + i);
        if (to_main) {
            *main_byte = *aram_byte;
        } else {
            *aram_byte = *main_byte;
        }
    }
    g_aram_dma.main += count;
    g_aram_dma.aram += count;
    g_aram_dma.count &= 0x80000000u;
    g_control = static_cast<uint16_t>(g_control & ~kDmaState);
    raise(kIntAram);
}

void accumulate(Clock::time_point now) {
    double seconds = std::chrono::duration<double>(now - g_last_update).count();
    g_last_update = now;
    g_owed = std::min(g_owed + seconds * kDspClock, kMaxLag * kDspClock);
}

void run_slice(int slice) {
    int executed = run_dsp(slice);
    g_owed -= executed;
    if (executed < slice || g_core->DSPState().GetAnalyzer().IsIdleSkip(g_core->DSPState().pc)) {
        g_owed = 0.0;
    }
}

void run_for_mail() {
    if (!halted()) {
        g_owed -= run_dsp(kMailSlice);
    }
}

// Native audio: nothing drives the DSP, but OS interrupt masking still writes
// the control register. Keep it as plain storage and count the accesses.
uint16_t g_native_control = 0;
uint32_t g_native_accesses = 0;

void note_native_access(uint32_t reg, bool write) {
    if (g_native_accesses++ < 16 && g_log) {
        std::fprintf(stderr, "native audio: DSP register %04x %s\n", reg, write ? "written" : "read");
    }
}

}

void update() {
    if (!g_core) {
        return;
    }
    Clock::time_point now = Clock::now();
    accumulate(now);
    while (g_owed >= kSlice && !halted()) {
        run_slice(kSlice);
    }
    if (halted()) {
        g_owed = 0.0;
    }
    pump_audio_dma(now);
    report(now);
}

uint16_t read16(uint32_t address) {
    uint32_t reg = 0x5000 | (address & 0xFFE);
    if (audio_hle::enabled()) {
        note_native_access(reg, false);
        return reg == kControl ? g_native_control : 0;
    }
    start();
    switch (reg) {
    case kMailToDspHigh:
        if (g_core && (g_core->PeekMailbox(DSP::Mailbox::CPU) & 0x80000000u)) {
            run_for_mail();
        }
        return g_core ? g_core->ReadMailboxHigh(DSP::Mailbox::CPU) : 0;
    case kMailToDspLow:
        return g_core ? g_core->ReadMailboxLow(DSP::Mailbox::CPU) : 0;
    case kMailFromDspHigh:
        if (g_core && !(g_core->PeekMailbox(DSP::Mailbox::DSP) & 0x80000000u)) {
            run_for_mail();
        }
        return g_core ? g_core->ReadMailboxHigh(DSP::Mailbox::DSP) : 0;
    case kMailFromDspLow:
        return g_core ? g_core->ReadMailboxLow(DSP::Mailbox::DSP) : 0;
    case kControl: {
        uint16_t engine = g_core ? g_core->GetInterpreter().ReadControlRegister() : kControlHalt;
        return static_cast<uint16_t>((g_control & ~kControlMask) | (engine & kControlMask));
    }
    case kAramInfo:
        return g_aram_info;
    case kAramMode:
        return g_aram_mode;
    case kAramRefresh:
        return g_aram_refresh;
    case kAramDmaMainHigh:
        return static_cast<uint16_t>(g_aram_dma.main >> 16);
    case kAramDmaMainLow:
        return static_cast<uint16_t>(g_aram_dma.main);
    case kAramDmaAramHigh:
        return static_cast<uint16_t>(g_aram_dma.aram >> 16);
    case kAramDmaAramLow:
        return static_cast<uint16_t>(g_aram_dma.aram);
    case kAramDmaCountHigh:
        return static_cast<uint16_t>(g_aram_dma.count >> 16);
    case kAramDmaCountLow:
        return static_cast<uint16_t>(g_aram_dma.count);
    case kAudioDmaStartHigh:
        return static_cast<uint16_t>(g_audio_dma.source >> 16);
    case kAudioDmaStartLow:
        return static_cast<uint16_t>(g_audio_dma.source);
    case kAudioDmaControl:
        return g_audio_dma.control;
    case kAudioDmaBlocksLeft:
        return static_cast<uint16_t>(g_audio_dma.remaining > 0 ? g_audio_dma.remaining - 1 : 0);
    default:
        return 0;
    }
}

void write16(uint32_t address, uint16_t value) {
    uint32_t reg = 0x5000 | (address & 0xFFE);
    if (audio_hle::enabled()) {
        note_native_access(reg, true);
        if (reg == kControl) {
            g_native_control = static_cast<uint16_t>(value & ~(kIntAll | kControlReset));
        }
        return;
    }
    start();
    switch (reg) {
    case kMailToDspHigh:
        if (g_core) {
            g_core->WriteMailboxHigh(DSP::Mailbox::CPU, value);
        }
        break;
    case kMailToDspLow:
        if (g_core) {
            g_core->WriteMailboxLow(DSP::Mailbox::CPU, value);
        }
        break;
    case kControl: {
        uint16_t engine = kControlHalt;
        if (g_core) {
            DSP::Interpreter::Interpreter& interpreter = g_core->GetInterpreter();
            interpreter.WriteControlRegister(value);
            if (value & DSP::CR_EXTERNAL_INT) {
                g_core->CheckExternalInterrupt();
                g_core->CheckExceptions();
            }
            engine = interpreter.ReadControlRegister();
        }
        if (value & kControlReset) {
            g_audio_dma.control = 0;
        }
        uint16_t kept = static_cast<uint16_t>(g_control & (kIntAll | kDmaState));
        uint16_t acknowledged = static_cast<uint16_t>(value & kIntAll);
        g_control = static_cast<uint16_t>((value & ~(kIntAll | kDmaState | kControlMask)) | (engine & kControlMask) | (kept & ~acknowledged));
        break;
    }
    case kAramInfo:
        g_aram_info = value & 0x007F;
        break;
    case kAramRefresh:
        g_aram_refresh = value & 0x07FF;
        break;
    case kAramDmaMainHigh:
        g_aram_dma.main = (g_aram_dma.main & 0xFFFF) | (static_cast<uint32_t>(value & 0x03FF) << 16);
        break;
    case kAramDmaMainLow:
        g_aram_dma.main = (g_aram_dma.main & 0xFFFF0000u) | (value & 0xFFE0);
        break;
    case kAramDmaAramHigh:
        g_aram_dma.aram = (g_aram_dma.aram & 0xFFFF) | (static_cast<uint32_t>(value & 0x03FF) << 16);
        break;
    case kAramDmaAramLow:
        g_aram_dma.aram = (g_aram_dma.aram & 0xFFFF0000u) | (value & 0xFFE0);
        break;
    case kAramDmaCountHigh:
        g_aram_dma.count = (g_aram_dma.count & 0xFFFF) | (static_cast<uint32_t>(value & 0x83FF) << 16);
        break;
    case kAramDmaCountLow:
        g_aram_dma.count = (g_aram_dma.count & 0xFFFF0000u) | (value & 0xFFE0);
        do_aram_dma();
        break;
    case kAudioDmaStartHigh:
        g_audio_dma.source = (g_audio_dma.source & 0xFFFF) | (static_cast<uint32_t>(value & 0x1FFF) << 16);
        break;
    case kAudioDmaStartLow:
        g_audio_dma.source = (g_audio_dma.source & 0xFFFF0000u) | (value & 0xFFE0);
        g_audio_dma.source_written = true;
        break;
    case kAudioDmaControl: {
        bool already = (g_audio_dma.control & 0x8000) != 0;
        g_audio_dma.control = value;
        if (!already && (value & 0x8000)) {
            g_audio_dma.current = g_audio_dma.source;
            g_audio_dma.remaining = value & 0x7FFF;
            g_audio_dma.source_written = false;
            g_audio_dma.started = false;
            g_audio_dma.next_block = Clock::now();
            raise(kIntAudioDma);
        }
        break;
    }
    default:
        break;
    }
}

Status status() {
    if (audio_hle::active()) {
        return {true, true, audio_hle::load_percent()};
    }
    return {g_code_loaded, g_native, g_load_percent};
}

uint32_t pending_interrupt() {
    uint16_t active = static_cast<uint16_t>((g_control >> 1) & g_control & kIntAll);
    if (active & kIntDsp) {
        return kInterruptDsp;
    }
    if (active & kIntAram) {
        return kInterruptAram;
    }
    if (active & kIntAudioDma) {
        return kInterruptAudioDma;
    }
    return 0;
}

}

namespace DSP::Host {

u8 ReadHostMemory(u32 address) {
    if (address & wp::dsp::kMem2Base) {
        return wp::rd8(wp::dsp::kMem2Base | (address & wp::dsp::kMem2Mask));
    }
    return wp::rd8(address & wp::dsp::kMem1Mask);
}

void WriteHostMemory(u8 value, u32 address) {
    if (wp::dsp::g_write_log) {
        wp::dsp::record_write(wp::dsp::kMem2Base | (address & wp::dsp::kMem2Mask), value);
    }
    wp::wr8(wp::dsp::kMem2Base | (address & wp::dsp::kMem2Mask), value);
}

u32 dma_address(u32 address) {
    return (address & wp::dsp::kMem2Base) ? wp::dsp::kMem2Base | (address & wp::dsp::kMem2Mask) : address & wp::dsp::kMem1Mask;
}

u32 dsp_words(const u16* pointer, u32 size) {
    DSP::SDSP& state = wp::dsp::g_core->DSPState();
    for (const u16* base : {state.dram, state.iram}) {
        if (pointer >= base && pointer < base + DSP_DRAM_SIZE) {
            return std::min<u32>(size / 2, static_cast<u32>(base + DSP_DRAM_SIZE - pointer));
        }
    }
    return 0;
}

void DMAToDSP(u16* destination, u32 address, u32 size) {
    u32 words = dsp_words(destination, size);
    for (u32 i = 0; i < words; i++) {
        destination[i] = __builtin_bswap16(wp::rd16_reversed(dma_address(address + 2 * i)));
    }
}

void DMAFromDSP(const u16* source, u32 address, u32 size) {
    u32 words = dsp_words(source, size);
    if (!wp::dsp::g_self_testing) {
        wp::dsp::note_dsp_write(dma_address(address), words * 2);
    }
    if (wp::dsp::g_write_log) {
        for (u32 i = 0; i < words; i++) {
            wp::dsp::record_write(dma_address(address + 2 * i), static_cast<uint8_t>(source[i] >> 8));
            wp::dsp::record_write(dma_address(address + 2 * i) + 1, static_cast<uint8_t>(source[i]));
        }
    }
    for (u32 i = 0; i < words; i++) {
        wp::wr16_reversed(dma_address(address + 2 * i), __builtin_bswap16(source[i]));
    }
}

void OSD_AddMessage(std::string message, u32) {
    std::fprintf(stderr, "DSP %s\n", message.c_str());
}

bool OnThread() {
    return false;
}

bool IsWiiHost() {
    return true;
}

void InterruptRequest() {
    wp::dsp::raise(wp::dsp::kIntDsp);
}

u64 TimeBase() {
    return wp::time_base();
}

void CodeLoaded(DSPCore& dsp, u32 address, size_t size) {
    CodeLoaded(dsp, wp::host(address), size);
}

void CodeLoaded(DSPCore& dsp, const u8* pointer, size_t size) {
    if (wp::dsp::g_restoring) {
        return;
    }
    wp::dsp::g_iram_written = true;
    if (wp::dsp::g_self_testing) {
        return;
    }
    SDSP& state = dsp.DSPState();
    uint32_t crc = 0;
    for (size_t i = 0; i < size; i++) {
        crc = (crc * 31) ^ pointer[i];
    }
    state.SetIRAMCRC(crc);
    wp::dsp::g_translated = nullptr;
    for (size_t i = 0; i < wp::dsp::g_translated_code_count && !wp::dsp::g_force_interpreter; i++) {
        if (wp::dsp::g_translated_code[i].checksum == crc) {
            wp::dsp::g_translated = wp::dsp::g_translated_code[i].function;
        }
    }
    if (wp::dsp::g_log) {
        std::fprintf(stderr, "DSP code loaded, %zu bytes, checksum %08x\n", size, crc);
    }
    wp::dsp::g_code_loaded = true;
    wp::dsp::g_native = wp::dsp::g_translated != nullptr;
    wp::log::write("audio", "DSP microcode loaded, %u bytes, checksum %08x, %s", static_cast<unsigned>(size), crc,
                   wp::dsp::g_translated ? "recompiled" : "interpreted");
    if (const char* directory = std::getenv("WP_DUMP_DSP_CODE")) {
        std::string path = std::string(directory) + "/dsp_" + std::to_string(crc) + ".bin";
        if (std::FILE* file = std::fopen(path.c_str(), "wb")) {
            std::fwrite(pointer, 1, size, file);
            std::fclose(file);
        }
    }
    dsp.ClearIRAM();
    state.GetAnalyzer().Analyze(state);
    if (const char* trials = std::getenv("WP_DSP_SELFTEST")) {
        if (wp::dsp::g_translated) {
            static int count = std::max(1, std::atoi(trials));
            wp::run_with_stack(64u << 20, [] { wp::dsp::run_self_test(count); });
        }
    }
}

}
