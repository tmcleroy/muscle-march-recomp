#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>

#include "wp/memory.h"

namespace wp {

struct Cpu {
    uint32_t r[32];
    double f[32];
    double ps1[32];
    uint8_t cr[8];
    uint32_t lr;
    uint32_t ctr;
    uint8_t xer_so;
    uint8_t xer_ov;
    uint8_t xer_ca;
    uint8_t xer_byte_count;
    uint32_t fpscr;
    uint32_t msr;
    uint32_t sr[16];
    uint32_t spr[1024];
};

constexpr uint32_t kSprXer = 1;
constexpr uint32_t kSprLr = 8;
constexpr uint32_t kSprCtr = 9;
constexpr uint32_t kSprGqr0 = 912;

[[noreturn]] void missing_function(Cpu& c, uint32_t address);
[[noreturn]] void illegal_instruction(Cpu& c, uint32_t address, uint32_t word);
[[noreturn]] void unsupported_instruction(Cpu& c, uint32_t address, const char* name);
[[noreturn]] void unresolved_jump(Cpu& c, uint32_t target);
[[noreturn]] void fatal_error(const char* message);
void call(Cpu& c, uint32_t address);
void trap(Cpu& c, uint32_t address);
void system_call(Cpu& c);
uint64_t time_base();
void locked_cache_dma(Cpu& c);
void set_decrementer(Cpu& c, uint32_t value);
uint32_t get_decrementer(Cpu& c);
bool decrementer_due();
void decrementer_fired();

void print_call_stack();
void start_profiler();
void print_profile();

extern uint32_t g_poll_counter;
void poll_interrupts(Cpu& c);

#define WP_POLL(c)                                          \
    do {                                                    \
        if (--::wp::g_poll_counter == 0) {                  \
            ::wp::poll_interrupts(c);                       \
        }                                                   \
    } while (0)

#ifdef WP_TRACE
constexpr size_t kCallStackSize = 16384;
extern uint32_t* g_call_stack;
extern volatile size_t g_call_depth;

struct TraceScope {
    explicit TraceScope(uint32_t address) : depth(g_call_depth) {
        if (depth < kCallStackSize) {
            g_call_stack[depth] = address;
        }
        g_call_depth = depth + 1;
    }

    ~TraceScope() {
        g_call_depth = depth;
    }

