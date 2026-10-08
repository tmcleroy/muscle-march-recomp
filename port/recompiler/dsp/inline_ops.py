# Copyright 2008 Dolphin Emulator Project
# Copyright 2004 Duddie & Tratax
# Copyright 2005 Duddie
# Copyright 2009 Dolphin Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later

AXL0 = 0x18
AXL1 = 0x19
AXH0 = 0x1A
ACL0 = 0x1C
ACM0 = 0x1E
ACH0 = 0x10


def h(value):
    return f"0x{value & 0xFFFF:x}"


def increment(reg):
    return f"increment_ar(s, {reg})"


def decrement(reg):
    return f"decrement_ar(s, {reg})"


def increase(reg, ix):
    return f"increase_ar(s, {reg}, static_cast<s16>(s.r.ix[{ix}]))"


def ext_dr(opc):
    return [f"u16 e0 = {decrement(opc & 3)};"], [f"write_reg(s, {opc & 3}, e0);"]


def ext_ir(opc):
    return [f"u16 e0 = {increment(opc & 3)};"], [f"write_reg(s, {opc & 3}, e0);"]


def ext_nr(opc):
    reg = opc & 3
    return [f"u16 e0 = {increase(reg, reg)};"], [f"write_reg(s, {reg}, e0);"]


def ext_mv(opc):
    sreg = (opc & 3) + ACL0
    dreg = ((opc >> 2) & 3) + AXL0
    return [f"u16 e0 = read_reg(s, {h(sreg)});"], [f"write_reg(s, {h(dreg)}, e0);"]


def store(dreg, sreg, advance):
    pre = [f"write_dmem(s, s.r.ar[{dreg}], read_reg(s, {h(sreg)}));", f"u16 e0 = {advance};"]
    return pre, [f"write_reg(s, {dreg}, e0);"]


def ext_s(opc):
    dreg = opc & 3
    return store(dreg, ((opc >> 3) & 3) + ACL0, increment(dreg))


def ext_sn(opc):
    dreg = opc & 3
    return store(dreg, ((opc >> 3) & 3) + ACL0, increase(dreg, dreg))


def load(opc, advance):
    sreg = opc & 3
    dreg = ((opc >> 3) & 7) + AXL0
    pre = [f"u16 e0 = read_dmem(s, s.r.ar[{sreg}]);", f"u16 e1 = {advance};"]
    if dreg >= ACM0:
        pre.append("bool e40 = (s.r.sr & DSP::SR_40_MODE_BIT) != 0;")
        post = [
            "if (e40) {",
            f"    write_reg(s, {h(dreg - ACM0 + ACH0)}, (e0 & 0x8000) != 0 ? 0xFFFF : 0x0000);",
            f"    write_reg(s, {h(dreg)}, e0);",
            f"    write_reg(s, {h(dreg - ACM0 + ACL0)}, 0);",
            f"    write_reg(s, {sreg}, e1);",
            "} else {",
            f"    write_reg(s, {h(dreg)}, e0);",
            f"    write_reg(s, {sreg}, e1);",
            "}",
        ]
    else:
        post = [f"write_reg(s, {h(dreg)}, e0);", f"write_reg(s, {sreg}, e1);"]
    return pre, post


def ext_l(opc):
    return load(opc, increment(opc & 3))


def ext_ln(opc):
    return load(opc, increase(opc & 3, opc & 3))


def load_store(opc, store_ar, load_ar, ar3_advance, ar0_advance):
    sreg = (opc & 1) + ACM0
    dreg = ((opc >> 4) & 3) + AXL0
    pre = [
        f"write_dmem(s, s.r.ar[{store_ar}], read_reg(s, {h(sreg)}));",
        f"u16 e0 = read_dmem(s, s.r.ar[{load_ar}]);",
        f"u16 e1 = {ar3_advance};",
        f"u16 e2 = {ar0_advance};",
    ]
    post = [f"write_reg(s, {h(dreg)}, e0);", "write_reg(s, 3, e1);", "write_reg(s, 0, e2);"]
    return pre, post


def ext_ls(opc):
    return load_store(opc, 3, 0, increment(3), increment(0))


