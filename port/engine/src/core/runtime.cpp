#include "wp/deterministic.h"
#include "wp/cpu.h"
#include "wp/threads.h"
#include "wp/profile.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include "wp/function_table.h"
#include "wp/modules.h"
#include "wp/watch.h"

namespace wp {

uint8_t* g_memory = nullptr;

namespace {

constexpr uint64_t kTimeBaseHz = 60750000;
constexpr size_t kReportedFrames = 24;

void dump_memory() {
    const char* path = std::getenv("WP_DUMP");
    if (!path) {
        return;
    }
    std::FILE* file = std::fopen(path, "wb");
    if (file) {
        std::fwrite(g_memory, 1, kMemorySize, file);
        std::fclose(file);
    }
}

[[noreturn]] void fail(const char* format, ...) {
    va_list arguments;
    va_start(arguments, format);
    std::vfprintf(stderr, format, arguments);
    va_end(arguments);
    std::fputc('\n', stderr);
    print_call_stack();
    dump_memory();
    std::abort();
}

}

const char* find_name(uint32_t address) {
    const NameEntry* end = g_name_table + g_name_count;
    const NameEntry* it = std::lower_bound(
        g_name_table, end, address,
        [](const NameEntry& entry, uint32_t value) { return entry.address < value; });
    return it != end && it->address == address ? it->name : "";
}

#ifdef WP_TRACE
uint32_t g_main_stack[kCallStackSize];
uint32_t* g_call_stack = g_main_stack;
volatile size_t g_call_depth = 0;

void print_call_stack() {
    size_t depth = g_call_depth;
    std::fprintf(stderr, "call stack depth %zu (innermost first):", depth);
    for (size_t i = 0; i < kReportedFrames && i < depth; i++) {
        size_t index = depth - 1 - i;
        if (index < kCallStackSize) {
            uint32_t address = g_call_stack[index];
            const char* name = find_name(address);
            std::fprintf(stderr, " %08x%s%s%s", address, *name ? "(" : "", name, *name ? ")" : "");
        }
    }
    std::fputc('\n', stderr);
}

namespace {
std::map<uint32_t, uint32_t> g_profile;
std::atomic<bool> g_sampling{false};
std::atomic<uint32_t> g_idle_samples{0};
std::atomic<uint32_t> g_samples{0};
std::atomic<bool> g_idle_sampler{false};
}

double idle_share() {
    if (!g_idle_sampler.exchange(true)) {
        static const uint32_t kOSSelectThread = symbol_address("OSSelectThread");
        std::thread([] {
            while (true) {
                size_t depth = g_call_depth;
                if (depth > 0 && depth <= kCallStackSize) {
                    g_samples++;
                    if (g_call_stack[depth - 1] == kOSSelectThread) {
                        g_idle_samples++;
                    }
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }).detach();
        return -1.0;
    }
    uint32_t samples = g_samples.exchange(0);
    uint32_t idle = g_idle_samples.exchange(0);
    return samples ? static_cast<double>(idle) / samples : -1.0;
}

void start_profiler() {
    g_sampling = true;
    std::thread([] {
        while (g_sampling) {
            size_t depth = g_call_depth;
            if (depth > 0 && depth <= kCallStackSize) {
                g_profile[g_call_stack[depth - 1]]++;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }).detach();
}

void start_watch(uint32_t address) {
    std::thread([address] {
        uint32_t last = rd32(address);
        int reported = 0;
        while (reported < 40) {
            uint32_t value = rd32(address);
            if (value == last) {
                continue;
            }
            size_t depth = g_call_depth;
            std::string line;
            char text[96];
            std::snprintf(text, sizeof text, "WATCH %08x: %08x -> %08x in", address, last, value);
            line = text;
            for (size_t i = 0; i < 8 && i < depth && depth <= kCallStackSize; i++) {
                std::snprintf(text, sizeof text, " %08x", g_call_stack[depth - 1 - i]);
                line += text;
            }
            std::fprintf(stderr, "%s\n", line.c_str());
            last = value;
            reported++;
        }
    }).detach();
}

void print_profile() {
    g_sampling = false;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    dump_memory();
    std::vector<std::pair<uint32_t, uint32_t>> rows(g_profile.begin(), g_profile.end());
    std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    std::fprintf(stderr, "profile (samples per innermost function):\n");
    for (size_t i = 0; i < rows.size() && i < 15; i++) {
        const char* name = find_name(rows[i].first);
        std::fprintf(stderr, "  %08x %6u %s\n", rows[i].first, rows[i].second, name);
    }
}
#else
void print_call_stack() {}
void start_profiler() {}
double idle_share() {
    return -1.0;
}
void start_watch(uint32_t) {}
void print_profile() {}
#endif

void call(Cpu& c, uint32_t address) {
    const FunctionEntry* end = g_function_table + g_function_count;
    const FunctionEntry* it = std::lower_bound(
        g_function_table, end, address,
        [](const FunctionEntry& entry, uint32_t value) { return entry.address < value; });
    if (it == end || it->address != address) {
        if (!call_module_function(c, address)) {
            missing_function(c, address);
        }
        return;
    }
    it->function(c);
}

void missing_function(Cpu& c, uint32_t address) {
    describe_loaded_modules(address);
    std::fprintf(stderr, "registers at the call: lr=%08x r3=%08x r4=%08x r5=%08x r12=%08x r30=%08x r31=%08x\n", c.lr, c.r[3], c.r[4], c.r[5], c.r[12], c.r[30],
                 c.r[31]);
    uint32_t object = c.r[28];
    uint32_t table = rd32(object);
    std::fprintf(stderr, "r28=%08x vtable=%08x slots:", object, table);
    for (uint32_t i = 0; i < 12; i++) {
        std::fprintf(stderr, " %08x", rd32(table + i * 4));
    }
    std::fputc('\n', stderr);
    fail("call to unknown function %08x", address);
}

void illegal_instruction(Cpu&, uint32_t address, uint32_t word) {
    fail("illegal instruction at %08x: %08x", address, word);
}

void unsupported_instruction(Cpu&, uint32_t address, const char* name) {
    fail("unsupported instruction at %08x: %s", address, name);
}

void unresolved_jump(Cpu&, uint32_t target) {
    fail("unresolved jump to %08x", target);
}

void fatal_error(const char* message) {
    fail("%s", message);
}

void trap(Cpu&, uint32_t address) {
    fail("trap at %08x", address);
}

void system_call(Cpu&) {}

void locked_cache_dma(Cpu& c) {
    constexpr uint32_t kDmaTrigger = 2;
    constexpr uint32_t kDmaLoad = 0x10;
    constexpr uint32_t kDmaClear = 3;
    constexpr uint32_t kBlockSize = 32;
    constexpr uint32_t kMaxBlocks = 128;
    uint32_t dmal = c.spr[923];
    if (!(dmal & kDmaTrigger)) {
        return;
    }
    uint32_t dmau = c.spr[922];
    uint32_t blocks = ((dmau & 0x1F) << 2) | ((dmal >> 2) & 3);
    if (blocks == 0) {
        blocks = kMaxBlocks;
    }
    uint32_t memory = (dmau & 0xFFFFFFE0) & kAddressMask;
    uint32_t cache = dmal & 0xFFFFFFE0;
    uint32_t bytes = blocks * kBlockSize;
    uint32_t cache_offset = cache & (kLockedCacheSize - 1);
    if (cache_offset + bytes > kLockedCacheSize || memory + bytes > kPhysicalSize) {
        fail("locked cache DMA out of range: memory=%08x cache=%08x blocks=%u", memory, cache, blocks);
    }
    if (dmal & kDmaLoad) {
        std::memcpy(host(cache), host(memory), bytes);
    } else {
        std::memcpy(host(memory), host(cache), bytes);
    }
    c.spr[923] &= ~kDmaClear;
}

namespace {

bool g_decrementer_armed = false;
uint64_t g_decrementer_deadline = 0;

}

void set_decrementer(Cpu& c, uint32_t value) {
    c.spr[22] = value;
    g_decrementer_deadline = time_base() + ((value & 0x80000000u) ? 0 : value);
    g_decrementer_armed = true;
}

uint32_t get_decrementer(Cpu&) {
    if (!g_decrementer_armed) {
        return 0x7FFFFFFFu;
    }
    return static_cast<uint32_t>(static_cast<int64_t>(g_decrementer_deadline) - static_cast<int64_t>(time_base()));
}

bool decrementer_due() {
    return g_decrementer_armed && time_base() >= g_decrementer_deadline;
}

void decrementer_fired() {
    g_decrementer_armed = false;
}

uint64_t wall_clock_ticks() {
    constexpr int64_t kSecondsFrom1970To2000 = 946684800;
    std::time_t now = std::time(nullptr);
    std::tm local = *std::localtime(&now);
    std::tm utc = *std::gmtime(&now);
    utc.tm_isdst = local.tm_isdst;
    int64_t offset = static_cast<int64_t>(std::difftime(std::mktime(&local), std::mktime(&utc)));
    return static_cast<uint64_t>(static_cast<int64_t>(now) - kSecondsFrom1970To2000 + offset) * kTimeBaseHz;
}

uint64_t time_base() {
    if (det::enabled()) {
        return det::ticks();
    }
    using clock = std::chrono::steady_clock;
    static const clock::time_point origin = clock::now();
    static const uint64_t start = wall_clock_ticks();
    uint64_t elapsed = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - origin).count());
    return start + elapsed / 1000000000ull * kTimeBaseHz + elapsed % 1000000000ull * kTimeBaseHz / 1000000000ull;
}

}
