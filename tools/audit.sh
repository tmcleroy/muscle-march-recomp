#!/bin/sh
# Checks the repository's tracked files for things that must never be committed (CONTRIBUTING.md):
# game files, keys, personal paths, generated code, unexpectedly large files. Run it before
# pushing; CI runs it on every pull request. Exits non-zero on a finding.
set -eu
cd "$(git rev-parse --show-toplevel)"
LIST="$(mktemp)"
trap 'rm -f "$LIST"' EXIT
git ls-files > "$LIST"

fail=0
check() { if [ -n "$2" ]; then echo "AUDIT FAIL: $1"; echo "$2" | head -10; fail=1; else echo "ok: $1"; fi; }
grep_files() { tr '\n' '\0' < "$LIST" | xargs -0 grep -nIE "$@" 2>/dev/null || true; }

check "no game file types" "$(grep -iE '\.(wad|dol|rel|app|arc|szs|brres|tpl|brstm|brsar|tmd|tik|sav|bnr|wbfs|rvz|iso|gcm)$' "$LIST" | grep -v 'third_party/dolphin/Data' || true)"
check "no 32-digit hex keys (outside known checksums)" "$(grep_files '\b[0-9a-fA-F]{32}\b' | grep -viE 'sha256|sha1|URL_HASH|toolchain.cpp' || true)"
check "no home folder paths" "$(grep_files -e '/Users/' -e '/home/[a-z]' -e 'C:\\\\Users' | grep -v -e '/Users/Shared' -e 'home/deck/Applications' -e '/home/deck/\.local' || true)"
check "no generated code or dtk lists" "$(grep -E '(^|/)build/|(^|/)symbols\.txt$|(^|/)splits\.txt$|musclemarch/analysis/(dol_functions|symbols)\.csv$' "$LIST" || true)"
check "no file over 1 MB" "$(tr '\n' '\0' < "$LIST" | xargs -0 find 2>/dev/null -prune -type f -size +1024k || true)"
[ "$fail" = 0 ] || { echo "audit failed"; exit 1; }
echo "audit passed ($(wc -l < "$LIST" | tr -d ' ') files)"
