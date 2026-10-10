#include "wp/fiber.h"

#ifdef _WIN32
#include <windows.h>
#elif defined(__ANDROID__) && defined(__aarch64__)
#include <cstdint>
#include <cstdlib>
#else
#ifdef __APPLE__
#define _XOPEN_SOURCE 700
#endif
#include <ucontext.h>

#include <cstdlib>
#endif

namespace wp::fiber {

#ifdef _WIN32

namespace {

struct Start {
    Entry entry;
    void* parameter;
};

thread_local Handle g_current = nullptr;

VOID CALLBACK run(PVOID data) {
    Start start = *static_cast<Start*>(data);
    delete static_cast<Start*>(data);
    start.entry(start.parameter);
}

}

Handle adopt_current_thread() {
    g_current = ConvertThreadToFiber(nullptr);
    return g_current;
}

Handle current() {
    return g_current;
}

Handle create(size_t stack_size, Entry entry, void* parameter) {
    return CreateFiber(stack_size, run, new Start{entry, parameter});
}

void switch_to(Handle target) {
    g_current = target;
    SwitchToFiber(target);
}

void destroy(Handle fiber) {
    DeleteFiber(fiber);
}

#elif defined(__ANDROID__) && defined(__aarch64__)

}

// Android's C library has no makecontext/swapcontext, so the switch is done here: the registers a
// call must keep (x19-x30, d8-d15) and the floating-point control register go on the stack of the
// fiber being left, and its stack pointer is all that is kept of it.
extern "C" void wp_fiber_switch(void** save_stack, void* load_stack);
extern "C" void wp_fiber_start();
extern "C" void wp_fiber_run(void* fiber);

asm(R"(
    .text
    .p2align 2
    .global wp_fiber_switch
    .type wp_fiber_switch, %function
wp_fiber_switch:
    sub sp, sp, #176
    stp x19, x20, [sp, #0]
    stp x21, x22, [sp, #16]
    stp x23, x24, [sp, #32]
    stp x25, x26, [sp, #48]
    stp x27, x28, [sp, #64]
    stp x29, x30, [sp, #80]
    stp d8, d9, [sp, #96]
    stp d10, d11, [sp, #112]
    stp d12, d13, [sp, #128]
    stp d14, d15, [sp, #144]
    mrs x9, fpcr
    str x9, [sp, #160]
    mov x9, sp
    str x9, [x0]
    mov sp, x1
    ldr x9, [sp, #160]
    msr fpcr, x9
    ldp x19, x20, [sp, #0]
    ldp x21, x22, [sp, #16]
    ldp x23, x24, [sp, #32]
    ldp x25, x26, [sp, #48]
    ldp x27, x28, [sp, #64]
    ldp x29, x30, [sp, #80]
    ldp d8, d9, [sp, #96]
    ldp d10, d11, [sp, #112]
    ldp d12, d13, [sp, #128]
    ldp d14, d15, [sp, #144]
    add sp, sp, #176
    ret
    .size wp_fiber_switch, .-wp_fiber_switch

    .global wp_fiber_start
    .type wp_fiber_start, %function
wp_fiber_start:
    mov x0, x19
    mov x29, #0
    mov x30, #0
    b wp_fiber_run
    .size wp_fiber_start, .-wp_fiber_start
)");

namespace wp::fiber {

namespace {

constexpr size_t kFrameSize = 176;       // as wp_fiber_switch lays it out
constexpr size_t kFrameX19 = 0;
constexpr size_t kFrameX30 = 88;
constexpr size_t kFrameFpcr = 160;

struct Fiber {
    void* stack_pointer = nullptr;
    void* stack = nullptr;
    Entry entry = nullptr;
    void* parameter = nullptr;
};

thread_local Fiber* g_current = nullptr;

}

Handle adopt_current_thread() {
    g_current = new Fiber;
    return g_current;
}

Handle current() {
    return g_current;
}

Handle create(size_t stack_size, Entry entry, void* parameter) {
    Fiber* fiber = new Fiber;
    fiber->stack = std::malloc(stack_size);
    fiber->entry = entry;
    fiber->parameter = parameter;
    // A first frame for wp_fiber_switch to "return" from, into wp_fiber_start with the fiber in x19.
    uintptr_t top = (reinterpret_cast<uintptr_t>(fiber->stack) + stack_size) & ~uintptr_t{15};
    uint64_t* frame = reinterpret_cast<uint64_t*>(top - kFrameSize);
    for (size_t i = 0; i < kFrameSize / sizeof(uint64_t); i++) {
        frame[i] = 0;
    }
    frame[kFrameX19 / 8] = reinterpret_cast<uint64_t>(fiber);
    frame[kFrameX30 / 8] = reinterpret_cast<uint64_t>(&wp_fiber_start);
    uint64_t fpcr;
    asm("mrs %0, fpcr" : "=r"(fpcr));
    frame[kFrameFpcr / 8] = fpcr;
    fiber->stack_pointer = frame;
    return fiber;
}

void switch_to(Handle target) {
    Fiber* from = g_current;
    Fiber* to = static_cast<Fiber*>(target);
    g_current = to;
    wp_fiber_switch(&from->stack_pointer, to->stack_pointer);
}

void destroy(Handle fiber) {
    Fiber* target = static_cast<Fiber*>(fiber);
    std::free(target->stack);
    delete target;
}

}

extern "C" void wp_fiber_run(void* fiber) {
    wp::fiber::Fiber* self = static_cast<wp::fiber::Fiber*>(fiber);
    self->entry(self->parameter);
    std::abort();
}

namespace wp::fiber {

#else

namespace {

struct Fiber {
    ucontext_t context;
    void* stack = nullptr;
    Entry entry = nullptr;
    void* parameter = nullptr;
};

thread_local Fiber* g_current = nullptr;
thread_local Fiber* g_starting = nullptr;

void run() {
    Fiber* self = g_starting;
    self->entry(self->parameter);
    std::abort();
}

}

Handle adopt_current_thread() {
    g_current = new Fiber;
    return g_current;
}

Handle current() {
    return g_current;
}

Handle create(size_t stack_size, Entry entry, void* parameter) {
    Fiber* fiber = new Fiber;
    fiber->stack = std::malloc(stack_size);
    fiber->entry = entry;
    fiber->parameter = parameter;
    getcontext(&fiber->context);
    fiber->context.uc_stack.ss_sp = fiber->stack;
    fiber->context.uc_stack.ss_size = stack_size;
    fiber->context.uc_link = nullptr;
    makecontext(&fiber->context, run, 0);
    return fiber;
}

void switch_to(Handle target) {
    Fiber* from = g_current;
    Fiber* to = static_cast<Fiber*>(target);
    g_current = to;
    g_starting = to;
    swapcontext(&from->context, &to->context);
}

void destroy(Handle fiber) {
    Fiber* target = static_cast<Fiber*>(fiber);
    std::free(target->stack);
    delete target;
}

#endif

}
