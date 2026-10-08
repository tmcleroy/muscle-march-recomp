#include "wp/threads.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <csetjmp>
#include <cstring>
#include <iterator>
#include <map>

#include "wp/fiber.h"
#include "wp/function_table.h"
#include "wp/hle.h"
#include "wp/modules.h"

namespace wp {

namespace {

constexpr uint32_t kGprOffset = 0x000;
constexpr uint32_t kConditionOffset = 0x080;
constexpr uint32_t kLinkOffset = 0x084;
constexpr uint32_t kCounterOffset = 0x088;
constexpr uint32_t kExceptionOffset = 0x08C;
constexpr uint32_t kFprOffset = 0x090;
constexpr uint32_t kFpscrOffset = 0x194;
constexpr uint32_t kSrr0Offset = 0x198;
constexpr uint32_t kSrr1Offset = 0x19C;
constexpr uint32_t kGqrOffset = 0x1A4;
constexpr uint32_t kPairedOffset = 0x1C8;
constexpr uint32_t kJumpLinkOffset = 0x00;
constexpr uint32_t kJumpConditionOffset = 0x04;
constexpr uint32_t kJumpStackOffset = 0x08;
constexpr uint32_t kJumpTocOffset = 0x0C;
constexpr uint32_t kJumpGprOffset = 0x14;
constexpr uint32_t kJumpFprOffset = 0x60;
constexpr uint32_t kJumpFpscrOffset = 0x180;
constexpr uint32_t kFirstSavedRegister = 13;
constexpr uint32_t kFirstSavedFloat = 14;
constexpr size_t kFiberStackSize = 4u << 20;

struct SavedContext {
    void* fiber;
    std::jmp_buf* point;
    uint32_t resume;
};

struct SavedJump {
    void* fiber;
    std::jmp_buf* point;
    uint32_t link;
    uint32_t stack;
};

#ifdef WP_TRACE
struct TraceState {
    uint32_t* stack;
    size_t depth;
};

std::map<void*, TraceState> g_traces;

void register_trace(void* fiber) {
    g_traces[fiber] = TraceState{new uint32_t[kCallStackSize], 0};
}
#endif

void switch_to(void* fiber) {
    interrupt_left();
#ifdef WP_TRACE
    TraceState& self = g_traces[fiber::current()];
    self.depth = g_call_depth;
    TraceState& target = g_traces[fiber];
    g_call_stack = target.stack;
    g_call_depth = target.depth;
#endif
    fiber::switch_to(fiber);
}

std::map<uint32_t, SavedContext> g_saved;
std::map<uint32_t, SavedJump> g_jumps;
uint32_t g_jump_buffer = 0;
uint32_t g_jump_value = 0;
uint32_t g_start_value = 1;
Cpu* g_cpu = nullptr;
std::jmp_buf* g_resume_point = nullptr;
uint32_t g_resume_context = 0;

void store(const Cpu& c, uint32_t context) {
    for (uint32_t i = 0; i < 32; i++) {
        wr32(context + kGprOffset + 4 * i, c.r[i]);
        wr64(context + kFprOffset + 8 * i, fpr_bits(c.f[i]));
        wr64(context + kPairedOffset + 8 * i, fpr_bits(c.ps1[i]));
    }
    wr32(context + kConditionOffset, mfcr(c));
    wr32(context + kLinkOffset, c.lr);
    wr32(context + kCounterOffset, c.ctr);
    wr32(context + kExceptionOffset, mfxer(c));
    wr32(context + kFpscrOffset, c.fpscr);
    wr32(context + kSrr0Offset, c.lr);
    wr32(context + kSrr1Offset, c.msr);
    for (uint32_t i = 0; i < 8; i++) {
        wr32(context + kGqrOffset + 4 * i, c.spr[kSprGqr0 + i]);
    }
}

void restore(Cpu& c, uint32_t context) {
    for (uint32_t i = 0; i < 32; i++) {
        c.r[i] = rd32(context + kGprOffset + 4 * i);
        c.f[i] = fpr_from_bits(rd64(context + kFprOffset + 8 * i));
        c.ps1[i] = fpr_from_bits(rd64(context + kPairedOffset + 8 * i));
    }
    mtcrf(c, 0xFF, rd32(context + kConditionOffset));
    c.lr = rd32(context + kLinkOffset);
    c.ctr = rd32(context + kCounterOffset);
    mtxer(c, rd32(context + kExceptionOffset));
    c.fpscr = rd32(context + kFpscrOffset);
    for (uint32_t i = 0; i < 8; i++) {
        c.spr[kSprGqr0 + i] = rd32(context + kGqrOffset + 4 * i);
    }
}

uint32_t float_bits(double value) {
    float single = static_cast<float>(value);
    uint32_t bits;
    std::memcpy(&bits, &single, sizeof bits);
    return bits;
}

double float_from_bits(uint32_t bits) {
    float single;
    std::memcpy(&single, &bits, sizeof single);
    return single;
}

void store_jump(const Cpu& c, uint32_t buffer) {
    wr32(buffer + kJumpLinkOffset, c.lr);
    wr32(buffer + kJumpConditionOffset, mfcr(c));
    wr32(buffer + kJumpStackOffset, c.r[1]);
    wr32(buffer + kJumpTocOffset, c.r[2]);
    for (uint32_t i = kFirstSavedRegister; i < 32; i++) {
        wr32(buffer + kJumpGprOffset + 4 * (i - kFirstSavedRegister), c.r[i]);
    }
    for (uint32_t i = kFirstSavedFloat; i < 32; i++) {
        uint32_t slot = buffer + kJumpFprOffset + 0x10 * (i - kFirstSavedFloat);
        wr64(slot, fpr_bits(c.f[i]));
        wr32(slot + 8, float_bits(c.f[i]));
        wr32(slot + 12, float_bits(c.ps1[i]));
    }
    wr64(buffer + kJumpFpscrOffset, c.fpscr);
}

void restore_jump(Cpu& c, uint32_t buffer) {
    c.lr = rd32(buffer + kJumpLinkOffset);
    mtcrf(c, 0xFF, rd32(buffer + kJumpConditionOffset));
    c.r[1] = rd32(buffer + kJumpStackOffset);
    c.r[2] = rd32(buffer + kJumpTocOffset);
    for (uint32_t i = kFirstSavedRegister; i < 32; i++) {
        c.r[i] = rd32(buffer + kJumpGprOffset + 4 * (i - kFirstSavedRegister));
    }
    for (uint32_t i = kFirstSavedFloat; i < 32; i++) {
        uint32_t slot = buffer + kJumpFprOffset + 0x10 * (i - kFirstSavedFloat);
        c.f[i] = float_from_bits(rd32(slot + 8));
        c.ps1[i] = float_from_bits(rd32(slot + 12));
    }
    c.fpscr = rd32(buffer + kJumpFpscrOffset + 4);
}

[[noreturn]] void resume_pending() {
    std::jmp_buf* point = g_resume_point;
    g_resume_point = nullptr;
    std::longjmp(*point, 1);
}

void discard_fiber(void* fiber) {
    for (auto it = g_saved.begin(); it != g_saved.end();) {
        it = it->second.fiber == fiber ? g_saved.erase(it) : std::next(it);
    }
    for (auto it = g_jumps.begin(); it != g_jumps.end();) {
        it = it->second.fiber == fiber ? g_jumps.erase(it) : std::next(it);
    }
    if (fiber == fiber::current()) {
        return;
    }
#ifdef WP_TRACE
    g_traces.erase(fiber);
#endif
    fiber::destroy(fiber);
}

void start_thread(void* parameter) {
    uint32_t context = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(parameter));
    Cpu& c = *g_cpu;
    restore(c, context);
    c.msr = rd32(context + kSrr1Offset);
    uint32_t entry = rd32(context + kSrr0Offset);
    uint32_t exit_function = c.lr;
    call(c, entry);
    call(c, exit_function);
    std::fprintf(stderr, "thread function %08x returned without exiting (exit function %08x)\n", entry, exit_function);
    std::abort();
}

using Function = void (*)(Cpu&);

Function find_resume(uint32_t address) {
    if (address < 0x80000000u) {
        return find_module_resume(address);
    }
    const FunctionEntry* end = g_resume_table + g_resume_count;
    const FunctionEntry* it = std::lower_bound(g_resume_table, end, address,
                                               [](const FunctionEntry& entry, uint32_t value) { return entry.address < value; });
    return it != end && it->address == address ? it->function : nullptr;
}

void continue_at(Cpu& c, uint32_t address) {
    while (address != 0) {
        Function function = find_resume(address);
        if (!function) {
            std::fprintf(stderr, "cannot continue at return address %08x", address);
            std::fputc(10, stderr);
            describe_loaded_modules(address);
            std::abort();
        }
        g_resume_address = address;
        function(c);
        address = c.lr;
    }
}

void start_jump(void* parameter) {
    uint32_t buffer = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(parameter));
    Cpu& c = *g_cpu;
    restore_jump(c, buffer);
    uint32_t target = c.lr;
    if (find_resume(target)) {
        c.r[3] = g_start_value;
        continue_at(c, target);
    } else {
        c.lr = 0;
        call(c, target);
    }
    std::fputs("jump function returned without exiting\n", stderr);
    std::abort();
}

}

