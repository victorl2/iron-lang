#!/usr/bin/env bash
# Optimizer parity oracle.
#
# Builds every runnable fixture twice, with the default pipeline and with
# --no-optimize, runs both binaries and compares their output with each
# other. The two pipelines implement the same language, so any difference
# is a compiler bug in one of them, whether or not a .expected file exists.
#
# Usage: tests/oracles/opt_parity.sh <iron-binary> [dir ...]
#   dirs default to tests/integration/v4 and tests/algorithms.
# Env:   PARITY_JOBS (default 4), PARITY_TIMEOUT seconds per run (default 10).
# Exit:  0 when every fixture agrees, 1 otherwise. Mismatches are listed
#        with a short diff; skipped fixtures are counted, not listed.
set -uo pipefail

IRON_BIN=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
shift
REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
if [ "$#" -eq 0 ]; then
    set -- "$REPO/tests/integration/v4" "$REPO/tests/algorithms"
fi
JOBS=${PARITY_JOBS:-4}
TIMEOUT=${PARITY_TIMEOUT:-10}
WORK=$(mktemp -d "${TMPDIR:-/tmp}/iron_parity.XXXXXX")
trap 'rm -rf "$WORK"' EXIT

run_with_timeout() {
    if command -v timeout >/dev/null 2>&1; then timeout "$TIMEOUT" "$@"
    elif command -v gtimeout >/dev/null 2>&1; then gtimeout "$TIMEOUT" "$@"
    else "$@"; fi
}
export -f run_with_timeout
export IRON_BIN WORK TIMEOUT

check_one() {
    local f="$1"
    local head10
    head10=$(head -n 20 "$f")
    # Fixtures that cannot run here or are parked: skip.
    if echo "$head10" | grep -qE '@compile-only|@expected-pass-after|@expect-panic|@skip-no-optimize|@file:'; then
        echo "SKIP $f"; return 0
    fi
    local name dir
    name=$(basename "$f" .iron)
    dir="$WORK/$(echo "$f" | tr '/' '_')"
    mkdir -p "$dir/o" "$dir/n"
    local bo bn
    (cd "$dir/o" && "$IRON_BIN" build "$f" >/dev/null 2>"$dir/o.err"); bo=$?
    (cd "$dir/n" && "$IRON_BIN" build --no-optimize "$f" >/dev/null 2>"$dir/n.err"); bn=$?
    if [ $bo -ne 0 ] && [ $bn -ne 0 ]; then echo "SKIP $f (does not build)"; return 0; fi
    if [ $bo -ne 0 ] || [ $bn -ne 0 ]; then
        echo "BUILD $f (optimized rc=$bo, --no-optimize rc=$bn)"
        grep -m1 -E "error" "$dir/o.err" "$dir/n.err" | sed 's/^/    /'
        return 0
    fi
    run_with_timeout "$dir/o/$name" >"$dir/o.out" 2>&1; local ro=$?
    run_with_timeout "$dir/n/$name" >"$dir/n.out" 2>&1; local rn=$?
    if [ $ro -ne $rn ] || ! cmp -s "$dir/o.out" "$dir/n.out"; then
        echo "DIFF $f (exit $ro vs $rn)"
        diff "$dir/o.out" "$dir/n.out" | head -6 | sed 's/^/    /'
        return 0
    fi
    echo "OK $f"
}
export -f check_one

abs_dirs=()
for d in "$@"; do abs_dirs+=("$(cd "$d" && pwd)"); done
find "${abs_dirs[@]}" -type f -name '*.iron' | sort \
    | xargs -P "$JOBS" -I{} bash -c 'check_one "$@"' _ {} > "$WORK/results.txt"

ok=$(grep -c '^OK ' "$WORK/results.txt")
skip=$(grep -c '^SKIP ' "$WORK/results.txt")
bad=$(grep -cE '^(DIFF|BUILD) ' "$WORK/results.txt")
awk '/^(DIFF|BUILD) /{p=1} /^(OK|SKIP) /{p=0} p' "$WORK/results.txt"
grep 'does not build' "$WORK/results.txt" | sed 's/^/NOTE /' 
echo "=== opt parity: $ok agree, $bad disagree, $skip skipped"
[ "$bad" -eq 0 ]
