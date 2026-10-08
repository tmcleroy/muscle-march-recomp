// Copyright 2008 Dolphin Emulator Project
// Copyright 2004 Duddie & Tratax
// Copyright 2005 Duddie
// Copyright 2009 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "Core/DSP/DSPCore.h"

#define WP_DSP_INLINE [[gnu::always_inline]] inline

namespace wp::dsp::ops {

using DSP::SDSP;

WP_DSP_INLINE s64 acc(const SDSP& s, int reg) {
    return static_cast<s64>(s.r.ac[reg].val);
}

WP_DSP_INLINE void set_acc(SDSP& s, int reg, s64 value) {
    s.r.ac[reg].val = static_cast<u64>((value << 24) >> 24);
}

WP_DSP_INLINE s64 acx(const SDSP& s, int reg) {
    return static_cast<s32>(s.r.ax[reg].val);
}

WP_DSP_INLINE s64 prod(const SDSP& s) {
    s64 high = static_cast<s64>(static_cast<s8>(static_cast<u8>(s.r.prod.h))) << 32;
    s64 low = ((static_cast<s64>(s.r.prod.m) + s.r.prod.m2) << 16) | s.r.prod.l;
    return high + low;
}

WP_DSP_INLINE void set_prod(SDSP& s, s64 value) {
    s.r.prod.val = static_cast<u64>(value & 0x000000FFFFFFFFFFULL);
}

WP_DSP_INLINE s64 multiply(const SDSP& s, u16 a, u16 b, int sign) {
    s64 product;
    if (sign == 1 && (s.r.sr & DSP::SR_MUL_UNSIGNED)) {
        product = static_cast<u32>(a) * b;
    } else if (sign == 2 && (s.r.sr & DSP::SR_MUL_UNSIGNED)) {
        product = a * static_cast<s16>(b);
    } else {
        product = static_cast<s16>(a) * static_cast<s16>(b);
    }
    if (!(s.r.sr & DSP::SR_MUL_MODIFY)) {
        product <<= 1;
    }
    return product;
}

WP_DSP_INLINE s64 multiply_mulx(const SDSP& s, int axh0, int axh1, u16 val1, u16 val2) {
    if (axh0 == 0 && axh1 == 0) {
        return multiply(s, val1, val2, 1);
    }
    if (axh0 == 0 && axh1 == 1) {
        return multiply(s, val1, val2, 2);
    }
    if (axh0 == 1 && axh1 == 0) {
        return multiply(s, val2, val1, 2);
    }
    return multiply(s, val1, val2, 0);
}

WP_DSP_INLINE u16 cmp_bits64(s64 value, bool carry, bool overflow) {
    u16 bits = static_cast<u16>(carry);
    bits |= static_cast<u16>(overflow) << 1;
    bits |= static_cast<u16>(value == 0) << 2;
    bits |= static_cast<u16>(value < 0) << 3;
    bits |= static_cast<u16>(value != static_cast<s32>(value)) << 4;
    u64 top = static_cast<u64>(value) & 0xc0000000;
    bits |= static_cast<u16>(top == 0 || top == 0xc0000000) << 5;
    return bits;
}

WP_DSP_INLINE bool carry_add(s64 val1, s64 result) {
    return static_cast<u64>(val1) > static_cast<u64>(result);
}

WP_DSP_INLINE bool overflow_add(s64 val1, s64 val2, s64 result) {
    return ((val1 ^ result) & (val2 ^ result)) < 0;
}

WP_DSP_INLINE bool carry_sub(s64 val1, s64 result) {
    return static_cast<u64>(val1) >= static_cast<u64>(result);
}

WP_DSP_INLINE bool overflow_sub(s64 val1, s64 val2, s64 result) {
    return ((val1 ^ result) & (-val2 ^ result)) < 0;
}

WP_DSP_INLINE void sticky(SDSP& s, bool overflow) {
    s.r.sr = static_cast<u16>(s.r.sr | (static_cast<u16>(overflow) * DSP::SR_OVERFLOW_STICKY));
}

WP_DSP_INLINE void set_cmp_bits(SDSP& s, s64 value, bool carry, bool overflow) {
    s.r.sr = static_cast<u16>((s.r.sr & ~DSP::SR_CMP_MASK) | cmp_bits64(value, carry, overflow));
}

WP_DSP_INLINE void flags64(SDSP& s, s64 value, bool carry = false, bool overflow = false) {
    set_cmp_bits(s, value, carry, overflow);
    sticky(s, overflow);
}

WP_DSP_INLINE void flags64_add(SDSP& s, s64 val1, s64 val2, s64 result) {
    flags64(s, result, carry_add(val1, result), overflow_add(val1, val2, result));
}

WP_DSP_INLINE void flags64_sub(SDSP& s, s64 val1, s64 val2, s64 result) {
    flags64(s, result, carry_sub(val1, result), overflow_sub(val1, val2, result));
}

WP_DSP_INLINE void flags16(SDSP& s, s16 value, bool carry, bool overflow, bool over_s32) {
    u16 bits = static_cast<u16>(carry);
    bits |= static_cast<u16>(overflow) * (DSP::SR_OVERFLOW | DSP::SR_OVERFLOW_STICKY);
    bits |= static_cast<u16>(value == 0) << 2;
    bits |= static_cast<u16>(value < 0) << 3;
    bits |= static_cast<u16>(over_s32) << 4;
    u16 top = static_cast<u16>(value) >> 14;
    bits |= static_cast<u16>(top == 0 || top == 3) << 5;
    s.r.sr = static_cast<u16>((s.r.sr & ~DSP::SR_CMP_MASK) | bits);
}

WP_DSP_INLINE void logic_zero(SDSP& s, bool value) {
    s.r.sr = static_cast<u16>(value ? (s.r.sr | DSP::SR_LOGIC_ZERO) : (s.r.sr & ~DSP::SR_LOGIC_ZERO));
}

WP_DSP_INLINE bool condition(const SDSP& s, int code) {
    u16 sr = s.r.sr;
    bool carry = (sr & DSP::SR_CARRY) != 0;
    bool overflow = (sr & DSP::SR_OVERFLOW) != 0;
    bool over32 = (sr & DSP::SR_OVER_S32) != 0;
    bool less = overflow != ((sr & DSP::SR_SIGN) != 0);
    bool zero = (sr & DSP::SR_ARITH_ZERO) != 0;
    bool logic = (sr & DSP::SR_LOGIC_ZERO) != 0;
    bool b = !(over32 || (sr & DSP::SR_TOP2BITS) != 0) || zero;
    switch (code & 0xf) {
    case 0x0:
        return !less;
    case 0x1:
        return less;
    case 0x2:
        return !less && !zero;
    case 0x3:
        return less || zero;
    case 0x4:
        return !zero;
    case 0x5:
        return zero;
    case 0x6:
        return !carry;
    case 0x7:
        return carry;
    case 0x8:
        return !over32;
    case 0x9:
        return over32;
    case 0xa:
        return !b;
    case 0xb:
        return b;
    case 0xc:
        return !logic;
    case 0xd:
        return logic;
    case 0xe:
        return overflow;
    default:
        return true;
    }
}

WP_DSP_INLINE bool over_s32(s64 value) {
    return value != static_cast<s32>(value);
}

WP_DSP_INLINE u16 increment_ar(const SDSP& s, int reg) {
    u32 ar = s.r.ar[reg];
    u32 wr = s.r.wr[reg];
    u32 nar = ar + 1;
    if ((nar ^ ar) > ((wr | 1) << 1)) {
        nar -= wr + 1;
    }
    return static_cast<u16>(nar);
}

WP_DSP_INLINE u16 decrement_ar(const SDSP& s, int reg) {
    u32 ar = s.r.ar[reg];
    u32 wr = s.r.wr[reg];
    u32 nar = ar + wr;
    if (((nar ^ ar) & ((wr | 1) << 1)) > wr) {
        nar -= wr + 1;
    }
    return static_cast<u16>(nar);
}

WP_DSP_INLINE u16 increase_ar(const SDSP& s, int reg, s16 ix_value) {
    u32 ar = s.r.ar[reg];
    u32 wr = s.r.wr[reg];
    s32 ix = ix_value;
    u32 mx = (wr | 1) << 1;
    u32 nar = ar + ix;
    u32 dar = (nar ^ ar ^ ix) & mx;
    if (ix >= 0) {
        if (dar > wr) {
            nar -= wr + 1;
        }
    } else if ((((nar + wr + 1) ^ nar) & dar) <= wr) {
        nar += wr + 1;
    }
    return static_cast<u16>(nar);
}

WP_DSP_INLINE u16 read_dmem(SDSP& s, u16 address) {
    if ((address >> 12) == 0) {
        return s.dram[address & DSP::DSP_DRAM_MASK];
    }
    return s.ReadDMEM(address);
}

WP_DSP_INLINE void write_dmem(SDSP& s, u16 address, u16 value) {
    if ((address >> 12) == 0) {
        s.dram[address & DSP::DSP_DRAM_MASK] = value;
    } else {
        s.WriteDMEM(address, value);
    }
}

WP_DSP_INLINE u16 read_imem(const SDSP& s, u16 address) {
    switch (address >> 12) {
    case 0:
        return s.iram[address & DSP::DSP_IRAM_MASK];
    case 8:
        return s.irom[address & DSP::DSP_IROM_MASK];
    default:
        return s.ReadIMEM(address);
    }
}

WP_DSP_INLINE u16 read_reg(SDSP& s, int reg) {
    switch (reg & 0x1f) {
    case DSP::DSP_REG_ST0:
    case DSP::DSP_REG_ST1:
    case DSP::DSP_REG_ST2:
    case DSP::DSP_REG_ST3:
        return s.PopStack(static_cast<DSP::StackRegister>((reg & 0x1f) - DSP::DSP_REG_ST0));
    case DSP::DSP_REG_AR0:
    case DSP::DSP_REG_AR1:
    case DSP::DSP_REG_AR2:
    case DSP::DSP_REG_AR3:
        return s.r.ar[reg & 3];
    case DSP::DSP_REG_IX0:
    case DSP::DSP_REG_IX1:
    case DSP::DSP_REG_IX2:
    case DSP::DSP_REG_IX3:
        return s.r.ix[reg & 3];
    case DSP::DSP_REG_WR0:
    case DSP::DSP_REG_WR1:
    case DSP::DSP_REG_WR2:
    case DSP::DSP_REG_WR3:
        return s.r.wr[reg & 3];
    case DSP::DSP_REG_ACH0:
    case DSP::DSP_REG_ACH1:
        return static_cast<u16>(s.r.ac[reg & 1].h);
    case DSP::DSP_REG_CR:
        return s.r.cr;
    case DSP::DSP_REG_SR:
        return s.r.sr;
    case DSP::DSP_REG_PRODL:
        return s.r.prod.l;
    case DSP::DSP_REG_PRODM:
        return s.r.prod.m;
    case DSP::DSP_REG_PRODH:
        return s.r.prod.h;
    case DSP::DSP_REG_PRODM2:
        return s.r.prod.m2;
    case DSP::DSP_REG_AXL0:
    case DSP::DSP_REG_AXL1:
        return s.r.ax[reg & 1].l;
    case DSP::DSP_REG_AXH0:
    case DSP::DSP_REG_AXH1:
        return s.r.ax[reg & 1].h;
    case DSP::DSP_REG_ACL0:
    case DSP::DSP_REG_ACL1:
        return s.r.ac[reg & 1].l;
    case DSP::DSP_REG_ACM0:
    case DSP::DSP_REG_ACM1: {
        if (s.r.sr & DSP::SR_40_MODE_BIT) {
            s64 value = acc(s, reg & 1);
            if (value != static_cast<s32>(value)) {
                return value > 0 ? 0x7fff : 0x8000;
            }
        }
        return s.r.ac[reg & 1].m;
    }
    default:
        return 0;
    }
}

WP_DSP_INLINE void write_reg(SDSP& s, int reg, u16 value) {
    switch (reg & 0x1f) {
    case DSP::DSP_REG_ACH0:
    case DSP::DSP_REG_ACH1:
        s.r.ac[reg & 1].h = static_cast<u32>(static_cast<s32>(static_cast<s8>(value)));
        break;
    case DSP::DSP_REG_ST0:
    case DSP::DSP_REG_ST1:
    case DSP::DSP_REG_ST2:
    case DSP::DSP_REG_ST3:
        s.StoreStack(static_cast<DSP::StackRegister>((reg & 0x1f) - DSP::DSP_REG_ST0), value);
        break;
    case DSP::DSP_REG_AR0:
    case DSP::DSP_REG_AR1:
    case DSP::DSP_REG_AR2:
    case DSP::DSP_REG_AR3:
        s.r.ar[reg & 3] = value;
        break;
    case DSP::DSP_REG_IX0:
    case DSP::DSP_REG_IX1:
    case DSP::DSP_REG_IX2:
    case DSP::DSP_REG_IX3:
        s.r.ix[reg & 3] = value;
        break;
    case DSP::DSP_REG_WR0:
    case DSP::DSP_REG_WR1:
    case DSP::DSP_REG_WR2:
    case DSP::DSP_REG_WR3:
        s.r.wr[reg & 3] = value;
        break;
    case DSP::DSP_REG_CR:
        s.r.cr = value & 0x00ff;
        break;
    case DSP::DSP_REG_SR:
        s.r.sr = value & ~DSP::SR_100;
        break;
    case DSP::DSP_REG_PRODL:
        s.r.prod.l = value;
        break;
    case DSP::DSP_REG_PRODM:
        s.r.prod.m = value;
        break;
    case DSP::DSP_REG_PRODH:
        s.r.prod.h = value & 0x00ff;
        break;
    case DSP::DSP_REG_PRODM2:
        s.r.prod.m2 = value;
        break;
    case DSP::DSP_REG_AXL0:
    case DSP::DSP_REG_AXL1:
        s.r.ax[reg & 1].l = value;
        break;
    case DSP::DSP_REG_AXH0:
    case DSP::DSP_REG_AXH1:
        s.r.ax[reg & 1].h = value;
        break;
    case DSP::DSP_REG_ACL0:
    case DSP::DSP_REG_ACL1:
        s.r.ac[reg & 1].l = value;
        break;
    case DSP::DSP_REG_ACM0:
    case DSP::DSP_REG_ACM1:
        s.r.ac[reg & 1].m = value;
        break;
    default:
        break;
    }
}

WP_DSP_INLINE void extend_acc(SDSP& s, int reg) {
    if (reg != DSP::DSP_REG_ACM0 && reg != DSP::DSP_REG_ACM1) {
        return;
    }
    if (!(s.r.sr & DSP::SR_40_MODE_BIT)) {
        return;
    }
    u16 value = s.r.ac[reg & 1].m;
    s.r.ac[reg & 1].h = (value & 0x8000) != 0 ? 0xFFFFFFFF : 0x0000;
    s.r.ac[reg & 1].l = 0;
}

}
