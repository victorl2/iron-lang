#!/bin/bash
# leak_check.sh - run fixtures under a leak detector.
#
# Builds every runnable fixture (<name>.iron with a <name>.expected sibling)
# in the given directories and runs the binary under `leaks --atExit` on
# macOS or valgrind on Linux, failing when either reports a leak. Fixtures
# that expect a panic are skipped: an aborting process leaks by design.
#
# usage: tests/oracles/leak_check.sh <iron-binary> <fixture-dir>...
#
# The runtime's own tracker (IRON_LEAK_CHECK=1) only sees `heap T`
# allocations; the containers (lists, maps, sets, strings) allocate with
# malloc, so an external detector is the oracle for them.
set -u
IRON=$(cd "$(dirname "$1")" && pwd)/$(basename "$1"); shift
if [ ! -x "$IRON" ]; then echo "leak_check: compiler not found: $IRON" >&2; exit 2; fi

detector=""
if [ "$(uname -s)" = Darwin ] && command -v leaks >/dev/null 2>&1; then
    detector=leaks
elif command -v valgrind >/dev/null 2>&1; then
    detector=valgrind
else
    echo "leak_check: no leak detector available (leaks or valgrind); skipping"
    exit 0
fi

work=$(mktemp -d "${TMPDIR:-/tmp}/iron-leaks.XXXXXX")
trap 'rm -rf "$work"' EXIT
pass=0; fail=0; skipped=0
for dir in "$@"; do
    for src in "$dir"/*.iron; do
        [ -e "$src" ] || continue
        name=$(basename "$src" .iron)
        [ -f "$dir/$name.expected" ] || { skipped=$((skipped+1)); continue; }
        # A panicking process leaks by design; a few fixtures demonstrate
        # the `leak` keyword or the forgotten-free lint and say so.
        if head -n 10 "$src" | grep -qE '^[[:space:]]*--[[:space:]]*@(expect-panic|compile-only|leaks-by-design)'; then
            skipped=$((skipped+1)); continue
        fi
        abs_src=$(cd "$(dirname "$src")" && pwd)/$(basename "$src")
        if ! (cd "$work" && "$IRON" build "$abs_src" >/dev/null 2>"$work/$name.build"); then
            echo "[FAIL] ${dir%/}/$name (build failed)"; cat "$work/$name.build" >&2; fail=$((fail+1)); continue
        fi
        bin="$work/$name"
        if [ "$detector" = leaks ]; then
            out=$(leaks --atExit -- "$bin" 2>&1)
            if echo "$out" | grep -qE 'Process [0-9]+: 0 leaks for 0 total leaked bytes'; then
                pass=$((pass+1))
            else
                echo "[FAIL] ${dir%/}/$name (leaks)"; echo "$out" | grep -E 'leaks for|ROOT LEAK|Iron_' | head -20 >&2; fail=$((fail+1))
            fi
        else
            if valgrind --quiet --leak-check=full --errors-for-leak-kinds=definite,indirect \
                        --error-exitcode=9 "$bin" >/dev/null 2>"$work/$name.vg"; then
                pass=$((pass+1))
            else
                echo "[FAIL] ${dir%/}/$name (valgrind)"; head -40 "$work/$name.vg" >&2; fail=$((fail+1))
            fi
        fi
    done
done
echo "leak_check ($detector): PASS=$pass FAIL=$fail SKIPPED=$skipped"
[ "$fail" -eq 0 ]
