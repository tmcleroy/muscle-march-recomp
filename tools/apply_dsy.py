#!/usr/bin/env python3
"""Name functions by matching Dolphin's signature database (totaldb.dsy, shipped
with Dolphin) against dtk's function list. Uses Dolphin's opcode-only checksum
(SignatureDB.cpp: HashSignatureDB::ComputeCodeChecksum).

  tools/apply_dsy.py [--dsy PATH] > build/dsy_names.csv     (address,name)
Accepts a match only when the size agrees and the hash is unique in our DOL.
"""
import argparse, collections, re, struct, sys
from pathlib import Path
ROOT = Path(__file__).resolve().parent.parent

def checksum(words):
    s = 0
    for w in words:
        op = w & 0xFC000000; op2 = op3 = 0; aux = op >> 26
        if aux == 4:
            op2 = w & 0x3F
            if op2 in (0, 8, 16, 21, 22):
                op3 = w & 0x7C0
        elif aux in (7, 8, 10, 11, 12, 13, 14, 15):
            op2 = w & 0x03FF0000
        elif aux in (19, 31, 63):
            op2 = w & 0x7FF
        elif aux == 59:
            op2 = w & 0x3F
            if op2 < 16:
                op3 = w & 0x7C0
        elif 32 <= aux < 56:
            op2 = w & 0x03FF0000
        s = ((s << 17) & 0xFFFE0000) | ((s >> 15) & 0x0001FFFF)
        s ^= (op | op2 | op3)
    return s

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dsy", default="/Applications/Dolphin.app/Contents/Resources/Sys/totaldb.dsy")
    ap.add_argument("--dol", default=str(ROOT / "extracted" / "main.dol"))
    ap.add_argument("--symbols", default=str(ROOT / "config" / "WMME01" / "symbols.txt"))
    args = ap.parse_args()
    raw = open(args.dsy, "rb").read()
    count = struct.unpack("<I", raw[:4])[0]
    db = {}
    for i in range(count):
        c, size = struct.unpack("<II", raw[4 + i * 136: 12 + i * 136])
        name = raw[12 + i * 136: 4 + (i + 1) * 136].split(b"\0")[0].decode("latin1")
        db[c] = (name, size)
    dol = open(args.dol, "rb").read()
    offs = struct.unpack(">18I", dol[:0x48]); addrs = struct.unpack(">18I", dol[0x48:0x90]); sizes = struct.unpack(">18I", dol[0x90:0xd8])
    def word(a):
        for i in range(7):
            if sizes[i] and addrs[i] <= a < addrs[i] + sizes[i]:
                o = offs[i] + a - addrs[i]
                return struct.unpack(">I", dol[o:o + 4])[0]
    funcs = []
    for line in open(args.symbols):
        m = re.match(r"(\S+) = (\.init|\.text):0x([0-9A-F]+); // type:function size:0x([0-9A-F]+)", line)
        if m:
            funcs.append((int(m.group(3), 16), int(m.group(4), 16), m.group(1)))
    hashes = collections.defaultdict(list)
    for a, size, name in funcs:
        words = [word(a + 4 * i) for i in range(size // 4)]
        while words and words[-1] == 0:
            words.pop()
        hashes[checksum(words)].append((a, len(words) * 4, name))
    matched = 0
    import csv as _csv
    out = _csv.writer(sys.stdout, lineterminator="\n")
    out.writerow(["address", "name"])
    for h, fs in sorted(hashes.items(), key=lambda x: x[1][0][0]):
        if h in db and len(fs) == 1:
            a, size, old = fs[0]
            name, dsize = db[h]
            if dsize == size and len(fs) == 1:
                out.writerow([f"{a:#010x}", name])
                matched += 1
    print(f"{len(db)} signatures, {len(funcs)} functions, {matched} named", file=sys.stderr)

main()
