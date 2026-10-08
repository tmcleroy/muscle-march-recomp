#!/usr/bin/env python3
"""Call graph + quick queries over dtk's asm (build/WMME01/asm).

  tools/callgraph.py callers  <fn>        functions that bl to <fn>
  tools/callgraph.py callees  <fn>
  tools/callgraph.py refs     <label>     functions referencing a data label
  tools/callgraph.py show     <fn>        print the function's asm
Functions may be given as name or hex address.
"""
import glob, re, sys, pickle, os
from pathlib import Path
ROOT = Path(__file__).resolve().parent.parent
ASM = ROOT / "build" / "WMME01" / "asm"
CACHE = ROOT / "build" / "callgraph.pickle"

def build():
    funcs, order = {}, []
    cur = None
    for f in sorted(glob.glob(str(ASM / "*.s"))):
        for line in open(f, encoding="utf-8", errors="replace"):
            m = re.match(r"^\.fn (\S+),", line)
            if m:
                cur = {"name": m.group(1), "addr": None, "lines": [], "callees": set(), "refs": set()}
                funcs[cur["name"]] = cur
                continue
            if line.startswith(".endfn"):
                cur = None
                continue
            if cur is None:
                continue
            cur["lines"].append(line.rstrip("\n"))
            m = re.match(r"/\* ([0-9A-F]{8}) ", line)
            if m and cur["addr"] is None:
                cur["addr"] = int(m.group(1), 16)
            m = re.search(r"\tbl (\S+)", line)
            if m:
                cur["callees"].add(m.group(1))
            for lab in re.findall(r"\b((?:lbl|fn|gap)_[0-9A-F]{8}|[A-Za-z_][A-Za-z0-9_]*)@(?:ha|l|sda21)", line):
                cur["refs"].add(lab)
    return funcs

def load():
    if CACHE.exists() and CACHE.stat().st_mtime > max(os.path.getmtime(p) for p in glob.glob(str(ASM / "*.s"))):
        return pickle.load(open(CACHE, "rb"))
    funcs = build()
    pickle.dump(funcs, open(CACHE, "wb"))
    return funcs

def resolve(funcs, key):
    if key in funcs:
        return key
    try:
        a = int(key, 16)
    except ValueError:
        sys.exit(f"unknown {key}")
    for n, f in funcs.items():
        if f["addr"] == a:
            return n
    sys.exit(f"no function at {key}")

def main():
    funcs = load()
    cmd, key = sys.argv[1], sys.argv[2]
    if cmd == "refs":
        for n, f in sorted(funcs.items(), key=lambda x: x[1]["addr"] or 0):
            if key in f["refs"]:
                print(f"{f['addr']:08x} {n}")
        return
    name = resolve(funcs, key)
    if cmd == "callers":
        for n, f in sorted(funcs.items(), key=lambda x: x[1]["addr"] or 0):
            if name in f["callees"]:
                print(f"{f['addr']:08x} {n}")
    elif cmd == "callees":
        for c in sorted(funcs[name]["callees"]):
            a = funcs[c]["addr"] if c in funcs else 0
            print(f"{a or 0:08x} {c}")
    elif cmd == "show":
        print("\n".join(funcs[name]["lines"]))

main()
