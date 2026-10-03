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
if [ "${1:-}" != "--one" ]; then
    IRON=$(cd "$(dirname "$1")" && pwd)/$(basename "$1"); shift
    if [ ! -x "$IRON" ]; then echo "leak_check: compiler not found: $IRON" >&2; exit 2; fi
fi

detector=""
if [ "$(uname -s)" = Darwin ] && command -v leaks >/dev/null 2>&1; then
    detector=leaks
elif command -v valgrind >/dev/null 2>&1; then
    detector=valgrind
else
    echo "leak_check: no leak detector available (leaks or valgrind); skipping"
    exit 0
fi

# One fixture: build it, run it under the detector, print one result line
# ("PASS", or "FAIL <reason>" followed by the detector's report indented).
# Fixtures run in parallel, so each writes into its own directory.
check_one() {
    local src=$1 dir name abs_src d bin out
    dir=$(dirname "$src"); name=$(basename "$src" .iron)
    abs_src=$(cd "$dir" && pwd)/$(basename "$src")
    d="$work/$name.$$.$RANDOM"; mkdir -p "$d"
    if ! (cd "$d" && "$IRON" build "$abs_src" >/dev/null 2>"$d/build.log"); then
        echo "[FAIL] ${dir%/}/$name (build failed)"; sed 's/^/    /' "$d/build.log"; return
    fi
    bin="$d/$name"
    if [ "$detector" = leaks ]; then
        out=$(leaks --atExit -- "$bin" 2>&1)
        if echo "$out" | grep -qE 'Process [0-9]+: 0 leaks for 0 total leaked bytes'; then
            echo "[PASS] ${dir%/}/$name"
        else
            echo "[FAIL] ${dir%/}/$name (leaks)"; echo "$out" | grep -E 'leaks for|ROOT LEAK|Iron_' | head -20 | sed 's/^/    /'
        fi
    else
        if valgrind --quiet --leak-check=full --errors-for-leak-kinds=definite,indirect \
                    --error-exitcode=9 "$bin" >/dev/null 2>"$d/vg.log"; then
            echo "[PASS] ${dir%/}/$name"
        else
            echo "[FAIL] ${dir%/}/$name (valgrind)"; head -40 "$d/vg.log" | sed 's/^/    /'
        fi
    fi
}

if [ "${1:-}" = "--one" ]; then
    # Worker entry point used by xargs below.
    detector=$2; IRON=$3; work=$4; shift 4
    for src in "$@"; do check_one "$src"; done
    exit 0
fi

work=$(mktemp -d "${TMPDIR:-/tmp}/iron-leaks.XXXXXX")
trap 'rm -rf "$work"' EXIT


# Collect the runnable fixtures. A panicking process leaks by design; a
# few fixtures demonstrate the `leak` keyword or the forgotten-free lint
# and say so.
skipped=0
: > "$work/list"
for dir in "$@"; do
    for src in "$dir"/*.iron; do
        [ -e "$src" ] || continue
        name=$(basename "$src" .iron)
        [ -f "$dir/$name.expected" ] || { skipped=$((skipped+1)); continue; }
        if head -n 10 "$src" | grep -qE '^[[:space:]]*--[[:space:]]*@(expect-panic|compile-only|leaks-by-design)'; then
            skipped=$((skipped+1)); continue
        fi
        echo "$src" >> "$work/list"
    done
done

# Half the cores by default: the oracle runs inside ctest next to other
# tests, and a build plus a detector per worker is heavy (a hosted macOS
# runner lost contact with GitHub when every core was taken).
cores=$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)
jobs=${LEAK_CHECK_JOBS:-$(( cores > 1 ? cores / 2 : 1 ))}
self=$(cd "$(dirname "$0")" && pwd)/$(basename "$0")
tr '\n' '\0' < "$work/list" | xargs -0 -n 8 -P "$jobs" bash "$self" --one "$detector" "$IRON" "$work" > "$work/results" 2>&1
grep -v '^\[PASS\]' "$work/results" >&2
pass=$(grep -c '^\[PASS\]' "$work/results" || true)
fail=$(grep -c '^\[FAIL\]' "$work/results" || true)
echo "leak_check ($detector): PASS=$pass FAIL=$fail SKIPPED=$skipped"
[ "$fail" -eq 0 ]
