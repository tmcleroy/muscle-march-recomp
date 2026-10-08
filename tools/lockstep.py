#!/usr/bin/env python3
"""Lockstep comparison of the native port against Dolphin.

Both sides stop at the same guest function (a sync point; by default
VIWaitForRetrace, which the main thread calls once per frame) and record a
hash of every 4 KiB page of the chosen guest memory ranges:

  tools/lockstep.py dolphin --hits 300 -o build/lockstep/dolphin.txt
  WP_SYNC_OUT=... WP_SYNC_LIMIT=300 <port>        (make lockstep does both)
  tools/lockstep.py compare build/lockstep/dolphin.txt build/lockstep/port.txt

`compare` reports the first sync hit at which any page differs and which
pages (with the nearest symbol). `--dump N` on both sides writes the raw bytes
at hit N so `compare --bytes` can list the differing words.

Dolphin side: our own user folder (dolphin_user/) with the GDB stub on
--port; the stub halts the CPU until we connect and continue.
"""
import argparse
import bisect
import re
import os
import signal
import socket
import subprocess
import sys
import time
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "port" / "tools"))
os.environ.setdefault("WP_GAME", "musclemarch")
sys.path.insert(0, str(ROOT / "port" / "recompiler"))
from dolphin_gdb import Session, GdbError  # noqa: E402

DOLPHIN = "/Applications/Dolphin.app/Contents/MacOS/Dolphin"
TITLE = "00010001574d4d45"
SYNC = 0x801D5720
RANGES = "8027dc60-80424cd0"
PAGE = int(os.environ.get("LOCKSTEP_GRANULE", "100"), 16)  # must match WP_SYNC_GRANULE
CHUNK = 0x800
SYMBOLS = ROOT / "config" / "WMME01" / "symbols.txt"


class FastSession(Session):
    """Session with buffered packet reads (the upstream one is byte-at-a-time)."""

    def read_packet(self):
        buf = self.buffer
        while True:
            start = buf.find(b"$")
            if start >= 0:
                end = buf.find(b"#", start)
                if end >= 0 and len(buf) >= end + 3:
                    self.buffer = buf[end + 3:]
                    self.sock.sendall(b"+")
                    return buf[start + 1:end].decode("ascii")
            data = self.sock.recv(1 << 16)
            if not data:
                raise GdbError("connection closed")
            buf += data
            self.buffer = buf


def parse_ranges(text):
    out = []
    for item in text.split(","):
        a, b = item.split("-")
        out.append((int(a, 16), int(b, 16)))
    return out


def page_hash(data):
    return zlib.crc32(data)


def page_hashes(blob, ranges):
    hashes, offset = [], 0
    for start, end in ranges:
        for page in range(start, end, PAGE):
            size = min(PAGE, end - page)
            hashes.append(page_hash(blob[offset:offset + size]))
            offset += size
    return hashes