def ext_lsn(opc):
    return load_store(opc, 3, 0, increment(3), increase(0, 0))


def ext_lsm(opc):
    return load_store(opc, 3, 0, increase(3, 3), increment(0))


def ext_lsnm(opc):
    return load_store(opc, 3, 0, increase(3, 3), increase(0, 0))


def ext_sl(opc):
    return load_store(opc, 0, 3, increment(3), increment(0))


def ext_sln(opc):
    return load_store(opc, 0, 3, increment(3), increase(0, 0))


def ext_slm(opc):
    return load_store(opc, 0, 3, increase(3, 3), increment(0))


def ext_slnm(opc):
    return load_store(opc, 0, 3, increase(3, 3), increase(0, 0))


def dual_load(sreg, first, second, sreg_advance, ar3_advance):
    pre = [
        f"u16 e0 = read_dmem(s, s.r.ar[{sreg}]);",
        f"u16 e1 = ((s.r.ar[{sreg}] >> 10) == (s.r.ar[3] >> 10)) ? read_dmem(s, s.r.ar[{sreg}]) : read_dmem(s, s.r.ar[3]);",
        f"u16 e2 = {sreg_advance};",
        f"u16 e3 = {ar3_advance};",
    ]
    post = [f"write_reg(s, {h(first)}, e0);", f"write_reg(s, {h(second)}, e1);", f"write_reg(s, {sreg}, e2);", "write_reg(s, 3, e3);"]
    return pre, post


def ld_family(opc, n, m):
    dreg = (opc >> 5) & 1
    rreg = (opc >> 4) & 1
    sreg = opc & 3
    return dual_load(sreg, (dreg << 1) + AXL0, (rreg << 1) + AXL1, increase(sreg, sreg) if n else increment(sreg),
                     increase(3, 3) if m else increment(3))


def ldax_family(opc, n, m):
    sreg = (opc >> 5) & 1
    rreg = (opc >> 4) & 1
    return dual_load(sreg, rreg + AXH0, rreg + AXL0, increase(sreg, sreg) if n else increment(sreg),
                     increase(3, 3) if m else increment(3))


EXT = {
    "dr": ext_dr,
    "ir": ext_ir,
    "nr": ext_nr,
    "mv": ext_mv,
    "s": ext_s,
    "sn": ext_sn,
    "l": ext_l,
    "ln": ext_ln,
    "ls": ext_ls,
    "lsn": ext_lsn,
    "lsm": ext_lsm,
    "lsnm": ext_lsnm,
    "sl": ext_sl,
    "sln": ext_sln,
    "slm": ext_slm,
    "slnm": ext_slnm,
    "ld": lambda opc: ld_family(opc, False, False),
    "ldn": lambda opc: ld_family(opc, True, False),
    "ldm": lambda opc: ld_family(opc, False, True),
    "ldnm": lambda opc: ld_family(opc, True, True),
    "ldax": lambda opc: ldax_family(opc, False, False),
    "ldaxn": lambda opc: ldax_family(opc, True, False),
    "ldaxm": lambda opc: ldax_family(opc, False, True),
    "ldaxnm": lambda opc: ldax_family(opc, True, True),
    "nop_ext": lambda opc: ([], []),
}


class Flags:
    def __init__(self, value, carry="false"):
        self.value = value
        self.carry = carry


def ax_operand(opc):
    sreg = (opc >> 9) & 3
    field = "l" if sreg < 2 else "h"
    return f"(static_cast<s64>(static_cast<s16>(s.r.ax[{sreg & 1}].{field})) << 16)"


def mulx_operands(opc):
    treg = (opc >> 11) & 1
    sreg = (opc >> 12) & 1
    val1 = f"s.r.ax[0].{'h' if sreg else 'l'}"
    val2 = f"s.r.ax[1].{'h' if treg else 'l'}"
    return f"multiply_mulx(s, {sreg}, {treg}, {val1}, {val2})"


def op_mrr(opc, imm):
    sreg = opc & 0x1F
    dreg = (opc >> 5) & 0x1F
    return [f"write_reg(s, {h(dreg)}, read_reg(s, {h(sreg)}));", f"extend_acc(s, {h(dreg)});"], None


