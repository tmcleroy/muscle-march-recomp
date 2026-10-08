#!/bin/sh
# Release build for Windows x64, cross-compiled with MinGW-w64 on Linux:
# build/release/MuscleMarch-windows.zip (launcher, game, SDL3.dll, MinGW runtime, licenses).
# Needs g++-mingw-w64-x86-64-posix, CMake, Ninja, Python, curl and zip, and the generated code.
set -eu
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD="${BUILD:-$ROOT/port/build/release-windows}"
GENERATED="${GENERATED:-$ROOT/port/build/musclemarch/generated}"
OUT="$ROOT/build/release"
SDL_VERSION=3.4.16
SDL_SHA256=c7ef65bd72eabac6e5b535411dbd8d5824d0aab24fd62ff8812666b336f18a9c
DEPS="$ROOT/port/build/deps"
mkdir -p "$OUT" "$DEPS"

# SDL3's official MinGW development package, where the engine looks for it.
if [ ! -d "$DEPS/SDL3-$SDL_VERSION/x86_64-w64-mingw32" ]; then
    curl -fsSL -o "$DEPS/sdl3-mingw.tar.gz" "https://github.com/libsdl-org/SDL/releases/download/release-$SDL_VERSION/SDL3-devel-$SDL_VERSION-mingw.tar.gz"
    echo "$SDL_SHA256  $DEPS/sdl3-mingw.tar.gz" | sha256sum -c -
    tar -xzf "$DEPS/sdl3-mingw.tar.gz" -C "$DEPS"
fi

cmake -S "$ROOT/port" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release -DWP_GAME=musclemarch \
    -DCMAKE_TOOLCHAIN_FILE="$ROOT/tools/release/mingw-w64.cmake" \
    -DWP_GENERATED="$GENERATED" -DPython3_EXECUTABLE="$(command -v python3)"
cmake --build "$BUILD" --target musclemarch musclemarch_launcher

STAGE="$BUILD/stage/MuscleMarch"
rm -rf "$BUILD/stage" && mkdir -p "$STAGE/licenses"
find "$BUILD" -maxdepth 2 \( -name 'musclemarch.exe' -o -name 'musclemarch-launcher.exe' \) -exec cp {} "$STAGE/" \;
cp "$DEPS/SDL3-$SDL_VERSION/x86_64-w64-mingw32/bin/SDL3.dll" "$STAGE/"
# The MinGW runtime the binaries use, unless they were linked statically.
for dll in libstdc++-6.dll libgcc_s_seh-1.dll libwinpthread-1.dll; do
    if x86_64-w64-mingw32-objdump -p "$STAGE"/*.exe | grep -qi "$dll"; then
        cp "$(x86_64-w64-mingw32-g++-posix -print-file-name=$dll)" "$STAGE/" 2>/dev/null || cp "$(find /usr/lib/gcc/x86_64-w64-mingw32 /usr/x86_64-w64-mingw32 -name "$dll" | head -1)" "$STAGE/"
    fi
done
x86_64-w64-mingw32-strip "$STAGE"/*.exe
x86_64-w64-mingw32-strip --strip-unneeded "$STAGE"/*.dll
cp "$ROOT/LICENSE" "$STAGE/LICENSE.txt"
cp "$ROOT/THIRD_PARTY_NOTICES.md" "$ROOT/README.md" "$STAGE/"
cp "$DEPS/SDL3-$SDL_VERSION/LICENSE.txt" "$STAGE/licenses/SDL3.txt" 2>/dev/null || true
cp "$BUILD/_deps/imgui-src/LICENSE.txt" "$STAGE/licenses/DearImGui.txt"
cp "$(find "$BUILD/_deps/inter-src" -iname 'LICENSE*' | head -1)" "$STAGE/licenses/Inter-OFL.txt"
cp "$ROOT/port/launcher/licenses/"*.txt "$STAGE/licenses/" 2>/dev/null || true
rm -f "$OUT/MuscleMarch-windows.zip"
(cd "$BUILD/stage" && zip -qr "$OUT/MuscleMarch-windows.zip" MuscleMarch)
echo "wrote $OUT/MuscleMarch-windows.zip"
