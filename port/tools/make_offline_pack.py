import hashlib
import os
import re
import shutil
import stat
import subprocess
import sys
import urllib.request
import zipfile
from pathlib import Path

root = Path(__file__).resolve().parent.parent
work = root / "build" / "offline"
downloads = work / "downloads"
stage = work / "stage"
launcher = root / "build" / "launcher" / "wiipartyrecomp-launcher.exe"
output = root / "build" / "wiipartyrecomp-offline-windows.zip"
sources_output = root / "build" / "wiipartyrecomp-sources.zip"
SDL_FILE = "SDL3-devel-3.4.16-mingw.tar.gz"
SDL_URL = f"https://github.com/libsdl-org/SDL/releases/download/release-3.4.16/{SDL_FILE}"
SDL_SHA256 = "c7ef65bd72eabac6e5b535411dbd8d5824d0aab24fd62ff8812666b336f18a9c"

MINGW_EXTRAS = [
    "bin/gdb.exe", "bin/gdbserver.exe", "bin/jpegtran.exe", "bin/oggdec.exe", "bin/oggenc.exe", "bin/ogginfo.exe", "bin/optipng.exe",
    "bin/pngcheck.exe", "bin/rg.exe", "bin/sed.exe", "bin/vcut.exe", "bin/vorbiscomment.exe", "bin/zstd.exe", "bin/coreutils.exe", "bin/make.exe",
    "bin/mingw32-make.exe", "include/boost", "include/SDL2", "include/glm", "include/glbinding", "include/glbinding-aux", "include/GLFW",
    "include/freetype2", "include/ogg", "include/vorbis", "lib/cmake", "lib/pkgconfig", "share/gdb",
]
MINGW_EXTRA_PATTERNS = [
    "include/ft2build.h", "include/png*.h", "include/j*.h", "include/turbojpeg.h", "include/zlib.h", "include/zconf.h", "include/zstd*.h",
    "include/zdict.h", "lib/libboost*", "lib/libSDL2*", "lib/libglbinding*", "lib/libglfw*", "lib/libfreetype*", "lib/libpng*", "lib/libjpeg*",
    "lib/libturbojpeg*", "lib/libogg*", "lib/libvorbis*", "lib/libz.a", "lib/libzstd*", "scripts-20.0/boost.sh", "scripts-20.0/freetype.sh",
    "scripts-20.0/gdb.sh", "scripts-20.0/glbinding.sh", "scripts-20.0/glfw.sh", "scripts-20.0/glm.sh", "scripts-20.0/libjpeg-turbo.sh",
    "scripts-20.0/libpng.sh", "scripts-20.0/make.sh", "scripts-20.0/optipng.sh", "scripts-20.0/pngcheck.sh", "scripts-20.0/ripgrep.sh",
    "scripts-20.0/sdl*.sh", "scripts-20.0/sed.sh", "scripts-20.0/uutils-coreutils.sh", "scripts-20.0/vorbis-tools*", "scripts-20.0/zlib.sh",
]

CMAKE_EXTRAS = ["*/doc/cmake/html", "*/doc/cmake/CMake.qch", "*/man"]

GPL_SOURCES = [
    ("https://ftp.gnu.org/gnu/gcc/gcc-15.2.0/gcc-15.2.0.tar.xz", "gcc-15.2.0.tar.xz"),
    ("https://ftp.gnu.org/gnu/binutils/binutils-2.45.1.tar.xz", "binutils-2.45.1.tar.xz"),
    ("https://ftp.gnu.org/gnu/gmp/gmp-6.3.0.tar.xz", "gmp-6.3.0.tar.xz"),
    ("https://ftp.gnu.org/gnu/mpfr/mpfr-4.2.2.tar.xz", "mpfr-4.2.2.tar.xz"),
    ("https://ftp.gnu.org/gnu/mpc/mpc-1.3.1.tar.gz", "mpc-1.3.1.tar.gz"),
    ("https://gcc.gnu.org/pub/gcc/infrastructure/isl-0.24.tar.bz2", "isl-0.24.tar.bz2"),
    ("https://downloads.sourceforge.net/project/mingw-w64/mingw-w64/mingw-w64-release/mingw-w64-v11.0.1.tar.bz2", "mingw-w64-v11.0.1.tar.bz2"),
    ("https://github.com/libfuse/libfuse/releases/download/fuse-3.15.0/fuse-3.15.0.tar.xz", "fuse-3.15.0.tar.xz"),
    ("https://github.com/AppImage/type2-runtime/archive/refs/tags/20251108.tar.gz", "type2-runtime-20251108.tar.gz"),
]