def store_register(opc, advance):
    dreg = (opc >> 5) & 3
    sreg = opc & 0x1F
    lines = [f"write_dmem(s, s.r.ar[{dreg}], read_reg(s, {h(sreg)}));"]
    if advance:
        lines.append(f"s.r.ar[{dreg}] = {advance(dreg)};")
    return lines, None


def load_register(opc, advance):
    sreg = (opc >> 5) & 3
    dreg = opc & 0x1F
    lines = [f"{{ u16 v = read_dmem(s, read_reg(s, {sreg})); write_reg(s, {h(dreg)}, v); }}", f"extend_acc(s, {h(dreg)});"]
    if advance:
        lines.append(f"s.r.ar[{sreg}] = {advance(sreg)};")
    return lines, None


def op_mulxac(opc, imm):
    rreg = (opc >> 8) & 1
    return [
        f"s64 a = acc(s, {rreg}) + prod(s);",
        f"s64 p = {mulx_operands(opc)};",
        "set_prod(s, p);",
        f"set_acc(s, {rreg}, a);",
    ], Flags(f"acc(s, {rreg})")


def op_mulxmv(opc, imm):
    rreg = (opc >> 8) & 1
    return [
        "s64 a = prod(s);",
        f"s64 p = {mulx_operands(opc)};",
        "set_prod(s, p);",
        f"set_acc(s, {rreg}, a);",
    ], Flags(f"acc(s, {rreg})")


def op_mulx(opc, imm):
    return [f"set_prod(s, {mulx_operands(opc)});"], None


def op_mulac(opc, imm):
    rreg = (opc >> 8) & 1
    sreg = (opc >> 11) & 1
    return [
        f"s64 a = acc(s, {rreg}) + prod(s);",
        f"s64 p = multiply(s, s.r.ax[{sreg}].l, s.r.ax[{sreg}].h, 0);",
        "set_prod(s, p);",
        f"set_acc(s, {rreg}, a);",
    ], Flags(f"acc(s, {rreg})")


def op_mulmv(opc, imm):
    rreg = (opc >> 8) & 1
    sreg = (opc >> 11) & 1
    return [
        "s64 a = prod(s);",
        f"s64 p = multiply(s, s.r.ax[{sreg}].l, s.r.ax[{sreg}].h, 0);",
        "set_prod(s, p);",
        f"set_acc(s, {rreg}, a);",
    ], Flags(f"acc(s, {rreg})")


def op_mul(opc, imm):
    sreg = (opc >> 11) & 1
    return [f"set_prod(s, multiply(s, s.r.ax[{sreg}].h, s.r.ax[{sreg}].l, 0));"], None


def op_mulcac(opc, imm):
    rreg = (opc >> 8) & 1
    treg = (opc >> 11) & 1
    sreg = (opc >> 12) & 1
    return [
        f"s64 a = acc(s, {rreg}) + prod(s);",
        f"s64 p = multiply(s, s.r.ac[{sreg}].m, s.r.ax[{treg}].h, 0);",
        "set_prod(s, p);",
        f"set_acc(s, {rreg}, a);",
    ], Flags(f"acc(s, {rreg})")


def op_asr16(opc, imm):
    reg = (opc >> 11) & 1
    return [f"set_acc(s, {reg}, acc(s, {reg}) >> 16);"], Flags(f"acc(s, {reg})")


def op_lsl16(opc, imm):
    reg = (opc >> 8) & 1
    return [f"set_acc(s, {reg}, static_cast<s64>(static_cast<u64>(acc(s, {reg})) << 16));"], Flags(f"acc(s, {reg})")


def op_lsl(opc, imm):
    reg = (opc >> 8) & 1
    shift = opc & 0x3F
    return [f"set_acc(s, {reg}, static_cast<s64>(static_cast<u64>(acc(s, {reg})) << {shift}));"], Flags(f"acc(s, {reg})")


def op_lsr(opc, imm):
    reg = (opc >> 8) & 1
    shift = 0 if (opc & 0x3F) == 0 else 0x40 - (opc & 0x3F)
    return [f"set_acc(s, {reg}, static_cast<s64>((static_cast<u64>(acc(s, {reg})) & 0x000000FFFFFFFFFFULL) >> {shift}));"], Flags(f"acc(s, {reg})")


