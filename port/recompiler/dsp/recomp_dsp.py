import argparse
import csv
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

import game
import inline_ops
from tables import Tables, load_idle_signatures

GAME = game.load()
UCODE_LIST = GAME.analysis / "dsp_ucode.csv"
DOL = GAME.dol
OUTPUT = GAME.dsp_code
IRAM_WORDS = 0x1000
EXCEPTION_VECTORS = [2 * i for i in range(1, 8)]
SR_ACCESS = re.compile(r"(read|write)_reg\(s, 0x13[,)]")


def write_if_changed(path, text):
    if not path.exists() or path.read_text(encoding="utf-8") != text:
        path.write_text(text, encoding="utf-8")


def checksum(data):
    value = 0
    for byte in data:
        value = ((value * 31) ^ byte) & 0xFFFFFFFF
    return value


def instruction_starts(words, tables):
    starts = []
    loop_ends = set()
    loop_starts = {}
    address = 0
    while address < len(words):
        inst = words[address]
        template = tables.template(inst)
        starts.append(address)
        if (inst & 0xFFE0) == 0x0060 or (inst & 0xFF00) == 0x1100:
            if address + 1 < len(words):
                loop_ends.add(words[address + 1])
                loop_starts.setdefault(words[address + 1], set()).add((address + 2) & 0xFFFF)
        elif (inst & 0xFFE0) == 0x0040 or (inst & 0xFF00) == 0x1000:
            loop_ends.add((address + 1) & 0xFFFF)
            loop_starts.setdefault((address + 1) & 0xFFFF, set()).add((address + 1) & 0xFFFF)
        address += template.size
    return starts, loop_ends, loop_starts


def idle_skips(words):
    def word(address):
        return words[address] if address < len(words) else 0

    found = set()
    for signature in load_idle_signatures():
        for address in range(len(words)):
            matched = False
            for i, expected in enumerate(signature):
                if expected == 0:
                    matched = True
                if expected == 0xFFFF:
                    continue
                if expected != word(address + i):
                    break
            if matched:
                found.add(address)
    return found


class Inline:
    def __init__(self, pre, lines, flags, post):
        self.pre = pre
        self.lines = lines
        self.flags = flags
        self.post = post
        text = "\n".join(pre + lines + post)
        self.touches_sr = SR_ACCESS.search(text) is not None
        self.overwrites_flags = flags is not None or "flags16(" in text


def inline_info(inst, imm, tables):
    template = tables.template(inst)
    main = inline_ops.MAIN.get(tables.handler(inst))
    if main is None:
        return None
    pre, post = [], []
    if template.extended:
        ext = inline_ops.EXT.get(tables.ext_handler(inst))
        if ext is None:
            return None
        pre, post = ext(inst)
    lines, flags = main(inst, imm)
    return Inline(pre, lines, flags, post)


class Program:
    def __init__(self, words, tables):
        self.words = words
        self.tables = tables
        self.starts, self.loop_ends, self.loop_starts = instruction_starts(words, tables)
        self.entry = set(self.starts)
        self.idle = idle_skips(words)
        self.info = {}
        self.following = {}
        for address in self.starts:
            inst = words[address]
            template = tables.template(inst)
            self.following[address] = (address + template.size) & 0xFFFF
            self.info[address] = inline_info(inst, self.immediate(address), tables)
        self.deferred = set()
        self.in_chain = set()
        self.clears = set()
        for address in self.starts:
            chain = self.chain_after(address)
            if chain is not None:
                self.deferred.add(address)
                self.in_chain.add(address)
                self.in_chain.update(chain[:-1])
                self.clears.add(chain[-1])

    def immediate(self, address):
        template = self.tables.template(self.words[address])
        return self.words[address + 1] if template.size > 1 and address + 1 < len(self.words) else 0

    def size_at(self, address):
        if address >= len(self.words):
            return 1
        return self.tables.template(self.words[address]).size

    def straight(self, address):
        following = self.following[address]
        return ((following - 1) & 0xFFFF) not in self.loop_ends and following in self.entry

    def chain_after(self, address):
        info = self.info[address]
        if info is None or info.flags is None or not self.straight(address):
            return None
        chain = []
        current = self.following[address]
        while True:
            info = self.info.get(current)
            if info is None or info.touches_sr:
                return None
            chain.append(current)
            if info.overwrites_flags:
                return chain
            if not self.straight(current):
                return None
            current = self.following[current]

    def direct(self, target):
        return target in self.entry and target < IRAM_WORDS and target not in self.idle

    def enter(self, target):
        return [
            f"if (__builtin_expect((s.control_reg & DSP::CR_HALT) != 0, 0)) {{ s.pc = 0x{target:04x}; return cycles; }}",
            f"goto L_{target:04x};",
        ]

    def jump(self, target):
        target &= 0xFFFF
        if self.direct(target) and ((target - 1) & 0xFFFF) not in self.loop_ends:
            return self.enter(target)
        return [f"s.pc = 0x{target:04x};", "goto changed;"]

    def loop_end(self, end):
        if end == 0:
            return []
        lines = [f"if (s.r.st[2] == 0x{end:04x} && s.r.st[3] > 0) {{", "    s.r.st[3]--;", "    if (s.r.st[3] > 0) {"]
        for start in sorted(self.loop_starts.get(end, ())):
            if self.direct(start):
                lines.append(f"        if (s.r.st[0] == 0x{start:04x}) {{")
                lines += [f"            {line}" for line in self.enter(start)]
                lines.append("        }")
        lines += [
            "        s.pc = s.r.st[0];",
            "        goto dispatch;",
            "    }",
            "    s.PopStack(DSP::StackRegister::Call);",
            "    s.PopStack(DSP::StackRegister::LoopAddress);",
            "    s.PopStack(DSP::StackRegister::LoopCounter);",
            "}",
        ]
        return lines

    def fall_through(self, address):
        following = self.following[address]
        lines = []
        if ((following - 1) & 0xFFFF) in self.loop_ends:
            lines += self.loop_end((following - 1) & 0xFFFF)
        if following not in self.entry:
            lines += [f"s.pc = 0x{following:04x};", "goto dispatch;"]
        return lines


