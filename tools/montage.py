#!/usr/bin/env python3
"""tools/montage.py OUT.png COLS WIDTH img1 img2 ...  -> grid image (ffmpeg)."""
import subprocess, sys
out, cols, width, files = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), sys.argv[4:]
height = width * 448 // 640 // 2 * 2
args = ["ffmpeg", "-loglevel", "error", "-y"]
for f in files:
    args += ["-i", f]
chains = "".join(f"[{i}]scale={width}:{height}[a{i}];" for i in range(len(files)))
layout = "|".join(f"{(i % cols) * width}_{(i // cols) * height}" for i in range(len(files)))
inputs = "".join(f"[a{i}]" for i in range(len(files)))
graph = chains + (f"{inputs}xstack=inputs={len(files)}:layout={layout}:fill=black" if len(files) > 1 else "[a0]null")
subprocess.run(args + ["-filter_complex", graph, out], check=True)
