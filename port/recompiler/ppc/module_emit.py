import rel
from ppc.emit import Emitter, u32

ADDR16_KINDS = (rel.R_PPC_ADDR16_LO, rel.R_PPC_ADDR16_HI, rel.R_PPC_ADDR16_HA)
SECTION_SHIFT = 24
OFFSET_MASK = (1 << SECTION_SHIFT) - 1


def synthetic(section, offset):
    return (section << SECTION_SHIFT) | offset


def split(address):
    return address >> SECTION_SHIFT, address & OFFSET_MASK


class ModuleEmitter(Emitter):
    def __init__(self, name, identifier, start, end, entries, jump_tables, relocations, dol_entries, save_context=None, set_jump=None):
        super().__init__(start, end, entries, jump_tables, save_context, set_jump)
        self.name = name
        self.identifier = identifier
        self.relocations = relocations
        self.dol_entries = dol_entries
        self.dol_targets = set()

    def relocation_at(self, instr):
        section, offset = split(instr.addr)
        immediate = self.relocations.get((section, offset + 2))
        if immediate is not None and immediate.kind in ADDR16_KINDS:
            return immediate
        return self.relocations.get((section, offset))

    def symbol_expression(self, reloc):
        if reloc.module == 0:
            return u32(reloc.addend)
        if reloc.module == self.identifier:
            return f"(g_{self.name}_bases[{reloc.target_section}] + {u32(reloc.addend)})"
        return f"wp::external_address({reloc.module}, {reloc.target_section}, {u32(reloc.addend)})"

    def simm_expr(self, instr, shift=0):
        reloc = self.relocation_at(instr)
        if reloc is None:
            return super().simm_expr(instr, shift)
        symbol = self.symbol_expression(reloc)
        if reloc.kind == rel.R_PPC_ADDR16_LO:
            return f"wp::sign_extend16({symbol} & 0xFFFFu)"
        if reloc.kind == rel.R_PPC_ADDR16_HA:
            return f"((({symbol} + 0x8000u) >> 16) << 16)"
        if reloc.kind == rel.R_PPC_ADDR16_HI:
            return f"(({symbol} >> 16) << 16)"
        return super().simm_expr(instr, shift)

    def uimm_expr(self, instr, shift=0):
        reloc = self.relocation_at(instr)
        if reloc is None:
            return super().uimm_expr(instr, shift)
        symbol = self.symbol_expression(reloc)
        if reloc.kind == rel.R_PPC_ADDR16_LO:
            return f"({symbol} & 0xFFFFu)"
        if reloc.kind in (rel.R_PPC_ADDR16_HI, rel.R_PPC_ADDR16_HA):
            return f"(({symbol} >> 16) << 16)"
        return super().uimm_expr(instr, shift)

    def target_of(self, instr):
        reloc = self.relocation_at(instr)
        if reloc is None or reloc.kind != rel.R_PPC_REL24:
            return instr.target
        if reloc.module == 0:
            return ("dol", reloc.addend)
        if reloc.module == self.identifier:
            return synthetic(reloc.target_section, reloc.addend)
        return ("ext", reloc.module, reloc.target_section, reloc.addend)

    def function_of(self, target):
        if isinstance(target, tuple):
            if target[0] == "dol":
                if target[1] in self.dol_entries:
                    return f"f_{target[1]:08x}"
                self.dol_targets.add(target[1])
                return None
            return None
        return f"f_{self.name}_{target:08x}" if target in self.entries else None

    def address_of(self, target):
        if isinstance(target, tuple):
            if target[0] == "dol":
                return u32(target[1])
            return f"wp::external_address({target[1]}, {target[2]}, {u32(target[3])})"
        section, offset = split(target)
        return f"(g_{self.name}_bases[{section}] + {u32(offset)})"

    def label_of(self, target):
        return f"L_{target:08x}"

    def switch_expression(self, instr):
        section, _ = split(instr.addr)
        return f"c.ctr - g_{self.name}_bases[{section}]"

    def case_constant(self, target):
        return u32(split(target)[1])