def op_asr(opc, imm):
    reg = (opc >> 8) & 1
    shift = 0 if (opc & 0x3F) == 0 else 0x40 - (opc & 0x3F)
    return [f"set_acc(s, {reg}, acc(s, {reg}) >> {shift});"], Flags(f"acc(s, {reg})")


def op_clr(opc, imm):
    reg = (opc >> 11) & 1
    return [f"set_acc(s, {reg}, 0);"], Flags("0")


def op_tst(opc, imm):
    reg = (opc >> 11) & 1
    return [], Flags(f"acc(s, {reg})")


def add_like(dreg, operand, subtract):
    sign = "-" if subtract else "+"
    kind = "sub" if subtract else "add"
    return [
        f"s64 a = acc(s, {dreg});",
        f"s64 b = {operand};",
        f"set_acc(s, {dreg}, a {sign} b);",
    ], Flags(f"acc(s, {dreg})", kind)


def op_addp(opc, imm):
    return add_like((opc >> 8) & 1, "prod(s)", False)


def op_subp(opc, imm):
    return add_like((opc >> 8) & 1, "prod(s)", True)


def op_addax(opc, imm):
    return add_like((opc >> 8) & 1, f"acx(s, {(opc >> 9) & 1})", False)


def op_subax(opc, imm):
    return add_like((opc >> 8) & 1, f"acx(s, {(opc >> 9) & 1})", True)


def op_addr(opc, imm):
    return add_like((opc >> 8) & 1, ax_operand(opc), False)


def op_subr(opc, imm):
    return add_like((opc >> 8) & 1, ax_operand(opc), True)


def op_add(opc, imm):
    dreg = (opc >> 8) & 1
    return add_like(dreg, f"acc(s, {1 - dreg})", False)


def op_sub(opc, imm):
    dreg = (opc >> 8) & 1
    return add_like(dreg, f"acc(s, {1 - dreg})", True)


def op_addis(opc, imm):
    value = ((opc & 0xFF) ^ 0x80) - 0x80
    return add_like((opc >> 8) & 1, f"static_cast<s64>({value}) << 16", False)


def op_addi(opc, imm):
    value = (imm ^ 0x8000) - 0x8000
    return add_like((opc >> 8) & 1, f"static_cast<s64>({value}) << 16", False)


def op_inc(opc, imm):
    return add_like((opc >> 8) & 1, "1", False)


def op_dec(opc, imm):
    return add_like((opc >> 8) & 1, "1", True)


def compare(first, operand):
    return [
        f"s64 a = {first};",
        f"s64 b = {operand};",
        "s64 r = ((a - b) << 24) >> 24;",
    ], Flags("r", "sub")


def op_cmp(opc, imm):
    return compare("acc(s, 0)", "acc(s, 1)")


def op_cmpi(opc, imm):
    value = (imm ^ 0x8000) - 0x8000
    return compare(f"acc(s, {(opc >> 8) & 1})", f"static_cast<s64>({value}) << 16")


def op_cmpis(opc, imm):
    value = ((opc & 0xFF) ^ 0x80) - 0x80
    return compare(f"acc(s, {(opc >> 8) & 1})", f"static_cast<s64>({value}) << 16")


def op_movax(opc, imm):
    dreg = (opc >> 8) & 1
    sreg = (opc >> 9) & 1
    return [f"s64 x = acx(s, {sreg});", f"set_acc(s, {dreg}, x);"], Flags("x")


def op_movr(opc, imm):
    dreg = (opc >> 8) & 1
    return [f"s64 x = {ax_operand(opc)};", f"set_acc(s, {dreg}, x);"], Flags("x")


def op_mov(opc, imm):
    dreg = (opc >> 8) & 1
    return [f"s64 x = acc(s, {1 - dreg});", f"set_acc(s, {dreg}, x);"], Flags("x")


def op_movp(opc, imm):
    dreg = (opc >> 8) & 1
    return [f"s64 x = prod(s);", f"set_acc(s, {dreg}, x);"], Flags("x")


