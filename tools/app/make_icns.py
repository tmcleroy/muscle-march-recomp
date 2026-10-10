#!/usr/bin/env python3
"""Draw a simple original app icon (no game art) and pack it with iconutil."""
import math, struct, subprocess, sys, tempfile, zlib
from pathlib import Path

def png(size):
    rows = []
    c = size / 2
    for y in range(size):
        row = bytearray([0])
        for x in range(size):
            dx, dy = (x + 0.5 - c) / c, (y + 0.5 - c) / c
            r = math.hypot(dx, dy)
            if r > 0.94:
                row += bytes([0, 0, 0, 0]); continue
            # orange-to-red radial gradient with diagonal "march" stripes
            t = r / 0.94
            base = (255, int(150 - 90 * t), int(40 - 30 * t))
            stripe = (int((dx + dy) * 6) % 2 == 0) and r < 0.7
            col = (255, 215, 60) if stripe else base
            if r > 0.86:
                col = (120, 30, 10)
            row += bytes([*col, 255])
        rows.append(bytes(row))
    raw = zlib.compress(b"".join(rows), 9)
    chunk = lambda t, d: struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xffffffff)
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0)) + chunk(b"IDAT", raw) + chunk(b"IEND", b"")

def main():
    if sys.argv[1] == "--android":  # launcher icons as Android resources: <folder>/mipmap-*/ic_launcher.png
        for density, size in (("mdpi", 48), ("hdpi", 72), ("xhdpi", 96), ("xxhdpi", 144), ("xxxhdpi", 192)):
            folder = Path(sys.argv[2]) / f"mipmap-{density}"
            folder.mkdir(parents=True, exist_ok=True)
            (folder / "ic_launcher.png").write_bytes(png(size))
        return
    out = Path(sys.argv[1])
    with tempfile.TemporaryDirectory() as tmp:
        iconset = Path(tmp) / "icon.iconset"
        iconset.mkdir()
        for s in (16, 32, 128, 256, 512):
            (iconset / f"icon_{s}x{s}.png").write_bytes(png(s))
            (iconset / f"icon_{s}x{s}@2x.png").write_bytes(png(s * 2))
        subprocess.run(["iconutil", "-c", "icns", str(iconset), "-o", str(out)], check=True)


if __name__ == "__main__":
    main()