NOTICE = """Wii Party Recomp - offline tools pack for Windows

Unzip this folder anywhere and open wiipartyrecomp-launcher.exe. The launcher uses the
tools in the tools folder next to it and never downloads them. Turn on Offline mode in
Settings so the launcher also skips the update check.

The tools are copies of these official releases:
{tools}
From the MinGW-w64 distro only the compiler part is kept: GCC 15.2.0, binutils 2.45.1 and
the MinGW-w64 11.0.1 runtime. Its other programs and libraries (Boost, SDL2, gdb, make,
sed and the rest) were removed because the game build does not use them.

Licences:
- nodtool: MIT License or Apache License 2.0 (https://github.com/encounter/nod)
- Python: Python Software Foundation License (tools/python/LICENSE.txt)
- CMake: BSD 3-Clause License (tools/cmake/cmake-4.4.3-windows-x86_64/doc/cmake/LICENSE.rst)
- Ninja: Apache License 2.0 (https://github.com/ninja-build/ninja)
- SDL3: zlib License (https://github.com/libsdl-org/SDL)
- GCC and binutils: GNU General Public License version 3; GCC's runtime libraries carry
  the GCC Runtime Library Exception. GMP, MPFR and MPC (built into GCC): GNU LGPL
  version 3. ISL: MIT License. MinGW-w64 runtime: permissive licences (see its
  COPYING files in the sources).
- The complete corresponding source code of the GPL and LGPL programs is published as a
  separate file of the same release, {sources}: the official source releases listed
  below, unmodified, and the build scripts of the distro (also in
  tools/mingw/MinGW/scripts-20.0).
- The same file also carries the source of the AppImage runtime at the start of
  wiipartyrecomp-launcher-x86_64.AppImage: type2-runtime 20251108 (MIT License, with its
  build scripts and its libfuse patch) and libfuse 3.15.0 (GNU LGPL version 2.1), which
  the runtime links statically.
{source_list}
"""


def remove_tree(path):
    def writable(function, target, _):
        os.chmod(target, stat.S_IWRITE)
        function(target)

    if path.exists():
        shutil.rmtree(path, onexc=writable)


def packages():
    text = (root / "launcher" / "src" / "toolchain.cpp").read_text(encoding="utf-8")
    block = text[text.index("#ifdef _WIN32\nconstexpr Package kPackages[]"):text.index("constexpr const char* kPython = \"python\";")]
    pattern = re.compile(r'\{"([^"]+)", "([^"]+)", "([^"]+)",\s*"([0-9a-f]{64})", "([^"]+)", Packaging::(\w+), "([^"]+)",\s*"([^"]+)"\}')
    return [dict(zip(("program", "name", "url", "sha256", "file", "packaging", "folder", "bin"), match)) for match in pattern.findall(block)]


def download(url, name):
    target = downloads / name
    if not target.exists():
        print(f"downloading {url}")
        downloads.mkdir(parents=True, exist_ok=True)
        urllib.request.urlretrieve(url, target)
    return target


def fetch(package):
    target = download(package["url"], package["file"])
    digest = hashlib.sha256(target.read_bytes()).hexdigest()
    if digest != package["sha256"]:
        target.unlink()
        raise SystemExit(f"{package['file']}: checksum mismatch {digest}")
    return target


