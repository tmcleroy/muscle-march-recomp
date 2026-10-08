#!/bin/sh
# Release build for macOS (Apple Silicon): "Muscle March.app" (the launcher, with the game beside it)
# zipped as build/release/MuscleMarch-macOS.zip. Needs the generated code (`make port` once) and
# the build tools (CMake, Ninja, Python). SDL3 is downloaded and linked statically, and the
# binaries run on macOS 13 and later. The app is signed ad hoc (not notarized).
set -eu
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD="$ROOT/port/build/release-macos"
OUT="$ROOT/build/release"
APP="$OUT/Muscle March.app"
VERSION="${VERSION:-$(cat "$ROOT/VERSION")}"
PY="$ROOT/.venv/bin/python"
[ -x "$PY" ] || PY="$(command -v python3)"

cmake -S "$ROOT/port" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release -DWP_GAME=musclemarch \
    -DCMAKE_OSX_DEPLOYMENT_TARGET=13.0 -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_DISABLE_FIND_PACKAGE_SDL3=TRUE \
    -DWP_GENERATED="$ROOT/port/build/musclemarch/generated" -DPython3_EXECUTABLE="$PY"
cmake --build "$BUILD" --target musclemarch musclemarch_launcher

rm -rf "$APP" && mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources/licenses"
sed "s/@VERSION@/$VERSION/" "$ROOT/tools/release/Info.plist" > "$APP/Contents/Info.plist"
cp "$BUILD/musclemarch-launcher" "$BUILD/musclemarch" "$APP/Contents/MacOS/"
strip -x "$APP/Contents/MacOS/musclemarch-launcher" "$APP/Contents/MacOS/musclemarch"
python3 "$ROOT/tools/app/make_icns.py" "$APP/Contents/Resources/icon.icns"
cp "$ROOT/LICENSE" "$ROOT/THIRD_PARTY_NOTICES.md" "$ROOT/README.md" "$APP/Contents/Resources/"
cp "$BUILD/_deps/sdl3-src/LICENSE.txt" "$APP/Contents/Resources/licenses/SDL3.txt"
cp "$BUILD/_deps/imgui-src/LICENSE.txt" "$APP/Contents/Resources/licenses/DearImGui.txt"
cp "$(find "$BUILD/_deps/inter-src" -iname 'LICENSE*' | head -1)" "$APP/Contents/Resources/licenses/Inter-OFL.txt"
codesign --force --deep -s - "$APP"

for binary in "$APP/Contents/MacOS/"*; do
    if otool -L "$binary" | grep -q '/opt/homebrew\|/usr/local'; then
        echo "error: $binary links a library outside macOS" >&2
        exit 1
    fi
done
rm -f "$OUT/MuscleMarch-macOS.zip"
(cd "$OUT" && ditto -c -k --keepParent "Muscle March.app" MuscleMarch-macOS.zip)
echo "wrote $OUT/MuscleMarch-macOS.zip"
