import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "recompiler"))

import rel
from ppc.decoder import Instr
from ppc.emit import Emitter, rotate_mask
from ppc.module_emit import ModuleEmitter, synthetic


def d_form(opcode, rt, ra, imm):
    return (opcode << 26) | (rt << 21) | (ra << 16) | (imm & 0xFFFF)


def x_form(opcode, rt, ra, rb, xo, rc=0):
    return (opcode << 26) | (rt << 21) | (ra << 16) | (rb << 11) | (xo << 1) | rc


def m_form(opcode, rs, ra, sh, mb, me, rc=0):
    return (opcode << 26) | (rs << 21) | (ra << 16) | (sh << 11) | (mb << 6) | (me << 1) | rc


def lift(word, address=0x80004000, entries=(), tables=None):
    emitter = Emitter(0x80004000, 0x80005000, set(entries), tables or {})
    return emitter.emit(Instr(address, word))


class DecoderTests(unittest.TestCase):
    def test_known_encodings(self):
        self.assertEqual(Instr(0, 0x4E800020).mn, "bclr")
        self.assertEqual(Instr(0, 0x7C0802A6).mn, "mfspr")
        self.assertEqual(Instr(0, 0x9421FFE0).mn, "stwu")
        self.assertEqual(Instr(0, 0x38600000).mn, "addi")
        self.assertEqual(Instr(0, 0x7C0803A6).mn, "mtspr")
        self.assertEqual(Instr(0, 0x4E800420).mn, "bcctr")

    def test_spr_number_is_unswapped(self):
        self.assertEqual(Instr(0, 0x7C0802A6).spr, 8)
        self.assertEqual(Instr(0, 0x7C0903A6).spr, 9)
        self.assertEqual(Instr(0, 0x7C0A03A6).spr, 10)

    def test_oe_forms_decode_to_base_mnemonic(self):
        self.assertEqual(Instr(0, x_form(31, 3, 4, 5, 266 | 0x200)).mn, "add")
        self.assertEqual(Instr(0, x_form(31, 3, 4, 5, 23)).mn, "lwzx")

    def test_paired_single_forms(self):
        self.assertEqual(Instr(0, (4 << 26) | (21 << 1)).mn, "ps_add")
        self.assertEqual(Instr(0, (4 << 26) | (528 << 1)).mn, "ps_merge00")
        self.assertEqual(Instr(0, (4 << 26) | (6 << 1) | (1 << 10) | (3 << 7)).mn, "psq_lx")
        self.assertEqual(Instr(0, 56 << 26).mn, "psq_l")

    def test_signed_fields(self):
        self.assertEqual(Instr(0, d_form(14, 1, 1, -8)).simm, -8)
        self.assertEqual(Instr(0, 0x4BFFFFFC).li, -4)
        self.assertEqual(Instr(0x80004010, 0x4BFFFFF0).target, 0x80004000)

    def test_unknown_word(self):
        self.assertIsNone(Instr(0, 0x00000001).mn)


class MaskTests(unittest.TestCase):
    def test_rotate_mask(self):
        self.assertEqual(rotate_mask(28, 31), 0xF)
        self.assertEqual(rotate_mask(0, 0), 0x80000000)
        self.assertEqual(rotate_mask(0, 31), 0xFFFFFFFF)
        self.assertEqual(rotate_mask(30, 1), 0xC0000003)
        self.assertEqual(rotate_mask(16, 31), 0xFFFF)