void init_threads(Cpu& c) {
    g_cpu = &c;
    fiber::adopt_current_thread();
#ifdef WP_TRACE
    g_traces[fiber::current()] = TraceState{g_call_stack, 0};
#endif
}

void save_context(Cpu& c, std::jmp_buf* point) {
    uint32_t context = c.r[3];
    store(c, context);
    g_saved[context] = SavedContext{fiber::current(), point, c.lr};
    c.r[3] = 0;
}

void resume_context(Cpu& c) {
    restore(c, g_resume_context);
    c.r[3] = 1;
}

void load_context(Cpu& c) {
    uint32_t context = c.r[3];
    auto it = g_saved.find(context);
    if (it != g_saved.end() && rd32(context + kSrr0Offset) != it->second.resume) {
        discard_fiber(it->second.fiber);
        it = g_saved.find(context);
    }
    if (it == g_saved.end()) {
        void* fiber = fiber::create(kFiberStackSize, start_thread, reinterpret_cast<void*>(static_cast<uintptr_t>(context)));
#ifdef WP_TRACE
        register_trace(fiber);
#endif
        switch_to(fiber);
        if (g_resume_point) {
            resume_pending();
        }
        return;
    }
    SavedContext saved = it->second;
    g_saved.erase(it);
    g_resume_point = saved.point;
    g_resume_context = context;
    if (saved.fiber != fiber::current()) {
        switch_to(saved.fiber);
    }
    if (g_resume_point) {
        resume_pending();
    }
}

