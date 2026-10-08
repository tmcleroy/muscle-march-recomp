import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
GHIDRA = ROOT / "ghidra" / "install" / "support" / "analyzeHeadless.bat"
PROJECTS = ROOT / "ghidra" / "projects"
SCRIPTS = ROOT / "ghidra" / "scripts"
PREFIX = "DecompileFunctions.java> "


def main():
    if len(sys.argv) < 2:
        print("usage: ghidra_decompile.py 0xADDRESS [0xADDRESS ...]", file=sys.stderr)
        return 2
    command = [
        str(GHIDRA),
        str(PROJECTS),
        "wiiparty",
        "-process", "main.dol",
        "-noanalysis",
        "-readOnly",
        "-scriptPath", str(SCRIPTS),
        "-postScript", "DecompileFunctions.java",
    ] + sys.argv[1:]
    result = subprocess.run(command, capture_output=True, text=True)
    capturing = False
    for line in result.stdout.splitlines():
        start = line.find(PREFIX)
        if start >= 0:
            capturing = True
            line = line[start + len(PREFIX):]
        elif line.startswith(("INFO", "WARN", "ERROR")):
            capturing = False
        if capturing:
            print(line.replace(" (GhidraScript)", ""))
    return result.returncode


if __name__ == "__main__":
    sys.exit(main())
