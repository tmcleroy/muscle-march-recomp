import argparse
import bisect
import collections
import csv
import sys
from pathlib import Path

import dol as dol_module
import game
from ppc import cfg
from ppc.decoder import Instr
from ppc.emit import Emitter, function_name, label, resume_label, u32

ROOT = Path(__file__).resolve().parent.parent
LINES_PER_FILE = 40000


def load_functions(path):
    with open(path, newline="") as handle:
        return sorted((int(row["address"], 16), int(row["size"])) for row in csv.DictReader(handle))


def covered_by_other(functions, address):
    index = bisect.bisect_right(functions, (address, float("inf"))) - 1
    return any(start < address < start + size for start, size in functions[max(index - 1, 0):index + 1])


def data_pointers(dol, functions):
    found = set()
    for section in dol.sections:
        if section.executable:
            continue
        for offset in range(0, section.size - 3, 4):
            value = dol.word_at(section.address + offset)
            if is_code_address(dol, value) and not covered_by_other(functions, value):
                found.add(value)
    return found


def load_replacements(symbols_path, replacements_path):
    with open(symbols_path, newline="") as handle:
        addresses = {row["name"]: int(row["address"], 16) for row in csv.DictReader(handle)}
    with open(replacements_path, newline="") as handle:
        names = [row["name"] for row in csv.DictReader(handle)]
    return {addresses[name]: name for name in names}


def load_names(paths):
    names = {}
    for path in paths:
        if Path(path).exists():
            with open(path, newline="") as handle:
                for row in csv.DictReader(handle):
                    names[int(row["address"], 16)] = row["name"]
    return names


def render_replacement(start, name, stats):
    stats["replaced"] += 1
    return [
        f"void {function_name(start)}(wp::Cpu& c) {{",
        f"    WP_ENTER({u32(start)});",
        f'    static const wp::HleFunction handler = wp::find_replacement("{name}");',
        "    handler(c);",
        "}",
    ]


PATCHES = {}


def load_patches(path):
    """address,word[,comment] CSV: replace instruction words before translation (test builds)."""
    if path and Path(path).exists():
        with open(path, newline="") as handle:
            for row in csv.DictReader(handle):
                PATCHES[int(row["address"], 16)] = int(row["word"], 16)


def function_instructions(dol, start, limit):
    words = []
    for address in range(start, limit, 4):
        word = PATCHES.get(address, dol.word_at(address))
        if word is None:
            break
        words.append(word)
    while words and words[-1] == 0:
        words.pop()
    return [Instr(start + 4 * index, word) for index, word in enumerate(words)]


def build_bodies(dol, entries):
    bodies = {}
    for index, start in enumerate(entries):
        section_end = dol.code_section_end(start)
        next_entry = entries[index + 1] if index + 1 < len(entries) else section_end
        instrs = function_instructions(dol, start, min(next_entry, section_end))
        if instrs:
            bodies[start] = instrs
    return bodies


def is_code_address(dol, address):
    return address % 4 == 0 and dol.code_section_end(address) is not None and dol.word_at(address) not in (None, 0)


def load_extra_targets(path):
    if not Path(path).exists():
        return set()
    with open(path, newline="") as handle:
        return {int(row["address"], 16) for row in csv.DictReader(handle)}


def discover_entries(dol, functions, extra=()):
    entries = {start for start, _ in functions} | data_pointers(dol, functions)
    entries |= {address for address in extra if is_code_address(dol, address)}
    while True:
        found = set()
        for start, instrs in build_bodies(dol, sorted(entries)).items():
            end = start + 4 * len(instrs)
            for instr in instrs:
                if instr.mn not in ("b", "bc"):
                    continue
                target = instr.target
                outside = not (start <= target < end)
                if (instr.lk or outside) and target not in entries and is_code_address(dol, target):
                    found.add(target)
            found.update(v for v in cfg.code_constants(instrs) if v not in entries and is_code_address(dol, v))
            tables, _ = cfg.find_jump_tables(instrs, dol)
            for targets in tables.values():
                found.update(t for t in targets if t not in entries and not (start <= t < end))
        if not found:
            return sorted(entries)
        entries |= found


