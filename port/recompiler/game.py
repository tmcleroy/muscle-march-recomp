import os
import tomllib
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_GAME = "wiiparty"


class Game:
    def __init__(self, key):
        self.key = key
        self.folder = ROOT / "games" / key
        config = tomllib.loads((self.folder / "game.toml").read_text(encoding="utf-8"))
        self.name = config["name"]
        self.id = config["id"]
        self.executable = config["executable"]
        self.sdk = config.get("sdk", {})
        self.disc = self.folder / config["disc"]
        self.extracted = (self.folder / config.get("extracted", "extracted")).resolve()
        self.dol = self.extracted / config.get("dol", "sys/main.dol")
        self.compressed_modules = self.extracted / "files" / "rel"
        self.analysis = self.folder / "analysis"
        self.resources = self.folder / "res"
        self.symbol_map = ROOT / "reference" / "symbols" / f"{self.id}.map"
        self.build = ROOT / "build" / key
        self.modules = self.build / "rel"
        self.elf = self.build / "elf"
        self.dolphin_symbols = self.build / "symbols" / "dolphin_symbols.csv"
        self.module_targets = self.build / "symbols" / "module_targets.csv"
        self.dol_code = self.build / "generated" / "dol"
        self.module_code = self.build / "generated" / "modules"
        self.dsp_code = self.build / "generated" / "dsp"


def load():
    return Game(os.environ.get("WP_GAME", DEFAULT_GAME))
