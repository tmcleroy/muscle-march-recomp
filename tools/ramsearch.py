#!/usr/bin/env python3
"""RAM search over dumps written with WP_CAPTURE_RAM=1.

  tools/ramsearch.py DIR "1:5,2:4,3:2,4:2,5:0" [--region mem1|mem2] [--types u8,u16,u32,f32]
Finds addresses whose value at capture N equals the given value, for every pair.
"""
import argparse, struct, sys
from pathlib import Path
import numpy as np

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dir"); ap.add_argument("spec")
    ap.add_argument("--region", default="mem1,mem2")
    ap.add_argument("--types", default="u8,u16,u32,f32")
    ap.add_argument("--limit", type=int, default=40)
    args = ap.parse_args()
    pairs = [(int(a), float(b)) for a, b in (x.split(":") for x in args.spec.split(","))]
    for region in args.region.split(","):
        base = 0x80000000 if region == "mem1" else 0x90000000
        dumps = {n: np.fromfile(Path(args.dir) / f"gx_capture_{n:03d}.{region}", dtype=np.uint8) for n, _ in pairs}
        for t in args.types.split(","):
            dt = {"u8": ">u1", "u16": ">u2", "u32": ">u4", "f32": ">f4"}[t]
            size = np.dtype(dt).itemsize
            mask = None
            for n, v in pairs:
                arr = dumps[n][: len(dumps[n]) // size * size].view(dt)
                m = np.isclose(arr, v) if t == "f32" else (arr == v)
                mask = m if mask is None else (mask & m)
            hits = np.nonzero(mask)[0]
            print(f"{region} {t}: {len(hits)} matches")
            for i in hits[: args.limit]:
                print(f"   {base + i * size:08x}")

main()