def logic16(dreg, expression):
    return [
        f"u16 m = {expression};",
        f"s.r.ac[{dreg}].m = m;",
        f"flags16(s, static_cast<s16>(m), false, false, over_s32(acc(s, {dreg})));",
    ], None


def op_andr(opc, imm):
    dreg = (opc >> 8) & 1
    return logic16(dreg, f"static_cast<u16>(s.r.ac[{dreg}].m & s.r.ax[{(opc >> 9) & 1}].h)")


def op_orr(opc, imm):
    dreg = (opc >> 8) & 1
    return logic16(dreg, f"static_cast<u16>(s.r.ac[{dreg}].m | s.r.ax[{(opc >> 9) & 1}].h)")


def op_xorr(opc, imm):
    dreg = (opc >> 8) & 1
    return logic16(dreg, f"static_cast<u16>(s.r.ac[{dreg}].m ^ s.r.ax[{(opc >> 9) & 1}].h)")


def op_andc(opc, imm):
    dreg = (opc >> 8) & 1
    return logic16(dreg, f"static_cast<u16>(s.r.ac[{dreg}].m & s.r.ac[{1 - dreg}].m)")


def op_orc(opc, imm):
    dreg = (opc >> 8) & 1
    return logic16(dreg, f"static_cast<u16>(s.r.ac[{dreg}].m | s.r.ac[{1 - dreg}].m)")


def op_xorc(opc, imm):
    dreg = (opc >> 8) & 1
    return logic16(dreg, f"static_cast<u16>(s.r.ac[{dreg}].m ^ s.r.ac[{1 - dreg}].m)")


def op_tstaxh(opc, imm):
    return [f"flags16(s, static_cast<s16>(s.r.ax[{(opc >> 8) & 1}].h), false, false, false);"], None


def op_andf(opc, imm):
    return [f"logic_zero(s, (s.r.ac[{(opc >> 8) & 1}].m & {h(imm)}) == 0);"], None


def op_andcf(opc, imm):
    return [f"logic_zero(s, (s.r.ac[{(opc >> 8) & 1}].m & {h(imm)}) == {h(imm)});"], None


def op_addarn(opc, imm):
    dreg = opc & 3
    return [f"s.r.ar[{dreg}] = {increase(dreg, (opc >> 2) & 3)};"], None


def op_srbith(opc, imm):
    action = {
        2: "s.r.sr &= ~DSP::SR_MUL_MODIFY;",
        3: "s.r.sr |= DSP::SR_MUL_MODIFY;",
        4: "s.r.sr &= ~DSP::SR_MUL_UNSIGNED;",
        5: "s.r.sr |= DSP::SR_MUL_UNSIGNED;",
        6: "s.r.sr &= ~DSP::SR_40_MODE_BIT;",
        7: "s.r.sr |= DSP::SR_40_MODE_BIT;",
    }.get((opc >> 8) & 7)
    return ([action] if action else []), None


def op_sbset(opc, imm):
    return [f"s.r.sr |= {h(1 << ((opc & 7) + 6))};"], None


def op_sbclr(opc, imm):
    return [f"s.r.sr &= static_cast<u16>(~{h(1 << ((opc & 7) + 6))});"], None


def op_sr(opc, imm):
    return [f"write_dmem(s, {h(imm)}, read_reg(s, {h(opc & 0x1F)}));"], None


def op_lr(opc, imm):
    reg = opc & 0x1F
    return [f"{{ u16 v = read_dmem(s, {h(imm)}); write_reg(s, {h(reg)}, v); }}", f"extend_acc(s, {h(reg)});"], None


def op_lri(opc, imm):
    reg = opc & 0x1F
    return [f"write_reg(s, {h(reg)}, {h(imm)});", f"extend_acc(s, {h(reg)});"], None


def op_lris(opc, imm):
    reg = ((opc >> 8) & 7) + AXL0
    value = ((opc & 0xFF) ^ 0x80) - 0x80
    return [f"write_reg(s, {h(reg)}, {h(value)});", f"extend_acc(s, {h(reg)});"], None


def op_si(opc, imm):
    address = (((opc & 0xFF) ^ 0x80) - 0x80) & 0xFFFF
    return [f"write_dmem(s, {h(address)}, {h(imm)});"], None