def falls_through(instr):
    if instr.mn == "b":
        return bool(instr.lk)
    if instr.mn in ("bclr", "bcctr"):
        return instr.bo != 20 or bool(instr.lk)
    return instr.mn != "rfi"


def render_function(start, instrs, entries, dol, stats, replacements, save_context, set_jump, hooks=frozenset()):
    if start in replacements:
        lines = render_replacement(start, replacements[start], stats)
        # Also translate the original body as f_XXXXXXXX_orig, for handlers that fall back to it.
        scratch = {"functions": 0, "instructions": 0, "illegal": 0, "jump_tables": 0, "unresolved_jumps": 0,
                   "replaced": 0, "unsupported": collections.Counter(), "resumes": []}
        original = render_function(start, instrs, entries, dol, scratch, {}, save_context, set_jump, hooks)
        original[0] = original[0].replace(function_name(start) + "(", function_name(start) + "_orig(", 1)
        stats.setdefault("originals", []).append(start)
        return lines + [""] + original
    end = start + 4 * len(instrs)
    tables, unresolved = cfg.find_jump_tables(instrs, dol)
    labels = cfg.find_labels(instrs, start, end, tables)
    emitter = Emitter(start, end, entries, tables, save_context, set_jump)
    lines = [f"void {function_name(start)}(wp::Cpu& c) {{", f"    WP_ENTER({u32(start)});", "    [[maybe_unused]] uint32_t ea = 0;", "    [[maybe_unused]] uint32_t q = 0;"]
    if start in hooks:
        lines.append(f"    wp::sync_hook(c, {u32(start)});")
    body = []
    for instr in instrs:
        if instr.addr in labels:
            body.append(f"{label(instr.addr)}:")
        if instr.addr in emitter.resume_points:
            body.append(f"{resume_label(instr.addr)}:")
        if instr.word == 0:
            continue
        if instr.mn is None:
            stats["illegal"] += 1
        body.extend("    " + line for line in emitter.emit(instr))
    if end in emitter.resume_points:
        body.append(f"{resume_label(end)}:")
    if end in entries and falls_through(instrs[-1]):
        body.append(f"    {function_name(end)}(c);")
    elif end in emitter.resume_points:
        body.append(f"    wp::unresolved_jump(c, {u32(end)});")
    if emitter.save_sites:
        lines.append(f"    std::jmp_buf wp_jump[{emitter.save_sites}];")
    lines.extend(emitter.resume_prologue())
    lines.extend(body)
    lines.append("}")
    stats["functions"] += 1
    stats["instructions"] += len(instrs)
    stats["jump_tables"] += len(tables)
    stats["unresolved_jumps"] += unresolved
    stats["unsupported"].update(emitter.unsupported)
    stats["resumes"].extend((address, start) for address in emitter.resume_points)
    return lines


