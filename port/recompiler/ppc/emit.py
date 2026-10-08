from ppc.decoder import Instr


def u32(value):
    return f"{value & 0xFFFFFFFF:#x}u"


def reg(n):
    return f"c.r[{n}]"


def fpr(n):
    return f"c.f[{n}]"


def ps1(n):
    return f"c.ps1[{n}]"


def label(address):
    return f"L_{address:08x}"


def resume_label(address):
    return f"R_{address:08x}"


def function_name(address):
    return f"f_{address:08x}"


def rotate_mask(mb, me):
    value = 0
    for bit in range(32):
        if (mb <= bit <= me) if mb <= me else (bit >= mb or bit <= me):
            value |= 0x80000000 >> bit
    return value


def build_memory_table():
    table = {}
    bases = [
        ("lwz", "load", "wp::rd32({ea})"),
        ("lbz", "load", "wp::rd8({ea})"),
        ("lhz", "load", "wp::rd16({ea})"),
        ("lha", "load", "wp::sign_extend16(wp::rd16({ea}))"),
        ("stw", "store", "wp::wr32({ea}, {value})"),
        ("stb", "store", "wp::wr8({ea}, {value})"),
        ("sth", "store", "wp::wr16({ea}, {value})"),
        ("lfs", "fload", "wp::rdf32({ea})"),
        ("lfd", "fload", "wp::rdf64({ea})"),
        ("stfs", "fstore", "wp::wrf32({ea}, static_cast<float>({value}))"),
        ("stfd", "fstore", "wp::wrf64({ea}, {value})"),
    ]
    for base, kind, template in bases:
        table[base] = (kind, template, False, False)
        table[base + "u"] = (kind, template, False, True)
        table[base + "x"] = (kind, template, True, False)
        table[base + "ux"] = (kind, template, True, True)
    table["lwbrx"] = ("load", "wp::rd32_reversed({ea})", True, False)
    table["lhbrx"] = ("load", "wp::rd16_reversed({ea})", True, False)
    table["stwbrx"] = ("store", "wp::wr32_reversed({ea}, {value})", True, False)
    table["sthbrx"] = ("store", "wp::wr16_reversed({ea}, {value})", True, False)
    table["stfiwx"] = ("fstore", "wp::wr32({ea}, static_cast<uint32_t>(wp::fpr_bits({value})))", True, False)
    return table


MEMORY = build_memory_table()

CR_LOGIC = {
    "crand": "{a} && {b}",
    "crandc": "{a} && !{b}",
    "creqv": "{a} == {b}",
    "crnand": "!({a} && {b})",
    "crnor": "!({a} || {b})",
    "cror": "{a} || {b}",
    "crorc": "{a} || !{b}",
    "crxor": "{a} != {b}",
}

BITWISE = {
    "and": "{a} & {b}",
    "andc": "{a} & ~{b}",
    "or": "{a} | {b}",
    "orc": "{a} | ~{b}",
    "xor": "{a} ^ {b}",
    "nand": "~({a} & {b})",
    "nor": "~({a} | {b})",
    "eqv": "~({a} ^ {b})",
}

IMMEDIATE_LOGIC = {
    "ori": ("|", 0, False),
    "oris": ("|", 16, False),
    "xori": ("^", 0, False),
    "xoris": ("^", 16, False),
    "andi.": ("&", 0, True),
    "andis.": ("&", 16, True),
}

