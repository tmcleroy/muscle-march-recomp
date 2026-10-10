#pragma once

// Lifting: replacing translated guest functions with hand-written C++.
//
// A lifted function is installed through hle_functions.csv like any host handler. It works on
// the guest's own data in guest memory (so translated and lifted code can be mixed freely) and
// is declared with WP_LIFT, which adds:
//   - an on/off switch per group of functions (setting + environment variable); when off, the
//     translated original runs;
//   - differential verification (WP_VERIFY=1): for the first calls of each function and a
//     sample afterwards, the translated original runs first (its whole call tree translated),
//     memory/registers/GX command bytes are recorded and rolled back, the lifted version runs,
//     and any difference is reported. Calls marked Ret::Blocking (they wait for interrupts)
//     are never verified, nor are calls a WP_LIFT_WHEN condition excludes (the verifier holds
//     back interrupts and I/O, so a call that waits for them would never finish). Stack slots
//     below the stack pointer are skipped unless WP_VERIFY_STACK=1. WP_VERIFY_FAST=1 tracks
//     written pages copy-on-write instead of copying all guest memory per call (~50x faster);
//     WP_VERIFY_SHOW=N reports up to N differing bytes per call;
//   - WP_VERIFY_ONLY="Name1,Name2" verifies just those functions;
//   - WP_LIFT_OFF="Name1,Name2" runs just those functions' originals (bisecting).
//
// Helpers for writing lifted code: guest calls (gcall, gcall_from), scratch stack frames
// (GuestFrame), the original's own frame and the stale bytes it leaves (OriginalFrame),
// PowerPC bit operations (rlwinm/rlwimi, IBM bit numbering), single-precision loads/stores,
// and typed big-endian views of guest data (Field<T>, WP_FIELD).

#include <atomic>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <type_traits>

#include "wp/cpu.h"
#include "wp/hle.h"
#include "wp/memory.h"

namespace wp::lift {

// --- PowerPC bit operations (IBM numbering: bit 0 is the MSB) ------------------------------

constexpr uint32_t ppc_mask(int mb, int me) {
    uint32_t begin = 0xFFFFFFFFu >> mb;
    uint32_t end = 0xFFFFFFFFu << (31 - me);
    return mb <= me ? (begin & end) : (begin | end);
}
constexpr uint32_t rotl(uint32_t value, int shift) {
    return shift == 0 ? value : (value << shift) | (value >> (32 - shift));
}
constexpr uint32_t rlwinm(uint32_t value, int shift, int mb, int me) {
    return rotl(value, shift) & ppc_mask(mb, me);
}
// Insert rotl(value, shift) into word under mask(mb, me).
constexpr uint32_t rlwimi(uint32_t word, uint32_t value, int shift, int mb, int me) {
    uint32_t mask = ppc_mask(mb, me);
    return (word & ~mask) | (rotl(value, shift) & mask);
}

// --- guest calls, stack, floats ---------------------------------------------------------------

// Call a guest function (translated or lifted) with integer and float arguments; returns r3.
inline uint32_t gcall(Cpu& c, uint32_t function, std::initializer_list<uint32_t> ints = {}, std::initializer_list<double> floats = {}) {
    int i = 3;
    for (uint32_t value : ints) {
        c.r[i++] = value;
    }
    i = 1;
    for (double value : floats) {
        c.f[i++] = value;
    }
    call(c, function);
    return c.r[3];
}

// Scratch space on the guest stack for data passed by pointer (data at base()).
class GuestFrame {
public:
    GuestFrame(Cpu& c, uint32_t size) : c_(c), saved_(c.r[1]) {
        c.r[1] = (saved_ - ((size + 0x10 + 15) & ~15u)) & ~15u;
        wr32(c.r[1], saved_);
    }
    ~GuestFrame() {
        c_.r[1] = saved_;
    }
    GuestFrame(const GuestFrame&) = delete;
    GuestFrame& operator=(const GuestFrame&) = delete;
    uint32_t base() const {
        return c_.r[1] + 0x10;
    }

private:
    Cpu& c_;
    uint32_t saved_;
};

// Call a guest function as the original's `bl` did: LR holds the original's return address
// (callees save it on the stack, where later code can find it).
inline uint32_t gcall_from(Cpu& c, uint32_t return_address, uint32_t function, std::initializer_list<uint32_t> ints = {}, std::initializer_list<double> floats = {}) {
    c.lr = return_address;
    return gcall(c, function, ints, floats);
}

// The original's own stack frame, written as its prologue, body and epilogue write it. Game code
// can read stale stack slots (uninitialized locals), so what a lifted function leaves below the
// stack pointer must match the original byte for byte; the verifier does not compare the stack.
class OriginalFrame {
public:
    enum Kind { kSavesLr, kLeaf };
    // stwu r1, -size(r1); and unless the function is a leaf: mflr r0; stw r0, size + 4(r1)
    OriginalFrame(Cpu& c, uint32_t size, Kind kind = kSavesLr) : c_(c), caller_(c.r[1]), base_(c.r[1] - size), kind_(kind) {
        wr32(base_, caller_);
        if (kind_ == kSavesLr) {
            wr32(caller_ + 4, c.lr);
        }
        c.r[1] = base_;
    }
    // lwz r0, size + 4(r1); mtlr r0; addi r1, r1, size
    ~OriginalFrame() {
        if (kind_ == kSavesLr) {
            c_.lr = rd32(caller_ + 4);
        }
        c_.r[1] = caller_;
    }
    OriginalFrame(const OriginalFrame&) = delete;
    OriginalFrame& operator=(const OriginalFrame&) = delete;
    uint32_t at(uint32_t offset) const {
        return base_ + offset;
    }
    void save_gpr(int reg, uint32_t offset) {
        wr32(base_ + offset, c_.r[reg]);
    }
    void restore_gpr(int reg, uint32_t offset) {
        c_.r[reg] = rd32(base_ + offset);
    }
    // stfd + psq_st: a nonvolatile FPR, both paired-single halves.
    void save_fpr(int reg, uint32_t offset, uint32_t pair_offset) {
        wrf64(base_ + offset, c_.f[reg]);
        uint32_t size = 0;
        store_quantized(c_, static_cast<uint32_t>(reg), base_ + pair_offset, 0, 0, size);
    }
    // psq_l + lfd
    void restore_fpr(int reg, uint32_t offset, uint32_t pair_offset) {
        uint32_t size = 0;
        load_quantized(c_, static_cast<uint32_t>(reg), base_ + pair_offset, 0, 0, size);
        c_.f[reg] = rdf64(base_ + offset);
    }
    // The compiler's int -> float conversion through a stack slot pair: {0x43300000, value ^ 0x80000000}.
    float int_to_float(uint32_t offset, int32_t value) {
        wr32(base_ + offset, 0x43300000);
        wr32(base_ + offset + 4, static_cast<uint32_t>(value) ^ 0x80000000u);
        return static_cast<float>(static_cast<double>(value));
    }

