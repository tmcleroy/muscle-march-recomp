import argparse
import random
import struct
import sys
from fractions import Fraction
from pathlib import Path

import numpy

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "recompiler"))

from ppc.decoder import Instr
from ppc.emit import Emitter

MASK = 0xFFFFFFFF
CASES_PER_INSTRUCTION = 24
PRIMARIES = [16, 4, 7, 8, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 60, 61, 10, 11, 12, 13, 14, 15, 19, 20, 21, 23, 24, 25, 26, 27, 28, 29, 31, 59, 63]

INTEGER = {
    "addi", "addis", "addic", "addic.", "subfic", "mulli", "add", "addc", "adde", "addze", "addme", "subf", "subfc", "subfe",
    "subfze", "subfme", "neg", "mullw", "mulhw", "mulhwu", "divw", "divwu", "extsb", "extsh", "cntlzw", "slw", "srw", "sraw",
    "srawi", "rlwinm", "rlwnm", "rlwimi", "and", "andc", "or", "orc", "xor", "nand", "nor", "eqv", "ori", "oris", "xori",
    "xoris", "andi.", "andis.", "cmp", "cmpl", "cmpi", "cmpli", "crand", "crandc", "creqv", "crnand", "crnor", "cror",
    "crorc", "crxor", "mcrf", "mfcr", "mtcrf", "mfspr", "mtspr", "bc",
}
SPECIAL_REGISTERS = [1, 8, 9, 272, 273, 274, 275, 912, 913, 914, 915, 916, 917, 918, 919]
FLOAT = {
    "fadd", "fsub", "fmul", "fdiv", "fadds", "fsubs", "fmuls", "fdivs", "fmadd", "fmsub", "fnmadd", "fnmsub", "fmadds",
    "fmsubs", "fnmadds", "fnmsubs", "fneg", "fabs", "fnabs", "fmr", "fsel", "frsp", "fctiwz", "fctiw", "fres", "frsqrte", "fcmpu", "fcmpo",
}
BUFFER = 0x80200000
BUFFER_SIZE = 512
MEMORY = {
    "lwz": (4, "load", "word"), "lwzu": (4, "load", "word"), "lwzx": (4, "load", "word"), "lwzux": (4, "load", "word"),
    "lbz": (1, "load", "byte"), "lbzu": (1, "load", "byte"), "lbzx": (1, "load", "byte"), "lbzux": (1, "load", "byte"),
    "lhz": (2, "load", "half"), "lhzu": (2, "load", "half"), "lhzx": (2, "load", "half"), "lhzux": (2, "load", "half"),
    "lha": (2, "load", "signed half"), "lhau": (2, "load", "signed half"), "lhax": (2, "load", "signed half"), "lhaux": (2, "load", "signed half"),
    "stw": (4, "store", "word"), "stwu": (4, "store", "word"), "stwx": (4, "store", "word"), "stwux": (4, "store", "word"),
    "stb": (1, "store", "byte"), "stbu": (1, "store", "byte"), "stbx": (1, "store", "byte"), "stbux": (1, "store", "byte"),
    "sth": (2, "store", "half"), "sthu": (2, "store", "half"), "sthx": (2, "store", "half"), "sthux": (2, "store", "half"),
    "lwbrx": (4, "load", "reversed word"), "lhbrx": (2, "load", "reversed half"), "stwbrx": (4, "store", "reversed word"),
    "sthbrx": (2, "store", "reversed half"), "lfs": (4, "fload", "single"), "lfsu": (4, "fload", "single"), "lfsx": (4, "fload", "single"),
    "lfsux": (4, "fload", "single"), "lfd": (8, "fload", "double"), "lfdu": (8, "fload", "double"), "lfdx": (8, "fload", "double"),
    "lfdux": (8, "fload", "double"), "stfs": (4, "fstore", "single"), "stfsu": (4, "fstore", "single"), "stfsx": (4, "fstore", "single"),
    "stfsux": (4, "fstore", "single"), "stfd": (8, "fstore", "double"), "stfdu": (8, "fstore", "double"), "stfdx": (8, "fstore", "double"),
    "stfdux": (8, "fstore", "double"), "stfiwx": (4, "fstore", "integer word"), "lmw": (4, "load", "multiple"), "stmw": (4, "store", "multiple"),
    "psq_l": (8, "pload", "quantized"), "psq_lu": (8, "pload", "quantized"), "psq_lx": (8, "pload", "quantized"),
    "psq_lux": (8, "pload", "quantized"), "psq_st": (8, "pstore", "quantized"), "psq_stu": (8, "pstore", "quantized"),
    "psq_stx": (8, "pstore", "quantized"), "psq_stux": (8, "pstore", "quantized"),
}

PAIRED = {
    "ps_add", "ps_sub", "ps_mul", "ps_div", "ps_muls0", "ps_muls1", "ps_madd", "ps_msub", "ps_nmadd", "ps_nmsub", "ps_madds0",
    "ps_madds1", "ps_sum0", "ps_sum1", "ps_neg", "ps_abs", "ps_nabs", "ps_mr", "ps_merge00", "ps_merge01", "ps_merge10",
    "ps_merge11", "ps_sel", "ps_res", "ps_rsqrte", "ps_cmpu0", "ps_cmpu1", "ps_cmpo0", "ps_cmpo1",
}


def s32(value):
    value &= MASK
    return value - (1 << 32) if value & 0x80000000 else value


def rotl(value, amount):
    amount &= 31
    return ((value << amount) | (value >> (32 - amount))) & MASK if amount else value & MASK


