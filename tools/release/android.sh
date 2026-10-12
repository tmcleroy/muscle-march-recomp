#!/bin/sh
# Android build (arm64): build/release/MuscleMarch-android.apk. Needs the generated code
# (`make port` once), CMake 3.24 or newer and Ninja on the PATH, a JDK 17, and the Android SDK with
# platform 34, build-tools and NDK 27.2 (ANDROID_HOME; Homebrew's android-commandlinetools is found
# on its own).
#
#   tools/release/android.sh                 the app alone: it asks the player for their WAD and key
#   GAME_FILES=extracted tools/release/android.sh
#                                            a development build with your own game files packed
#                                            in, to try on your own phone. Never share that APK.
#
# SDL3's source (its Java half is compiled into the app) is downloaded to port/build/deps.
#
# Signing: with KEYSTORE, KEYSTORE_PASSWORD, KEY_ALIAS and KEY_PASSWORD the APK is signed with
# that key. They are also read from the file SIGNING names (default
# ~/.config/musclemarch/android-signing.env, kept outside the repository), as lines of
# NAME="value". Without either it is signed with the Android debug key, which is fine for
# sideloading your own build; RELEASE=1 refuses that, because a published APK has to carry the
# same key every time or it won't install over the one before.
set -eu
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="$ROOT/build/release"
STAGED="$ROOT/build/android/staged"
GENERATED="${GENERATED:-$ROOT/port/build/musclemarch/generated}"
SDL_VERSION=3.4.16
SDL_SHA256=7322236cd12090c3eb40b9728be4d49c76f66ad17d04369584d4ecad5cf77c68
SDL_DIR="$ROOT/port/build/deps/SDL3-$SDL_VERSION-src"
PY="$ROOT/.venv/bin/python"
[ -x "$PY" ] || PY="$(command -v python3)"
SIGNING="${SIGNING:-$HOME/.config/musclemarch/android-signing.env}"
if [ -z "${KEYSTORE:-}" ] && [ -f "$SIGNING" ]; then
    . "$SIGNING"
fi
if [ -n "${RELEASE:-}" ]; then
    [ -n "${KEYSTORE:-}" ] || { echo "error: a release APK needs the signing key ($SIGNING)" >&2; exit 1; }
    [ -z "${GAME_FILES:-}" ] || { echo "error: a release APK can't have game files packed in" >&2; exit 1; }
fi

if [ -z "${ANDROID_HOME:-}" ]; then
    for candidate in /opt/homebrew/share/android-commandlinetools "$HOME/Library/Android/sdk" "$HOME/Android/Sdk"; do
        [ -d "$candidate/platforms" ] && ANDROID_HOME="$candidate" && break
    done
fi
[ -d "${ANDROID_HOME:-}" ] || { echo "error: no Android SDK found; set ANDROID_HOME" >&2; exit 1; }
export ANDROID_HOME
if [ -z "${JAVA_HOME:-}" ] && [ -d /opt/homebrew/opt/openjdk@17 ]; then
    export JAVA_HOME=/opt/homebrew/opt/openjdk@17
fi
[ -d "$GENERATED/dol" ] || { echo "error: no generated code in $GENERATED (run make port)" >&2; exit 1; }

if [ ! -f "$SDL_DIR/CMakeLists.txt" ]; then
    mkdir -p "$SDL_DIR"
    archive="$SDL_DIR.tar.gz"
    curl -fL -o "$archive" "https://github.com/libsdl-org/SDL/releases/download/release-$SDL_VERSION/SDL3-$SDL_VERSION.tar.gz"
    echo "$SDL_SHA256  $archive" | shasum -a 256 -c -
    tar -xzf "$archive" -C "$SDL_DIR" --strip-components 1
    rm -f "$archive"
fi

# Staged outside the repository: the icon, and the game files of a development build.
rm -rf "$STAGED" && mkdir -p "$STAGED/assets" "$STAGED/res"
"$PY" "$ROOT/tools/app/make_icns.py" --android "$STAGED/res"
NAME=MuscleMarch-android.apk
if [ -n "${GAME_FILES:-}" ]; then
    [ -f "$GAME_FILES/main.dol" ] || { echo "error: no main.dol in $GAME_FILES" >&2; exit 1; }
    mkdir -p "$STAGED/assets/game"
    (cd "$GAME_FILES" && find . -type f ! -name '.*' | sed 's|^\./||' | LC_ALL=C sort) > "$STAGED/files.txt"
    (cd "$GAME_FILES" && tar -cf - -T "$STAGED/files.txt") | (cd "$STAGED/assets/game" && tar -xf -)
    { (cd "$GAME_FILES" && tar -cf - -T "$STAGED/files.txt") | shasum -a 256 | cut -d' ' -f1; cat "$STAGED/files.txt"; } > "$STAGED/assets/game/manifest.txt"
    rm -f "$STAGED/files.txt"
    NAME=MuscleMarch-android-with-game-DO-NOT-SHARE.apk
    echo "packing the game files in $GAME_FILES: a development build, not to be shared"
fi

CMAKE_VERSION="$(cmake --version | sed -n 's/^cmake version \([0-9.]*\).*/\1/p')"
set -- -PcmakeVersion="$CMAKE_VERSION" -PsdlDir="$SDL_DIR" -PgeneratedDir="$GENERATED" -PstagedDir="$STAGED" -Ppython="$PY"
if [ -n "${KEYSTORE:-}" ]; then
    set -- "$@" -PkeyStore="$KEYSTORE" -PkeyStorePassword="$KEYSTORE_PASSWORD" -PkeyAlias="$KEY_ALIAS" -PkeyPassword="$KEY_PASSWORD"
fi
(cd "$ROOT/android" && ./gradlew --console=plain assembleRelease "$@")
mkdir -p "$OUT"
cp "$ROOT/android/app/build/outputs/apk/release/app-release.apk" "$OUT/$NAME"
if [ -n "${KEYSTORE:-}" ]; then echo "wrote $OUT/$NAME, signed with $KEYSTORE"; else echo "wrote $OUT/$NAME, signed with the debug key"; fi
