import argparse
import csv
import re
import sys
from pathlib import Path

import game

LINE = re.compile(r"^([0-9a-f]{8}) ([0-9a-f]+) ([0-9a-f]{8}) \d+ (\S.*)$")
INVALID = re.compile(r"[^A-Za-z0-9_$.]+")
UNNAMED = re.compile(r"^zz_[0-9a-f]{8}_$")


def sanitize(name):
    return INVALID.sub("_", name).strip("_")


def main():
    parser = argparse.ArgumentParser()
    current = game.load()
    parser.add_argument("--map", default=str(current.symbol_map))
    parser.add_argument("--output", default=str(current.dolphin_symbols))
    args = parser.parse_args()

    symbols = {}
    with open(args.map, encoding="utf-8", errors="replace") as handle:
        for line in handle:
            match = LINE.match(line.rstrip("\n"))
            if not match:
                continue
            raw = match.group(4).split("\t")[0].strip()
            name = sanitize(raw)
            if name and not UNNAMED.match(raw):
                symbols[int(match.group(1), 16)] = name

    output = Path(args.output)
    output.parent.mkdir(parents=True, exist_ok=True)
    with open(output, "w", newline="") as handle:
        writer = csv.writer(handle, lineterminator="\n")
        writer.writerow(["address", "name"])
        for address in sorted(symbols):
            writer.writerow([f"0x{address:08x}", symbols[address]])
    print(f"{len(symbols)} named symbols written to {output}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
