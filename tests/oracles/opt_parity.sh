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
# Env:   PARITY_JOBS (default: online CPUs), PARITY_TIMEOUT seconds per run
#        (default 10), IRON_TEST_TIMEOUT_SCALE (default 1).
# Exit:  0 when every fixture agrees, 1 otherwise. Mismatches are listed
#        with a short diff; skipped fixtures are counted, not listed.
set -uo pipefail

IRON_BIN=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
shift
REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
if [ "$#" -eq 0 ]; then
    set -- "$REPO/tests/integration/v4" "$REPO/tests/algorithms"
fi
JOBS=${PARITY_JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)}
TIMEOUT=${PARITY_TIMEOUT:-10}
TIMEOUT_SCALE=${IRON_TEST_TIMEOUT_SCALE:-1}
case "$JOBS:$TIMEOUT:$TIMEOUT_SCALE" in
    *[!0-9:]*|0:*|*:0:*|*:0) echo "opt_parity: job and timeout values must be positive integers" >&2; exit 2 ;;
esac
WORK=$(mktemp -d "${TMPDIR:-/tmp}/iron_parity.XXXXXX")
trap 'rm -rf "$WORK"' EXIT

run_with_timeout() {
    local seconds=$1
    shift
    if command -v timeout >/dev/null 2>&1; then timeout "$seconds" "$@"
    elif command -v gtimeout >/dev/null 2>&1; then gtimeout "$seconds" "$@"
    else "$@"; fi
}
export -f run_with_timeout
export IRON_BIN WORK TIMEOUT TIMEOUT_SCALE

check_one() {
    local f="$1" started=$SECONDS
    local head20
    head20=$(head -n 20 "$f")
    # Fixtures that cannot run here or are parked: skip.
    if echo "$head20" | grep -qE '@compile-only|@expected-pass-after|@expect-panic|@skip-no-optimize|@file:'; then
        echo "SKIP $((SECONDS - started))s $f"; return 0
    fi
    local name dir fixture_timeout
    name=$(basename "$f" .iron)
    dir="$WORK/$(echo "$f" | tr '/' '_')"
    mkdir -p "$dir/o" "$dir/n"
    fixture_timeout=$(printf '%s\n' "$head20" | sed -nE 's/^[[:space:]]*--[[:space:]]*@parity-timeout:[[:space:]]*([0-9]+).*/\1/p' | head -1)
    fixture_timeout=${fixture_timeout:-$TIMEOUT}
    fixture_timeout=$((fixture_timeout * TIMEOUT_SCALE))

    # Each pair consumes two slots: optimized and non-optimized compilation
    # run together, while the outer pool keeps total compiler processes at or
    # below PARITY_JOBS.
    local bo bn po pn
    (cd "$dir/o" && "$IRON_BIN" build "$f" >/dev/null 2>"$dir/o.err") & po=$!
    (cd "$dir/n" && "$IRON_BIN" build --no-optimize "$f" >/dev/null 2>"$dir/n.err") & pn=$!
    wait "$po"; bo=$?
    wait "$pn"; bn=$?
    if [ $bo -ne 0 ] && [ $bn -ne 0 ]; then
        echo "SKIP $((SECONDS - started))s $f (does not build)"; return 0
    fi
    if [ $bo -ne 0 ] || [ $bn -ne 0 ]; then
        echo "BUILD $((SECONDS - started))s $f (optimized rc=$bo, --no-optimize rc=$bn)"
        grep -m1 -E "error" "$dir/o.err" "$dir/n.err" | sed 's/^/    /'
        return 0
    fi
    local ro rn
    run_with_timeout "$fixture_timeout" "$dir/o/$name" >"$dir/o.out" 2>&1 & po=$!
    run_with_timeout "$fixture_timeout" "$dir/n/$name" >"$dir/n.out" 2>&1 & pn=$!
    wait "$po"; ro=$?
    wait "$pn"; rn=$?
    if [ $ro -ne $rn ] || ! cmp -s "$dir/o.out" "$dir/n.out"; then
        echo "DIFF $((SECONDS - started))s $f (exit $ro vs $rn, timeout=${fixture_timeout}s)"
        diff "$dir/o.out" "$dir/n.out" | head -6 | sed 's/^/    /'
        return 0
    fi
    echo "OK $((SECONDS - started))s $f"
}
export -f check_one

abs_dirs=()
for d in "$@"; do abs_dirs+=("$(cd "$d" && pwd)"); done
pair_jobs=$((JOBS / 2))
[ "$pair_jobs" -lt 1 ] && pair_jobs=1
find "${abs_dirs[@]}" -type f -name '*.iron' | sort \
    | xargs -P "$pair_jobs" -I{} bash -c 'check_one "$@"' _ {} > "$WORK/results.txt"

ok=$(grep -c '^OK ' "$WORK/results.txt")
skip=$(grep -c '^SKIP ' "$WORK/results.txt")
bad=$(grep -cE '^(DIFF|BUILD) ' "$WORK/results.txt")
awk '/^(DIFF|BUILD) /{p=1} /^(OK|SKIP) /{p=0} p' "$WORK/results.txt"
grep 'does not build' "$WORK/results.txt" | sed 's/^/NOTE /' 
echo "=== slowest optimizer parity fixtures ==="
awk '/^(OK|DIFF|BUILD) / { seconds=$2; sub(/s$/, "", seconds); print seconds, $0 }' "$WORK/results.txt" \
    | sort -nr | head -10 | cut -d' ' -f2-
echo "=== opt parity: $ok agree, $bad disagree, $skip skipped"
[ "$bad" -eq 0 ]