def op_srs(opc, imm):
    reg = ((opc >> 8) & 3) + ACL0
    return [f"write_dmem(s, static_cast<u16>((s.r.cr << 8) | {h(opc & 0xFF)}), read_reg(s, {h(reg)}));"], None


def op_lrs(opc, imm):
    reg = ((opc >> 8) & 7) + 0x18
    return [
        f"{{ u16 v = read_dmem(s, static_cast<u16>((s.r.cr << 8) | {h(opc & 0xFF)})); write_reg(s, {h(reg)}, v); }}",
        f"extend_acc(s, {h(reg)});",
    ], None


def op_ilrr(opc, imm):
    reg = opc & 3
    dreg = ACM0 + ((opc >> 8) & 1)
    return [f"s.r.ac[{(opc >> 8) & 1}].m = read_imem(s, s.r.ar[{reg}]);", f"extend_acc(s, {h(dreg)});"], None


MAIN = {
    "mrr": op_mrr,
    "srr": lambda opc, imm: store_register(opc, None),
    "srri": lambda opc, imm: store_register(opc, increment),
    "srrd": lambda opc, imm: store_register(opc, decrement),
    "srrn": lambda opc, imm: store_register(opc, lambda reg: increase(reg, reg)),
    "lrr": lambda opc, imm: load_register(opc, None),
    "lrri": lambda opc, imm: load_register(opc, increment),
    "lrrd": lambda opc, imm: load_register(opc, decrement),
    "lrrn": lambda opc, imm: load_register(opc, lambda reg: increase(reg, reg)),
    "mulxac": op_mulxac,
    "mulxmv": op_mulxmv,
    "mulx": op_mulx,
    "mulac": op_mulac,
    "mulmv": op_mulmv,
    "mul": op_mul,
    "mulcac": op_mulcac,
    "asr16": op_asr16,
    "lsl16": op_lsl16,
    "lsl": op_lsl,
    "lsr": op_lsr,
    "asr": op_asr,
    "clr": op_clr,
    "tst": op_tst,
    "nx": lambda opc, imm: ([], None),
    "addp": op_addp,
    "subp": op_subp,
    "addax": op_addax,
    "subax": op_subax,
    "addr": op_addr,
    "subr": op_subr,
    "add": op_add,
    "sub": op_sub,
    "addis": op_addis,
    "addi": op_addi,
    "inc": op_inc,
    "dec": op_dec,
    "cmp": op_cmp,
    "cmpi": op_cmpi,
    "cmpis": op_cmpis,
    "movax": op_movax,
    "movr": op_movr,
    "mov": op_mov,
    "movp": op_movp,
    "andr": op_andr,
    "orr": op_orr,
    "xorr": op_xorr,
    "andc": op_andc,
    "orc": op_orc,
    "xorc": op_xorc,
    "tstaxh": op_tstaxh,
    "andf": op_andf,
    "andcf": op_andcf,
    "addarn": op_addarn,
    "srbith": op_srbith,
    "sbset": op_sbset,
    "sbclr": op_sbclr,
    "sr": op_sr,
    "lr": op_lr,
    "lri": op_lri,
    "lris": op_lris,
    "si": op_si,
    "srs": op_srs,
    "lrs": op_lrs,
    "ilrr": op_ilrr,
}


FLAG_CHECKS = {
    "false": ("false", "false"),
    "add": ("carry_add(a, {v})", "overflow_add(a, b, {v})"),
    "sub": ("carry_sub(a, {v})", "overflow_sub(a, b, {v})"),
}


def flag_statement(flags):
    if flags.carry == "false":
        return f"flags64(s, {flags.value});"
    return f"flags64_{flags.carry}(s, a, b, {flags.value});"


def deferred_flag_statements(flags):
    carry, overflow = FLAG_CHECKS[flags.carry]
    lines = [
        f"fv = {flags.value};",
        f"fc = {carry.format(v='fv')};",
        f"fo = {overflow.format(v='fv')};",
        "pending = true;",
    ]
    if flags.carry != "false":
        lines.append("sticky(s, fo);")
    return lines


MATERIALIZE = "if (pending) { set_cmp_bits(s, fv, fc, fo); pending = false; }"
