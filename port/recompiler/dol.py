import struct
from dataclasses import dataclass

TEXT_SECTIONS = 7
DATA_SECTIONS = 11


@dataclass(frozen=True)
class Section:
    name: str
    file_offset: int
    address: int
    size: int
    executable: bool


@dataclass(frozen=True)
class Dol:
    sections: list
    bss_address: int
    bss_size: int
    entry: int
    data: bytes

    def section_bytes(self, section):
        return self.data[section.file_offset:section.file_offset + section.size]

    def word_at(self, address):
        for section in self.sections:
            if section.address <= address < section.address + section.size:
                offset = section.file_offset + address - section.address
                return struct.unpack(">I", self.data[offset:offset + 4])[0]
        return None

    def code_section_end(self, address):
        for section in self.sections:
            if section.executable and section.address <= address < section.address + section.size:
                return section.address + section.size
        return None


def parse(data):
    total = TEXT_SECTIONS + DATA_SECTIONS
    offsets = struct.unpack(">18I", data[0x00:0x48])
    addresses = struct.unpack(">18I", data[0x48:0x90])
    sizes = struct.unpack(">18I", data[0x90:0xD8])
    bss_address, bss_size, entry = struct.unpack(">3I", data[0xD8:0xE4])
    sections = []
    for index in range(total):
        if sizes[index] == 0:
            continue
        executable = index < TEXT_SECTIONS
        name = f".text{index}" if executable else f".data{index - TEXT_SECTIONS}"
        sections.append(Section(name, offsets[index], addresses[index], sizes[index], executable))
    return Dol(sections, bss_address, bss_size, entry, data)


def load(path):
    with open(path, "rb") as handle:
        return parse(handle.read())