def mask(mb, me):
    value = 0
    for bit in range(32):
        if (mb <= bit <= me) if mb <= me else (bit >= mb or bit <= me):
            value |= 0x80000000 >> bit
    return value


def bits_of(value):
    return struct.unpack("<Q", struct.pack("<d", value))[0]


def from_bits(bits):
    return struct.unpack("<d", struct.pack("<Q", bits & 0xFFFFFFFFFFFFFFFF))[0]


def single(value):
    with numpy.errstate(all="ignore"):
        return float(numpy.float32(value))


def single_exact(fraction):
    rounded = float(fraction)
    candidate = single(rounded)
    if Fraction(candidate) == fraction:
        return candidate
    below = numpy.nextafter(numpy.float32(candidate), numpy.float32(-numpy.inf))
    above = numpy.nextafter(numpy.float32(candidate), numpy.float32(numpy.inf))
    options = sorted({float(below), candidate, float(above)}, key=lambda value: (abs(Fraction(value) - fraction), bits_of(value) & 0x20000000))
    best = options[0]
    tied = [value for value in options if abs(Fraction(value) - fraction) == abs(Fraction(best) - fraction)]
    if len(tied) > 1:
        even = [value for value in tied if (struct.unpack("<I", struct.pack("<f", value))[0] & 1) == 0]
        best = even[0]
    return best


def force25(value):
    integral = bits_of(value)
    exponent = integral & 0x7FF0000000000000
    fraction = integral & 0x000FFFFFFFFFFFFF
    if exponent == 0 and fraction != 0:
        shift = (64 - fraction.bit_length()) - 11
        keep = (0xFFFFFFFFF8000000 >> shift) | ((0xFFFFFFFFFFFFFFFF << (64 - shift)) & 0xFFFFFFFFFFFFFFFF)
        integral = (integral & keep) + (integral & (0x8000000 >> shift))
    else:
        integral = (integral & 0xFFFFFFFFF8000000) + (integral & 0x8000000)
    return from_bits(integral)


FRES = [(0x7ff800, 0x3e1), (0x783800, 0x3a7), (0x70ea00, 0x371), (0x6a0800, 0x340), (0x638800, 0x313), (0x5d6200, 0x2ea),
        (0x579000, 0x2c4), (0x520800, 0x2a0), (0x4cc800, 0x27f), (0x47ca00, 0x261), (0x430800, 0x245), (0x3e8000, 0x22a),
        (0x3a2c00, 0x212), (0x360800, 0x1fb), (0x321400, 0x1e5), (0x2e4a00, 0x1d1), (0x2aa800, 0x1be), (0x272c00, 0x1ac),
        (0x23d600, 0x19b), (0x209e00, 0x18b), (0x1d8800, 0x17c), (0x1a9000, 0x16e), (0x17ae00, 0x15b), (0x14f800, 0x15b),
        (0x124400, 0x143), (0x0fbe00, 0x143), (0x0d3800, 0x12d), (0x0ade00, 0x12d), (0x088400, 0x11a), (0x065000, 0x11a),
        (0x041c00, 0x108), (0x020c00, 0x106)]
FRSQRTE = [(0x1a7e800, -0x568), (0x17cb800, -0x4f3), (0x1552800, -0x48d), (0x130c000, -0x435), (0x10f2000, -0x3e7),
           (0x0eff000, -0x3a2), (0x0d2e000, -0x365), (0x0b7c000, -0x32e), (0x09e5000, -0x2fc), (0x0867000, -0x2d0),
           (0x06ff000, -0x2a8), (0x05ab800, -0x283), (0x046a000, -0x261), (0x0339800, -0x243), (0x0218800, -0x226),
           (0x0105800, -0x20b), (0x3ffa000, -0x7a4), (0x3c29000, -0x700), (0x38aa000, -0x670), (0x3572000, -0x5f2),
           (0x3279000, -0x584), (0x2fb7000, -0x524), (0x2d26000, -0x4cc), (0x2ac0000, -0x47e), (0x2881000, -0x43a),
           (0x2665000, -0x3fa), (0x2468000, -0x3c2), (0x2287000, -0x38e), (0x20c1000, -0x35e), (0x1f12000, -0x332),
           (0x1d79000, -0x30a), (0x1bf4000, -0x2e6)]


