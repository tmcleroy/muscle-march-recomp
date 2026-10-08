import collections
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "recompiler"))

import dol as dol_module
import game
import recomp
import recomp_rel
import rel
from ppc import cfg

LOOKBACK = 24


def producer(instrs, index, register):
    for k in range(index - 1, max(index - LOOKBACK, -1), -1):
        instr = instrs[k]
        if instr.mn in ("bl", "bctrl", "blrl"):
            return "call result", k
        if instr.mn in ("lwz", "lwzu") and instr.rt == register:
            return "load", k
        if instr.mn == "lwzx" and instr.rt == register:
            return "indexed load", k
        if instr.mn in ("or", "mr") and instr.ra == register:
            return producer(instrs, k, instr.rs)[0], k
        if instr.mn in ("addi", "addis", "ori", "oris") and instr.rt == register:
            return "constant", k
        if instr.mn.startswith(("cmp", "b", "st", "mt", "sync", "isync", "dcb", "icb", "tw")):
            continue
        if instr.rt == register:
            return "computed", k
    return "argument", -1


def classify(instrs, index):
    mtctr = None
    for k in range(index - 1, max(index - LOOKBACK, -1), -1):
        if instrs[k].mn == "mtspr" and instrs[k].spr == 9:
            mtctr = k
            break
    if mtctr is None:
        return "no mtctr"
    kind, _ = producer(instrs, mtctr, instrs[mtctr].rs)
    return kind


def main():
    current = game.load()
    dol = dol_module.load(str(current.dol))
    entries = recomp.discover_entries(dol, recomp.load_functions(str(current.analysis / "dol_functions.csv")), recomp.load_extra_targets(str(current.module_targets)))
    bodies = recomp.build_bodies(dol, entries)
    totals = collections.Counter()
    examples = {}
    for start, instrs in bodies.items():
        tables, _ = cfg.find_jump_tables(instrs, dol)
        for index, instr in enumerate(instrs):
            if instr.mn == "bcctr" and not instr.lk and instr.bo == 20 and instr.addr not in tables:
                kind = classify(instrs, index)
                totals["dol " + kind] += 1
                examples.setdefault("dol " + kind, hex(instr.addr))
    dol_entries = recomp_rel.load_dol_entries()
    for path in sorted(recomp_rel.MODULES.glob("*.rel")):
        module = rel.parse(path.read_bytes())
        relocations = module.relocations()
        by_site = recomp_rel.relocation_map(relocations)
        _, module_bodies = recomp_rel.discover(module, relocations, by_site)
        for start, instrs in module_bodies.items():
            for index, instr in enumerate(instrs):
                if instr.mn == "bcctr" and not instr.lk and instr.bo == 20 and recomp_rel.find_table(instrs, index, by_site, module) is None:
                    kind = classify(instrs, index)
                    totals["modules " + kind] += 1
                    examples.setdefault("modules " + kind, f"{path.stem}+{instr.addr:x}")
    for key, count in sorted(totals.items()):
        print(f"{key}: {count} (for example {examples[key]})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
