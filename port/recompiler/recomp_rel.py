import argparse
import collections
import re
import struct
import sys
from pathlib import Path

import game
import rel
from ppc.decoder import Instr
from ppc.emit import resume_label, u32
from ppc.module_emit import ModuleEmitter, split, synthetic

GAME = game.load()
MODULES = GAME.modules
OUTPUT = GAME.module_code
DOL_HEADER = GAME.dol_code / "functions.h"
DOL_TARGETS = GAME.module_targets
LINES_PER_FILE = 40000
OSSAVECONTEXT = GAME.sdk["os_save_context"]
SETJMP = GAME.sdk["setjmp"]
MFLR_R0 = 0x7C0802A6
FNV_PRIME = 16777619
FNV_OFFSET = 2166136261
MFCTR_LOOKBACK = 12
LOAD_LOOKBACK = 8
BASE_LOOKBACK = 48
BOUND_LOOKBACK = 64
MAX_TABLE_ENTRIES = 512


def load_dol_entries():
    text = DOL_HEADER.read_text()
    return {int(match, 16) for match in re.findall(r"void f_([0-9a-f]{8})\(", text)}


def mix(value_hash, value):
    for shift in (0, 8, 16, 24):
        value_hash = ((value_hash ^ ((value >> shift) & 0xFF)) * FNV_PRIME) & 0xFFFFFFFF
    return value_hash


def signature(module):
    value_hash = mix(FNV_OFFSET, module.module_id)
    value_hash = mix(value_hash, len(module.sections))
    for section in module.sections:
        value_hash = mix(value_hash, section.size)
    return mix(value_hash, module.bss_size)


