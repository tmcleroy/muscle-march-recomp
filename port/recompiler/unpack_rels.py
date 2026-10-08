import sys
from pathlib import Path

import game
import lz11

GAME = game.load()
SOURCE = GAME.compressed_modules
TARGET = GAME.modules


def main():
    TARGET.mkdir(parents=True, exist_ok=True)
    failures = 0
    for source in sorted(SOURCE.glob("*.rel.lz")):
        try:
            data = lz11.decompress(source.read_bytes())
        except (ValueError, IndexError) as error:
            print(f"{source.name}: {error}", file=sys.stderr)
            failures += 1
            continue
        target = TARGET / source.with_suffix("").name
        if not target.exists() or target.read_bytes() != data:
            target.write_bytes(data)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