    size_t depth;
};

#define WP_ENTER(address) ::wp::TraceScope wp_trace_scope(address)
#else
#define WP_ENTER(address) ((void)0)
#endif

inline uint32_t rotl(uint32_t value, uint32_t amount) {
    amount &= 31;
    return amount ? (value << amount) | (value >> (32 - amount)) : value;
}

inline void record(Cpu& c, uint32_t value) {
    int32_t s = static_cast<int32_t>(value);
    c.cr[0] = static_cast<uint8_t>((s < 0 ? 8 : s > 0 ? 4 : 2) | c.xer_so);
}

inline bool cr_get(const Cpu& c, uint32_t bit) {
    return (c.cr[bit >> 2] >> (3 - (bit & 3))) & 1;
}

inline void cr_set(Cpu& c, uint32_t bit, bool value) {
    uint8_t mask = static_cast<uint8_t>(1u << (3 - (bit & 3)));
    if (value) {
        c.cr[bit >> 2] |= mask;
    } else {
        c.cr[bit >> 2] &= static_cast<uint8_t>(~mask);
    }
}

inline uint32_t mfcr(const Cpu& c) {
    uint32_t value = 0;
    for (int i = 0; i < 8; i++) {
        value = (value << 4) | (c.cr[i] & 0xF);
    }
    return value;
}

inline void mtcrf(Cpu& c, uint32_t mask, uint32_t value) {
    for (int i = 0; i < 8; i++) {
        if (mask & (0x80u >> i)) {
            c.cr[i] = static_cast<uint8_t>((value >> (28 - 4 * i)) & 0xF);
        }
    }
}

inline uint32_t mfxer(const Cpu& c) {
    return (static_cast<uint32_t>(c.xer_so) << 31) | (static_cast<uint32_t>(c.xer_ov) << 30) |
           (static_cast<uint32_t>(c.xer_ca) << 29) | (c.xer_byte_count & 0x7F);
}

inline void mtxer(Cpu& c, uint32_t value) {
    c.xer_so = (value >> 31) & 1;
    c.xer_ov = (value >> 30) & 1;
    c.xer_ca = (value >> 29) & 1;
    c.xer_byte_count = value & 0x7F;
}

inline void compare_signed(Cpu& c, uint32_t field, uint32_t a, uint32_t b) {
    int32_t x = static_cast<int32_t>(a);
    int32_t y = static_cast<int32_t>(b);
    c.cr[field] = static_cast<uint8_t>((x < y ? 8 : x > y ? 4 : 2) | c.xer_so);
}

inline void compare_unsigned(Cpu& c, uint32_t field, uint32_t a, uint32_t b) {
    c.cr[field] = static_cast<uint8_t>((a < b ? 8 : a > b ? 4 : 2) | c.xer_so);
}

inline void compare_float(Cpu& c, uint32_t field, double a, double b) {
    uint8_t bits = std::isnan(a) || std::isnan(b) ? 1 : a < b ? 8 : a > b ? 4 : 2;
    c.cr[field] = bits;
    c.fpscr = (c.fpscr & ~0xF000u) | (static_cast<uint32_t>(bits) << 12);
}

inline uint32_t add_carry(Cpu& c, uint32_t a, uint32_t b, uint32_t carry) {
    uint64_t sum = static_cast<uint64_t>(a) + b + carry;
    c.xer_ca = static_cast<uint8_t>(sum >> 32);
    return static_cast<uint32_t>(sum);
}

inline uint32_t add_extended(Cpu& c, uint32_t a, uint32_t b) {
    return add_carry(c, a, b, c.xer_ca);
}

inline uint32_t sub_carry(Cpu& c, uint32_t a, uint32_t b) {
    return add_carry(c, ~a, b, 1);
}

inline uint32_t sub_extended(Cpu& c, uint32_t a, uint32_t b) {
    return add_carry(c, ~a, b, c.xer_ca);
}

inline uint32_t mulhw(uint32_t a, uint32_t b) {
    return static_cast<uint32_t>((static_cast<int64_t>(static_cast<int32_t>(a)) * static_cast<int32_t>(b)) >> 32);
}

inline uint32_t mulhwu(uint32_t a, uint32_t b) {
    return static_cast<uint32_t>((static_cast<uint64_t>(a) * b) >> 32);
}

inline uint32_t divw(uint32_t a, uint32_t b) {
    int32_t x = static_cast<int32_t>(a);
    int32_t y = static_cast<int32_t>(b);
    if (y == 0 || (x == INT32_MIN && y == -1)) {
        return x < 0 ? 0xFFFFFFFFu : 0;
    }
    return static_cast<uint32_t>(x / y);
}

inline uint32_t divwu(uint32_t a, uint32_t b) {
    return b == 0 ? 0 : a / b;
}

inline uint32_t cntlzw(uint32_t value) {
    return value == 0 ? 32 : static_cast<uint32_t>(__builtin_clz(value));
}

inline uint32_t slw(uint32_t value, uint32_t amount) {
    amount &= 0x3F;
    return amount > 31 ? 0 : value << amount;
}

inline uint32_t srw(uint32_t value, uint32_t amount) {
    amount &= 0x3F;
    return amount > 31 ? 0 : value >> amount;
}

inline uint32_t sraw(Cpu& c, uint32_t value, uint32_t amount) {
    amount &= 0x3F;
    int32_t s = static_cast<int32_t>(value);
    if (amount > 31) {
        c.xer_ca = s < 0;
        return static_cast<uint32_t>(s >> 31);
    }
    c.xer_ca = s < 0 && (value & ((1u << amount) - 1)) != 0;
    return static_cast<uint32_t>(s >> amount);
}

inline uint32_t sign_extend8(uint32_t value) {
    return static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(value)));
}

inline uint32_t sign_extend16(uint32_t value) {
    return static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(value)));
}

inline double round_single(double value) {
    return static_cast<double>(static_cast<float>(value));
}

inline uint64_t fpr_bits(double value) {
    uint64_t bits;
    std::memcpy(&bits, &value, sizeof bits);
    return bits;
}

inline double fpr_from_bits(uint64_t bits) {
    double value;
    std::memcpy(&value, &bits, sizeof value);
    return value;
}

