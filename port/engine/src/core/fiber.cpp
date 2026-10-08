#include "wp/fiber.h"

#ifdef _WIN32
#include <windows.h>
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