def prune_mingw(folder):
    distro = folder / "MinGW"
    for relative in MINGW_EXTRAS:
        path = distro / relative
        if path.is_dir():
            remove_tree(path)
        elif path.exists():
            path.unlink()
    for pattern in MINGW_EXTRA_PATTERNS:
        for path in distro.glob(pattern):
            if path.is_dir():
                remove_tree(path)
            else:
                path.unlink()


def unpack(package, archive):
    destination = stage / "tools" / package["folder"]
    remove_tree(destination)
    destination.mkdir(parents=True)
    kind = package["packaging"]
    if kind == "Single":
        shutil.copy2(archive, destination / f"{package['program']}.exe")
    elif kind == "Archive":
        with zipfile.ZipFile(archive) as bundle:
            bundle.extractall(destination)
    else:
        subprocess.run([str(archive), "-y", f"-o{destination}"], check=True)
        prune_mingw(destination)
    for pattern in CMAKE_EXTRAS:
        for path in destination.glob(pattern):
            remove_tree(path) if path.is_dir() else path.unlink()
    for leftover in destination.glob("*._pth"):
        leftover.unlink()


def zip_folder(folder, target, prefix, compression):
    target.unlink(missing_ok=True)
    with zipfile.ZipFile(target, "w", compression, compresslevel=9 if compression == zipfile.ZIP_DEFLATED else None) as bundle:
        for path in sorted(folder.rglob("*")):
            if path.is_file():
                bundle.write(path, Path(prefix) / path.relative_to(folder))
    print(f"{target} ({target.stat().st_size / 1e6:.1f} MB)")


def main():
    if not launcher.exists():
        print(f"build the launcher first: {launcher}", file=sys.stderr)
        return 1
    remove_tree(stage)
    stage.mkdir(parents=True)
    listed = []
    for package in packages():
        unpack(package, fetch(package))
        listed.append(f"- {package['name']}: {package['url']} (SHA-256 {package['sha256']})")
    sources_stage = work / "sources"
    remove_tree(sources_stage)
    sources_stage.mkdir(parents=True)
    source_list = []
    for url, name in GPL_SOURCES:
        archive = download(url, name)
        shutil.copy2(archive, sources_stage / name)
        source_list.append(f"- {name}: {url} (SHA-256 {hashlib.sha256(archive.read_bytes()).hexdigest()})")
    shutil.copytree(stage / "tools" / "mingw" / "MinGW" / "scripts-20.0", sources_stage / "nuwen-mingw-scripts-20.0")
    notice = NOTICE.format(tools="\n".join(listed), sources=sources_output.name, source_list="\n".join(source_list))
    (sources_stage / "SOURCES.txt").write_text(notice, encoding="utf-8", newline="\r\n")
    sdl_folder = stage / "tools" / "sdl3"
    sdl_folder.mkdir(parents=True)
    sdl_archive = download(SDL_URL, SDL_FILE)
    if hashlib.sha256(sdl_archive.read_bytes()).hexdigest() != SDL_SHA256:
        sdl_archive.unlink()
        raise SystemExit(f"{SDL_FILE}: checksum mismatch")
    shutil.copy2(sdl_archive, sdl_folder / SDL_FILE)
    listed.append(f"- SDL 3.4.16 development package: {SDL_URL} (SHA-256 {SDL_SHA256})")
    shutil.copy2(launcher, stage / launcher.name)
    (stage / "OFFLINE-PACK.txt").write_text(notice, encoding="utf-8", newline="\r\n")
    zip_folder(stage, output, "wiipartyrecomp", zipfile.ZIP_DEFLATED)
    zip_folder(sources_stage, sources_output, "wiipartyrecomp-sources", zipfile.ZIP_STORED)
    return 0


if __name__ == "__main__":
    sys.exit(main())