def write_sources(output, chunks, entries, names, resumes, originals=()):
    output.mkdir(parents=True, exist_ok=True)
    written = set()

    def emit(name, text):
        path = output / name
        written.add(name)
        if not path.exists() or path.read_text() != text:
            path.write_text(text)

    for index, chunk in enumerate(chunks):
        body = ['#include "wp/cpu.h"', '#include "wp/hle.h"', '#include "wp/sync.h"', '#include "wp/threads.h"', '#include "functions.h"', ""] + chunk
        emit(f"dol_{index:03d}.cpp", "\n".join(body) + "\n")
    declarations = ["#pragma once", '#include "wp/cpu.h"', ""]
    declarations += [f"void {function_name(a)}(wp::Cpu& c);" for a in entries]
    declarations += [f"void {function_name(a)}_orig(wp::Cpu& c);" for a in originals]
    emit("functions.h", "\n".join(declarations) + "\n")
    table = ['#include "wp/function_table.h"', '#include "functions.h"', "", "namespace wp {", "", "const FunctionEntry g_function_table[] = {"]
    table += [f"    {{{u32(a)}, {function_name(a)}}}," for a in entries]
    table += ["};", "", f"const size_t g_function_count = {len(entries)};", "", "const FunctionEntry g_resume_table[] = {"]
    table += [f"    {{{u32(a)}, {function_name(f)}}}," for a, f in sorted(resumes)]
    table += ["};", "", f"const size_t g_resume_count = {len(resumes)};", "", "const FunctionEntry g_original_table[] = {"]
    table += [f"    {{{u32(a)}, {function_name(a)}_orig}}," for a in sorted(originals)] or ["    {0, nullptr},"]
    table += ["};", "", f"const size_t g_original_count = {len(originals)};", "", "}"]
    emit("function_table.cpp", "\n".join(table) + "\n")
    listing = ['#include "wp/function_table.h"', "", "namespace wp {", "", "const NameEntry g_name_table[] = {"]
    listing += [f'    {{{u32(a)}, "{n}"}},' for a, n in sorted(names.items())]
    listing += ['    {0, ""},', "};", "", f"const size_t g_name_count = {len(names)};", "", "}"]
    emit("names.cpp", "\n".join(listing) + "\n")
    for old in list(output.glob("*.cpp")) + list(output.glob("*.h")):
        if old.name not in written:
            old.unlink()


def main():
    parser = argparse.ArgumentParser()
    current = game.load()
    parser.add_argument("--dol", default=str(current.dol))
    parser.add_argument("--functions", default=str(current.analysis / "dol_functions.csv"))
    parser.add_argument("--symbols", default=str(current.analysis / "symbols.csv"))
    parser.add_argument("--dolphin-symbols", default=str(current.dolphin_symbols))
    parser.add_argument("--module-targets", default=str(current.module_targets))
    parser.add_argument("--replacements", default=str(current.analysis / "hle_functions.csv"))
    parser.add_argument("--output", default=str(current.dol_code))
    parser.add_argument("--hooks", default=str(current.analysis / "sync_hooks.csv"))
    parser.add_argument("--patches", help="instruction patches (test builds only)")
    args = parser.parse_args()

    dol = dol_module.load(args.dol)
    load_patches(args.patches)
    if PATCHES:
        print(f"applying {len(PATCHES)} instruction patches from {args.patches}")
    replacements = load_replacements(args.symbols, args.replacements)
    names = load_names([args.dolphin_symbols, args.symbols])
    save_context = next((address for address, name in names.items() if name == "OSSaveContext"), None)
    set_jump = next((address for address, name in names.items() if name == "setjmp"), None)
    hooks = frozenset(load_extra_targets(args.hooks))
    entries = discover_entries(dol, load_functions(args.functions), load_extra_targets(args.module_targets))
    bodies = build_bodies(dol, entries)
    entries = sorted(bodies)
    entry_set = set(entries)
    stats = {"functions": 0, "instructions": 0, "illegal": 0, "jump_tables": 0, "unresolved_jumps": 0, "replaced": 0, "unsupported": collections.Counter(), "resumes": []}

    chunks = []
    current = []
    for start in entries:
        current.extend(render_function(start, bodies[start], entry_set, dol, stats, replacements, save_context, set_jump, hooks))
        current.append("")
        if len(current) >= LINES_PER_FILE:
            chunks.append(current)
            current = []
    if current:
        chunks.append(current)

    write_sources(Path(args.output), chunks, entries, names, stats["resumes"], stats.get("originals", []))
    print(f"functions {stats['functions']}, instructions {stats['instructions']}, files {len(chunks)}")
    print(f"illegal words {stats['illegal']}, jump tables {stats['jump_tables']}, unresolved indirect jumps {stats['unresolved_jumps']}")
    print(f"replaced by runtime handlers {stats['replaced']}")
    print(f"unsupported {dict(stats['unsupported'])}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