inline double convert_to_int(double value, bool truncate) {
    int32_t result;
    if (std::isnan(value)) {
        result = INT32_MIN;
    } else {
        double rounded = truncate ? std::trunc(value) : std::nearbyint(value);
        result = rounded >= 2147483647.0    ? INT32_MAX
                 : rounded <= -2147483648.0 ? INT32_MIN
                                            : static_cast<int32_t>(rounded);
    }
    return fpr_from_bits(0xFFF8000000000000ull | static_cast<uint32_t>(result));
}

inline double fsel(double a, double b, double c) {
    return a >= 0.0 ? c : b;
}

extern bool g_hardware_fma;
double hardware_fma(double a, double b, double c);

inline void two_sum(double a, double b, double& sum, double& error) {
    sum = a + b;
    double b_part = sum - a;
    error = (a - (sum - b_part)) + (b - b_part);
}

inline void split(double value, double& high, double& low) {
    double scaled = value * 134217729.0;
    high = scaled - (scaled - value);
    low = value - high;
}

inline double software_fma(double a, double b, double c) {
    double product = a * b;
    if (!std::isfinite(product) || !std::isfinite(c) || a == 0.0 || b == 0.0 || std::fabs(a) > 0x1p995 || std::fabs(b) > 0x1p995 ||
        std::fabs(product) < 0x1p-900) {
        return product + c;
    }
    double a_high, a_low, b_high, b_low;
    split(a, a_high, a_low);
    split(b, b_high, b_low);
    double product_error = ((a_high * b_high - product) + a_high * b_low + a_low * b_high) + a_low * b_low;
    double high, low;
    two_sum(c, product, high, low);
    double sum, error;
    two_sum(low, product_error, sum, error);
    if (error != 0.0) {
        if ((error > 0.0) != (sum > 0.0)) {
            sum = std::nextafter(sum, 0.0);
        }
        uint64_t bits;
        std::memcpy(&bits, &sum, sizeof bits);
        bits |= 1;
        std::memcpy(&sum, &bits, sizeof bits);
    }
    return high + sum;
}

inline double fused_multiply_add(double a, double b, double c) {
    return g_hardware_fma ? hardware_fma(a, b, c) : software_fma(a, b, c);
}

inline void set_overflow(Cpu& c, bool overflow) {
    c.xer_ov = overflow ? 1 : 0;
    c.xer_so |= c.xer_ov;
}

inline bool multiply_overflows(uint32_t a, uint32_t b) {
    int64_t product = static_cast<int64_t>(static_cast<int32_t>(a)) * static_cast<int32_t>(b);
    return product != static_cast<int32_t>(product);
}

struct EstimateEntry {
    int32_t base;
    int32_t decrement;
};

inline constexpr EstimateEntry kReciprocalSqrtTable[32] = {
    {0x1a7e800, -0x568}, {0x17cb800, -0x4f3}, {0x1552800, -0x48d}, {0x130c000, -0x435}, {0x10f2000, -0x3e7}, {0x0eff000, -0x3a2},
    {0x0d2e000, -0x365}, {0x0b7c000, -0x32e}, {0x09e5000, -0x2fc}, {0x0867000, -0x2d0}, {0x06ff000, -0x2a8}, {0x05ab800, -0x283},
    {0x046a000, -0x261}, {0x0339800, -0x243}, {0x0218800, -0x226}, {0x0105800, -0x20b}, {0x3ffa000, -0x7a4}, {0x3c29000, -0x700},
    {0x38aa000, -0x670}, {0x3572000, -0x5f2}, {0x3279000, -0x584}, {0x2fb7000, -0x524}, {0x2d26000, -0x4cc}, {0x2ac0000, -0x47e},
    {0x2881000, -0x43a}, {0x2665000, -0x3fa}, {0x2468000, -0x3c2}, {0x2287000, -0x38e}, {0x20c1000, -0x35e}, {0x1f12000, -0x332},
    {0x1d79000, -0x30a}, {0x1bf4000, -0x2e6},
};

