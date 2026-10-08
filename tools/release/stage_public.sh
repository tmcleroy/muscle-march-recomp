#!/bin/sh
# Stages the public repository: this checkout's HEAD as one fresh commit in a separate folder
# (default ../muscle-march-public), without private files, then audits it. It never pushes.
set -eu
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
DEST="${1:-$ROOT/../muscle-march-public}"
EXCLUDE="docs/dev/LEGAL_AUDIT.md docs/dev/HANDOFF.md"
VERSION="$(cat "$ROOT/VERSION")"

rm -rf "$DEST" && mkdir -p "$DEST"
git -C "$ROOT" archive HEAD | tar -x -C "$DEST"
for path in $EXCLUDE; do rm -f "$DEST/$path"; done

cd "$DEST"
fail=0
check() { if [ -n "$2" ]; then echo "AUDIT FAIL: $1"; echo "$2" | head -5; fail=1; else echo "ok: $1"; fi; }
check "no game file types" "$(find . -type f \( -iname '*.wad' -o -iname '*.dol' -o -iname '*.rel' -o -iname '*.app' -o -iname '*.arc' -o -iname '*.szs' -o -iname '*.brres' -o -iname '*.tpl' -o -iname '*.brstm' -o -iname '*.brsar' -o -iname '*.tmd' -o -iname '*.tik' -o -iname '*.sav' -o -iname '*.bnr' \) | grep -v 'third_party/dolphin/Data')"
check "no 32-digit hex keys (outside known checksums)" "$(grep -rnIE '\b[0-9a-fA-F]{32}\b' . | grep -viE 'sha256|sha1|URL_HASH|toolchain.cpp' || true)"
check "no home folder paths" "$(grep -rnI -e '/Users/' -e '/home/deck' -e 'C:\\\\Users' . | grep -v 'home/deck/Applications' || true)"
check "no generated code or dtk lists" "$(find . -path '*/build/*' -o -name 'symbols.txt' -o -name 'splits.txt' -o -name 'dol_functions.csv' -path '*musclemarch*' | head -5)"
check "no file over 1 MB" "$(find . -type f -size +1024k)"
[ "$fail" = 0 ] || { echo "not committed: fix the audit failures"; exit 1; }

git init -q -b main
git add -A
git commit -q -m "Muscle March Recomp $VERSION

First public beta. See README.md." 
echo "staged $(git rev-list --count HEAD) commit, $(git ls-files | wc -l | tr -d ' ') files in $DEST (not pushed)"
