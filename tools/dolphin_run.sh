#!/bin/sh
# Run Muscle March in Dolphin with the project's own user folder (dolphin_user/),
# dump every frame as PNG, and stop after N seconds of wall time.
#   tools/dolphin_run.sh [seconds] [extra Dolphin args...]   e.g. -m movies/x.dtm
# Frames land in dolphin_user/Dump/Frames/framedump_<n>.png (cleared first).
set -e
cd "$(dirname "$0")/.."
DOLPHIN=${DOLPHIN:-/Applications/Dolphin.app/Contents/MacOS/Dolphin}
U="$PWD/dolphin_user"
SECS=${1:-30}; [ $# -gt 0 ] && shift
mkdir -p "$U/Config" "$U/Dump/Frames"
python3 tools/dolphin_config.py "$U"
rm -f "$U"/Dump/Frames/*.png
"$DOLPHIN" -u "$U" -b -n 00010001574d4d45 "$@" > "$U/run.log" 2>&1 &
PID=$!
sleep "$SECS"
kill -TERM $PID 2>/dev/null || true
wait $PID 2>/dev/null || true
echo "$(ls "$U"/Dump/Frames | wc -l | tr -d ' ') frames in $U/Dump/Frames"
