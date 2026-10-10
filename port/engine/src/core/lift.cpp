#include "wp/lift.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "wp/gx.h"
#include "wp/settings.h"

#ifndef _WIN32
#include <signal.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace wp {
extern bool g_polling_blocked;  // interrupts.cpp
}

namespace wp::lift {

namespace {

struct Range {
    uint32_t start;
    uint32_t size;
};

// MEM1, the hardware register window (minus the FIFO port), MEM2.
std::vector<Range> g_ranges = {{0x00000000, 0x01800000}, {0x0C000000, 0x8000}, {0x0C008004, 0x7FFC}, {0x10000000, 0x04000000}};
const std::vector<Range> kFullRanges = g_ranges;

bool verifying() {
    static const bool value = std::getenv("WP_VERIFY") || std::getenv("WP_GX_VERIFY");
    return value;
}

int g_nesting = 0;
bool g_reference = false;
std::map<std::string, uint64_t> g_calls;
uint64_t g_mismatches = 0;

void save(std::vector<uint8_t>& out) {
    size_t total = 0;
    for (const Range& r : g_ranges) {
        total += r.size;
    }
    out.resize(total);
    size_t at = 0;
    for (const Range& r : g_ranges) {
        std::memcpy(out.data() + at, g_memory + r.start, r.size);
        at += r.size;
    }
}

void restore(const std::vector<uint8_t>& in) {
    size_t at = 0;
    for (const Range& r : g_ranges) {
        std::memcpy(g_memory + r.start, in.data() + at, r.size);
        at += r.size;
    }
}

void report(const char* name, const char* what) {
    g_mismatches++;
    std::fprintf(stderr, "verify %s: %s\n", name, what);
}

// Report the differing bytes of one block (guest address `start`); `shown` counts reports per call.
void compare_block(const char* name, uint32_t start, const uint8_t* want, const uint8_t* got, uint32_t size, uint32_t stack, int& shown) {
    static const bool check_stack = std::getenv("WP_VERIFY_STACK") != nullptr;
    // WP_VERIFY_SHOW=N: differing bytes reported per call (default 8).
    static const int limit = std::getenv("WP_VERIFY_SHOW") ? std::atoi(std::getenv("WP_VERIFY_SHOW")) : 8;
    if (std::memcmp(want, got, size) == 0) {
        return;
    }
    for (uint32_t i = 0; i < size && shown < limit; i++) {
        uint32_t address = start + i;
        bool in_frames = address >= stack - 0x8000 && address < stack + 8;
        if (want[i] == got[i] || (in_frames && !check_stack)) {
            continue;  // the original's own stack frames (WP_VERIFY_STACK=1 compares them too)
        }
        char text[128];
        std::snprintf(text, sizeof text, "memory %08x (sp%+d): original %02x, lifted %02x", address | 0x80000000,
                      static_cast<int>(address - stack), want[i], got[i]);
        report(name, text);
        shown++;
    }
}

void compare_memory(const char* name, const std::vector<uint8_t>& expected, uint32_t stack) {
    size_t at = 0;
    int shown = 0;
    for (const Range& r : g_ranges) {
        compare_block(name, r.start, expected.data() + at, g_memory + r.start, r.size, stack, shown);
        at += r.size;
    }
}

#ifndef _WIN32
// Copy-on-write page tracking (WP_VERIFY_FAST=1): instead of copying and comparing all of MEM1
// and MEM2 for every verified call, the two runs write-protect guest memory; the first write to
// a page saves its original contents and unprotects it, so only written pages are restored and
// compared. Host code must not write guest memory through system calls (read() into a guest
// buffer would fail) during a verified call; the game code verified this way does not.
namespace dirty {

constexpr Range kTracked[2] = {{0x00000000, 0x01800000}, {0x10000000, 0x04000000}};

size_t g_page = 0;
uint8_t* g_shadow = nullptr;     // originals, at the same offsets as g_memory
uint8_t* g_expected = nullptr;   // the reference run's results
std::vector<uint8_t> g_dirty;    // per page
std::vector<uint32_t> g_pages;   // pages written during the current run (capacity reserved)
volatile sig_atomic_t g_active = 0;
struct sigaction g_old_segv;
struct sigaction g_old_bus;

bool tracked(size_t offset) {
    for (const Range& r : kTracked) {
        if (offset >= r.start && offset < static_cast<size_t>(r.start) + r.size) {
            return true;
        }
    }
    return false;
}

void handler(int signal_number, siginfo_t* info, void* context) {
    uint8_t* address = static_cast<uint8_t*>(info->si_addr);
    if (g_active && address >= g_memory && address < g_memory + kPhysicalSize) {
        size_t offset = static_cast<size_t>(address - g_memory);
        size_t page = offset / g_page;
        if (tracked(offset) && !g_dirty[page]) {
            uint8_t* base = g_memory + page * g_page;
            g_dirty[page] = 1;
            g_pages.push_back(static_cast<uint32_t>(page));
            std::memcpy(g_shadow + page * g_page, base, g_page);
            mprotect(base, g_page, PROT_READ | PROT_WRITE);
            return;
        }
    }
    struct sigaction& old = signal_number == SIGBUS ? g_old_bus : g_old_segv;
    if (old.sa_flags & SA_SIGINFO) {
        old.sa_sigaction(signal_number, info, context);
    } else if (old.sa_handler != SIG_DFL && old.sa_handler != SIG_IGN) {
        old.sa_handler(signal_number);
    } else {
        signal(signal_number, SIG_DFL);
        raise(signal_number);
    }
}

bool available() {
    static const bool value = [] {
        if (!std::getenv("WP_VERIFY_FAST")) {
            return false;
        }
        g_page = static_cast<size_t>(getpagesize());
        if (reinterpret_cast<uintptr_t>(g_memory) % g_page != 0) {
            std::fprintf(stderr, "verify: guest memory is not page-aligned, WP_VERIFY_FAST ignored\n");
            return false;
        }
        g_shadow = static_cast<uint8_t*>(std::calloc(kPhysicalSize, 1));
        g_expected = static_cast<uint8_t*>(std::calloc(kPhysicalSize, 1));
        g_dirty.assign(kPhysicalSize / g_page, 0);
        g_pages.reserve(kPhysicalSize / g_page);
        struct sigaction action {};
        action.sa_sigaction = handler;
        action.sa_flags = SA_SIGINFO;
        sigemptyset(&action.sa_mask);
        sigaction(SIGSEGV, &action, &g_old_segv);
        sigaction(SIGBUS, &action, &g_old_bus);
        return true;
    }();
    return value;
}

void protect(int protection) {
    for (const Range& r : kTracked) {
        mprotect(g_memory + r.start, r.size, protection);
    }
}

// Track one run; returns the pages it wrote (their originals are in g_shadow).
template <typename Run>
std::vector<uint32_t> track(Run run) {
    g_pages.clear();
    g_active = 1;
    protect(PROT_READ);
    run();
    protect(PROT_READ | PROT_WRITE);
    g_active = 0;
    std::vector<uint32_t> pages = g_pages;
    for (uint32_t page : pages) {
        g_dirty[page] = 0;
    }
    return pages;
}

}  // namespace dirty
#endif

std::string hex_bytes(const std::vector<uint8_t>& bytes) {
    std::string text;
    char hex[4];
    for (size_t i = 0; i < bytes.size() && i < 48; i++) {
        std::snprintf(hex, sizeof hex, " %02x", bytes[i]);
        text += hex;
    }
    return text;
}

// The hardware register window (always copied in full; it is small).
const std::vector<Range> kWindow = {{0x0C000000, 0x8000}, {0x0C008004, 0x7FFC}};

void save_window(std::vector<uint8_t>& out) {
    out.clear();
    for (const Range& r : kWindow) {
        out.insert(out.end(), g_memory + r.start, g_memory + r.start + r.size);
    }
}

void restore_window(const std::vector<uint8_t>& in) {
    size_t at = 0;
    for (const Range& r : kWindow) {
        std::memcpy(g_memory + r.start, in.data() + at, r.size);
        at += r.size;
    }
}

void compare_rest(const char* name, Cpu& c, const Cpu& expected, Ret ret, const std::vector<uint8_t>& fifo, const std::vector<uint8_t>& expected_fifo);

}  // namespace

bool Group::enabled() const {
    uint64_t current = settings::revision();
    uint64_t cached = cache.load(std::memory_order_acquire);
    if ((cached >> 32) != current) {
        std::string text = settings::text(setting, environment);
        cached = (current << 32) | (text.empty() || std::atoi(text.c_str()) != 0 ? 1u : 0u);
        cache.store(cached, std::memory_order_release);
    }
    return (cached & 1) != 0;
}

std::set<std::string> name_list(const char* variable) {
    std::set<std::string> result;
    const char* list = std::getenv(variable);
    std::string text = list ? list : "";
    size_t start = 0;
    while (start <= text.size()) {
        size_t end = text.find(',', start);
        end = end == std::string::npos ? text.size() : end;
        if (end > start) {
            result.insert(text.substr(start, end - start));
        }
        start = end + 1;
    }
    return result;
}

bool switched_off(const char* name) {
    static const std::set<std::string> names = name_list("WP_LIFT_OFF");
    return !names.empty() && names.count(name) != 0;
}

bool in_reference() {
    return g_reference;
}

bool due(const char* name, Ret ret) {
    if (!verifying()) {
        return false;
    }
    // WP_VERIFY_EVERY=N: after the first 8 calls, verify every Nth (default 509).
    static const uint64_t every = std::getenv("WP_VERIFY_EVERY") ? std::max<uint64_t>(1, std::strtoull(std::getenv("WP_VERIFY_EVERY"), nullptr, 10)) : 509;
    // WP_VERIFY_ONLY=Name1,Name2: verify just these functions.
    static const std::set<std::string> only = name_list("WP_VERIFY_ONLY");
    if (!only.empty() && only.count(name) == 0) {
        return false;
    }
    uint64_t n = ++g_calls[name];
    return ret != Ret::Blocking && g_nesting == 0 && (n <= 8 || n % every == 0);
}

namespace {

void compare_rest(const char* name, Cpu& c, const Cpu& expected, Ret ret, const std::vector<uint8_t>& fifo, const std::vector<uint8_t>& expected_fifo) {
    if (fifo != expected_fifo) {
        report(name, ("command bytes differ: original" + hex_bytes(expected_fifo) + " / lifted" + hex_bytes(fifo)).c_str());
    }
    char text[96];
    if (ret == Ret::Int && c.r[3] != expected.r[3]) {
        std::snprintf(text, sizeof text, "returned %08x, original %08x", c.r[3], expected.r[3]);
        report(name, text);
    }
    if (ret == Ret::Float && c.f[1] != expected.f[1]) {
        std::snprintf(text, sizeof text, "returned %g, original %g", c.f[1], expected.f[1]);
        report(name, text);
    }
    for (int i : {1, 2, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31}) {
        if (c.r[i] != expected.r[i]) {
            std::snprintf(text, sizeof text, "r%d is %08x, original %08x", i, c.r[i], expected.r[i]);
            report(name, text);
        }
    }
}

#ifndef _WIN32
// verify() with copy-on-write page tracking (see dirty above).
void verify_fast(Cpu& c, const char* name, void (*lifted)(Cpu&), Ret ret, bool commit) {
    static std::vector<uint8_t> window_before;
    static std::vector<uint8_t> window_after;
    static std::vector<uint8_t> marks;
    const size_t page_size = dirty::g_page;
    g_nesting++;
    g_polling_blocked = true;
    Cpu input = c;
    save_window(window_before);
    std::vector<uint8_t> expected_fifo;
    gx::capture_fifo(&expected_fifo);
    std::vector<uint32_t> reference_pages = dirty::track([&] {
        g_reference = true;
        call_original(c, name);
        g_reference = false;
    });
    gx::capture_fifo(nullptr);
    Cpu expected = c;
    save_window(window_after);
    for (uint32_t page : reference_pages) {
        size_t offset = page * page_size;
        std::memcpy(dirty::g_expected + offset, g_memory + offset, page_size);
        std::memcpy(g_memory + offset, dirty::g_shadow + offset, page_size);
    }
    restore_window(window_before);
    c = input;
    std::vector<uint8_t> fifo;
    gx::capture_fifo(&fifo);
    std::vector<uint32_t> lifted_pages = dirty::track([&] { lifted(c); });
    gx::capture_fifo(nullptr);
    g_polling_blocked = false;
    g_nesting--;

    // Compare the pages either run wrote (expected: the reference's result, or the original).
    marks.assign(dirty::g_dirty.size(), 0);
    for (uint32_t page : reference_pages) {
        marks[page] = 1;
    }
    std::vector<uint32_t> pages = reference_pages;
    for (uint32_t page : lifted_pages) {
        if (!marks[page]) {
            marks[page] = 2;
            pages.push_back(page);
        }
    }
    std::sort(pages.begin(), pages.end());
    uint32_t stack = input.r[1] & kAddressMask;
    int shown = 0;
    for (uint32_t page : pages) {
        size_t offset = page * page_size;
        const uint8_t* want = (marks[page] == 1 ? dirty::g_expected : dirty::g_shadow) + offset;
        compare_block(name, static_cast<uint32_t>(offset), want, g_memory + offset, static_cast<uint32_t>(page_size), stack, shown);
    }
    size_t at = 0;
    for (const Range& r : kWindow) {
        compare_block(name, r.start, window_after.data() + at, g_memory + r.start, r.size, stack, shown);
        at += r.size;
    }
    compare_rest(name, c, expected, ret, fifo, expected_fifo);
    if (!commit) {
        for (uint32_t page : pages) {
            size_t offset = page * page_size;
            std::memcpy(g_memory + offset, dirty::g_shadow + offset, page_size);
        }
        restore_window(window_before);
        c = input;
        return;
    }
    for (uint8_t byte : fifo) {
        gx::push(byte, 1);
    }
}
#endif

}  // namespace

void verify(Cpu& c, const char* name, void (*lifted)(Cpu&), Ret ret, bool commit) {
#ifndef _WIN32
    if (g_ranges.size() == kFullRanges.size() && g_ranges[0].size == kFullRanges[0].size && dirty::available()) {
        verify_fast(c, name, lifted, ret, commit);
        return;
    }
#endif
    static std::vector<uint8_t> before;
    static std::vector<uint8_t> after;
    g_nesting++;
    g_polling_blocked = true;  // no interrupts or IPC progress while the two runs are compared
    Cpu input = c;
    save(before);
    std::vector<uint8_t> expected_fifo;
    gx::capture_fifo(&expected_fifo);
    g_reference = true;
    call_original(c, name);
    g_reference = false;
    gx::capture_fifo(nullptr);
    Cpu expected = c;
    save(after);
    restore(before);
    c = input;
    std::vector<uint8_t> fifo;
    gx::capture_fifo(&fifo);
    lifted(c);
    gx::capture_fifo(nullptr);
    g_polling_blocked = false;
    g_nesting--;
    compare_memory(name, after, input.r[1] & kAddressMask);
    compare_rest(name, c, expected, ret, fifo, expected_fifo);
    if (!commit) {
        restore(before);
        c = input;
        return;
    }
    for (uint8_t byte : fifo) {
        gx::push(byte, 1);
    }
}

uint64_t mismatches() {
    return g_mismatches;
}

void print_report() {
    if (!verifying()) {
        return;
    }
    std::fprintf(stderr, "verify: %llu mismatches\n", static_cast<unsigned long long>(g_mismatches));
    for (const auto& [name, count] : g_calls) {
        std::fprintf(stderr, "  %-32s %llu calls\n", name.c_str(), static_cast<unsigned long long>(count));
    }
}

void use_self_test_ranges(bool enable, uint32_t scratch, uint32_t size) {
    if (enable) {
        g_ranges = {{0x00000000, 0x01800000}, {0x0C000000, 0x8000}, {0x0C008004, 0x7FFC}, {scratch & kAddressMask, size}};
    } else {
        g_ranges = kFullRanges;
    }
}

}  // namespace wp::lift