class EmitterTests(unittest.TestCase):
    def test_li(self):
        self.assertEqual(lift(d_form(14, 3, 0, 0)), ["c.r[3] = 0x0u;"])
        self.assertEqual(lift(d_form(14, 3, 0, -1)), ["c.r[3] = 0xffffffffu;"])

    def test_addi_with_register(self):
        self.assertEqual(lift(d_form(14, 3, 4, -8)), ["c.r[3] = c.r[4] + 0xfffffff8u;"])

    def test_lis(self):
        self.assertEqual(lift(d_form(15, 3, 0, 0x8000)), ["c.r[3] = 0x80000000u;"])

    def test_load_and_store_update(self):
        self.assertEqual(
            lift(d_form(37, 1, 1, -32)),
            ["ea = c.r[1] + 0xffffffe0u;", "wp::wr32(ea, c.r[1]);", "c.r[1] = ea;"],
        )
        self.assertEqual(lift(d_form(32, 5, 0, 0x10)), ["ea = 0x10u;", "c.r[5] = wp::rd32(ea);"])

    def test_indexed_load(self):
        self.assertEqual(
            lift(x_form(31, 5, 3, 4, 87)),
            ["ea = c.r[3] + c.r[4];", "c.r[5] = wp::rd8(ea);"],
        )

    def test_mr_is_or(self):
        self.assertEqual(lift(x_form(31, 4, 3, 4, 444)), ["c.r[3] = c.r[4] | c.r[4];"])

    def test_record_form(self):
        self.assertEqual(
            lift(x_form(31, 4, 3, 5, 444, 1)),
            ["c.r[3] = c.r[4] | c.r[5];", "wp::record(c, c.r[3]);"],
        )

    def test_immediate_operations_ignore_the_low_immediate_bit(self):
        self.assertEqual(lift(d_form(26, 9, 11, 1)), ["c.r[11] = c.r[9] ^ 0x1u;"])
        self.assertEqual(lift(d_form(24, 9, 11, 1)), ["c.r[11] = c.r[9] | 0x1u;"])
        self.assertEqual(lift(d_form(14, 9, 11, 1)), ["c.r[9] = c.r[11] + 0x1u;"])
        self.assertEqual(
            lift(d_form(28, 9, 11, 1)),
            ["c.r[11] = c.r[9] & 0x1u;", "wp::record(c, c.r[11]);"],
        )

    def test_rlwinm(self):
        self.assertEqual(lift(m_form(21, 4, 3, 0, 28, 31)), ["c.r[3] = c.r[4] & 0xfu;"])
        self.assertEqual(lift(m_form(21, 4, 3, 2, 0, 29)), ["c.r[3] = wp::rotl(c.r[4], 2) & 0xfffffffcu;"])
        self.assertEqual(lift(m_form(21, 4, 3, 2, 0, 31)), ["c.r[3] = wp::rotl(c.r[4], 2);"])

    def test_rlwimi(self):
        self.assertEqual(
            lift(m_form(20, 4, 3, 8, 24, 31)),
            ["c.r[3] = (wp::rotl(c.r[4], 8) & 0xffu) | (c.r[3] & 0xffffff00u);"],
        )

    def test_compare(self):
        self.assertEqual(lift(d_form(11, 0, 3, 5)), ["wp::compare_signed(c, 0, c.r[3], 0x5u);"])
        self.assertEqual(lift(d_form(10, 4, 3, 5)), ["wp::compare_unsigned(c, 1, c.r[3], 0x5u);"])

    def test_mflr_and_mtctr(self):
        self.assertEqual(lift(0x7C0802A6), ["c.r[0] = c.lr;"])
        self.assertEqual(lift(0x7C0903A6), ["c.ctr = c.r[0];"])

    def test_blr(self):
        self.assertEqual(lift(0x4E800020), ["return;"])

    def test_bl_direct_and_indirect(self):
        word = 0x48000001 | 0x100
        self.assertEqual(
            lift(word, entries=[0x80004100]),
            ["c.lr = 0x80004004u;", "f_80004100(c);"],
        )
        self.assertEqual(
            lift(word),
            ["c.lr = 0x80004004u;", "wp::call(c, 0x80004100u);"],
        )

    def test_calls_record_resume_points(self):
        emitter = Emitter(0x80004000, 0x80005000, {0x80004100}, {})
        emitter.emit(Instr(0x80004000, 0x48000001 | 0x100))
        emitter.emit(Instr(0x80004004, 0x4E800421))
        emitter.emit(Instr(0x80004008, 0x48000008))
        self.assertEqual(emitter.resume_points, [0x80004004, 0x80004008])
        prologue = emitter.resume_prologue()
        self.assertIn("        case 0x80004004u: goto R_80004004;", prologue)
        self.assertIn("        case 0x80004008u: goto R_80004008;", prologue)
        self.assertEqual(Emitter(0x80004000, 0x80005000, set(), {}).resume_prologue(), [])

    def test_branch_inside_and_outside_function(self):
        self.assertEqual(lift(0x48000008), ["goto L_80004008;"])
        self.assertEqual(lift(0x48002000, entries=[0x80006000]), ["f_80006000(c);", "return;"])

    def test_conditional_branch(self):
        beq = (16 << 26) | (12 << 21) | (2 << 16) | 0x20
        self.assertEqual(lift(beq), ["if (wp::cr_get(c, 2)) goto L_80004020;"])
        bne = (16 << 26) | (4 << 21) | (2 << 16) | 0x20
        self.assertEqual(lift(bne), ["if (!wp::cr_get(c, 2)) goto L_80004020;"])

    def test_bdnz(self):
        bdnz = (16 << 26) | (16 << 21) | 0xFFF0
        self.assertEqual(
            lift(bdnz, address=0x80004040),
            ["c.ctr--;", "if (c.ctr != 0) {", "WP_POLL(c);", "goto L_80004030;", "}"],
        )

    def test_blrl_calls_through_link_register(self):
        self.assertEqual(
            lift(0x4E800021),
            ["ea = c.lr;", "c.lr = 0x80004004u;", "wp::call(c, ea);"],
        )

    def test_bcl_to_next_instruction_reads_pc(self):
        word = (16 << 26) | (20 << 21) | (31 << 16) | 4 | 1
        self.assertEqual(lift(word), ["c.lr = 0x80004004u;"])

    def test_jump_table_switch(self):
        lines = lift(0x4E800420, tables={0x80004000: [0x80004010, 0x80004020]})
        self.assertEqual(lines[0], "switch (c.ctr) {")
        self.assertIn("case 0x80004010u: goto L_80004010;", lines)
        self.assertIn("default: wp::unresolved_jump(c, c.ctr);", lines)

    def test_unresolved_bctr_is_tail_call(self):
        self.assertEqual(lift(0x4E800420), ["wp::call(c, c.ctr);", "return;"])

    def test_paired_single_load(self):
        word = (56 << 26) | (1 << 21) | (3 << 16) | (0 << 15) | (2 << 12) | 8
        self.assertEqual(
            lift(word),
            ["ea = c.r[3] + 0x8u;", "wp::load_quantized(c, 1, ea, 0, 2, q);"],
        )

    def test_float_single_arithmetic(self):
        fadds = (59 << 26) | (1 << 21) | (2 << 16) | (3 << 11) | (21 << 1)
        self.assertEqual(lift(fadds), ["c.f[1] = wp::round_single(c.f[2] + c.f[3]);", "c.ps1[1] = c.f[1];"])

    def test_round_to_single_fills_both_lanes(self):
        frsp = (63 << 26) | (3 << 21) | (1 << 11) | (12 << 1)
        self.assertEqual(lift(frsp), ["c.f[3] = wp::round_single(c.f[1]);", "c.ps1[3] = c.f[3];"])

    def test_double_arithmetic_keeps_second_lane(self):
        fadd = (63 << 26) | (1 << 21) | (2 << 16) | (3 << 11) | (21 << 1)
        self.assertEqual(lift(fadd), ["c.f[1] = c.f[2] + c.f[3];"])

    def test_illegal_word(self):
        self.assertEqual(lift(1), ["wp::illegal_instruction(c, 0x80004000u, 0x1u);"])