    // fctiwz + stfd + lwz: truncate to int through a stack slot (the high word reads 0xFFF80000).
    int32_t float_to_int(uint32_t offset, double value) {
        double converted = convert_to_int(value, true);
        wrf64(base_ + offset, converted);
        return static_cast<int32_t>(rd32(base_ + offset + 4));
    }

private:
    Cpu& c_;
    uint32_t caller_;
    uint32_t base_;
    Kind kind_;
};

inline float ldf(uint32_t address) {
    uint32_t bits = rd32(address);
    float value;
    std::memcpy(&value, &bits, 4);
    return value;
}
inline void stf(uint32_t address, float value) {
    uint32_t bits;
    std::memcpy(&bits, &value, 4);
    wr32(address, bits);
}
// A double argument rounded the way the guest holds a single.
inline double single(double value) {
    return static_cast<double>(static_cast<float>(value));
}

// --- single-precision arithmetic as the translated code does it -----------------------------
// Lifted float code must round exactly like the translated original (see recompiler/ppc/emit.py):
// single-precision ops compute in double and round once to single; multiplies first round their
// second operand to 25 bits (Gekko); fused ops use the Gekko-accurate madd_single.
namespace ppc {
inline float fadds(float a, float b) {
    return static_cast<float>(static_cast<double>(a) + static_cast<double>(b));
}
inline float fsubs(float a, float b) {
    return static_cast<float>(static_cast<double>(a) - static_cast<double>(b));
}
inline float fmuls(float a, float c) {
    return static_cast<float>(static_cast<double>(a) * force25(static_cast<double>(c)));
}
inline float fdivs(float a, float b) {
    return static_cast<float>(static_cast<double>(a) / static_cast<double>(b));
}
// a * c + b
inline float fmadds(float a, float c, float b) {
    return static_cast<float>(madd_single(a, c, b));
}
// b - a * c
inline float fnmsubs(float a, float c, float b) {
    return static_cast<float>(-madd_single(a, c, -static_cast<double>(b)));
}
// Integer to float as the compiler's magic-number sequence does it (exact for |value| < 2^24).
inline float from_int(int32_t value) {
    return static_cast<float>(static_cast<double>(value));
}
inline float from_unsigned(uint32_t value) {
    return static_cast<float>(static_cast<double>(value));
}
// fctiwz: truncate toward zero, saturating.
inline int32_t fctiwz(double value) {
    if (value != value) {
        return static_cast<int32_t>(0x80000000u);
    }
    if (value >= 2147483647.0) {
        return 0x7FFFFFFF;
    }
    if (value <= -2147483648.0) {
        return static_cast<int32_t>(0x80000000u);
    }
    return static_cast<int32_t>(value);
}
}  // namespace ppc

// --- typed guest data ----------------------------------------------------------------------------

// A big-endian value of type T at a guest address: reads and writes go through guest memory.
template <typename T>
class Field {
    static_assert(std::is_arithmetic_v<T> && (sizeof(T) == 1 || sizeof(T) == 2 || sizeof(T) == 4), "8/16/32-bit integers or float");

public:
    explicit Field(uint32_t address) : address_(address) {}
    T get() const {
        if constexpr (std::is_same_v<T, float>) {
            return ldf(address_);
        } else if constexpr (sizeof(T) == 1) {
            return static_cast<T>(rd8(address_));
        } else if constexpr (sizeof(T) == 2) {
            return static_cast<T>(rd16(address_));
        } else {
            return static_cast<T>(rd32(address_));
        }
    }
    void set(T value) const {
        if constexpr (std::is_same_v<T, float>) {
            stf(address_, value);
        } else if constexpr (sizeof(T) == 1) {
            wr8(address_, static_cast<uint8_t>(value));
        } else if constexpr (sizeof(T) == 2) {
            wr16(address_, static_cast<uint16_t>(value));
        } else {
            wr32(address_, static_cast<uint32_t>(value));
        }
    }
    operator T() const {
        return get();
    }
    const Field& operator=(T value) const {
        set(value);
        return *this;
    }
    // a.x() = b.y() copies the value (a field is a view of guest memory, not a pointer).
    Field(const Field&) = default;
    const Field& operator=(const Field& other) const {
        set(other.get());
        return *this;
    }
    uint32_t address() const {
        return address_;
    }

private:
    uint32_t address_;
};

// A view of a guest structure: struct Race : lift::View { WP_FIELD(int32_t, stamina, 0x194); };
// then Race{address}.stamina() reads and Race{address}.stamina() = 5 writes.
struct View {
    uint32_t base = 0;
    explicit operator bool() const {
        return base != 0;
    }
};
#define WP_FIELD(type, name, offset)                    \
    ::wp::lift::Field<type> name() const {               \
        return ::wp::lift::Field<type>(base + (offset)); \
    }

// --- lifted functions -----------------------------------------------------------------------------

enum class Ret { None, Int, Float, Blocking };

// A group of lifted functions switched together: setting key (e.g. "gx.native") and environment
// variable (e.g. "WP_NATIVE_GX"); on unless set to 0.
struct Group {
    const char* setting;
    const char* environment;
    bool enabled() const;
    // The answer for one settings revision: every call of a lifted function asks, and looking the
    // setting up each time (getenv walks the whole environment on Windows) cost half of the CPU thread.
    mutable std::atomic<uint64_t> cache{0};
};

// WP_LIFT_OFF="Name1,Name2": run these functions' translated originals (for bisecting).
bool switched_off(const char* name);

// True while the translated reference of a verified call runs (lifted functions defer to it).
bool in_reference();
// Counts the call; true when this call should be verified.
bool due(const char* name, Ret ret);
// Run original and lifted versions on the same input and compare. With commit=false (self-tests)
// all effects are rolled back afterwards.
void verify(Cpu& c, const char* name, void (*lifted)(Cpu&), Ret ret, bool commit = true);
// WP_VERIFY report: mismatch count and calls per function.
void print_report();
uint64_t mismatches();
// Self-tests touch only MEM1, the hardware window and this MEM2 scratch range (faster snapshots).
void use_self_test_ranges(bool enable, uint32_t scratch, uint32_t size);

}  // namespace wp::lift

