#!/bin/sh
# Stages the public repository from this checkout's HEAD, without private files, and audits it.
# It never pushes.
#   stage_public.sh [DEST]                    a fresh one-commit repository (the first publication)
#   stage_public.sh --update "message" [DEST] a new commit on top of DEST's history (later updates)
# DEST defaults to ../muscle-march-public.
set -eu
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
UPDATE=""
if [ "${1:-}" = "--update" ]; then
    UPDATE="$2"
    shift 2
fi
DEST="${1:-$ROOT/../muscle-march-public}"
EXCLUDE="docs/dev/LEGAL_AUDIT.md docs/dev/HANDOFF.md"
VERSION="$(cat "$ROOT/VERSION")"

EXPORT="$(mktemp -d)"
trap 'rm -rf "$EXPORT"' EXIT
git -C "$ROOT" archive HEAD | tar -x -C "$EXPORT"
for path in $EXCLUDE; do rm -f "$EXPORT/$path"; done

cd "$EXPORT"
fail=0
check() { if [ -n "$2" ]; then echo "AUDIT FAIL: $1"; echo "$2" | head -5; fail=1; else echo "ok: $1"; fi; }
check "no game file types" "$(find . -type f \( -iname '*.wad' -o -iname '*.dol' -o -iname '*.rel' -o -iname '*.app' -o -iname '*.arc' -o -iname '*.szs' -o -iname '*.brres' -o -iname '*.tpl' -o -iname '*.brstm' -o -iname '*.brsar' -o -iname '*.tmd' -o -iname '*.tik' -o -iname '*.sav' -o -iname '*.bnr' \) | grep -v 'third_party/dolphin/Data')"
check "no 32-digit hex keys (outside known checksums)" "$(grep -rnIE '\b[0-9a-fA-F]{32}\b' . | grep -viE 'sha256|sha1|URL_HASH|toolchain.cpp' || true)"
check "no home folder paths" "$(grep -rnI -e '/Users/' -e '/home/deck' -e 'C:\\\\Users' . | grep -v 'home/deck/Applications' | grep -v '/Users/Shared' || true)"
check "no generated code or dtk lists" "$(find . -path '*/build/*' -o -name 'symbols.txt' -o -name 'splits.txt' -o -name 'dol_functions.csv' -path '*musclemarch*' | head -5)"
check "no file over 1 MB" "$(find . -type f -size +1024k)"
[ "$fail" = 0 ] || { echo "not committed: fix the audit failures"; exit 1; }

if [ -n "$UPDATE" ]; then
    [ -d "$DEST/.git" ] || { echo "$DEST is not a repository; stage it first without --update"; exit 1; }
    rsync -a --delete --exclude .git "$EXPORT/" "$DEST/"
    cd "$DEST"
    git add -A
    if git diff --cached --quiet; then
        echo "nothing changed"
        exit 0
    fi
    git commit -q -m "$UPDATE"
else
    rm -rf "$DEST" && mkdir -p "$DEST"
    cp -R "$EXPORT/." "$DEST/"
    cd "$DEST"
    git init -q -b main
    git add -A
    git commit -q -m "Muscle March Recomp $VERSION

First public beta. See README.md."
fi
echo "staged in $DEST: $(git log --oneline -1) (not pushed)"
