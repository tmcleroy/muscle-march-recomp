#!/usr/bin/env python3
"""Golden replay tests: replay input movies deterministically and compare state hashes.

  tools/replay_test.py [--bless] [--frames] [movies/NAME.wpm ...]

Each movie (default: every movies/*.wpm) is replayed headless and unthrottled from a fresh
save (deterministic mode, see port/engine/include/wp/deterministic.h). Every 60 retraces the
port writes a hash of guest RAM (and with --frames, of the rendered frame); the run is compared
line by line with build/golden/NAME.txt. --bless records the current results as the golden ones.

Golden files stay in build/ (they are derived from the game's memory); movies are only inputs
and can be committed.
"""
import argparse
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
GAME = ROOT / "port" / "build" / "out" / "musclemarch"
SYSCONF = ROOT / "dolphin_user" / "Wii" / "shared2" / "sys" / "SYSCONF"


def fresh_nand(run):
    shutil.rmtree(run, ignore_errors=True)
    (run / "nand" / "shared2" / "sys").mkdir(parents=True)
    if SYSCONF.exists():
        shutil.copy(SYSCONF, run / "nand" / "shared2" / "sys" / "SYSCONF")


def replay(movie, frames, game, data):
    run = ROOT / "build" / "replay" / movie.stem
    fresh_nand(run)
    hashes = run / "hashes.txt"
    env = dict(os.environ, WP_REPLAY=str(movie), WP_DETERMINISTIC_FAST="1", WP_HEADLESS="1", WP_REPLAY_EXIT="120",
               WP_DET_HASHES=str(hashes), WP_DET_HASH_EVERY="60")
    if frames:
        env["WP_DET_FRAMES"] = "1"
    started = time.time()
    with open(run / "stderr.txt", "wb") as err:
        code = subprocess.run([str(game), str(data), "3600", "nand"], cwd=run, env=env, stdout=subprocess.DEVNULL,
                              stderr=err).returncode
    lines = hashes.read_text().splitlines() if hashes.exists() else []
    return code, lines, time.time() - started


def compare(expected, actual):
    """First differing line as (retrace, what), or None."""
    for want, got in zip(expected, actual):
        w = want.split()
        g = got.split()
        if w[0] != g[0] or w[1] != g[1]:
            return g[0], "memory"
        if len(w) > 3 and len(g) > 3 and w[3] != g[3]:
            return g[0], "frame"
    if len(expected) != len(actual):
        return str(min(len(expected), len(actual)) * 60), f"length ({len(expected)} expected, {len(actual)} lines)"
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("movies", nargs="*")
    ap.add_argument("--bless", action="store_true", help="store the results as the new golden files")
    ap.add_argument("--frames", action="store_true", help="also hash rendered frames")
    ap.add_argument("--game", default=str(GAME))
    ap.add_argument("--data", default=str(ROOT / "extracted"), help="the game's files (e.g. a launcher install)")
    args = ap.parse_args()
    movies = [Path(m).resolve() for m in args.movies] or sorted((ROOT / "movies").glob("*.wpm"))
    if not movies:
        sys.exit("no movies (record one with: make record NAME=...)")
    golden_dir = ROOT / "build" / "golden"
    golden_dir.mkdir(parents=True, exist_ok=True)
    failures = 0
    for movie in movies:
        code, lines, seconds = replay(movie, args.frames, Path(args.game), Path(args.data).resolve())
        golden = golden_dir / f"{movie.stem}.txt"
        if code != 0 or not lines:
            print(f"{movie.stem}: FAILED to run (exit {code}, {len(lines)} hash lines), see build/replay/{movie.stem}/stderr.txt")
            failures += 1
            continue
        if args.bless or not golden.exists():
            golden.write_text("\n".join(lines) + "\n")
            print(f"{movie.stem}: blessed {len(lines)} checkpoints ({seconds:.0f}s)")
            continue
        difference = compare(golden.read_text().splitlines(), lines)
        if difference:
            failures += 1
            print(f"{movie.stem}: MISMATCH at retrace {difference[0]} ({difference[1]})")
        else:
            print(f"{movie.stem}: ok, {len(lines)} checkpoints ({seconds:.0f}s)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