inline constexpr EstimateEntry kReciprocalTable[32] = {
    {0x7ff800, 0x3e1}, {0x783800, 0x3a7}, {0x70ea00, 0x371}, {0x6a0800, 0x340}, {0x638800, 0x313}, {0x5d6200, 0x2ea},
    {0x579000, 0x2c4}, {0x520800, 0x2a0}, {0x4cc800, 0x27f}, {0x47ca00, 0x261}, {0x430800, 0x245}, {0x3e8000, 0x22a},
    {0x3a2c00, 0x212}, {0x360800, 0x1fb}, {0x321400, 0x1e5}, {0x2e4a00, 0x1d1}, {0x2aa800, 0x1be}, {0x272c00, 0x1ac},
    {0x23d600, 0x19b}, {0x209e00, 0x18b}, {0x1d8800, 0x17c}, {0x1a9000, 0x16e}, {0x17ae00, 0x15b}, {0x14f800, 0x15b},
    {0x124400, 0x143}, {0x0fbe00, 0x143}, {0x0d3800, 0x12d}, {0x0ade00, 0x12d}, {0x088400, 0x11a}, {0x065000, 0x11a},
    {0x041c00, 0x108}, {0x020c00, 0x106},
};

inline double quiet_nan_of(double value) {
    return fpr_from_bits(fpr_bits(value) | 0x0008000000000000ull);
}

inline double reciprocal_estimate(double value) {
    int64_t integral = static_cast<int64_t>(fpr_bits(value));
    int64_t mantissa = integral & ((1LL << 52) - 1);
    int64_t sign = integral & static_cast<int64_t>(1ULL << 63);
    int64_t exponent = integral & (0x7FFLL << 52);
    if (mantissa == 0 && exponent == 0) {
        return std::copysign(HUGE_VAL, value);
    }
    if (exponent == (0x7FFLL << 52)) {
        return mantissa == 0 ? std::copysign(0.0, value) : quiet_nan_of(value);
    }
    if (exponent < (895LL << 52)) {
        return std::copysign(3.4028234663852886e38, value);
    }
    if (exponent >= (1149LL << 52)) {
        return std::copysign(0.0, value);
    }
    exponent = (0x7FDLL << 52) - exponent;
    int index = static_cast<int>(mantissa >> 37);
    const EstimateEntry& entry = kReciprocalTable[index / 1024];
    integral = sign | exponent;
    integral |= static_cast<int64_t>(entry.base - (entry.decrement * (index % 1024) + 1) / 2) << 29;
    return fpr_from_bits(static_cast<uint64_t>(integral));
}

inline double reciprocal_sqrt_estimate(double value) {
    int64_t integral = static_cast<int64_t>(fpr_bits(value));
    int64_t mantissa = integral & ((1LL << 52) - 1);
    int64_t sign = integral & static_cast<int64_t>(1ULL << 63);
    int64_t exponent = integral & (0x7FFLL << 52);
    if (mantissa == 0 && exponent == 0) {
        return sign ? -HUGE_VAL : HUGE_VAL;
    }
    if (exponent == (0x7FFLL << 52)) {
        if (mantissa == 0) {
            return sign ? std::nan("") : 0.0;
        }
        return quiet_nan_of(value);
    }
    if (sign) {
        return std::nan("");
    }
    if (!exponent) {
        do {
            exponent -= 1LL << 52;
            mantissa <<= 1;
        } while (!(mantissa & (1LL << 52)));
        mantissa &= (1LL << 52) - 1;
        exponent += 1LL << 52;
    }
    int64_t exponent_lsb = exponent & (1LL << 52);
    exponent = ((0x3FFLL << 52) - ((exponent - (0x3FELL << 52)) / 2)) & (0x7FFLL << 52);
    integral = sign | exponent;
    int index = static_cast<int>((exponent_lsb | mantissa) >> 37);
    const EstimateEntry& entry = kReciprocalSqrtTable[index / 2048];
    integral |= static_cast<int64_t>(entry.base + entry.decrement * (index % 2048)) << 26;
    return fpr_from_bits(static_cast<uint64_t>(integral));
}

inline double force25(double value) {
    uint64_t integral = fpr_bits(value);
    uint64_t exponent = integral & 0x7FF0000000000000ull;
    uint64_t fraction = integral & 0x000FFFFFFFFFFFFFull;
    if (exponent == 0 && fraction != 0) {
        int64_t keep = static_cast<int64_t>(0xFFFFFFFFF8000000ull);
        uint64_t round = 0x8000000;
        int shift = __builtin_clzll(fraction) - 11;
        keep >>= shift;
        round >>= shift;
        integral = (integral & static_cast<uint64_t>(keep)) + (integral & round);
    } else {
        integral = (integral & 0xFFFFFFFFF8000000ull) + (integral & 0x8000000);
    }
    return fpr_from_bits(integral);
}

