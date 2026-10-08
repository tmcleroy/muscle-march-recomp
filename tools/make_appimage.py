#!/usr/bin/env python3
"""Package a Linux build as one AppImage: the launcher (the entry point) and the game side by side.

Usage (on x86_64 Linux, after building the `musclemarch` and `musclemarch_launcher` targets):
  tools/make_appimage.py --build BUILD_DIR [--out MuscleMarch-x86_64.AppImage] [--licenses DIR]

The AppImage holds no game data: the launcher builds the game's files from each player's own WAD.
`MuscleMarch-x86_64.AppImage --play` goes straight into the game set up earlier (Steam Game Mode).
"""
import argparse
import hashlib
import os
import shutil
import subprocess
import sys
import tempfile
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools" / "app"))
from make_icns import png  # noqa: E402  (the original app icon)

TOOL_URL = "https://github.com/AppImage/appimagetool/releases/download/1.9.1/appimagetool-x86_64.AppImage"
TOOL_SHA256 = "ed4ce84f0d9caff66f50bcca6ff6f35aae54ce8135408b3fa33abfc3cb384eb0"
RUNTIME_URL = "https://github.com/AppImage/type2-runtime/releases/download/20251108/runtime-x86_64"
RUNTIME_SHA256 = "2fca8b443c92510f1483a883f60061ad09b46b978b2631c807cd873a47ec260d"

DESKTOP = """[Desktop Entry]
Type=Application
Name=Muscle March
Comment=Native port of Muscle March, built from your own copy of the game
Exec=musclemarch-launcher
Icon=musclemarch
Terminal=false
Categories=Game;
"""


def fetch(url, sha256, folder):
    target = folder / url.rsplit("/", 1)[1]
    if not target.exists() or hashlib.sha256(target.read_bytes()).hexdigest() != sha256:
        print(f"downloading {url}")
        folder.mkdir(parents=True, exist_ok=True)
        urllib.request.urlretrieve(url, target)
    digest = hashlib.sha256(target.read_bytes()).hexdigest()
    if digest != sha256:
        target.unlink()
        raise SystemExit(f"{target.name}: checksum mismatch {digest}")
    target.chmod(0o755)
    return target


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--build", type=Path, required=True, help="the CMake build folder (holds out/musclemarch and the launcher)")
    parser.add_argument("--out", type=Path, default=Path("MuscleMarch-x86_64.AppImage"))
    parser.add_argument("--licenses", type=Path, help="folder of third-party license texts to include")
    args = parser.parse_args()
    if sys.platform != "linux":
        sys.exit("run this on Linux")
    out_dir = args.build / "out" if (args.build / "out").is_dir() else args.build
    game = out_dir / "musclemarch"
    launcher = out_dir / "musclemarch-launcher"
    for binary in (game, launcher):
        if not binary.exists():
            sys.exit(f"missing {binary}: build the musclemarch and musclemarch_launcher targets first")
    downloads = Path(tempfile.gettempdir()) / "musclemarch-appimage"
    tool = fetch(TOOL_URL, TOOL_SHA256, downloads)
    runtime = fetch(RUNTIME_URL, RUNTIME_SHA256, downloads)
    with tempfile.TemporaryDirectory() as work:
        app = Path(work) / "AppDir"
        bin_dir = app / "usr" / "bin"
        bin_dir.mkdir(parents=True)
        shutil.copy2(launcher, bin_dir / "musclemarch-launcher")
        shutil.copy2(game, bin_dir / "musclemarch")
        subprocess.run(["strip", str(bin_dir / "musclemarch-launcher"), str(bin_dir / "musclemarch")], check=False)
        os.symlink("usr/bin/musclemarch-launcher", app / "AppRun")
        (app / "musclemarch.desktop").write_text(DESKTOP, encoding="utf-8")
        (app / "musclemarch.png").write_bytes(png(256))
        docs = app / "usr" / "share" / "doc" / "musclemarch"
        docs.mkdir(parents=True)
        for name in ("LICENSE", "THIRD_PARTY_NOTICES.md", "README.md"):
            shutil.copy2(ROOT / name, docs / name)
        if args.licenses:
            shutil.copytree(args.licenses, docs / "licenses")
        env = dict(os.environ, ARCH="x86_64", APPIMAGE_EXTRACT_AND_RUN="1")
        subprocess.run([str(tool), "--runtime-file", str(runtime), "--no-appstream", str(app), str(args.out)], check=True, env=env)
    print(f"wrote {args.out}")


if __name__ == "__main__":
    main()