def launch_dolphin(port):
    subprocess.run([sys.executable, str(ROOT / "tools" / "dolphin_config.py"), str(ROOT / "dolphin_user")], check=True)
    proc = subprocess.Popen([DOLPHIN, "-u", str(ROOT / "dolphin_user"), "-b", "-n", TITLE,
                             "-C", f"Dolphin.General.GDBPort={port}", "-C", "Dolphin.Interface.DebugModeEnabled=True"],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    deadline = time.time() + 30
    while time.time() < deadline:
        try:
            return proc, FastSession("127.0.0.1", port, 30.0)
        except OSError:
            time.sleep(0.3)
    proc.terminate()
    raise SystemExit("Dolphin's GDB stub did not open")


def read_blob(session, ranges):
    blob = b""
    for start, end in ranges:
        address = start
        while address < end:
            size = min(CHUNK, end - address)
            reply = session.send(f"m{address:x},{size:x}")
            if len(reply) == 3 and reply.startswith("E"):
                raise GdbError(f"read error at {address:08x}")
            blob += bytes.fromhex(reply)
            address += size
    return blob


def cmd_dolphin(args):
    ranges = parse_ranges(args.ranges)
    out_path = Path(args.output)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    proc, session = launch_dolphin(args.port)
    t0 = time.time()
    try:
        session.send("?")
        session.send(f"Z0,{args.sync:x},4")
        with open(out_path, "w") as out:
            for hit in range(args.hits):
                session.send("c", wait=False)
                session.wait_stop(args.timeout)
                lr = int(session.send("p43"), 16)
                blob = read_blob(session, ranges)
                hashes = page_hashes(blob, ranges)
                out.write(f"{hit} {args.sync:08x} {lr:08x} " + " ".join(f"{h:08x}" for h in hashes) + "\n")
                if hit == args.dump:
                    Path(f"{out_path}.{hit}.bin").write_bytes(blob)
                if hit % 50 == 0:
                    print(f"hit {hit} ({time.time() - t0:.0f}s)", file=sys.stderr)
    finally:
        session.close()
        proc.send_signal(signal.SIGTERM)
        try:
            proc.wait(5)
        except subprocess.TimeoutExpired:
            proc.kill()
    print(f"{args.hits} hits in {time.time() - t0:.0f}s -> {out_path}")


def load_sync(path):
    rows = []
    for line in open(path):
        parts = line.split()
        rows.append((int(parts[0]), int(parts[2], 16), parts[3:]))
    return rows


def load_symbols():
    """All of dtk's symbols (functions and data), as sorted (address, name)."""
    names = []
    if SYMBOLS.exists():
        for line in open(SYMBOLS):
            m = re.match(r"(\S+) = \S+:0x([0-9A-Fa-f]+);", line)
            if m:
                names.append((int(m.group(2), 16), m.group(1)))
    return sorted(names)


# Code regions of main.dol (from version-string anchors and the call graph).
REGIONS = [
    (0x80004000, 0x8001FF40, "init"),
    (0x8001FF40, 0x8014F4DC, "game"),
    (0x8014F4DC, 0x80165000, "runtime/MSL/TRK"),
    (0x80165000, 0x8016DD00, "SDK AI/AX/sound"),
    (0x8016DD00, 0x8019B000, "SDK Bluetooth"),
    (0x8019B000, 0x801F1400, "SDK OS/GX/IO/WPAD"),
    (0x801F1400, 0x80248000, "NU engine"),
    (0x80248000, 0x8026E440, "SDK CNT/HBM"),
]


def region_of(address):
    for start, end, name in REGIONS:
        if start <= address < end:
            return name
    return "?"


_owners = None


def owners(symbol):
    """Code regions whose functions reference a data symbol (needs build/callgraph.pickle)."""
    global _owners
    if _owners is None:
        _owners = {}
        cache = ROOT / "build" / "callgraph.pickle"
        if cache.exists():
            import pickle
            for f in pickle.load(open(cache, "rb")).values():
                for ref in f["refs"]:
                    _owners.setdefault(ref, set()).add(region_of(f["addr"] or 0))
    return ",".join(sorted(_owners.get(symbol, []))) or "-"


def nearest(names, address):
    i = bisect.bisect_right(names, (address, "￿")) - 1
    return f"{names[i][1]}+{address - names[i][0]:#x}" if i >= 0 else "?"


def symbol_extents():
    """dtk symbols with sizes: name -> (start, end)."""
    out = {}
    if SYMBOLS.exists():
        for line in open(SYMBOLS):
            m = re.match(r"(\S+) = \S+:0x([0-9A-Fa-f]+);.*size:0x([0-9A-Fa-f]+)", line)
            if m:
                start = int(m.group(2), 16)
                out[m.group(1)] = (start, start + int(m.group(3), 16))
    return out


def ignored_granules(pages, ignore_file):
    """Indexes of granules lying entirely inside symbols listed in ignore_file
    (SDK/hardware state that legitimately differs between Dolphin and the port)."""
    path = Path(ignore_file)
    if not path.exists():
        return set()
    extents = symbol_extents()
    spans = []
    for line in open(path):
        name = line.split("#")[0].strip()
        if name in extents:
            spans.append(extents[name])
        elif name:
            print(f"warning: ignore list symbol {name} not found", file=sys.stderr)
    covered = set()
    for start, end in spans:
        covered.update(range(start, end))
    return {k for k, page in enumerate(pages) if all(a in covered for a in range(page, page + PAGE))}


def page_addresses(ranges):
    return [page for start, end in ranges for page in range(start, end, PAGE)]


def cmd_compare(args):
    ranges = parse_ranges(args.ranges)
    pages = page_addresses(ranges)
    a, b = load_sync(args.reference), load_sync(args.candidate)
    n = min(len(a), len(b))
    ignore = {int(x, 16) for x in args.ignore.split(",")} if args.ignore else set()
    ignored = ignored_granules(pages, args.ignore_file)
    if args.ignore_owners:
        names = load_symbols()
        skip = set(args.ignore_owners.split(","))
        for k, page in enumerate(pages):
            o = owners(nearest(names, page).split("+")[0])
            if o != "-" and set(o.split(",")) <= skip:
                ignored.add(k)

    def differing(i):
        return [k for k, (x, y) in enumerate(zip(a[i][2], b[i][2])) if x != y and pages[k] not in ignore and k not in ignored]

    baseline = set(differing(args.baseline)) if args.baseline >= 0 else set()
    if baseline:
        print(f"baseline: {len(baseline)} granules already differ at hit {args.baseline}; ignoring those")
    first = None
    for i in range(n):
        diff_k = [k for k in differing(i) if k not in baseline]
        if args.trend and i % args.trend == 0:
            print(f"  hit {i:5d}: {len(diff_k)} new differing granules, lr {a[i][1]:08x}/{b[i][1]:08x}")
        if diff_k and first is None:
            first = i
            diff = [pages[k] for k in diff_k]
            if not args.trend:
                break
    if first is None:
        print(f"no differences in {n} sync hits")
        return 0
    print(f"first difference at sync hit {first} (reference lr {a[first][1]:08x}, candidate lr {b[first][1]:08x})")
    names = load_symbols()
    print(f"{len(diff)} differing granules: " + " ".join(f"{p:08x}({nearest(names, p)})" for p in diff[:24]))
    if args.bytes >= 0:
        ra = Path(f"{args.reference}.{args.bytes}.bin").read_bytes()
        rb = Path(f"{args.candidate}.{args.bytes}.bin").read_bytes()
        offset, shown = 0, 0
        for start, end in ranges:
            for addr in range(start, end, 4):
                o = offset + addr - start
                if ra[o:o + 4] != rb[o:o + 4] and shown < args.limit:
                    sym = nearest(names, addr)
                    print(f"  {addr:08x} {ra[o:o+4].hex()} {rb[o:o+4].hex()}  {sym}  [{owners(sym.split('+')[0])}]")
                    shown += 1
            offset += end - start
    return 1


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    d = sub.add_parser("dolphin")
    d.add_argument("--hits", type=int, default=300)
    d.add_argument("--sync", type=lambda s: int(s, 16), default=SYNC)
    d.add_argument("--ranges", default=RANGES)
    d.add_argument("--port", type=int, default=2159)
    d.add_argument("--timeout", type=float, default=30.0)
    d.add_argument("--dump", type=int, default=-1)
    d.add_argument("-o", "--output", default="build/lockstep/dolphin.txt")
    c = sub.add_parser("compare")
    c.add_argument("reference")
    c.add_argument("candidate")
    c.add_argument("--ranges", default=RANGES)
    c.add_argument("--ignore", default="", help="comma-separated granule addresses to skip")
    c.add_argument("--ignore-file", default=str(ROOT / "tools" / "lockstep_ignore.txt"))
    c.add_argument("--bytes", type=int, default=-1, help="hit whose raw dumps to diff")
    c.add_argument("--limit", type=int, default=64)
    c.add_argument("--baseline", type=int, default=-1, help="ignore granules that already differ at this hit")
    c.add_argument("--ignore-owners", default="SDK AI/AX/sound,SDK Bluetooth,SDK OS/GX/IO/WPAD,SDK CNT/HBM",
                   help="skip data only referenced from these code regions ('' to compare everything)")
    c.add_argument("--trend", type=int, default=0, help="print the difference count every N hits")
    args = ap.parse_args()
    return cmd_dolphin(args) if args.cmd == "dolphin" else cmd_compare(args)


if __name__ == "__main__":
    sys.exit(main())
