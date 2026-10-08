#include "wp/ipc.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <vector>

#include "wp/cpu.h"
#include "wp/ios.h"
#include "wp/memory.h"

namespace wp::ipc {

namespace {

constexpr uint32_t kPpcMessage = 0x00;
constexpr uint32_t kPpcControl = 0x04;
constexpr uint32_t kArmMessage = 0x08;
constexpr uint32_t kPpcInterruptFlags = 0x30;
constexpr uint32_t kPpcInterruptMask = 0x34;
constexpr uint32_t kBroadwayCause = 0x40000000;
constexpr uint32_t kReplyCommand = 8;
constexpr uint64_t kAcknowledgeTicks = 500;
constexpr uint64_t kInterruptTicks = 100;

struct Control {
    bool x1 = false;
    bool x2 = false;
    bool y1 = false;
    bool y2 = false;
    bool iy1 = false;
    bool iy2 = false;

    uint32_t ppc() const {
        return (static_cast<uint32_t>(iy2) << 5) | (static_cast<uint32_t>(iy1) << 4) | (static_cast<uint32_t>(x2) << 3) |
               (static_cast<uint32_t>(y1) << 2) | (static_cast<uint32_t>(y2) << 1) | static_cast<uint32_t>(x1);
    }

    void ppc(uint32_t value) {
        x1 = value & 1;
        x2 = (value >> 3) & 1;
        if ((value >> 2) & 1) {
            y1 = false;
        }
        if ((value >> 1) & 1) {
            y2 = false;
        }
        iy1 = (value >> 4) & 1;
        iy2 = (value >> 5) & 1;
    }
};

enum class EventKind {
    Request,
    Reply,
    Interrupt,
};

struct Event {
    uint64_t due;
    uint64_t order;
    EventKind kind;
    uint32_t address;
};

uint32_t g_ppc_message = 0;
uint32_t g_arm_message = 0;
Control g_control;
uint32_t g_interrupt_flags = 0;
uint32_t g_interrupt_mask = kBroadwayCause;
std::vector<Event> g_events;
uint64_t g_event_order = 0;
std::deque<uint32_t> g_requests;
std::deque<uint32_t> g_replies;
uint64_t g_last_reply = 0;

struct Trace {
    uint64_t time;
    char kind;
    uint32_t a;
    uint32_t b;
};

Trace g_trace[64] = {};
uint32_t g_trace_next = 0;

void trace(char kind, uint32_t a, uint32_t b) {
    g_trace[g_trace_next++ % 64] = {time_base(), kind, a, b};
}

void schedule(uint64_t delay, EventKind kind, uint32_t address) {
    g_events.push_back({time_base() + delay, g_event_order++, kind, address});
}

bool ready() {
    return !g_control.y1 && !g_control.y2 && (g_interrupt_flags & kBroadwayCause) == 0;
}

void update_interrupts() {
    if ((g_control.y1 && g_control.iy1) || (g_control.y2 && g_control.iy2)) {
        g_interrupt_flags |= kBroadwayCause;
    }
}

void execute(uint32_t address) {
    uint64_t ticks = 0;
    int32_t result = ios::send(address, ticks);
    if (result != ios::kDeferred) {
        reply(address, result, ticks);
    }
}

void update_ipc() {
    if (!ready()) {
        return;
    }
    if (!g_requests.empty()) {
        uint32_t address = g_requests.front();
        g_requests.pop_front();
        trace('A', address, rd32(address));
        g_control.x1 = false;
        g_control.y2 = true;
        schedule(kInterruptTicks, EventKind::Interrupt, 0);
        execute(address);
        return;
    }
    if (!g_replies.empty()) {
        g_arm_message = g_replies.front();
        g_replies.pop_front();
        trace('R', g_arm_message, rd32(g_arm_message + 4));
        g_control.y1 = true;
        schedule(kInterruptTicks, EventKind::Interrupt, 0);
    }
}

uint32_t& backing(uint32_t address) {
    return *reinterpret_cast<uint32_t*>(g_memory + (address & kAddressMask));
}

}

uint32_t read32(uint32_t address) {
    trace('r', address & 0xFF, g_control.ppc());
    switch (address & 0xFF) {
    case kPpcControl:
        return g_control.ppc();
    case kArmMessage:
        return g_arm_message;
    default:
        return __builtin_bswap32(backing(address));
    }
}

void write32(uint32_t address, uint32_t value) {
    trace('w', address & 0xFF, value);
    switch (address & 0xFF) {
    case kPpcMessage:
        g_ppc_message = value;
        break;
    case kPpcControl:
        g_control.ppc(value);
        if ((((value >> 2) & 1) && g_control.iy1) || (((value >> 1) & 1) && g_control.iy2)) {
            g_interrupt_flags |= kBroadwayCause;
        }
        if (g_control.x1) {
            schedule(kAcknowledgeTicks, EventKind::Request, g_ppc_message);
        }
        update_ipc();
        update_interrupts();
        break;
    case kPpcInterruptFlags:
        g_interrupt_flags &= ~value;
        update_ipc();
        update_interrupts();
        break;
    case kPpcInterruptMask:
        g_interrupt_mask = value;
        update_ipc();
        update_interrupts();
        break;
    default:
        backing(address) = __builtin_bswap32(value);
        break;
    }
}

void reply(uint32_t request, int32_t result, uint64_t ticks) {
    uint64_t now = time_base();
    if (g_last_reply > now) {
        ticks += g_last_reply - now;
    }
    g_last_reply = now + ticks;
    uint32_t command = rd32(request);
    wr32(request + 4, static_cast<uint32_t>(result));
    wr32(request + 8, command);
    wr32(request, kReplyCommand);
    schedule(ticks, EventKind::Reply, request);
}

void update() {
    ios::update();
    uint32_t request = 0;
    int32_t result = 0;
    while (ios::take_completion(request, result)) {
        reply(request, result, 0);
    }
    uint64_t now = time_base();
    while (!g_events.empty()) {
        auto next = std::min_element(g_events.begin(), g_events.end(), [](const Event& a, const Event& b) {
            return a.due != b.due ? a.due < b.due : a.order < b.order;
        });
        if (next->due > now) {
            break;
        }
        Event event = *next;
        g_events.erase(next);
        switch (event.kind) {
        case EventKind::Request:
            g_requests.push_back(event.address);
            update_ipc();
            break;
        case EventKind::Reply:
            g_replies.push_back(event.address);
            update_ipc();
            break;
        case EventKind::Interrupt:
            update_interrupts();
            break;
        }
    }
}

bool interrupt_pending() {
    return (g_interrupt_flags & g_interrupt_mask) != 0;
}

void report() {
    std::fprintf(stderr, "IPC control=%02x flags=%08x mask=%08x ppc message=%08x arm message=%08x requests=%zu replies=%zu events=%zu", g_control.ppc(),
                 g_interrupt_flags, g_interrupt_mask, g_ppc_message, g_arm_message, g_requests.size(), g_replies.size(), g_events.size());
    std::fputc(10, stderr);
    uint64_t now = time_base();
    for (uint32_t i = 0; i < 64; i++) {
        const Trace& t = g_trace[(g_trace_next + i) % 64];
        if (t.kind) {
            std::fprintf(stderr, "IPC trace %lld %c %08x %08x", static_cast<long long>(t.time) - static_cast<long long>(now), t.kind, t.a, t.b);
            std::fputc(10, stderr);
        }
    }
    for (const Event& event : g_events) {
        std::fprintf(stderr, "IPC event kind=%d address=%08x due in %lld ticks", static_cast<int>(event.kind), event.address,
                     static_cast<long long>(event.due) - static_cast<long long>(now));
        std::fputc(10, stderr);
    }
}

}