// Declare a lifted guest function: WP_LIFT(group, handler, "GuestName", Ret::None) { body }.
// Engine code lists the handler in a table; game code uses WP_LIFTED, which registers it.
#define WP_LIFT(group, fn, name, ret) WP_LIFT_WHEN(group, fn, name, ret, true)

// WP_LIFT whose verification is limited to calls where `verifiable` (an expression over the
// guest state `c`) holds: calls that may wait for interrupts or I/O must not be verified, since
// the verifier holds them back.
#define WP_LIFT_WHEN(group, fn, name, ret, verifiable)                    \
    void fn##_lifted(::wp::Cpu& c);                                       \
    void fn(::wp::Cpu& c) {                                               \
        if (!(group).enabled() || ::wp::lift::in_reference() ||           \
            ::wp::lift::switched_off(name)) {                             \
            ::wp::call_original(c, name);                                 \
            return;                                                       \
        }                                                                 \
        if ((verifiable) && ::wp::lift::due(name, ret)) {                 \
            ::wp::lift::verify(c, name, fn##_lifted, ret);                \
            return;                                                       \
        }                                                                 \
        fn##_lifted(c);                                                   \
    }                                                                     \
    void fn##_lifted([[maybe_unused]] ::wp::Cpu& c)

// A lifted function in game code (port/games/<game>/src): defines and registers the handler.
// The guest function must also be listed in the game's hle_functions.csv.
#define WP_LIFTED(group, fn, name, ret)                                   \
    void fn(::wp::Cpu& c);                                                \
    static ::wp::HandlerRegistrar fn##_registrar{name, fn};               \
    WP_LIFT(group, fn, name, ret)

#define WP_LIFTED_WHEN(group, fn, name, ret, verifiable)                  \
    void fn(::wp::Cpu& c);                                                \
    static ::wp::HandlerRegistrar fn##_registrar{name, fn};               \
    WP_LIFT_WHEN(group, fn, name, ret, verifiable)