def section_words(module, index):
    section = module.sections[index]
    if section.offset == 0:
        return []
    raw = module.data[section.offset:section.offset + section.size]
    return list(struct.unpack(f">{len(raw) // 4}I", raw[:len(raw) // 4 * 4]))


def decode_section(module, index, entries):
    words = section_words(module, index)
    starts = sorted(offset for section, offset in (split(e) for e in entries) if section == index and offset < len(words) * 4)
    bodies = {}
    for position, start in enumerate(starts):
        limit = starts[position + 1] if position + 1 < len(starts) else len(words) * 4
        chunk = words[start // 4:limit // 4]
        while chunk and chunk[-1] == 0:
            chunk.pop()
        if chunk:
            address = synthetic(index, start)
            bodies[address] = [Instr(address + 4 * n, word) for n, word in enumerate(chunk)]
    return bodies


def initial_entries(module, relocations):
    entries = set()
    for label in (module.prolog, module.epilog, module.unresolved):
        if label[0] != 0:
            entries.add(synthetic(label[0], label[1]))
    for reloc in relocations:
        if reloc.module == module.module_id and reloc.target_section < len(module.sections):
            if module.sections[reloc.target_section].executable and reloc.kind in (
                rel.R_PPC_ADDR32, rel.R_PPC_ADDR16_LO, rel.R_PPC_ADDR16_HA, rel.R_PPC_ADDR16_HI, rel.R_PPC_REL24,
            ):
                entries.add(synthetic(reloc.target_section, reloc.addend))
    for index, section in enumerate(module.sections):
        if not section.executable or section.size == 0:
            continue
        words = section_words(module, index)
        for position, word in enumerate(words):
            if word == MFLR_R0 and position > 0 and (words[position - 1] >> 16) == 0x9421:
                entries.add(synthetic(index, (position - 1) * 4))
    return entries


def relocation_map(relocations):
    return {(r.site_section, r.site_offset): r for r in relocations}


def symbol_of(relocations_by_site, instr, position_offset):
    section, offset = split(instr.addr)
    reloc = relocations_by_site.get((section, offset + position_offset))
    return reloc


def find_table(instrs, index, relocations_by_site, module):
    branch = instrs[index]
    if branch.mn != "bcctr" or branch.lk or branch.bo != 20:
        return None
    ctr = None
    for k in range(index - 1, max(index - MFCTR_LOOKBACK, -1), -1):
        if instrs[k].mn == "mtspr" and instrs[k].spr == 9:
            ctr = k
            break
    if ctr is None:
        return None
    load = None
    for k in range(ctr - 1, max(ctr - LOAD_LOOKBACK, -1), -1):
        if instrs[k].mn == "lwzx" and instrs[k].rt == instrs[ctr].rs:
            load = k
            break
    if load is None:
        return None
    base = None
    for register in (instrs[load].ra, instrs[load].rb):
        for k in range(load - 1, max(load - BASE_LOOKBACK, -1), -1):
            candidate = instrs[k]
            if candidate.mn in ("addi", "addis") and candidate.rt == register:
                reloc = symbol_of(relocations_by_site, candidate, 2)
                if reloc is not None and reloc.module == module.module_id:
                    base = (reloc.target_section, reloc.addend)
                break
        if base is not None:
            break
    if base is None:
        return None
    bound = None
    for k in range(load - 1, max(load - BOUND_LOOKBACK, -1), -1):
        if instrs[k].mn == "cmpli":
            bound = instrs[k].uimm + 1
            break
    targets = []
    for n in range(bound if bound is not None else MAX_TABLE_ENTRIES):
        reloc = relocations_by_site.get((base[0], base[1] + 4 * n))
        if reloc is None or reloc.module != module.module_id or reloc.kind != rel.R_PPC_ADDR32:
            if bound is not None:
                return None
            break
        targets.append(synthetic(reloc.target_section, reloc.addend))
    return targets or None


def discover(module, relocations, relocations_by_site):
    entries = initial_entries(module, relocations)
    executable = [i for i, s in enumerate(module.sections) if s.executable and s.size > 0]
    while True:
        bodies = {}
        for index in executable:
            bodies.update(decode_section(module, index, entries))
        found = set()
        for start, instrs in bodies.items():
            end = start + 4 * len(instrs)
            section = split(start)[0]
            for n, instr in enumerate(instrs):
                if instr.mn in ("b", "bc") and (split(instr.addr)[0], split(instr.addr)[1]) not in relocations_by_site:
                    target = instr.target
                    if split(target)[0] == section and (instr.lk or not (start <= target < end)):
                        if split(target)[1] < module.sections[section].size and target not in entries:
                            found.add(target)
                if instr.mn == "bcctr":
                    table = find_table(instrs, n, relocations_by_site, module)
                    for target in table or []:
                        if target not in entries and not (start <= target < end):
                            found.add(target)
        if not found:
            return entries, bodies
        entries |= found


def falls_through(instr):
    if instr.mn == "b":
        return bool(instr.lk)
    if instr.mn in ("bclr", "bcctr"):
        return bool(instr.lk) or (instr.bo & 0x14) != 0x14
    if instr.mn == "rfi":
        return False
    return True


def render_function(name, module, start, instrs, entries, relocations_by_site, dol_entries, stats, dol_targets, bodies):
    end = start + 4 * len(instrs)
    tables = {}
    for n, instr in enumerate(instrs):
        if instr.mn == "bcctr":
            table = find_table(instrs, n, relocations_by_site, module)
            if table is not None:
                tables[instr.addr] = table
            elif not instr.lk and instr.bo == 20:
                stats["unresolved_jumps"] += 1
    labels = set()
    for instr in instrs:
        section, offset = split(instr.addr)
        if instr.mn in ("b", "bc") and not instr.lk and (section, offset) not in relocations_by_site and start <= instr.target < end:
            labels.add(instr.target)
    for targets in tables.values():
        labels.update(t for t in targets if start <= t < end)
    emitter = ModuleEmitter(
        name, module.module_id, start, end, entries, tables, relocations_by_site, dol_entries, ("dol", OSSAVECONTEXT), ("dol", SETJMP)
    )
    body = []
    for instr in instrs:
        if instr.addr in labels:
            body.append(f"L_{instr.addr:08x}:")
        if instr.addr in emitter.resume_points:
            body.append(f"{resume_label(instr.addr)}:")
        if instr.word == 0:
            continue
        if instr.mn is None:
            stats["illegal"] += 1
        body.extend("    " + line for line in emitter.emit(instr))
    lines = [
        f"void f_{name}_{start:08x}(wp::Cpu& c) {{",
        f"    WP_ENTER({u32(start)});",
        "    [[maybe_unused]] uint32_t ea = 0;",
        "    [[maybe_unused]] uint32_t q = 0;",
    ]
    if emitter.save_sites:
        lines.append(f"    std::jmp_buf wp_jump[{emitter.save_sites}];")
    lines += emitter.resume_prologue()
    lines += body
    if end in emitter.resume_points:
        lines.append(f"{resume_label(end)}:")
    if falls_through(instrs[-1]) and end in bodies:
        lines.append(f"    f_{name}_{end:08x}(c);")
    elif end in emitter.resume_points:
        lines.append(f"    wp::unresolved_jump(c, {u32(end)});")
    lines.append("}")
    stats["resumes"].extend((address, start) for address in emitter.resume_points)
    stats["functions"] += 1
    stats["instructions"] += len(instrs)
    stats["unsupported"].update(emitter.unsupported)
    dol_targets.update(emitter.dol_targets)
    return lines


def write_module(name, module, bodies, chunks, resumes):
    folder = OUTPUT / name
    folder.mkdir(parents=True, exist_ok=True)
    written = set()

    def emit(file_name, text):
        path = folder / file_name
        written.add(file_name)
        if not path.exists() or path.read_text() != text:
            path.write_text(text)

    section_count = len(module.sections)
    header = ["#pragma once", '#include "wp/cpu.h"', "", f"extern uint32_t g_{name}_bases[{section_count}];", ""]
    header += [f"void f_{name}_{a:08x}(wp::Cpu& c);" for a in sorted(bodies)]
    emit(f"{name}.h", "\n".join(header) + "\n")
    for index, chunk in enumerate(chunks):
        text = [
            '#include "wp/cpu.h"',
            '#include "wp/modules.h"',
            '#include "wp/threads.h"',
            '#include "functions.h"',
            f'#include "{name}.h"',
            "",
        ] + chunk
        emit(f"{name}_{index:03d}.cpp", "\n".join(text) + "\n")
    table = ['#include "wp/modules.h"', f'#include "{name}.h"', "", f"uint32_t g_{name}_bases[{section_count}];", "", "namespace wp {", ""]
    table.append(f"const ModuleFunction g_{name}_functions[] = {{")
    for address in sorted(bodies):
        section, offset = split(address)
        table.append(f"    {{{section}, {offset:#x}, f_{name}_{address:08x}}},")
    table += ["};", "", f"const ModuleFunction g_{name}_resumes[] = {{"]
    for address, function in sorted(resumes):
        section, offset = split(address)
        table.append(f"    {{{section}, {offset:#x}, f_{name}_{function:08x}}},")
    if not resumes:
        table.append("    {0, 0, nullptr},")
    table += [
        "};",
        "",
        f"extern const ModuleDescriptor g_descriptor_{name};",
        f'const ModuleDescriptor g_descriptor_{name} = {{"{name}", {module.module_id}, {signature(module):#x}u, {section_count}, g_{name}_bases, g_{name}_functions, {len(bodies)}, g_{name}_resumes, {len(resumes)}}};',
        "",
        "}",
    ]
    emit(f"{name}_module.cpp", "\n".join(table) + "\n")
    for old in folder.iterdir():
        if old.name not in written:
            old.unlink()


def write_table():
    names = sorted(path.name[: -len("_module.cpp")] for path in OUTPUT.glob("*/*_module.cpp"))
    lines = ['#include "wp/modules.h"', "", "namespace wp {", ""]
    lines += [f"extern const ModuleDescriptor g_descriptor_{n};" for n in names]
    lines += ["", "const ModuleDescriptor* const g_module_table[] = {"]
    lines += [f"    &g_descriptor_{n}," for n in names]
    lines += ["    nullptr,", "};", "", f"const size_t g_module_count = {len(names)};", "", "}"]
    table = OUTPUT / "modules_table.cpp"
    text = "\n".join(lines) + "\n"
    if not table.exists() or table.read_text() != text:
        table.write_text(text)


def merge_dol_targets(targets):
    known = set()
    if DOL_TARGETS.exists():
        known = {int(line, 16) for line in DOL_TARGETS.read_text().split() if line != "address"}
    known |= targets
    DOL_TARGETS.parent.mkdir(parents=True, exist_ok=True)
    text = "address\n" + "\n".join(f"0x{a:08x}" for a in sorted(known)) + "\n"
    if not DOL_TARGETS.exists() or DOL_TARGETS.read_text() != text:
        DOL_TARGETS.write_text(text)


def generate(name, dol_entries):
    module = rel.parse((MODULES / f"{name}.rel").read_bytes())
    relocations = module.relocations()
    by_site = relocation_map(relocations)
    entries, bodies = discover(module, relocations, by_site)
    stats = {"functions": 0, "instructions": 0, "illegal": 0, "unresolved_jumps": 0, "unsupported": collections.Counter(), "resumes": []}
    dol_targets = set()
    chunks = []
    current = []
    for start in sorted(bodies):
        current.extend(render_function(name, module, start, bodies[start], entries, by_site, dol_entries, stats, dol_targets, bodies))
        current.append("")
        if len(current) >= LINES_PER_FILE:
            chunks.append(current)
            current = []
    if current:
        chunks.append(current)
    write_module(name, module, bodies, chunks, stats["resumes"])
    merge_dol_targets({t for t in dol_targets if t not in dol_entries})
    print(
        f"{name}: functions {stats['functions']}, instructions {stats['instructions']}, illegal {stats['illegal']}, "
        f"unresolved jumps {stats['unresolved_jumps']}, unsupported {dict(stats['unsupported'])}"
    )


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("modules", nargs="*")
    parser.add_argument("--all", action="store_true")
    args = parser.parse_args()
    names = [p.stem for p in sorted(MODULES.glob("*.rel"))] if args.all else args.modules
    if not names:
        parser.error("name at least one module or pass --all")
    dol_entries = load_dol_entries()
    for name in names:
        generate(name, dol_entries)
    write_table()
    return 0


if __name__ == "__main__":
    sys.exit(main())
