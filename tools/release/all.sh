#!/bin/sh
# All release builds on an Apple Silicon Mac with Docker (e.g. OrbStack): macOS and Android
# natively, Linux and Windows in the x86_64 build image (tools/release/Dockerfile). Writes
# build/release/ with the four downloads and SHA256SUMS.txt. Needs the generated code
# (`make port`), and for the APK the Android SDK and the release signing key
# (tools/release/android.sh).
set -eu
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="$ROOT/build/release"
cd "$ROOT"
docker build --platform linux/amd64 -q -t musclemarch-build tools/release >/dev/null
tools/release/macos.sh
env -u GAME_FILES RELEASE=1 tools/release/android.sh
docker run --rm --platform linux/amd64 -v "$ROOT":/src -w /src musclemarch-build tools/release/linux.sh
docker run --rm --platform linux/amd64 -v "$ROOT":/src -w /src musclemarch-build tools/release/windows.sh
cd "$OUT"
shasum -a 256 MuscleMarch-macOS.zip MuscleMarch-x86_64.AppImage MuscleMarch-windows.zip MuscleMarch-android.apk > SHA256SUMS.txt
cat SHA256SUMS.txt
echo "version $(cat "$ROOT/VERSION"): release files in $OUT"
