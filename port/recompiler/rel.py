import struct
from dataclasses import dataclass

R_PPC_ADDR32 = 1
R_PPC_ADDR16_LO = 4
R_PPC_ADDR16_HI = 5
R_PPC_ADDR16_HA = 6
R_PPC_REL24 = 10
R_DOLPHIN_NOP = 201
R_DOLPHIN_SECTION = 202
R_DOLPHIN_END = 203


@dataclass(frozen=True)
class RelSection:
    offset: int
    executable: bool
    size: int


@dataclass(frozen=True)
class Relocation:
    module: int
    site_section: int
    site_offset: int
    kind: int
    target_section: int
    addend: int


@dataclass(frozen=True)
class Rel:
    module_id: int
    version: int
    bss_size: int
    rel_offset: int
    imp_offset: int
    imp_size: int
    prolog: tuple
    epilog: tuple
    unresolved: tuple
    sections: list
    data: bytes

    def relocations(self):
        result = []
        for index in range(self.imp_size // 8):
            module, offset = struct.unpack(">2I", self.data[self.imp_offset + index * 8:self.imp_offset + index * 8 + 8])
            result.extend(self.read_relocations(module, offset))
        return result

    def read_relocations(self, module, position):
        result = []
        section = 0
        site = 0
        while True:
            delta, kind, target_section, addend = struct.unpack(">HBBI", self.data[position:position + 8])
            position += 8
            if kind == R_DOLPHIN_END:
                return result
            if kind == R_DOLPHIN_SECTION:
                section = target_section
                site = 0
                continue
            site += delta
            if kind == R_DOLPHIN_NOP:
                continue
            result.append(Relocation(module, section, site, kind, target_section, addend))


def parse(data):
    module_id, _, _, section_count, section_offset = struct.unpack(">5I", data[0x00:0x14])
    version, bss_size, rel_offset, imp_offset, imp_size = struct.unpack(">5I", data[0x1C:0x30])
    prolog_section, epilog_section, unresolved_section, _ = struct.unpack(">4B", data[0x30:0x34])
    prolog, epilog, unresolved = struct.unpack(">3I", data[0x34:0x40])
    sections = []
    for index in range(section_count):
        raw_offset, size = struct.unpack(">2I", data[section_offset + index * 8:section_offset + index * 8 + 8])
        sections.append(RelSection(raw_offset & ~1, bool(raw_offset & 1), size))
    return Rel(
        module_id,
        version,
        bss_size,
        rel_offset,
        imp_offset,
        imp_size,
        (prolog_section, prolog),
        (epilog_section, epilog),
        (unresolved_section, unresolved),
        sections,
        data,
    )