void save_jump(Cpu& c, std::jmp_buf* point) {
    uint32_t buffer = c.r[3];
    store_jump(c, buffer);
    g_jumps[buffer] = SavedJump{fiber::current(), point, rd32(buffer + kJumpLinkOffset), rd32(buffer + kJumpStackOffset)};
    c.r[3] = 0;
}

void resume_jump(Cpu& c) {
    restore_jump(c, g_jump_buffer);
    c.r[3] = g_jump_value;
}

void long_jump(Cpu& c) {
    uint32_t buffer = c.r[3];
    uint32_t value = c.r[4] != 0 ? c.r[4] : 1;
    auto it = g_jumps.find(buffer);
    bool resumable = it != g_jumps.end() && it->second.link == rd32(buffer + kJumpLinkOffset) &&
                     it->second.stack == rd32(buffer + kJumpStackOffset);
    if (!resumable) {
        if (it != g_jumps.end()) {
            g_jumps.erase(it);
        }
        g_start_value = value;
        void* fiber = fiber::create(kFiberStackSize, start_jump, reinterpret_cast<void*>(static_cast<uintptr_t>(buffer)));
#ifdef WP_TRACE
        register_trace(fiber);
#endif
        switch_to(fiber);
        if (g_resume_point) {
            resume_pending();
        }
        return;
    }
    SavedJump saved = it->second;
    g_jumps.erase(it);
    g_resume_point = saved.point;
    g_jump_buffer = buffer;
    g_jump_value = value;
    if (saved.fiber != fiber::current()) {
        switch_to(saved.fiber);
    }
    if (g_resume_point) {
        resume_pending();
    }
}

uint32_t g_resume_address = 0;
uint32_t g_taken_resume_address = 0;

void bad_resume(Cpu&) {
    std::fprintf(stderr, "no resume point at %08x in the function found for it", g_taken_resume_address);
    std::fputc(10, stderr);
    std::abort();
}

void print_guest_registers() {
    if (!g_cpu) {
        return;
    }
    std::fprintf(stderr, "guest registers: lr=%08x sp=%08x r3=%08x r4=%08x r5=%08x r6=%08x r7=%08x r30=%08x r31=%08x", g_cpu->lr, g_cpu->r[1],
                 g_cpu->r[3], g_cpu->r[4], g_cpu->r[5], g_cpu->r[6], g_cpu->r[7], g_cpu->r[30], g_cpu->r[31]);
    std::fputc(10, stderr);
    describe_loaded_modules(g_cpu->lr);
}

void print_thread_stacks() {
#ifdef WP_TRACE
    void* current = fiber::current();
    int index = 0;
    for (const auto& entry : g_traces) {
        size_t depth = entry.first == current ? g_call_depth : entry.second.depth;
        std::fprintf(stderr, "fiber %d%s depth %zu:", index++, entry.first == current ? " (current)" : "", depth);
        for (size_t i = 0; i < 12 && i < depth && depth <= kCallStackSize; i++) {
            uint32_t address = entry.second.stack[depth - 1 - i];
            const char* name = find_name(address);
            std::fprintf(stderr, " %08x%s%s%s", address, *name ? "(" : "", name, *name ? ")" : "");
        }
        std::fputc(10, stderr);
    }
#endif
}

uint32_t symbol_address(const char* name) {
    for (size_t i = 0; i < g_name_count; i++) {
        if (std::strcmp(g_name_table[i].name, name) == 0) {
            return g_name_table[i].address;
        }
    }
    std::fprintf(stderr, "unknown symbol %s\n", name);
    std::abort();
}

}
