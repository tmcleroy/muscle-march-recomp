import io
import sys
from pathlib import Path

import resvg_py
from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "recompiler"))

import game

GAME = game.load()
SOURCE = GAME.resources / "wiiparty.svg"
TARGET = GAME.resources / "wiiparty.ico"
SIZES = [16, 20, 24, 32, 40, 48, 64, 96, 128, 256]


def render(svg, size):
    data = bytes(resvg_py.svg_to_bytes(svg_string=svg, width=size, height=size))
    return Image.open(io.BytesIO(data)).convert("RGBA")


def main():
    svg = SOURCE.read_text(encoding="utf-8")
    frames = [render(svg, size) for size in SIZES]
    frames[-1].save(TARGET, append_images=frames[:-1], sizes=[(size, size) for size in SIZES])
    print(TARGET)


if __name__ == "__main__":
    main()
