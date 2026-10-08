import argparse
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "recompiler"))

import game

GAME = game.load()
GHIDRA = ROOT / "ghidra" / "install" / "support" / "analyzeHeadless.bat"
SCRIPTS = ROOT / "ghidra" / "scripts"
LANGUAGE = "PowerPC:BE:32:Gekko_Broadway"


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--projects", default=str(ROOT / "ghidra" / "projects"))
    args = parser.parse_args()

    projects = Path(args.projects)
    projects.mkdir(parents=True, exist_ok=True)
    dol = GAME.dol
    command = [
        str(GHIDRA),
        str(projects),
        "wiiparty",
        "-import", str(dol),
        "-loader", "BinaryLoader",
        "-loader-baseAddr", "0x0",
        "-processor", LANGUAGE,
        "-overwrite",
        "-scriptPath", str(SCRIPTS),
        "-preScript", "LoadDol.java", str(dol),
        "-postScript", "FindFunctions.java",
        "-postScript", "ApplySymbols.java", str(GAME.dolphin_symbols), str(GAME.analysis / "symbols.csv"),
    ]
    if subprocess.call(command) != 0:
        return 1
    export = [
        str(GHIDRA),
        str(projects),
        "wiiparty",
        "-process", "main.dol",
        "-noanalysis",
        "-scriptPath", str(SCRIPTS),
        "-postScript", "ExportFunctions.java", str(GAME.analysis / "dol_functions.csv"),
    ]
    return subprocess.call(export)


if __name__ == "__main__":
    sys.exit(main())