FLOAT_ARITHMETIC = {
    "fadd": ("{a} + {b}", False),
    "fsub": ("{a} - {b}", False),
    "fmul": ("{a} * {c}", False),
    "fdiv": ("{a} / {b}", False),
    "fadds": ("{a} + {b}", True),
    "fsubs": ("{a} - {b}", True),
    "fmuls": ("{a} * wp::force25({c})", True),
    "fdivs": ("{a} / {b}", True),
    "fmadd": ("wp::fused_multiply_add({a}, {c}, {b})", False),
    "fmsub": ("wp::fused_multiply_add({a}, {c}, -{b})", False),
    "fnmadd": ("-wp::fused_multiply_add({a}, {c}, {b})", False),
    "fnmsub": ("-wp::fused_multiply_add({a}, {c}, -{b})", False),
    "fmadds": ("wp::madd_single({a}, {c}, {b})", True),
    "fmsubs": ("wp::madd_single({a}, {c}, -{b})", True),
    "fnmadds": ("-wp::madd_single({a}, {c}, {b})", True),
    "fnmsubs": ("-wp::madd_single({a}, {c}, -{b})", True),
    "fsqrt": ("std::sqrt({b})", False),
    "fsqrts": ("std::sqrt({b})", True),
    "fres": ("wp::reciprocal_estimate({b})", True),
    "frsqrte": ("wp::reciprocal_sqrt_estimate({b})", False),
    "fsel": ("wp::fsel({a}, {b}, {c})", False),
    "frsp": ("{b}", True),
    "fneg": ("-{b}", False),
    "fabs": ("std::fabs({b})", False),
    "fnabs": ("-std::fabs({b})", False),
    "fmr": ("{b}", False),
}

PAIRED_SIGNS = {
    "ps_madd": (1, 1),
    "ps_msub": (1, -1),
    "ps_nmadd": (-1, -1),
    "ps_nmsub": (-1, 1),
}

PAIRED_MERGE = {
    "ps_merge00": (False, False),
    "ps_merge01": (False, True),
    "ps_merge10": (True, False),
    "ps_merge11": (True, True),
}

NO_OPERATION = {
    "isync", "sync", "eieio", "dcbf", "dcbst", "dcbi", "dcbt", "dcbtst", "icbi",
    "tlbsync", "tlbie",
}


