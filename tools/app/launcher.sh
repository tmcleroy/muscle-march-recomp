#!/bin/sh
# MuscleMarch.app launcher: saves and settings live in Application Support; the
# game data is read from the extracted folder of the checkout that built the app.
HERE="$(cd "$(dirname "$0")" && pwd)"
DATA="$HOME/Library/Application Support/MuscleMarch"
mkdir -p "$DATA"
cd "$DATA" || exit 1
EXTRACTED="@EXTRACTED@"
if [ ! -f "$EXTRACTED/main.dol" ]; then
    osascript -e 'display alert "Muscle March" message "Game data not found. Run make extract in the repo first."' >/dev/null 2>&1
    exit 1
fi
exec "$HERE/musclemarch-bin" "$EXTRACTED" 0 nand
