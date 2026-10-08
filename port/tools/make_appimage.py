import argparse
import hashlib
import os
import shutil
import subprocess
import sys
import tempfile
import urllib.request
from pathlib import Path

root = Path(__file__).resolve().parent.parent
output = root / "build" / "wiipartyrecomp-launcher-x86_64.AppImage"
TOOL_URL = "https://github.com/AppImage/appimagetool/releases/download/1.9.1/appimagetool-x86_64.AppImage"
TOOL_SHA256 = "ed4ce84f0d9caff66f50bcca6ff6f35aae54ce8135408b3fa33abfc3cb384eb0"
RUNTIME_URL = "https://github.com/AppImage/type2-runtime/releases/download/20251108/runtime-x86_64"
RUNTIME_SHA256 = "2fca8b443c92510f1483a883f60061ad09b46b978b2631c807cd873a47ec260d"

DESKTOP = """[Desktop Entry]
Type=Application
Name=Wii Party Recomp
Comment=Build and play Wii Party from your own disc
Exec=wiipartyrecomp-launcher
Icon=wiipartyrecomp
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
    parser.add_argument("--build", type=Path, default=root / "build" / "launcher-linux")
    arguments = parser.parse_args()
    if sys.platform != "linux":
        print("run this on Linux", file=sys.stderr)
        return 1
    subprocess.run(["cmake", "-S", str(root / "launcher"), "-B", str(arguments.build), "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release", "-UWP_LAUNCHER_VERSION", "-UWP_LAUNCHER_BUILD"], check=True)
    subprocess.run(["cmake", "--build", str(arguments.build)], check=True)
    downloads = Path(tempfile.gettempdir()) / "wiipartyrecomp-appimage"
    tool = fetch(TOOL_URL, TOOL_SHA256, downloads)
    runtime = fetch(RUNTIME_URL, RUNTIME_SHA256, downloads)
    with tempfile.TemporaryDirectory() as work:
        app = Path(work) / "AppDir"
        (app / "usr" / "bin").mkdir(parents=True)
        shutil.copy2(arguments.build / "wiipartyrecomp-launcher", app / "usr" / "bin" / "wiipartyrecomp-launcher")
        os.symlink("usr/bin/wiipartyrecomp-launcher", app / "AppRun")
        (app / "wiipartyrecomp-launcher.desktop").write_text(DESKTOP, encoding="utf-8")
        shutil.copy2(root / "games" / "wiiparty" / "res" / "wiiparty.svg", app / "wiipartyrecomp.svg")
        output.unlink(missing_ok=True)
        environment = dict(os.environ, ARCH="x86_64", APPIMAGE_EXTRACT_AND_RUN="1")
        subprocess.run([str(tool), "--no-appstream", "--runtime-file", str(runtime), str(app), str(output)], check=True, env=environment)
    print(f"{output} ({output.stat().st_size / 1e6:.1f} MB)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