class Emitter:
    def __init__(self, start, end, entries, jump_tables, save_context=None, set_jump=None):
        self.start = start
        self.end = end
        self.entries = entries
        self.jump_tables = jump_tables
        self.save_context = save_context
        self.set_jump = set_jump
        self.save_sites = 0
        self.unsupported = []
        self.resume_points = []

    def inside(self, target):
        return isinstance(target, int) and self.start <= target < self.end

    def target_of(self, instr):
        return instr.target

    def function_of(self, target):
        return function_name(target) if target in self.entries else None

    def address_of(self, target):
        return u32(target)

    def label_of(self, target):
        return label(target)

    def switch_expression(self, instr):
        return "c.ctr"

    def case_constant(self, target):
        return u32(target)

    def simm_expr(self, instr, shift=0):
        return u32(instr.simm << shift)

    def uimm_expr(self, instr, shift=0):
        return u32(instr.uimm << shift)

    def call_statement(self, target):
        name = self.function_of(target)
        if name:
            return f"{name}(c);"
        return f"wp::call(c, {self.address_of(target)});"

    def emit(self, instr):
        if instr.mn is None:
            return [f"wp::illegal_instruction(c, {u32(instr.addr)}, {u32(instr.word)});"]
        if instr.mn in NO_OPERATION:
            return []
        if instr.mn in MEMORY:
            return self.memory(instr)
        if instr.mn in BITWISE:
            return self.bitwise(instr)
        if instr.mn in IMMEDIATE_LOGIC:
            return self.immediate_logic(instr)
        if instr.mn in CR_LOGIC:
            return self.condition_logic(instr)
        if instr.mn in FLOAT_ARITHMETIC:
            return self.float_arithmetic(instr)
        if instr.mn in PAIRED_SIGNS:
            sign_product, sign_addend = PAIRED_SIGNS[instr.mn]
            return [f"wp::ps_madd(c, {instr.frd}, {instr.fra}, {instr.frc}, {instr.frb}, {sign_product}.0, {sign_addend}.0);"]
        if instr.mn in PAIRED_MERGE:
            high_a, high_b = PAIRED_MERGE[instr.mn]
            return [f"wp::ps_merge(c, {instr.frd}, {instr.fra}, {instr.frb}, {str(high_a).lower()}, {str(high_b).lower()});"]
        handler = getattr(self, "op_" + instr.mn.replace(".", "_dot"), None)
        if handler is None:
            self.unsupported.append(instr.mn)
            return [f'wp::unsupported_instruction(c, {u32(instr.addr)}, "{instr.mn}");']
        return handler(instr)

    def record(self, instr, value, force=False):
        return [f"wp::record(c, {value});"] if force or instr.rc else []

    def assign(self, instr, target, expression, force=False):
        return [f"{target} = {expression};"] + self.record(instr, target, force)

    def overflowing(self, instr, expression, first, second, check=None):
        target = reg(instr.rd)
        if not instr.oe:
            return self.assign(instr, target, expression)
        condition = check or f"(((q ^ {target}) & (ea ^ {target})) >> 31) != 0"
        lines = [f"q = {first};", f"ea = {second};", f"{target} = {expression};", f"wp::set_overflow(c, {condition});"]
        return lines + self.record(instr, target, False)

    def base(self, register):
        return "0u" if register == 0 else reg(register)

    def effective_address(self, instr, indexed):
        if indexed:
            if instr.ra == 0:
                return reg(instr.rb)
            return f"{reg(instr.ra)} + {reg(instr.rb)}"
        if instr.ra == 0:
            return self.simm_expr(instr)
        return f"{reg(instr.ra)} + {self.simm_expr(instr)}"

    def memory(self, instr):
        kind, template, indexed, update = MEMORY[instr.mn]
        lines = [f"ea = {self.effective_address(instr, indexed)};"]
        if kind == "load":
            lines.append(f"{reg(instr.rt)} = {template.format(ea='ea')};")
        elif kind == "fload":
            lines.append(f"{fpr(instr.rt)} = {template.format(ea='ea')};")
            if instr.mn.startswith("lfs"):
                lines.append(f"{ps1(instr.rt)} = {fpr(instr.rt)};")
        elif kind == "store":
            lines.append(template.format(ea="ea", value=reg(instr.rs)) + ";")
        else:
            lines.append(template.format(ea="ea", value=fpr(instr.rs)) + ";")
        if update:
            lines.append(f"{reg(instr.ra)} = ea;")
        return lines

    def bitwise(self, instr):
        expression = BITWISE[instr.mn].format(a=reg(instr.rs), b=reg(instr.rb))
        return self.assign(instr, reg(instr.ra), expression)

    def immediate_logic(self, instr):
        operator, shift, force = IMMEDIATE_LOGIC[instr.mn]
        expression = f"{reg(instr.rs)} {operator} {self.uimm_expr(instr, shift)}"
        return self.assign(instr, reg(instr.ra), expression, force)

    def condition_logic(self, instr):
        expression = CR_LOGIC[instr.mn].format(
            a=f"wp::cr_get(c, {instr.ra})", b=f"wp::cr_get(c, {instr.rb})"
        )
        return [f"wp::cr_set(c, {instr.rt}, {expression});"]

    def float_arithmetic(self, instr):
        expression = FLOAT_ARITHMETIC[instr.mn][0].format(
            a=fpr(instr.fra), b=fpr(instr.frb), c=fpr(instr.frc)
        )
        single = FLOAT_ARITHMETIC[instr.mn][1]
        if single:
            expression = f"wp::round_single({expression})"
        lines = [f"{fpr(instr.frd)} = {expression};"]
        if single:
            lines.append(f"{ps1(instr.frd)} = {fpr(instr.frd)};")
        if instr.rc:
            lines.append("wp::update_cr1(c);")
        return lines

    def op_addi(self, i):
        if i.ra == 0:
            return [f"{reg(i.rd)} = {self.simm_expr(i)};"]
        return [f"{reg(i.rd)} = {reg(i.ra)} + {self.simm_expr(i)};"]

    def op_addis(self, i):
        if i.ra == 0:
            return [f"{reg(i.rd)} = {self.simm_expr(i, 16)};"]
        return [f"{reg(i.rd)} = {reg(i.ra)} + {self.simm_expr(i, 16)};"]

    def op_addic(self, i):
        return [f"{reg(i.rd)} = wp::add_carry(c, {reg(i.ra)}, {self.simm_expr(i)}, 0);"]

    def op_addic_dot(self, i):
        return self.op_addic(i) + self.record(i, reg(i.rd), True)

    def op_subfic(self, i):
        return [f"{reg(i.rd)} = wp::sub_carry(c, {reg(i.ra)}, {self.simm_expr(i)});"]

    def op_mulli(self, i):
        return [f"{reg(i.rd)} = {reg(i.ra)} * {self.simm_expr(i)};"]

    def op_add(self, i):
        return self.overflowing(i, f"{reg(i.ra)} + {reg(i.rb)}", reg(i.ra), reg(i.rb))

    def op_addc(self, i):
        return self.overflowing(i, f"wp::add_carry(c, {reg(i.ra)}, {reg(i.rb)}, 0)", reg(i.ra), reg(i.rb))

    def op_adde(self, i):
        return self.overflowing(i, f"wp::add_extended(c, {reg(i.ra)}, {reg(i.rb)})", reg(i.ra), reg(i.rb))

    def op_addze(self, i):
        return self.overflowing(i, f"wp::add_extended(c, {reg(i.ra)}, 0)", reg(i.ra), "0u")

    def op_addme(self, i):
        return self.overflowing(i, f"wp::add_extended(c, {reg(i.ra)}, 0xFFFFFFFFu)", reg(i.ra), "0xFFFFFFFFu")

    def op_subf(self, i):
        return self.overflowing(i, f"{reg(i.rb)} - {reg(i.ra)}", f"~{reg(i.ra)}", reg(i.rb))

    def op_subfc(self, i):
        return self.overflowing(i, f"wp::sub_carry(c, {reg(i.ra)}, {reg(i.rb)})", f"~{reg(i.ra)}", reg(i.rb))

    def op_subfe(self, i):
        return self.overflowing(i, f"wp::sub_extended(c, {reg(i.ra)}, {reg(i.rb)})", f"~{reg(i.ra)}", reg(i.rb))

    def op_subfze(self, i):
        return self.overflowing(i, f"wp::sub_extended(c, {reg(i.ra)}, 0)", f"~{reg(i.ra)}", "0u")

    def op_subfme(self, i):
        return self.overflowing(i, f"wp::sub_extended(c, {reg(i.ra)}, 0xFFFFFFFFu)", f"~{reg(i.ra)}", "0xFFFFFFFFu")

    def op_neg(self, i):
        return self.overflowing(i, f"0u - {reg(i.ra)}", reg(i.ra), "0u", "q == 0x80000000u")

    def op_mullw(self, i):
        return self.overflowing(i, f"{reg(i.ra)} * {reg(i.rb)}", reg(i.ra), reg(i.rb), "wp::multiply_overflows(q, ea)")

    def op_mulhw(self, i):
        return self.assign(i, reg(i.rd), f"wp::mulhw({reg(i.ra)}, {reg(i.rb)})")

    def op_mulhwu(self, i):
        return self.assign(i, reg(i.rd), f"wp::mulhwu({reg(i.ra)}, {reg(i.rb)})")

    def op_divw(self, i):
        return self.overflowing(i, f"wp::divw({reg(i.ra)}, {reg(i.rb)})", reg(i.ra), reg(i.rb), "ea == 0u || (q == 0x80000000u && ea == 0xFFFFFFFFu)")

    def op_divwu(self, i):
        return self.overflowing(i, f"wp::divwu({reg(i.ra)}, {reg(i.rb)})", reg(i.ra), reg(i.rb), "ea == 0u")

    def op_extsb(self, i):
        return self.assign(i, reg(i.ra), f"wp::sign_extend8({reg(i.rs)})")

    def op_extsh(self, i):
        return self.assign(i, reg(i.ra), f"wp::sign_extend16({reg(i.rs)})")

    def op_cntlzw(self, i):
        return self.assign(i, reg(i.ra), f"wp::cntlzw({reg(i.rs)})")

    def op_slw(self, i):
        return self.assign(i, reg(i.ra), f"wp::slw({reg(i.rs)}, {reg(i.rb)})")

    def op_srw(self, i):
        return self.assign(i, reg(i.ra), f"wp::srw({reg(i.rs)}, {reg(i.rb)})")

    def op_sraw(self, i):
        return self.assign(i, reg(i.ra), f"wp::sraw(c, {reg(i.rs)}, {reg(i.rb)})")

    def op_srawi(self, i):
        return self.assign(i, reg(i.ra), f"wp::sraw(c, {reg(i.rs)}, {i.sh})")

    def op_rlwinm(self, i):
        mask = rotate_mask(i.mb, i.me)
        rotated = reg(i.rs) if i.sh == 0 else f"wp::rotl({reg(i.rs)}, {i.sh})"
        expression = rotated if mask == 0xFFFFFFFF else f"{rotated} & {u32(mask)}"
        return self.assign(i, reg(i.ra), expression)

    def op_rlwnm(self, i):
        mask = rotate_mask(i.mb, i.me)
        return self.assign(i, reg(i.ra), f"wp::rotl({reg(i.rs)}, {reg(i.rb)}) & {u32(mask)}")

    def op_rlwimi(self, i):
        mask = rotate_mask(i.mb, i.me)
        expression = f"(wp::rotl({reg(i.rs)}, {i.sh}) & {u32(mask)}) | ({reg(i.ra)} & {u32(~mask)})"
        return self.assign(i, reg(i.ra), expression)

    def op_cmpi(self, i):
        return [f"wp::compare_signed(c, {i.crfd}, {reg(i.ra)}, {self.simm_expr(i)});"]

    def op_cmpli(self, i):
        return [f"wp::compare_unsigned(c, {i.crfd}, {reg(i.ra)}, {self.uimm_expr(i)});"]

    def op_cmp(self, i):
        return [f"wp::compare_signed(c, {i.crfd}, {reg(i.ra)}, {reg(i.rb)});"]

    def op_cmpl(self, i):
        return [f"wp::compare_unsigned(c, {i.crfd}, {reg(i.ra)}, {reg(i.rb)});"]

    def op_fcmpu(self, i):
        return [f"wp::compare_float(c, {i.crfd}, {fpr(i.fra)}, {fpr(i.frb)});"]

    op_fcmpo = op_fcmpu

    def op_fctiw(self, i):
        return [f"{fpr(i.frd)} = wp::convert_to_int({fpr(i.frb)}, false);"]

    def op_fctiwz(self, i):
        return [f"{fpr(i.frd)} = wp::convert_to_int({fpr(i.frb)}, true);"]

    def op_mffs(self, i):
        return [f"{fpr(i.frd)} = wp::fpr_from_bits(0xFFF8000000000000ull | c.fpscr);"]

    def op_mtfsf(self, i):
        mask = 0
        for field in range(8):
            if i.fm & (0x80 >> field):
                mask |= 0xF << (28 - 4 * field)
        return [
            f"c.fpscr = (c.fpscr & {u32(~mask)}) | (static_cast<uint32_t>(wp::fpr_bits({fpr(i.frb)})) & {u32(mask)});"
        ]

    def op_mtfsfi(self, i):
        shift = 28 - 4 * i.crfd
        value = ((i.word >> 12) & 0xF) << shift
        return [f"c.fpscr = (c.fpscr & {u32(~(0xF << shift))}) | {u32(value)};"]

    def op_mtfsb0(self, i):
        return [f"c.fpscr &= {u32(~(0x80000000 >> i.rt))};"]

    def op_mtfsb1(self, i):
        return [f"c.fpscr |= {u32(0x80000000 >> i.rt)};"]

    def op_mcrfs(self, i):
        return [f"c.cr[{i.crfd}] = (c.fpscr >> {28 - 4 * i.crfs}) & 0xF;"]

    def op_mcrf(self, i):
        return [f"c.cr[{i.crfd}] = c.cr[{i.crfs}];"]

    def op_mfcr(self, i):
        return [f"{reg(i.rd)} = wp::mfcr(c);"]

    def op_mtcrf(self, i):
        return [f"wp::mtcrf(c, {i.crm:#x}, {reg(i.rs)});"]

    def op_mfmsr(self, i):
        return [f"{reg(i.rd)} = c.msr;"]

    def op_mtmsr(self, i):
        return [f"c.msr = {reg(i.rs)};"]

    def op_mfsr(self, i):
        return [f"{reg(i.rd)} = c.sr[{i.sr}];"]

    def op_mtsr(self, i):
        return [f"c.sr[{i.sr}] = {reg(i.rs)};"]

    def op_mfspr(self, i):
        special = {1: "wp::mfxer(c)", 8: "c.lr", 9: "c.ctr", 22: "wp::get_decrementer(c)"}
        return [f"{reg(i.rd)} = {special.get(i.spr, f'c.spr[{i.spr}]')};"]

    def op_mtspr(self, i):
        source = reg(i.rs)
        special = {
            1: f"wp::mtxer(c, {source});",
            8: f"c.lr = {source};",
            9: f"c.ctr = {source};",
            22: f"wp::set_decrementer(c, {source});",
            923: f"c.spr[923] = {source}; wp::locked_cache_dma(c);",
        }
        return [special.get(i.spr, f"c.spr[{i.spr}] = {source};")]

    def op_mftb(self, i):
        if i.spr == 269:
            return [f"{reg(i.rd)} = static_cast<uint32_t>(wp::time_base() >> 32);"]
        return [f"{reg(i.rd)} = static_cast<uint32_t>(wp::time_base());"]

    def op_lmw(self, i):
        return [
            f"ea = {self.effective_address(i, False)};",
            f"for (uint32_t k = {i.rd}; k < 32; k++) {{ c.r[k] = wp::rd32(ea); ea += 4; }}",
        ]

    def op_stmw(self, i):
        return [
            f"ea = {self.effective_address(i, False)};",
            f"for (uint32_t k = {i.rs}; k < 32; k++) {{ wp::wr32(ea, c.r[k]); ea += 4; }}",
        ]

    def op_lwarx(self, i):
        return [f"ea = {self.effective_address(i, True)};", f"{reg(i.rd)} = wp::rd32(ea);"]

    def op_stwcx_dot(self, i):
        return [
            f"ea = {self.effective_address(i, True)};",
            f"wp::wr32(ea, {reg(i.rs)});",
            "c.cr[0] = static_cast<uint8_t>(2 | c.xer_so);",
        ]

    def op_dcbz(self, i):
        return [f"wp::dcbz({self.effective_address(i, True)});"]

    op_dcbz_l = op_dcbz

    def op_sc(self, i):
        return ["wp::system_call(c);"]

    def op_twi(self, i):
        return self.trap_lines(i, reg(i.ra), u32(i.simm))

    def op_tw(self, i):
        return self.trap_lines(i, reg(i.ra), reg(i.rb))

    def trap_lines(self, i, a, b):
        to = i.rt
        if to == 31:
            return [f"wp::trap(c, {u32(i.addr)});"]
        conditions = []
        signed_a = f"static_cast<int32_t>({a})"
        signed_b = f"static_cast<int32_t>({b})"
        if to & 16:
            conditions.append(f"{signed_a} < {signed_b}")
        if to & 8:
            conditions.append(f"{signed_a} > {signed_b}")
        if to & 4:
            conditions.append(f"{a} == {b}")
        if to & 2:
            conditions.append(f"{a} < {b}")
        if to & 1:
            conditions.append(f"{a} > {b}")
        if not conditions:
            return []
        return [f"if ({' || '.join(conditions)}) wp::trap(c, {u32(i.addr)});"]

    def op_psq_l(self, i):
        return self.quantized(i, "load", f"{self.base(i.ra)} + {u32(i.q_d)}", i.q_w, i.q_i, i.rd)

    def op_psq_lu(self, i):
        return self.quantized(i, "load", f"{reg(i.ra)} + {u32(i.q_d)}", i.q_w, i.q_i, i.rd, True)

    def op_psq_st(self, i):
        return self.quantized(i, "store", f"{self.base(i.ra)} + {u32(i.q_d)}", i.q_w, i.q_i, i.rs)

    def op_psq_stu(self, i):
        return self.quantized(i, "store", f"{reg(i.ra)} + {u32(i.q_d)}", i.q_w, i.q_i, i.rs, True)

    def op_psq_lx(self, i):
        return self.quantized(i, "load", self.effective_address(i, True), i.q_wx, i.q_ix, i.rd)

    def op_psq_lux(self, i):
        return self.quantized(i, "load", self.effective_address(i, True), i.q_wx, i.q_ix, i.rd, True)

    def op_psq_stx(self, i):
        return self.quantized(i, "store", self.effective_address(i, True), i.q_wx, i.q_ix, i.rs)

    def op_psq_stux(self, i):
        return self.quantized(i, "store", self.effective_address(i, True), i.q_wx, i.q_ix, i.rs, True)

    def quantized(self, i, direction, address, w, index, register, update=False):
        function = "load_quantized" if direction == "load" else "store_quantized"
        lines = [f"ea = {address};", f"wp::{function}(c, {register}, ea, {w}, {index}, q);"]
        if update:
            lines.append(f"{reg(i.ra)} = ea;")
        return lines

    def op_ps_add(self, i):
        return [f"wp::ps_add(c, {i.frd}, {i.fra}, {i.frb});"]

    def op_ps_sub(self, i):
        return [f"wp::ps_sub(c, {i.frd}, {i.fra}, {i.frb});"]

    def op_ps_mul(self, i):
        return [f"wp::ps_mul(c, {i.frd}, {i.fra}, {i.frc});"]

    def op_ps_div(self, i):
        return [f"wp::ps_div(c, {i.frd}, {i.fra}, {i.frb});"]

    def op_ps_muls0(self, i):
        return [f"wp::ps_muls0(c, {i.frd}, {i.fra}, {i.frc});"]

    def op_ps_muls1(self, i):
        return [f"wp::ps_muls1(c, {i.frd}, {i.fra}, {i.frc});"]

    def op_ps_madds0(self, i):
        return [f"wp::ps_madds0(c, {i.frd}, {i.fra}, {i.frc}, {i.frb});"]

    def op_ps_madds1(self, i):
        return [f"wp::ps_madds1(c, {i.frd}, {i.fra}, {i.frc}, {i.frb});"]

    def op_ps_sum0(self, i):
        return [f"wp::ps_sum0(c, {i.frd}, {i.fra}, {i.frc}, {i.frb});"]

    def op_ps_sum1(self, i):
        return [f"wp::ps_sum1(c, {i.frd}, {i.fra}, {i.frc}, {i.frb});"]

    def op_ps_sel(self, i):
        return [f"wp::ps_sel(c, {i.frd}, {i.fra}, {i.frc}, {i.frb});"]

    def op_ps_res(self, i):
        return [f"wp::ps_res(c, {i.frd}, {i.frb});"]

    def op_ps_rsqrte(self, i):
        return [f"wp::ps_rsqrte(c, {i.frd}, {i.frb});"]

    def op_ps_mr(self, i):
        return [f"wp::ps_mr(c, {i.frd}, {i.frb});"]

    def op_ps_neg(self, i):
        return [f"wp::ps_neg(c, {i.frd}, {i.frb});"]

    def op_ps_abs(self, i):
        return [f"wp::ps_abs(c, {i.frd}, {i.frb});"]

    def op_ps_nabs(self, i):
        return [f"wp::ps_nabs(c, {i.frd}, {i.frb});"]

    def op_ps_cmpu0(self, i):
        return [f"wp::compare_float(c, {i.crfd}, {fpr(i.fra)}, {fpr(i.frb)});"]

    op_ps_cmpo0 = op_ps_cmpu0

    def op_ps_cmpu1(self, i):
        return [f"wp::compare_float(c, {i.crfd}, {ps1(i.fra)}, {ps1(i.frb)});"]

    op_ps_cmpo1 = op_ps_cmpu1

    def condition(self, i):
        bo = i.bo
        parts = []
        lines = []
        if not bo & 4:
            lines.append("c.ctr--;")
            parts.append("c.ctr == 0" if bo & 2 else "c.ctr != 0")
        if not bo & 16:
            bit = f"wp::cr_get(c, {i.bi})"
            parts.append(bit if bo & 8 else f"!{bit}")
        return lines, " && ".join(parts)

    def save_context_lines(self, return_address):
        site = self.save_sites
        self.save_sites += 1
        return [
            f"c.lr = {u32(return_address)};",
            f"if (setjmp(wp_jump[{site}]) == 0) {{",
            f"wp::save_context(c, &wp_jump[{site}]);",
            "} else {",
            "wp::resume_context(c);",
            "}",
        ]

    def save_jump_lines(self, return_address):
        site = self.save_sites
        self.save_sites += 1
        return [
            f"c.lr = {u32(return_address)};",
            f"if (setjmp(wp_jump[{site}]) == 0) {{",
            f"wp::save_jump(c, &wp_jump[{site}]);",
            "} else {",
            "wp::resume_jump(c);",
            "}",
        ]

    def resume_after(self, address):
        if address not in self.resume_points:
            self.resume_points.append(address)

    def resume_prologue(self):
        if not self.resume_points:
            return []
        lines = ["    if (wp::g_resume_address != 0) [[unlikely]] {", "        switch (wp::take_resume_address()) {"]
        lines += [f"        case {u32(a)}: goto {resume_label(a)};" for a in self.resume_points]
        lines += ["        default: wp::bad_resume(c);", "        }", "    }"]
        return lines

    def call_lines(self, target, return_address):
        self.resume_after(return_address)
        if target == self.save_context:
            return self.save_context_lines(return_address)
        if target == self.set_jump:
            return self.save_jump_lines(return_address)
        return [f"c.lr = {u32(return_address)};", self.call_statement(target)]

    def tail_lines(self, target):
        return [self.call_statement(target), "return;"]

    def jump_lines(self, target, origin):
        if self.inside(target):
            if target <= origin:
                return ["WP_POLL(c);", f"goto {self.label_of(target)};"]
            return [f"goto {self.label_of(target)};"]
        return self.tail_lines(target)

    def op_b(self, i):
        target = self.target_of(i)
        if i.lk:
            return self.call_lines(target, i.addr + 4)
        return self.jump_lines(target, i.addr)

    def op_bc(self, i):
        target = self.target_of(i)
        prelude, test = self.condition(i)
        if i.lk and not test and target == i.addr + 4:
            return [f"c.lr = {u32(target)};"]
        if i.lk:
            body = self.call_lines(target, i.addr + 4)
        else:
            body = self.jump_lines(target, i.addr)
        if not test:
            return prelude + body
        if len(body) == 1:
            return prelude + [f"if ({test}) {body[0]}"]
        return prelude + [f"if ({test}) {{"] + body + ["}"]

    def op_bclr(self, i):
        prelude, test = self.condition(i)
        if i.lk:
            self.resume_after(i.addr + 4)
            lines = ["ea = c.lr;", f"c.lr = {u32(i.addr + 4)};", "wp::call(c, ea);"]
            if test:
                return prelude + [f"if ({test}) {{"] + lines + ["}"]
            return prelude + lines
        if not test:
            return prelude + ["return;"]
        return prelude + [f"if ({test}) return;"]

    def op_bcctr(self, i):
        prelude, test = self.condition(i)
        if i.lk:
            self.resume_after(i.addr + 4)
            lines = [f"c.lr = {u32(i.addr + 4)};", "wp::call(c, c.ctr);"]
            if test:
                return prelude + [f"if ({test}) {{"] + lines + ["}"]
            return prelude + lines
        table = self.jump_tables.get(i.addr)
        if table is not None:
            lines = [f"switch ({self.switch_expression(i)}) {{"]
            for target in sorted(set(table)):
                if self.inside(target):
                    lines.append(f"case {self.case_constant(target)}: goto {self.label_of(target)};")
                else:
                    lines.append(f"case {self.case_constant(target)}: " + " ".join(self.tail_lines(target)))
            lines.append("default: wp::unresolved_jump(c, c.ctr);")
            lines.append("}")
            return lines
        lines = ["wp::call(c, c.ctr);", "return;"]
        if test:
            return prelude + [f"if ({test}) {{"] + lines + ["}"]
        return lines

    def op_rfi(self, i):
        self.unsupported.append("rfi")
        return [f'wp::unsupported_instruction(c, {u32(i.addr)}, "rfi");']
