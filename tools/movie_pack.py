#!/usr/bin/env python3
"""Pack an input movie (version 2) into version 3, with runs of identical samples stored once.

  tools/movie_pack.py IN.wpm OUT.wpm

Recording writes version 2; pack movies before committing them (they shrink ~100-300x).
The game reads both versions.
"""
import struct, sys

MAGIC = b"WPMOVIE"
RECORD = 35


def main():
    data = open(sys.argv[1], "rb").read()
    if data[:7] != MAGIC or data[7] != 2:
        sys.exit(f"{sys.argv[1]}: not a version 2 input movie")
    length = data[8] | (data[9] << 8)
    header_end = 10 + length
    out = bytearray(MAGIC + bytes([3]) + data[8:header_end])
    body = data[header_end:]
    records = [body[i:i + RECORD] for i in range(0, len(body) - RECORD + 1, RECORD)]
    i = 0
    while i < len(records):
        j = i
        while j < len(records) and j - i < 0xFFFF and records[j] == records[i]:
            j += 1
        out += struct.pack("<H", j - i) + records[i]
        i = j
    open(sys.argv[2], "wb").write(out)
    print(f"{len(records)} samples, {len(data)} -> {len(out)} bytes")


if __name__ == "__main__":
    main()