inline double madd_single(double a, double c, double b) {
    double rounded = force25(c);
    double result = fused_multiply_add(a, rounded, b);
    uint64_t bits = fpr_bits(result);
    if ((bits & 0x1FFFFFFFull) == 0x10000000ull) {
        double a_prime = b - result;
        double b_prime = result + a_prime;
        double delta_a = fused_multiply_add(a, rounded, a_prime);
        double delta_b = b - b_prime;
        double error = delta_a + delta_b;
        if (error != 0.0) {
            result = fpr_from_bits((error > 0.0) == (result > 0.0) ? bits + 1 : bits - 1);
        }
    }
    return result;
}

inline void update_cr1(Cpu& c) {
    c.cr[1] = static_cast<uint8_t>((c.fpscr >> 28) & 0xF);
}

inline int quantizer_scale(uint32_t bits) {
    int scale = static_cast<int>(bits & 0x3F);
    return scale & 0x20 ? scale - 0x40 : scale;
}

inline float dequantize(uint32_t address, uint32_t type, uint32_t scale_bits, uint32_t& size) {
    float factor = std::ldexp(1.0f, -quantizer_scale(scale_bits));
    switch (type) {
    case 4:
        size = 1;
        return static_cast<float>(rd8(address)) * factor;
    case 5:
        size = 2;
        return static_cast<float>(rd16(address)) * factor;
    case 6:
        size = 1;
        return static_cast<float>(static_cast<int8_t>(rd8(address))) * factor;
    case 7:
        size = 2;
        return static_cast<float>(static_cast<int16_t>(rd16(address))) * factor;
    default:
        size = 4;
        return rdf32(address);
    }
}

inline uint32_t clamp_quantized(float value, uint32_t type) {
    long lo = 0;
    long hi = 0;
    switch (type) {
    case 4:
        lo = 0;
        hi = 255;
        break;
    case 5:
        lo = 0;
        hi = 65535;
        break;
    case 6:
        lo = -128;
        hi = 127;
        break;
    default:
        lo = -32768;
        hi = 32767;
        break;
    }
    if (std::isnan(value) || value <= static_cast<float>(lo)) {
        return static_cast<uint32_t>(lo);
    }
    if (value >= static_cast<float>(hi)) {
        return static_cast<uint32_t>(hi);
    }
    return static_cast<uint32_t>(static_cast<long>(value));
}

inline void quantize(uint32_t address, float value, uint32_t type, uint32_t scale_bits, uint32_t& size) {
    float scaled = value * std::ldexp(1.0f, quantizer_scale(scale_bits));
    switch (type) {
    case 4:
    case 6:
        size = 1;
        wr8(address, static_cast<uint8_t>(clamp_quantized(scaled, type)));
        break;
    case 5:
    case 7:
        size = 2;
        wr16(address, static_cast<uint16_t>(clamp_quantized(scaled, type)));
        break;
    default:
        size = 4;
        wrf32(address, value);
        break;
    }
}

inline void load_quantized(Cpu& c, uint32_t d, uint32_t address, uint32_t w, uint32_t index, uint32_t& size) {
    uint32_t gqr = c.spr[kSprGqr0 + index];
    uint32_t type = (gqr >> 16) & 7;
    uint32_t scale = (gqr >> 24) & 0x3F;
    uint32_t first = 0;
    c.f[d] = dequantize(address, type, scale, first);
    if (w) {
        c.ps1[d] = 1.0;
        size = first;
    } else {
        uint32_t second = 0;
        c.ps1[d] = dequantize(address + first, type, scale, second);
        size = first + second;
    }
}

inline void store_quantized(Cpu& c, uint32_t s, uint32_t address, uint32_t w, uint32_t index, uint32_t& size) {
    uint32_t gqr = c.spr[kSprGqr0 + index];
    uint32_t type = gqr & 7;
    uint32_t scale = (gqr >> 8) & 0x3F;
    uint32_t first = 0;
    quantize(address, static_cast<float>(c.f[s]), type, scale, first);
    if (w) {
        size = first;
    } else {
        uint32_t second = 0;
        quantize(address == kFifoAddress ? address : address + first, static_cast<float>(c.ps1[s]), type, scale, second);
        size = first + second;
    }
}

