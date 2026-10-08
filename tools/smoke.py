#!/usr/bin/env python3
"""Long unattended play of the native port, looking for crashes, hangs and broken frames.

  tools/smoke.py NAME [--minutes 15] [--seed 1] [--mode 0]

Generates an input script on the game's frame clock:
  - A presses through the boot screens and menus (and every few seconds after,
    to get through results / continue screens),
  - from the first race on: shake both controllers continuously and cycle the
    four poses pseudo-randomly, so some walls are cleared and play advances,
captures a frame every --every seconds into build/smoke/NAME/captures, and
reports how the run ended (still running at the limit, crashed, or stuck).
"""
import argparse, os, random, re, subprocess, sys, time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
GAME = ROOT / "port" / "build" / "out" / "musclemarch"

A = 0x800
DOWN, RIGHT = 0x4, 0x2
SHAKE = 0x10000 | 0x4000000
POSES = [0x8000000 | 0x20000000, 0x10000000 | 0x40000000, 0x20000000 | 0x10000000, 0x8000000 | 0x40000000]


def script(minutes, seed, mode, right=0, point=None, autoplay=False):
    rng = random.Random(seed)
    entries = []
    end = int(minutes * 60 * 1000)
    # boot: skip strap / notices, then title -> start -> mode -> character
    for t in range(2000, 50000, 1500):
        entries.append((t, t + 120, A))
    if mode == 1:  # second mode on the select screen
        entries.append((33500, 33620, RIGHT))
    t = 52000
    while t < end:
        hold = rng.choice([400, 600, 900])
        if not autoplay:  # with --autoplay the game's autoplay poses and shakes
            entries.append((t, t + hold, SHAKE | rng.choice(POSES)))
        t += hold
    for t in range(52000, end, 4000):  # results / continue / next stage
        entries.append((t, t + 120, A))
        for k in range(right):  # move the stage-select cursor before confirming
            entries.append((t - 900 + 250 * k, t - 800 + 250 * k, RIGHT))
    text = ";".join(f"{a},{b},{c:x}" for a, b, c in entries)
    if point:  # aim the pointer here from the first race on (stage select etc.)
        text += f";52000,{end},0,{point[0]},{point[1]}"
    return text


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("name")
    ap.add_argument("--minutes", type=float, default=15)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--mode", type=int, default=0)
    ap.add_argument("--every", type=float, default=15)
    ap.add_argument("--test-build", action="store_true", help="use port/build/test (instruction patches, e.g. infinite stamina)")
    ap.add_argument("--ram", action="store_true", help="also dump MEM1/MEM2 at each capture (WP_CAPTURE_RAM)")
    ap.add_argument("--right", type=int, default=0, help="press Right this many times before each A after the boot (stage select)")
    ap.add_argument("--nand-from", help="start from the NAND (save) of an earlier smoke run")
    ap.add_argument("--point", help="x,y pointer position held after the boot, e.g. -0.05,0.04")
    ap.add_argument("--game", help="port binary to run (default port/build/out/musclemarch)")
    ap.add_argument("--native-input", default="", help="WP_NATIVE_INPUT level (1 = WPADRead, 2 = no Bluetooth)")
    ap.add_argument("--autoplay", action="store_true", help="WP_AUTOPLAY: hold the pose each wall asks for (test tool)")
    ap.add_argument("--record", help="record the run (deterministic mode) into this input movie")
    args = ap.parse_args()
    run = ROOT / "build" / "smoke" / args.name
    subprocess.run(["rm", "-rf", str(run)])
    run.mkdir(parents=True)
    if args.nand_from:
        subprocess.run(["cp", "-R", str(ROOT / "build" / "smoke" / args.nand_from / "nand"), str(run / "nand")], check=True)
    env = dict(os.environ, WP_HEADLESS="1", WP_SCRIPT_CLOCK="frames",
               WP_INPUT_SCRIPT=script(args.minutes, args.seed, args.mode, args.right,
                                      tuple(float(v) for v in args.point.split(",")) if args.point else None, args.autoplay),
               WP_CAPTURE_AT=",".join(str(x) for x in range(5, int(args.minutes * 60), int(args.every))),
               WP_LOG_FILE="game.log", WP_NATIVE_INPUT=args.native_input, **({"WP_CAPTURE_RAM": "1"} if args.ram else {}),
               **({"WP_AUTOPLAY": os.environ.get("WP_AUTOPLAY", "1")} if args.autoplay else {}),
               **({"WP_RECORD": str(Path(args.record).resolve()), "WP_DETERMINISTIC_FAST": "1"} if args.record else {}))
    limit = int(args.minutes * 60 * 1.6) + 60  # wall-clock budget
    t0 = time.time()
    with open(run / "stderr.txt", "wb") as err:
        game = Path(args.game).resolve() if args.game else ROOT / "port" / "build" / "test" / "musclemarch" if args.test_build else GAME
        proc = subprocess.Popen(["nice", str(game), str(ROOT / "extracted"), str(limit), "nand"],
                                cwd=run, stdout=subprocess.DEVNULL, stderr=err, env=env)
        code = proc.wait()
    text = (run / "stderr.txt").read_bytes().decode("latin1")
    shots = sorted((run / "captures").glob("*.png")) if (run / "captures").exists() else []
    if "crash:" in text:
        verdict = "CRASH"
    elif code == 3:
        verdict = "ran to the time limit"
    else:
        verdict = f"exited with code {code}"
    states = re.findall(r"SEQUENCE_STATE\s+:\s+(\S+)", text)
    print(f"{args.name}: {verdict} after {time.time() - t0:.0f}s, {len(shots)} frames captured")
    print("  states:", " -> ".join(dict.fromkeys(states)) or "-")
    for line in text.splitlines():
        if line.startswith(("crash:", "PANIC", "unsupported", "no runtime handler", "unresolved", "illegal")):
            print("  " + line[:160])
    return 0 if verdict != "CRASH" else 1


if __name__ == "__main__":
    sys.exit(main())
