import csv
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "recompiler"))

import game

GAME = game.load()
LABEL = "Progress"
HEIGHT = 20
CHAR_WIDTH = 6.6
PADDING = 10
LABEL_COLOR = "#555555"
VALUE_COLOR = "#1f6fbf"


def read_percentages():
    with open(GAME.analysis / "progress.csv", newline="", encoding="utf-8") as handle:
        return [int(row["percent"]) for row in csv.DictReader(handle)]


def text_width(text):
    return round(len(text) * CHAR_WIDTH) + PADDING * 2


def render(value):
    left = text_width(LABEL)
    right = text_width(value)
    total = left + right
    font = 'font-family="Verdana, DejaVu Sans, sans-serif" font-size="11" text-anchor="middle"'
    return "\n".join(
        [
            f'<svg xmlns="http://www.w3.org/2000/svg" width="{total}" height="{HEIGHT}" role="img" aria-label="{LABEL}: {value}">',
            f'<title>{LABEL}: {value}</title>',
            '<linearGradient id="s" x2="0" y2="100%"><stop offset="0" stop-color="#bbb" stop-opacity=".1"/><stop offset="1" stop-opacity=".1"/></linearGradient>',
            f'<clipPath id="r"><rect width="{total}" height="{HEIGHT}" rx="3" fill="#fff"/></clipPath>',
            '<g clip-path="url(#r)">',
            f'<rect width="{left}" height="{HEIGHT}" fill="{LABEL_COLOR}"/>',
            f'<rect x="{left}" width="{right}" height="{HEIGHT}" fill="{VALUE_COLOR}"/>',
            f'<rect width="{total}" height="{HEIGHT}" fill="url(#s)"/>',
            "</g>",
            '<g fill="#fff">',
            f'<text x="{left / 2}" y="15" fill="#010101" fill-opacity=".3" {font}>{LABEL}</text>',
            f'<text x="{left / 2}" y="14" {font}>{LABEL}</text>',
            f'<text x="{left + right / 2}" y="15" fill="#010101" fill-opacity=".3" {font}>{value}</text>',
            f'<text x="{left + right / 2}" y="14" {font}>{value}</text>',
            "</g>",
            "</svg>",
        ]
    ) + "\n"


def main():
    percentages = read_percentages()
    overall = round(sum(percentages) / len(percentages))
    (ROOT / "docs" / "progress.svg").write_text(render(f"{overall}%"), encoding="utf-8")
    print(f"overall {overall}%")
    return 0


if __name__ == "__main__":
    sys.exit(main())