def reciprocal(value):
    integral = bits_of(value)
    mantissa = integral & ((1 << 52) - 1)
    sign = integral & (1 << 63)
    exponent = integral & (0x7FF << 52)
    if exponent < (895 << 52) or exponent >= (1149 << 52):
        return None
    exponent = (0x7FD << 52) - exponent
    index = mantissa >> 37
    base, decrement = FRES[index // 1024]
    return from_bits(sign | exponent | ((base - (decrement * (index % 1024) + 1) // 2) << 29))


def reciprocal_sqrt(value):
    integral = bits_of(value)
    mantissa = integral & ((1 << 52) - 1)
    exponent = integral & (0x7FF << 52)
    if integral >> 63 or exponent == 0 or exponent == (0x7FF << 52):
        return None
    lsb = exponent & (1 << 52)
    exponent = ((0x3FF << 52) - ((exponent - (0x3FE << 52)) // 2)) & (0x7FF << 52)
    index = (lsb | mantissa) >> 37
    base, decrement = FRSQRTE[index // 2048]
    return from_bits(exponent | ((base + decrement * (index % 2048)) << 26))


class State:
    def __init__(self, rng):
        self.r = [rng.choice([rng.getrandbits(32), rng.getrandbits(8), 0x80000000, 0x7FFFFFFF, 0, MASK, rng.getrandbits(16) << 16])
                  for _ in range(32)]
        self.f = [random_float(rng) for _ in range(32)]
        self.ps1 = [random_float(rng) for _ in range(32)]
        self.cr = [rng.getrandbits(4) for _ in range(8)]
        self.so = rng.getrandbits(1)
        self.ov = rng.getrandbits(1)
        self.ca = rng.getrandbits(1)
        self.memory = bytearray(rng.getrandbits(8) for _ in range(BUFFER_SIZE))
        for offset in range(0, BUFFER_SIZE, 4):
            if rng.random() < 0.5:
                self.memory[offset:offset + 4] = struct.pack(">f", random_float(rng))
        self.lr = rng.getrandbits(32)
        self.ctr = rng.choice([rng.getrandbits(32), rng.randint(0, 3)])
        self.count = rng.getrandbits(7)
        self.sprg = [rng.getrandbits(32) for _ in range(4)]
        self.gqr = [(rng.choice([0, 4, 5, 6, 7]) | (rng.randint(-6, 6) & 0x3F) << 8 | rng.choice([0, 4, 5, 6, 7]) << 16 | (rng.randint(-6, 6) & 0x3F) << 24)
                    for _ in range(8)]

    def copy(self):
        other = State.__new__(State)
        other.r = list(self.r)
        other.f = list(self.f)
        other.ps1 = list(self.ps1)
        other.cr = list(self.cr)
        other.so, other.ov, other.ca = self.so, self.ov, self.ca
        other.lr, other.ctr, other.count, other.sprg = self.lr, self.ctr, self.count, list(self.sprg)
        other.memory = bytearray(self.memory)
        other.gqr = list(self.gqr)
        return other


def random_float(rng):
    kind = rng.random()
    if kind < 0.6:
        return single(rng.uniform(-1000.0, 1000.0))
    if kind < 0.8:
        return single(rng.uniform(-2.0, 2.0) * 2.0 ** rng.randint(-30, 30))
    if kind < 0.9:
        return single(float(rng.randint(-100, 100)))
    return rng.uniform(-1.0e6, 1.0e6)


def record(state, value):
    signed = s32(value)
    state.cr[0] = (8 if signed < 0 else 4 if signed > 0 else 2) | state.so


def set_overflow(state, overflow):
    state.ov = 1 if overflow else 0
    if overflow:
        state.so = 1


def add_with_carry(a, b, carry):
    total = a + b + carry
    return total & MASK, 1 if total > MASK else 0


def signed_overflow(a, b, result):
    return ((a ^ result) & (b ^ result) & 0x80000000) != 0


def integer(instr, s):
    mn = instr.mn
    rt, ra, rb = instr.rt, instr.ra, instr.rb
    oe = instr.oe
    rc = instr.rc
    a = s.r[ra]
    b = s.r[rb]
    if mn in ("addi", "addis"):
        base = 0 if ra == 0 else a
        s.r[rt] = (base + (instr.simm << (16 if mn == "addis" else 0))) & MASK
        return True
    if mn in ("addic", "addic."):
        s.r[rt], s.ca = add_with_carry(a, instr.simm & MASK, 0)
        if mn == "addic.":
            record(s, s.r[rt])
        return True
    if mn == "subfic":
        s.r[rt], s.ca = add_with_carry((~a) & MASK, instr.simm & MASK, 1)
        return True
    if mn == "mulli":
        s.r[rt] = (s32(a) * instr.simm) & MASK
        return True
    xo_forms = {
        "add": (a, b, 0, False), "addc": (a, b, 0, True), "adde": (a, b, s.ca, True), "addze": (a, 0, s.ca, True),
        "addme": (a, MASK, s.ca, True), "subf": ((~a) & MASK, b, 1, False), "subfc": ((~a) & MASK, b, 1, True),
        "subfe": ((~a) & MASK, b, s.ca, True), "subfze": ((~a) & MASK, 0, s.ca, True), "subfme": ((~a) & MASK, MASK, s.ca, True),
        "neg": ((~a) & MASK, 0, 1, False),
    }
    if mn in xo_forms:
        x, y, carry, sets_carry = xo_forms[mn]
        result, carried = add_with_carry(x, y, carry)
        if oe:
            if mn == "neg":
                set_overflow(s, a == 0x80000000)
            else:
                exact = s32(x) + s32(y) + carry
                set_overflow(s, exact != s32(result))
        if sets_carry:
            s.ca = carried
        s.r[rt] = result
        if rc:
            record(s, result)
        return True
    if mn == "mullw":
        product = s32(a) * s32(b)
        s.r[rt] = product & MASK
        if oe:
            set_overflow(s, product != s32(product & MASK))
        if rc:
            record(s, s.r[rt])
        return True
    if mn in ("mulhw", "mulhwu"):
        product = s32(a) * s32(b) if mn == "mulhw" else a * b
        s.r[rt] = (product >> 32) & MASK
        if rc:
            record(s, s.r[rt])
        return True
    if mn in ("divw", "divwu"):
        if mn == "divw":
            if b == 0 or (a == 0x80000000 and b == MASK):
                return False
            x, y = s32(a), s32(b)
            quotient = abs(x) // abs(y)
            s.r[rt] = (quotient if (x < 0) == (y < 0) else -quotient) & MASK
        else:
            if b == 0:
                return False
            s.r[rt] = a // b
        if oe:
            set_overflow(s, False)
        if rc:
            record(s, s.r[rt])
        return True
    rs = instr.rs
    value = s.r[rs]
    unary = {"extsb": lambda v: (v & 0xFF) - 0x100 if v & 0x80 else v & 0xFF,
             "extsh": lambda v: (v & 0xFFFF) - 0x10000 if v & 0x8000 else v & 0xFFFF,
             "cntlzw": lambda v: 32 - v.bit_length()}
    if mn in unary:
        s.r[ra] = unary[mn](value) & MASK
        if rc:
            record(s, s.r[ra])
        return True
    if mn in ("slw", "srw"):
        amount = b & 0x3F
        result = 0 if amount >= 32 else ((value << amount) & MASK if mn == "slw" else value >> amount)
        s.r[ra] = result
        if rc:
            record(s, result)
        return True
    if mn in ("sraw", "srawi"):
        amount = (b & 0x3F) if mn == "sraw" else instr.sh
        negative = value & 0x80000000
        if amount >= 32:
            result = MASK if negative else 0
            s.ca = 1 if negative else 0
        else:
            result = (s32(value) >> amount) & MASK
            s.ca = 1 if negative and (value & ((1 << amount) - 1)) else 0
        s.r[ra] = result
        if rc:
            record(s, result)
        return True
    if mn in ("rlwinm", "rlwnm", "rlwimi"):
        amount = instr.sh if mn != "rlwnm" else b & 31
        m = mask(instr.mb, instr.me)
        rotated = rotl(value, amount)
        result = rotated & m if mn != "rlwimi" else (rotated & m) | (s.r[ra] & ~m & MASK)
        s.r[ra] = result
        if rc:
            record(s, result)
        return True
    logic = {"and": lambda x, y: x & y, "andc": lambda x, y: x & ~y, "or": lambda x, y: x | y, "orc": lambda x, y: x | ~y,
             "xor": lambda x, y: x ^ y, "nand": lambda x, y: ~(x & y), "nor": lambda x, y: ~(x | y), "eqv": lambda x, y: ~(x ^ y)}
    if mn in logic:
        s.r[ra] = logic[mn](value, b) & MASK
        if rc:
            record(s, s.r[ra])
        return True
    immediate = {"ori": (lambda x, y: x | y, 0), "oris": (lambda x, y: x | y, 16), "xori": (lambda x, y: x ^ y, 0),
                 "xoris": (lambda x, y: x ^ y, 16), "andi.": (lambda x, y: x & y, 0), "andis.": (lambda x, y: x & y, 16)}
    if mn in immediate:
        operation, shift = immediate[mn]
        s.r[ra] = operation(value, instr.uimm << shift) & MASK
        if mn.endswith("."):
            record(s, s.r[ra])
        return True
    if mn in ("cmp", "cmpl", "cmpi", "cmpli"):
        if instr.cmp_l:
            return False
        if mn == "cmp":
            x, y = s32(a), s32(b)
        elif mn == "cmpl":
            x, y = a, b
        elif mn == "cmpi":
            x, y = s32(a), instr.simm
        else:
            x, y = a, instr.uimm
        s.cr[instr.crfd] = (8 if x < y else 4 if x > y else 2) | s.so
        return True
    crlogic = {"crand": lambda x, y: x & y, "crandc": lambda x, y: x & (1 - y), "creqv": lambda x, y: 1 - (x ^ y),
               "crnand": lambda x, y: 1 - (x & y), "crnor": lambda x, y: 1 - (x | y), "cror": lambda x, y: x | y,
               "crorc": lambda x, y: x | (1 - y), "crxor": lambda x, y: x ^ y}
    if mn in crlogic:
        def get(bit):
            return (s.cr[bit >> 2] >> (3 - (bit & 3))) & 1
        value = crlogic[mn](get(ra), get(rb))
        field, position = rt >> 2, 3 - (rt & 3)
        s.cr[field] = (s.cr[field] & ~(1 << position) & 0xF) | (value << position)
        return True
    if mn == "mcrf":
        s.cr[instr.crfd] = s.cr[instr.crfs]
        return True
    if mn in ("mfspr", "mtspr"):
        number = instr.spr
        if number not in SPECIAL_REGISTERS:
            return False
        if mn == "mfspr":
            values = {1: (s.so << 31) | (s.ov << 30) | (s.ca << 29) | s.count, 8: s.lr, 9: s.ctr}
            s.r[rt] = values[number] if number in values else s.sprg[number - 272] if number < 912 else s.gqr[number - 912]
            return True
        value = s.r[instr.rs]
        if number == 1:
            s.so, s.ov, s.ca, s.count = (value >> 31) & 1, (value >> 30) & 1, (value >> 29) & 1, value & 0x7F
        elif number == 8:
            s.lr = value
        elif number == 9:
            s.ctr = value
        elif number < 912:
            s.sprg[number - 272] = value
        else:
            s.gqr[number - 912] = value
        return True
    if mn == "bc":
        if instr.lk or instr.aa or instr.target != instr.addr + 8 or (instr.bo & 0x14) == 0x14 and instr.bo & 0xB:
            return False
        bo = instr.bo
        if not bo & 4:
            s.ctr = (s.ctr - 1) & MASK
        counter_ok = bo & 4 or ((s.ctr != 0) != bool(bo & 2))
        bit = (s.cr[instr.bi >> 2] >> (3 - (instr.bi & 3))) & 1
        condition_ok = bo & 16 or bit == ((bo >> 3) & 1)
        if not (counter_ok and condition_ok):
            s.r[3] = (s.r[3] + 1) & MASK
        s.r[4] = (s.r[4] + 1) & MASK
        return True
    if mn == "mfcr":
        s.r[rt] = sum(s.cr[i] << (28 - 4 * i) for i in range(8))
        return True
    if mn == "mtcrf":
        for i in range(8):
            if instr.crm & (0x80 >> i):
                s.cr[i] = (value >> (28 - 4 * i)) & 0xF
        return True
    return False


def floating(instr, s):
    mn = instr.mn
    if instr.rc:
        return False
    d, a, b, c = instr.frd, instr.fra, instr.frb, instr.frc
    fa, fb, fc = s.f[a], s.f[b], s.f[c]
    binary = {"fadd": lambda: Fraction(fa) + Fraction(fb), "fsub": lambda: Fraction(fa) - Fraction(fb),
              "fmul": lambda: Fraction(fa) * Fraction(fc), "fdiv": lambda: Fraction(fa) / Fraction(fb) if fb != 0 else None,
              "fmadd": lambda: Fraction(fa) * Fraction(fc) + Fraction(fb), "fmsub": lambda: Fraction(fa) * Fraction(fc) - Fraction(fb),
              "fnmadd": lambda: -(Fraction(fa) * Fraction(fc) + Fraction(fb)), "fnmsub": lambda: -(Fraction(fa) * Fraction(fc) - Fraction(fb))}
    if mn in binary:
        exact = binary[mn]()
        if exact is None:
            return False
        result = float(exact)
        s.f[d] = result
        return True
    if mn in ("fadds", "fsubs", "fdivs"):
        if mn == "fdivs" and fb == 0:
            return False
        exact = {"fadds": Fraction(fa) + Fraction(fb), "fsubs": Fraction(fa) - Fraction(fb), "fdivs": Fraction(fa) / Fraction(fb) if fb else 0}[mn]
        s.f[d] = single(float(exact))
        s.ps1[d] = s.f[d]
        return True
    if mn == "fmuls":
        s.f[d] = single(float(Fraction(fa) * Fraction(force25(fc))))
        s.ps1[d] = s.f[d]
        return True
    if mn in ("fmadds", "fmsubs", "fnmadds", "fnmsubs"):
        if any(value != single(value) for value in (fa, fb, fc)):
            return False
        product = Fraction(fa) * Fraction(force25(fc))
        exact = {"fmadds": product + Fraction(fb), "fmsubs": product - Fraction(fb),
                 "fnmadds": -(product + Fraction(fb)), "fnmsubs": -(product - Fraction(fb))}[mn]
        s.f[d] = single_exact(exact)
        s.ps1[d] = s.f[d]
        return True
    simple = {"fneg": lambda: -fb, "fabs": lambda: abs(fb), "fnabs": lambda: -abs(fb), "fmr": lambda: fb,
              "fsel": lambda: fc if fa >= 0.0 else fb, "frsqrte": lambda: reciprocal_sqrt(fb)}
    if mn in simple:
        result = simple[mn]()
        if result is None:
            return False
        s.f[d] = result
        return True
    if mn == "frsp":
        s.f[d] = single(fb)
        s.ps1[d] = s.f[d]
        return True
    if mn == "fres":
        result = reciprocal(fb)
        if result is None:
            return False
        s.f[d] = single(result)
        s.ps1[d] = s.f[d]
        return True
    if mn == "fctiwz":
        truncated = int(fb)
        truncated = max(-0x80000000, min(0x7FFFFFFF, truncated))
        s.f[d] = from_bits(0xFFF8000000000000 | (truncated & MASK))
        return True
    if mn in ("fcmpu", "fcmpo"):
        s.cr[instr.crfd] = 8 if fa < fb else 4 if fa > fb else 2
        return True
    if mn == "fctiw":
        rounded = round(fb)
        rounded = max(-0x80000000, min(0x7FFFFFFF, rounded))
        s.f[d] = from_bits(0xFFF8000000000000 | (rounded & MASK))
        return True
    return False


def paired(instr, s):
    mn = instr.mn
    if instr.rc:
        return False
    d, a, b, c = instr.frd, instr.fra, instr.frb, instr.frc
    p0 = lambda register: s.f[register]
    p1 = lambda register: s.ps1[register]
    if any(value != single(value) for value in (s.f[a], s.f[b], s.f[c], s.ps1[a], s.ps1[b], s.ps1[c])):
        return False

    def put(x, y):
        s.f[d], s.ps1[d] = x, y

    arithmetic = {"ps_add": lambda x, y, z: Fraction(x) + Fraction(y), "ps_sub": lambda x, y, z: Fraction(x) - Fraction(y),
                  "ps_div": lambda x, y, z: Fraction(x) / Fraction(y) if y else None}
    if mn in arithmetic:
        first = arithmetic[mn](p0(a), p0(b), None)
        second = arithmetic[mn](p1(a), p1(b), None)
        if first is None or second is None:
            return False
        put(single(float(first)), single(float(second)))
        return True
    if mn in ("ps_mul", "ps_muls0", "ps_muls1"):
        c0 = p0(c) if mn in ("ps_mul", "ps_muls0") else p1(c)
        c1 = p1(c) if mn in ("ps_mul", "ps_muls1") else p0(c)
        put(single(float(Fraction(p0(a)) * Fraction(force25(c0)))), single(float(Fraction(p1(a)) * Fraction(force25(c1)))))
        return True
    madds = {"ps_madd": (1, 1), "ps_msub": (1, -1), "ps_nmadd": (-1, -1), "ps_nmsub": (-1, 1)}
    if mn in madds or mn in ("ps_madds0", "ps_madds1"):
        product_sign, addend_sign = madds.get(mn, (1, 1))
        c0 = p1(c) if mn == "ps_madds1" else p0(c)
        c1 = p0(c) if mn == "ps_madds0" else p1(c)
        first = product_sign * (Fraction(p0(a)) * Fraction(force25(c0)) + addend_sign * product_sign * Fraction(p0(b)))
        second = product_sign * (Fraction(p1(a)) * Fraction(force25(c1)) + addend_sign * product_sign * Fraction(p1(b)))
        put(single_exact(first), single_exact(second))
        return True
    if mn == "ps_sum0":
        put(single(float(Fraction(p0(a)) + Fraction(p1(b)))), single(p1(c)))
        return True
    if mn == "ps_sum1":
        put(single(p0(c)), single(float(Fraction(p0(a)) + Fraction(p1(b)))))
        return True
    unary = {"ps_neg": lambda v: -v, "ps_abs": abs, "ps_nabs": lambda v: -abs(v), "ps_mr": lambda v: v}
    if mn in unary:
        put(unary[mn](p0(b)), unary[mn](p1(b)))
        return True
    merges = {"ps_merge00": (p0(a), p0(b)), "ps_merge01": (p0(a), p1(b)), "ps_merge10": (p1(a), p0(b)), "ps_merge11": (p1(a), p1(b))}
    if mn in merges:
        put(*merges[mn])
        return True
    if mn == "ps_sel":
        put(p0(c) if p0(a) >= 0.0 else p0(b), p1(c) if p1(a) >= 0.0 else p1(b))
        return True
    if mn in ("ps_cmpu0", "ps_cmpo0", "ps_cmpu1", "ps_cmpo1"):
        x, y = (p0(a), p0(b)) if mn.endswith("0") else (p1(a), p1(b))
        s.cr[instr.crfd] = 8 if x < y else 4 if x > y else 2
        return True
    if mn in ("ps_res", "ps_rsqrte"):
        estimate = reciprocal if mn == "ps_res" else reciprocal_sqrt
        x, y = estimate(p0(b)), estimate(p1(b))
        if x is None or y is None:
            return False
        put(single(x), single(y))
        return True
    return False


def dequantize(s, offset, kind, scale):
    factor = numpy.float32(2.0 ** -(scale - 64 if scale & 0x20 else scale))
    if kind == 0:
        return float(struct.unpack(">f", bytes(s.memory[offset:offset + 4]))[0]), 4
    size = 1 if kind in (4, 6) else 2
    raw = int.from_bytes(s.memory[offset:offset + size], "big", signed=kind in (6, 7))
    with numpy.errstate(all="ignore"):
        return float(numpy.float32(raw) * factor), size


def quantize(s, offset, value, kind, scale):
    if kind == 0:
        s.memory[offset:offset + 4] = struct.pack(">f", single(value))
        return 4
    with numpy.errstate(all="ignore"):
        scaled = float(numpy.float32(value) * numpy.float32(2.0 ** (scale - 64 if scale & 0x20 else scale)))
    low, high, size = {4: (0, 255, 1), 5: (0, 65535, 2), 6: (-128, 127, 1), 7: (-32768, 32767, 2)}[kind]
    number = max(low, min(high, int(scaled)))
    s.memory[offset:offset + size] = (number & ((1 << (8 * size)) - 1)).to_bytes(size, "big")
    return size


def memory_access(instr, s, rng):
    mn = instr.mn
    size, direction, form = MEMORY[mn]
    indexed = mn.endswith("x") and mn not in ("psq_l", "psq_st") or mn.endswith("brx") or mn in ("psq_lx", "psq_lux", "psq_stx", "psq_stux")
    update = mn.endswith("u") or mn.endswith("ux")
    paired = mn.startswith("psq")
    rt, ra, rb = instr.rt, instr.ra, instr.rb
    if paired:
        w = instr.q_wx if indexed else instr.q_w
        gqr = s.gqr[instr.q_ix if indexed else instr.q_i]
        kind = (gqr >> 16) & 7 if direction == "pload" else gqr & 7
        if kind in (1, 2, 3):
            return False
    if update and (ra == 0 or (direction in ("load", "fload", "pload") and direction != "pload" and ra == rt)):
        return False
    if mn in ("lmw", "stmw"):
        size = 4 * (32 - rt)
        if mn == "lmw" and rt <= ra:
            return False
    offset = rng.randrange(0, BUFFER_SIZE - 16 - size) & ~3
    if mn in ("lbz", "lbzu", "lbzx", "lbzux", "stb", "stbu", "stbx", "stbux"):
        offset += rng.randrange(4)
    elif size == 2:
        offset += rng.choice([0, 2])
    address = BUFFER + offset
    if indexed:
        if ra == 0:
            s.r[rb] = address
        elif ra == rb:
            if address & 1:
                return False
            s.r[ra] = address // 2
        else:
            s.r[ra] = BUFFER
            s.r[rb] = offset
    else:
        if ra == 0:
            return False
        displacement = instr.q_d if paired else instr.simm
        s.r[ra] = (address - displacement) & MASK
    return address, offset, size, direction, form, update, paired


def run_memory(instr, s, prepared):
    address, offset, size, direction, form, update, paired = prepared
    mn = instr.mn
    rt, ra = instr.rt, instr.ra
    if paired:
        indexed = mn in ("psq_lx", "psq_lux", "psq_stx", "psq_stux")
        w = instr.q_wx if indexed else instr.q_w
        gqr = s.gqr[instr.q_ix if indexed else instr.q_i]
        if direction == "pload":
            first, used = dequantize(s, offset, (gqr >> 16) & 7, (gqr >> 24) & 0x3F)
            s.f[rt] = first
            s.ps1[rt] = 1.0 if w else dequantize(s, offset + used, (gqr >> 16) & 7, (gqr >> 24) & 0x3F)[0]
        else:
            used = quantize(s, offset, s.f[rt], gqr & 7, (gqr >> 8) & 0x3F)
            if not w:
                quantize(s, offset + used, s.ps1[rt], gqr & 7, (gqr >> 8) & 0x3F)
    elif form == "multiple":
        for k in range(rt, 32):
            place = offset + 4 * (k - rt)
            if direction == "load":
                s.r[k] = int.from_bytes(s.memory[place:place + 4], "big")
            else:
                s.memory[place:place + 4] = s.r[k].to_bytes(4, "big")
    elif direction == "load":
        data = bytes(s.memory[offset:offset + size])
        if form.startswith("reversed"):
            data = data[::-1]
        value = int.from_bytes(data, "big")
        if form == "signed half" and value & 0x8000:
            value -= 0x10000
        s.r[rt] = value & MASK
    elif direction == "store":
        data = (s.r[rt] & ((1 << (8 * size)) - 1)).to_bytes(size, "big")
        if form.startswith("reversed"):
            data = data[::-1]
        s.memory[offset:offset + size] = data
    elif direction == "fload":
        if form == "single":
            value = struct.unpack(">f", bytes(s.memory[offset:offset + 4]))[0]
            if value != value:
                return False
            s.f[rt] = value
        else:
            value = struct.unpack(">d", bytes(s.memory[offset:offset + 8]))[0]
            if value != value:
                return False
            s.f[rt] = value
    else:
        if form == "single":
            s.memory[offset:offset + 4] = struct.pack(">f", single(s.f[rt]))
        elif form == "double":
            s.memory[offset:offset + 8] = struct.pack(">d", s.f[rt])
        else:
            s.memory[offset:offset + 4] = (bits_of(s.f[rt]) & MASK).to_bytes(4, "big")
    if update:
        s.r[ra] = address
    return True


def reference(instr, state):
    if instr.mn in INTEGER:
        return integer(instr, state)
    if instr.mn in FLOAT:
        return floating(instr, state)
    return paired(instr, state)


def cpp_double(value):
    return f"0x{bits_of(value):016x}ull"


def collect(rng):
    wanted = INTEGER | FLOAT | PAIRED | set(MEMORY)
    words = {name: [] for name in wanted}
    attempts = 0
    while any(len(found) < CASES_PER_INSTRUCTION * 3 for found in words.values()) and attempts < 4_000_000:
        attempts += 1
        word = (rng.choice(PRIMARIES) << 26) | rng.getrandbits(26)
        instr = Instr(0x80003000, word)
        if instr.mn in words and len(words[instr.mn]) < CASES_PER_INSTRUCTION * 3:
            words[instr.mn].append(word)
    for _ in range(CASES_PER_INSTRUCTION * 3):
        bo = rng.choice([0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 24, 25, 26, 27])
        words["bc"].append((16 << 26) | (bo << 21) | (rng.getrandbits(5) << 16) | 8)
        number = rng.choice(SPECIAL_REGISTERS)
        field = ((number & 31) << 5) | (number >> 5)
        words["mfspr"].append((31 << 26) | (rng.getrandbits(5) << 21) | (field << 11) | (339 << 1))
        words["mtspr"].append((31 << 26) | (rng.getrandbits(5) << 21) | (field << 11) | (467 << 1))
    return words


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", required=True)
    parser.add_argument("--seed", type=int, default=2026)
    args = parser.parse_args()
    rng = random.Random(args.seed)
    words = collect(rng)
    functions = []
    table = []
    values = []
    missing = []
    for name in sorted(words):
        made = 0
        for word in words[name]:
            if made >= CASES_PER_INSTRUCTION:
                break
            instr = Instr(0x80003000, word)
            before = State(rng)
            memory_case = name in MEMORY
            if memory_case:
                prepared = memory_access(instr, before, rng)
                if not prepared:
                    continue
            after = before.copy()
            if not (run_memory(instr, after, prepared) if memory_case else reference(instr, after)):
                continue
            if name == "bc":
                emitter = Emitter(0x80003000, 0x8000300C, set(), {})
                lines = emitter.emit(instr) + emitter.emit(Instr(0x80003004, 0x38630001)) + ["L_80003008:"] + emitter.emit(Instr(0x80003008, 0x38840001))
            else:
                emitter = Emitter(0x80003000, 0x80003004, set(), {})
                lines = emitter.emit(instr)
            if emitter.unsupported:
                continue
            index = len(table)
            functions.append(f"void case_{index}(wp::Cpu& c) {{\n    [[maybe_unused]] uint32_t ea = 0;\n    [[maybe_unused]] uint32_t q = 0;\n"
                             + "".join(f"    {line}\n" for line in lines) + "}\n")
            setup = [f"{{0, {i}, {before.r[i]:#x}ull}}" for i in range(32)]
            setup += [f"{{1, {i}, {cpp_double(before.f[i])}}}" for i in range(32)]
            setup += [f"{{2, {i}, {cpp_double(before.ps1[i])}}}" for i in range(32)]
            setup += [f"{{3, {i}, {before.cr[i]}}}" for i in range(8)]
            setup += [f"{{4, 0, {before.so}}}", f"{{4, 1, {before.ov}}}", f"{{4, 2, {before.ca}}}", f"{{4, 3, {before.count}}}"]
            setup += [f"{{6, 8, {before.lr:#x}ull}}", f"{{6, 9, {before.ctr:#x}ull}}"] + [f"{{6, {272 + k}, {before.sprg[k]:#x}ull}}" for k in range(4)]
            if not memory_case:
                setup += [f"{{6, {912 + k}, {before.gqr[k]:#x}ull}}" for k in range(8)]
            if memory_case:
                setup += [f"{{5, {k}, {int.from_bytes(before.memory[4 * k:4 * k + 4], 'big'):#x}ull}}" for k in range(BUFFER_SIZE // 4)]
                setup += [f"{{6, {912 + k}, {before.gqr[k]:#x}ull}}" for k in range(8)]
            expect = [f"{{0, {i}, {after.r[i]:#x}ull}}" for i in range(32) if after.r[i] != before.r[i] or True]
            expect += [f"{{1, {i}, {cpp_double(after.f[i])}}}" for i in range(32)]
            expect += [f"{{2, {i}, {cpp_double(after.ps1[i])}}}" for i in range(32) if name in PAIRED or after.ps1[i] != before.ps1[i]]
            expect += [f"{{3, {i}, {after.cr[i]}}}" for i in range(8)]
            expect += [f"{{4, 0, {after.so}}}", f"{{4, 1, {after.ov}}}", f"{{4, 2, {after.ca}}}", f"{{4, 3, {after.count}}}"]
            expect += [f"{{6, 8, {after.lr:#x}ull}}", f"{{6, 9, {after.ctr:#x}ull}}"] + [f"{{6, {272 + k}, {after.sprg[k]:#x}ull}}" for k in range(4)]
            expect += [f"{{6, {912 + k}, {after.gqr[k]:#x}ull}}" for k in range(8)]
            if memory_case:
                expect += [f"{{5, {k}, {int.from_bytes(after.memory[4 * k:4 * k + 4], 'big'):#x}ull}}" for k in range(BUFFER_SIZE // 4)]
            values.append(f"const Value setup_{index}[] = {{{', '.join(setup)}}};\nconst Value expect_{index}[] = {{{', '.join(expect)}}};\n")
            table.append(f"    {{\"{name}\", 0x{word:08x}u, case_{index}, setup_{index}, {len(setup)}, expect_{index}, {len(expect)}}},\n")
            made += 1
        if made < CASES_PER_INSTRUCTION:
            missing.append(f"{name} {made}")
    out = ["#include <cstdio>", "#include <cstdlib>", "#include <cstring>", "", '#include "wp/cpu.h"', "", "namespace {", ""]
    out += functions
    out += ["struct Value {", "    int kind;", "    int index;", "    unsigned long long bits;", "};", "",
            "struct Case {", "    const char* name;", "    uint32_t word;", "    void (*run)(wp::Cpu&);", "    const Value* setup;",
            "    size_t setup_count;", "    const Value* expect;", "    size_t expect_count;", "};", ""]
    out += values
    out += ["const Case kCases[] = {"]
    out += ["".join(table), "};", "", "unsigned long long read(const wp::Cpu& c, const Value& v) {",
            "    switch (v.kind) {", "    case 0:", "        return c.r[v.index];", "    case 1:", "        return wp::fpr_bits(c.f[v.index]);",
            "    case 2:", "        return wp::fpr_bits(c.ps1[v.index]);", "    case 3:", "        return c.cr[v.index];",
            "    case 5:", "        return wp::rd32(0x80200000u + 4u * v.index);", "    case 6:",
            "        return v.index == 8 ? c.lr : v.index == 9 ? c.ctr : c.spr[v.index];",
            "    default:", "        return v.index == 0 ? c.xer_so : v.index == 1 ? c.xer_ov : v.index == 2 ? c.xer_ca : c.xer_byte_count;", "    }", "}", "",
            "void write(wp::Cpu& c, const Value& v) {", "    switch (v.kind) {", "    case 0:", "        c.r[v.index] = static_cast<uint32_t>(v.bits);",
            "        break;", "    case 1:", "        c.f[v.index] = wp::fpr_from_bits(v.bits);", "        break;", "    case 2:",
            "        c.ps1[v.index] = wp::fpr_from_bits(v.bits);", "        break;", "    case 3:", "        c.cr[v.index] = static_cast<uint8_t>(v.bits);",
            "        break;", "    case 5:", "        wp::wr32(0x80200000u + 4u * v.index, static_cast<uint32_t>(v.bits));", "        break;", "    case 6:",
            "        (v.index == 8 ? c.lr : v.index == 9 ? c.ctr : c.spr[v.index]) = static_cast<uint32_t>(v.bits);", "        break;",
            "    default:", "        (v.index == 0 ? c.xer_so : v.index == 1 ? c.xer_ov : v.index == 2 ? c.xer_ca : c.xer_byte_count) = static_cast<uint8_t>(v.bits);", "    }", "}", "",
            "}", "", "int main() {", "    wp::g_memory = static_cast<uint8_t*>(std::calloc(wp::kMemorySize, 1));", "    const char* kinds[] = {\"r\", \"f\", \"ps1\", \"cr\", \"xer\", \"word\", \"spr\"};", "    int failures = 0;",
            "    for (const Case& test : kCases) {", "        static wp::Cpu c;", "        std::memset(&c, 0, sizeof c);",
            "        for (size_t i = 0; i < test.setup_count; i++) {", "            write(c, test.setup[i]);", "        }", "        test.run(c);",
            "        for (size_t i = 0; i < test.expect_count; i++) {", "            const Value& v = test.expect[i];", "            unsigned long long got = read(c, v);",
            "            if (got != v.bits) {",
            "                failures++;", "                std::printf(\"%s %08x: %s%d is %llx, expected %llx\\n\", test.name, test.word, kinds[v.kind], v.index, got, v.bits);",
            "            }", "        }", "    }", "    std::printf(\"%zu cases, %d mismatches\\n\", sizeof(kCases) / sizeof(kCases[0]), failures);",
            "    return failures == 0 ? 0 : 1;", "}", ""]
    Path(args.output).write_text("\n".join(out) + "\n", encoding="utf-8")
    print(f"{len(table)} cases, {len(set(words))} instructions" + (f", short: {', '.join(missing)}" if missing else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