def push_loop(following, loop_address, counter):
    return [
        f"s.StoreStack(DSP::StackRegister::Call, 0x{following:04x});",
        f"s.StoreStack(DSP::StackRegister::LoopAddress, 0x{loop_address:04x});",
        f"s.StoreStack(DSP::StackRegister::LoopCounter, {counter});",
    ]


def conditional(code, body):
    if code == 0xF:
        return body
    return [f"if (condition(s, 0x{code:x})) {{"] + [f"    {line}" for line in body] + ["}"]


def branch_body(program, address):
    inst = program.words[address]
    name = program.tables.handler(inst)
    following = program.following[address]
    imm = program.immediate(address)
    code = inst & 0xF
    if name == "jcc":
        return conditional(code, program.jump(imm))
    if name == "call":
        return conditional(code, ["s.StoreStack(DSP::StackRegister::Call, 0x%04x);" % following] + program.jump(imm))
    if name == "ret":
        return conditional(code, ["s.pc = s.PopStack(DSP::StackRegister::Call);", "goto changed;"])
    if name == "jmprcc":
        return conditional(code, [f"s.pc = read_reg(s, {(inst >> 5) & 7});", "goto changed;"])
    if name == "ifcc":
        if following >= IRAM_WORDS:
            return None
        if code == 0xF:
            return []
        skip = (following + program.size_at(following)) & 0xFFFF
        return [f"if (!condition(s, 0x{code:x})) {{"] + [f"    {line}" for line in program.jump(skip)] + ["}"]
    if name in ("bloopi", "loopi", "bloop", "loop") and (imm if name.startswith("b") else following) >= IRAM_WORDS:
        return None
    if name in ("bloopi", "loopi"):
        counter = inst & 0xFF
        loop_address = imm if name == "bloopi" else following
        if counter != 0:
            return push_loop(following, loop_address, counter)
        return program.jump((loop_address + program.size_at(loop_address)) & 0xFFFF)
    if name in ("bloop", "loop"):
        loop_address = imm if name == "bloop" else following
        body = [f"u16 counter = read_reg(s, 0x{inst & 0x1F:x});", "if (counter != 0) {"]
        body += [f"    {line}" for line in push_loop(following, loop_address, "counter")]
        body += ["} else {"]
        body += [f"    {line}" for line in program.jump((loop_address + program.size_at(loop_address)) & 0xFFFF)]
        body += ["}"]
        return body
    return None


def emit_instruction(program, address, slow):
    words = program.words
    tables = program.tables
    inst = words[address]
    template = tables.template(inst)
    following = program.following[address]
    lines = [
        f"L_{address:04x}:",
        f"    if (__builtin_expect(cycles <= 0 || s.exceptions != 0, 0)) goto S_{address:04x};",
        f"B_{address:04x}:",
        "    cycles--;",
    ]
    slow += [
        f"S_{address:04x}:",
        f"    if (cycles <= 0) {{ s.pc = 0x{address:04x}; return cycles; }}",
        f"    cycles--; s.pc = 0x{address:04x};",
        "    if (s.CheckExceptions()) goto exception;",
        f"    cycles++; goto B_{address:04x};",
    ]
    if address in EXCEPTION_VECTORS:
        lines.append(f"E_{address:04x}:")
    info = program.info[address]
    if info is not None:
        body = info.pre + info.lines
        if address in program.deferred:
            body += inline_ops.deferred_flag_statements(info.flags)
        elif info.flags:
            body.append(inline_ops.flag_statement(info.flags))
        if address in program.clears and address not in program.deferred:
            body.append("pending = false;")
        body += info.post
        lines.append("    {")
        lines += [f"        {line}" for line in body]
        lines.append("    }")
        if address in program.in_chain:
            lines.append(f"    if (__builtin_expect(cycles <= 0 || s.exceptions != 0, 0)) {{ {inline_ops.MATERIALIZE} goto S_{following:04x}; }}")
            lines.append(f"    goto B_{following:04x};")
            return lines, True
        lines += [f"    {line}" for line in program.fall_through(address)]
        return lines, True
    body = branch_body(program, address)
    if body is not None:
        lines.append("    {")
        lines += [f"        {line}" for line in body]
        lines.append("    }")
        lines += [f"    {line}" for line in program.fall_through(address)]
        return lines, True
    lines.append(f"    s.pc = 0x{(address + 1) & 0xFFFF:04x};")
    if template.extended:
        lines.append(f"    in.{tables.ext_handler(inst)}(0x{inst:04x});")
    lines.append(f"    in.{tables.handler(inst)}(0x{inst:04x});")
    if template.extended:
        lines.append("    in.ApplyWriteBackLog();")
    lines.append(f"    if (s.pc != 0x{following:04x}) goto changed;")
    lines += [f"    {line}" for line in program.fall_through(address)]
    return lines, False


