#!/bin/sh
# Release build for Linux x86_64 / Steam Deck: build/release/MuscleMarch-x86_64.AppImage.
# Run on x86_64 Linux (e.g. an Ubuntu 24.04 container) with the packages in docs/BUILDING.md and
# the generated code (GENERATED=..., from `make port` on any machine). SDL3 is built statically.
set -eu
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD="${BUILD:-$ROOT/port/build/release-linux}"
GENERATED="${GENERATED:-$ROOT/port/build/musclemarch/generated}"
OUT="$ROOT/build/release"
mkdir -p "$OUT"

cmake -S "$ROOT/port" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release -DWP_GAME=musclemarch \
    -DWP_GENERATED="$GENERATED" -DPython3_EXECUTABLE="$(command -v python3)"
cmake --build "$BUILD" --target musclemarch musclemarch_launcher

LICENSES="$BUILD/licenses"
rm -rf "$LICENSES" && mkdir -p "$LICENSES"
cp "$BUILD/_deps/sdl3-src/LICENSE.txt" "$LICENSES/SDL3.txt"
cp "$BUILD/_deps/imgui-src/LICENSE.txt" "$LICENSES/DearImGui.txt"
cp "$(find "$BUILD/_deps/inter-src" -iname 'LICENSE*' | head -1)" "$LICENSES/Inter-OFL.txt"
python3 "$ROOT/tools/make_appimage.py" --build "$BUILD" --out "$OUT/MuscleMarch-x86_64.AppImage" --licenses "$LICENSES"