def lift_module(word, offset, relocations, entries=(), dol_entries=()):
    start = synthetic(1, 0)
    emitter = ModuleEmitter("boot", 1, start, start + 0x1000, set(entries), {}, relocations, set(dol_entries))
    return emitter.emit(Instr(synthetic(1, offset), word))


class ModuleEmitterTests(unittest.TestCase):
    def test_high_adjusted_relocation(self):
        reloc = rel.Relocation(1, 1, 0x12, rel.R_PPC_ADDR16_HA, 4, 8)
        lines = lift_module(d_form(15, 3, 0, 0), 0x10, {(1, 0x12): reloc})
        self.assertEqual(lines, ["c.r[3] = ((((g_boot_bases[4] + 0x8u) + 0x8000u) >> 16) << 16);"])

    def test_low_relocation_as_displacement(self):
        reloc = rel.Relocation(1, 1, 0x22, rel.R_PPC_ADDR16_LO, 5, 0x10)
        lines = lift_module(d_form(32, 4, 3, 0), 0x20, {(1, 0x22): reloc})
        self.assertEqual(lines[0], "ea = c.r[3] + wp::sign_extend16((g_boot_bases[5] + 0x10u) & 0xFFFFu);")

    def test_low_relocation_in_ori(self):
        reloc = rel.Relocation(1, 1, 0x22, rel.R_PPC_ADDR16_LO, 5, 0x10)
        lines = lift_module(d_form(24, 4, 3, 0), 0x20, {(1, 0x22): reloc})
        self.assertEqual(lines, ["c.r[3] = c.r[4] | ((g_boot_bases[5] + 0x10u) & 0xFFFFu);"])

    def test_low_relocation_in_addic_record(self):
        reloc = rel.Relocation(1, 1, 0x22, rel.R_PPC_ADDR16_LO, 6, 0x478)
        lines = lift_module(d_form(13, 4, 0, 0), 0x20, {(1, 0x22): reloc})
        self.assertEqual(lines[0], "c.r[4] = wp::add_carry(c, c.r[0], wp::sign_extend16((g_boot_bases[6] + 0x478u) & 0xFFFFu), 0);")

    def test_call_into_the_dol(self):
        reloc = rel.Relocation(0, 1, 0x30, rel.R_PPC_REL24, 0, 0x80075F70)
        self.assertEqual(
            lift_module(0x48000001, 0x30, {(1, 0x30): reloc}, dol_entries=[0x80075F70]),
            ["c.lr = 0x1000034u;", "f_80075f70(c);"],
        )
        self.assertEqual(
            lift_module(0x48000001, 0x30, {(1, 0x30): reloc}),
            ["c.lr = 0x1000034u;", "wp::call(c, 0x80075f70u);"],
        )

    def test_call_to_own_function(self):
        reloc = rel.Relocation(1, 1, 0x30, rel.R_PPC_REL24, 1, 0x100)
        self.assertEqual(
            lift_module(0x48000001, 0x30, {(1, 0x30): reloc}, entries=[synthetic(1, 0x100)]),
            ["c.lr = 0x1000034u;", "f_boot_01000100(c);"],
        )

    def test_call_to_another_module(self):
        reloc = rel.Relocation(7, 1, 0x30, rel.R_PPC_REL24, 1, 0x100)
        self.assertEqual(
            lift_module(0x48000001, 0x30, {(1, 0x30): reloc}),
            ["c.lr = 0x1000034u;", "wp::call(c, wp::external_address(7, 1, 0x100u));"],
        )

    def test_relative_branch_inside_the_section_needs_no_relocation(self):
        self.assertEqual(lift_module(0x48000008, 0x40, {}), ["goto L_01000048;"])


if __name__ == "__main__":
    unittest.main()