def emit_function(name, words, tables):
    program = Program(words, tables)
    out = [
        f"int {name}(DSP::Interpreter::Interpreter& in, DSP::SDSP& s, int cycles, bool& idle) {{",
        "    s64 fv = 0;",
        "    bool fc = false;",
        "    bool fo = false;",
        "    bool pending = false;",
        "    if (cycles <= 0 || (s.control_reg & DSP::CR_HALT)) return cycles;",
        "    goto select;",
        "changed:",
        "    if (s.GetAnalyzer().IsLoopEnd(static_cast<u16>(s.pc - 1))) in.HandleLoop();",
        "dispatch:",
        "    if (cycles <= 0 || (s.control_reg & DSP::CR_HALT)) return cycles;",
        "    if (s.GetAnalyzer().IsIdleSkip(s.pc)) { idle = true; return cycles; }",
        "select:",
        "    switch (s.pc) {",
    ]
    for address in program.starts:
        out.append(f"    case 0x{address:04x}: goto L_{address:04x};")
    out.append("    default: return cycles;")
    out.append("    }")
    out.append("exception:")
    out.append("    switch (s.pc) {")
    for vector in EXCEPTION_VECTORS:
        if vector in program.entry:
            out.append(f"    case 0x{vector:04x}: goto E_{vector:04x};")
    out.append("    default: cycles++; return cycles;")
    out.append("    }")
    slow = []
    inlined = 0
    for address in program.starts:
        lines, was_inlined = emit_instruction(program, address, slow)
        out.extend(lines)
        inlined += was_inlined
    out.extend(slow)
    out.append("}")
    return out, inlined, len(program.starts), len(program.deferred)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--dol", default=str(DOL))
    parser.add_argument("--out", default=str(OUTPUT))
    args = parser.parse_args()
    tables = Tables()
    dol = Path(args.dol).read_bytes()
    out_dir = Path(args.out)
    out_dir.mkdir(parents=True, exist_ok=True)
    entries = []
    with UCODE_LIST.open(newline="", encoding="utf-8") as handle:
        for row in csv.DictReader(handle):
            offset = int(row["dol_offset"], 16)
            size = int(row["size"], 16)
            data = dol[offset:offset + size]
            crc = checksum(data)
            if crc != int(row["checksum"], 16):
                raise SystemExit(f"{row['name']}: checksum {crc:08x} does not match {row['checksum']}")
            words = [int.from_bytes(data[i:i + 2], "big") for i in range(0, size, 2)][:IRAM_WORDS]
            function = f"run_{row['name']}"
            body = [
                '#include "Core/DSP/DSPAnalyzer.h"',
                '#include "Core/DSP/DSPCore.h"',
                '#include "Core/DSP/Interpreter/DSPInterpreter.h"',
                '#include "wp/dsp_inline.h"',
                "",
                "namespace wp::dsp::generated {",
                "",
                "using namespace wp::dsp::ops;",
                "",
            ]
            code, inlined, total, deferred = emit_function(function, words, tables)
            body += code
            body += ["", "}", ""]
            write_if_changed(out_dir / f"{row['name']}.cpp", "\n".join(body))
            entries.append((row["name"], crc, function))
            print(f"{row['name']}: {len(words)} words, checksum {crc:08x}, {inlined} of {total} instructions inline, {deferred} with deferred flags")
    registry = [
        '#include "wp/dsp_translated.h"',
        "",
        "namespace wp::dsp::generated {",
        "",
    ]
    for _, _, function in entries:
        registry.append(f"int {function}(DSP::Interpreter::Interpreter& in, DSP::SDSP& s, int cycles, bool& idle);")
    registry += ["", "}", "", "namespace wp::dsp {", "", "const TranslatedCode g_translated_code[] = {"]
    for _, crc, function in entries:
        registry.append(f"    {{0x{crc:08x}u, &generated::{function}}},")
    registry += ["};", "", "const size_t g_translated_code_count = sizeof(g_translated_code) / sizeof(g_translated_code[0]);", "", "}", ""]
    write_if_changed(out_dir / "registry.cpp", "\n".join(registry))


if __name__ == "__main__":
    main()