inline void ps_set(Cpu& c, int d, double x, double y) {
    c.f[d] = round_single(x);
    c.ps1[d] = round_single(y);
}

inline void ps_add(Cpu& c, int d, int a, int b) {
    ps_set(c, d, c.f[a] + c.f[b], c.ps1[a] + c.ps1[b]);
}

inline void ps_sub(Cpu& c, int d, int a, int b) {
    ps_set(c, d, c.f[a] - c.f[b], c.ps1[a] - c.ps1[b]);
}

inline void ps_mul(Cpu& c, int d, int a, int f) {
    ps_set(c, d, c.f[a] * force25(c.f[f]), c.ps1[a] * force25(c.ps1[f]));
}

inline void ps_div(Cpu& c, int d, int a, int b) {
    ps_set(c, d, c.f[a] / c.f[b], c.ps1[a] / c.ps1[b]);
}

inline void ps_muls0(Cpu& c, int d, int a, int f) {
    ps_set(c, d, c.f[a] * force25(c.f[f]), c.ps1[a] * force25(c.f[f]));
}

inline void ps_muls1(Cpu& c, int d, int a, int f) {
    ps_set(c, d, c.f[a] * force25(c.ps1[f]), c.ps1[a] * force25(c.ps1[f]));
}

inline void ps_madd(Cpu& c, int d, int a, int f, int b, double product_sign, double addend_sign) {
    ps_set(c, d, product_sign * madd_single(c.f[a], c.f[f], c.f[b] * addend_sign * product_sign),
           product_sign * madd_single(c.ps1[a], c.ps1[f], c.ps1[b] * addend_sign * product_sign));
}

inline void ps_madds0(Cpu& c, int d, int a, int f, int b) {
    ps_set(c, d, madd_single(c.f[a], c.f[f], c.f[b]), madd_single(c.ps1[a], c.f[f], c.ps1[b]));
}

inline void ps_madds1(Cpu& c, int d, int a, int f, int b) {
    ps_set(c, d, madd_single(c.f[a], c.ps1[f], c.f[b]), madd_single(c.ps1[a], c.ps1[f], c.ps1[b]));
}

inline void ps_sum0(Cpu& c, int d, int a, int f, int b) {
    ps_set(c, d, c.f[a] + c.ps1[b], c.ps1[f]);
}

inline void ps_sum1(Cpu& c, int d, int a, int f, int b) {
    ps_set(c, d, c.f[f], c.f[a] + c.ps1[b]);
}

inline void ps_sel(Cpu& c, int d, int a, int f, int b) {
    double x = c.f[a] >= 0.0 ? c.f[f] : c.f[b];
    double y = c.ps1[a] >= 0.0 ? c.ps1[f] : c.ps1[b];
    c.f[d] = x;
    c.ps1[d] = y;
}

inline void ps_res(Cpu& c, int d, int b) {
    ps_set(c, d, reciprocal_estimate(c.f[b]), reciprocal_estimate(c.ps1[b]));
}

inline void ps_rsqrte(Cpu& c, int d, int b) {
    ps_set(c, d, reciprocal_sqrt_estimate(c.f[b]), reciprocal_sqrt_estimate(c.ps1[b]));
}

inline void ps_merge(Cpu& c, int d, int a, int b, bool high_a, bool high_b) {
    double x = high_a ? c.ps1[a] : c.f[a];
    double y = high_b ? c.ps1[b] : c.f[b];
    c.f[d] = x;
    c.ps1[d] = y;
}

inline void ps_mr(Cpu& c, int d, int b) {
    double x = c.f[b];
    double y = c.ps1[b];
    c.f[d] = x;
    c.ps1[d] = y;
}

inline void ps_neg(Cpu& c, int d, int b) {
    double x = -c.f[b];
    double y = -c.ps1[b];
    c.f[d] = x;
    c.ps1[d] = y;
}

inline void ps_abs(Cpu& c, int d, int b) {
    double x = std::fabs(c.f[b]);
    double y = std::fabs(c.ps1[b]);
    c.f[d] = x;
    c.ps1[d] = y;
}

inline void ps_nabs(Cpu& c, int d, int b) {
    double x = -std::fabs(c.f[b]);
    double y = -std::fabs(c.ps1[b]);
    c.f[d] = x;
    c.ps1[d] = y;
}

}
