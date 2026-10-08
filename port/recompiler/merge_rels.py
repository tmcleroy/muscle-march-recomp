import argparse
import subprocess
import sys

import game

GAME = game.load()
DTK = game.ROOT / "reference" / "dtk" / "dtk-windows-x86_64.exe"
DOL = GAME.dol
MODULES = GAME.modules
OUTPUT = GAME.elf


def merge(name):
    command = [str(DTK), "rel", "merge", "-o", str(OUTPUT / f"{name}.elf"), str(DOL), str(MODULES / f"{name}.rel")]
    return subprocess.run(command, capture_output=True).returncode


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("modules", nargs="*")
    parser.add_argument("--all", action="store_true")
    args = parser.parse_args()

    names = [path.stem for path in sorted(MODULES.glob("*.rel"))] if args.all else args.modules
    if not names:
        parser.error("name at least one module or pass --all")
    OUTPUT.mkdir(parents=True, exist_ok=True)
    failures = [name for name in names if merge(name) != 0]
    print(f"merged {len(names) - len(failures)} of {len(names)} modules into {OUTPUT}")
    for name in failures:
        print(f"failed: {name}", file=sys.stderr)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
