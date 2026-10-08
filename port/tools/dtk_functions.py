#!/usr/bin/env python3
"""Turn a decomp-toolkit symbols.txt into the recompiler's analysis CSVs.

  dol_functions.csv  address,size of every function dtk found in .init/.text
  symbols.csv        address,name of dtk's named SDK/runtime functions plus
                     the names in symbols_extra.csv (hand-identified)
"""
import argparse
import csv
import re
from pathlib import Path

LINE = re.compile(r"(\S+) = (\.init|\.text):0x([0-9A-Fa-f]+); // type:function size:0x([0-9A-Fa-f]+)")
AUTO = re.compile(r"^(fn|dtor|ctor|lbl|sinit)_[0-9A-F]{8}$")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("symbols")
    ap.add_argument("analysis")
    ap.add_argument("--dsy", help="address,name CSV from tools/apply_dsy.py (Dolphin signature matches)")
    args = ap.parse_args()
    out = Path(args.analysis)
    functions, names = [], {}
    for line in open(args.symbols):
        m = LINE.match(line)
        if not m:
            continue
        name, address, size = m.group(1), int(m.group(3), 16), int(m.group(4), 16)
        functions.append((address, size))
        if not AUTO.match(name):
            names[address] = name
    if args.dsy and Path(args.dsy).exists():
        # Dolphin signature names fill in auto-named functions. Names from libraries this game
        # doesn't link (EAD's J*, SComponent) are false positives of tiny functions.
        reserved = set()
        for path in (out / "symbols_extra.csv", out / "hle_functions.csv"):
            if path.exists():
                reserved |= {row["name"] for row in csv.DictReader(open(path))}
        for row in csv.DictReader(open(args.dsy)):
            text, _, lib = row["name"].partition("\t")
            name = text.split("(")[0].strip()
            lib = lib.strip()
            if not name:
                continue
            if lib.startswith("J") or lib.startswith("SComponent") or name in reserved:
                continue
            address = int(row["address"], 16)
            if address not in names:
                names[address] = name
    extra = out / "symbols_extra.csv"
    if extra.exists():
        for row in csv.DictReader(open(extra)):
            names[int(row["address"], 16)] = row["name"]
    with open(out / "dol_functions.csv", "w", newline="") as f:
        f.write("address,size\n")
        for address, size in sorted(set(functions)):
            f.write(f"{address:#010x},{size}\n")
    with open(out / "symbols.csv", "w", newline="") as f:
        writer = csv.writer(f, lineterminator="\n")
        writer.writerow(["address", "name"])
        for address, name in sorted(names.items()):
            writer.writerow([f"{address:#010x}", name])
    print(f"{len(functions)} functions, {len(names)} names")


if __name__ == "__main__":
    main()
