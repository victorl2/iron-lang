#!/bin/bash
# run_layout_bench.sh: Compile and run layout optimization benchmark
# Usage: ./run_layout_bench.sh [path/to/iron]
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
IRON_BIN="${1:-${SCRIPT_DIR}/../../build/iron}"

if [ ! -x "$IRON_BIN" ]; then
    echo "error: iron not found at $IRON_BIN" >&2
    exit 1
fi

echo "=== Layout Optimization Benchmark ==="
echo ""

# Compile
echo "Compiling layout_bench.iron..."
TMPDIR=$(mktemp -d)
trap "rm -rf $TMPDIR" EXIT

(cd "$TMPDIR" && "$IRON_BIN" build "$SCRIPT_DIR/layout_bench.iron") 2>&1

if [ ! -f "$TMPDIR/layout_bench" ]; then
    echo "FAIL: compilation failed"
    exit 1
fi

# Run and compare
echo "Running..."
ACTUAL=$("$TMPDIR/layout_bench" 2>&1)
EXPECTED=$(cat "$SCRIPT_DIR/layout_bench.expected")

# Trim trailing newlines for comparison
ACTUAL="${ACTUAL%$'\n'}"
EXPECTED="${EXPECTED%$'\n'}"

if [ "$ACTUAL" = "$EXPECTED" ]; then
    echo "PASS: all layout optimization tests correct"
    echo ""
    echo "Results:"
    echo "  2-field (1 accessed, 50%): $(echo "$ACTUAL" | sed -n '1p')"
    echo "  4-field (1 accessed, 25%): $(echo "$ACTUAL" | sed -n '2p')"
    echo "  4-field (4 accessed, 100%): $(echo "$ACTUAL" | sed -n '3p')"
    echo "  Dead field elimination:     $(echo "$ACTUAL" | sed -n '4p')"
    echo "  Common field factoring:     $(echo "$ACTUAL" | sed -n '5p')"
    echo "  Direct concrete access:     $(echo "$ACTUAL" | sed -n '6p'), $(echo "$ACTUAL" | sed -n '7p'), $(echo "$ACTUAL" | sed -n '8p')"
    exit 0
else
    echo "FAIL: output mismatch"
    echo "--- Expected ---"
    echo "$EXPECTED"
    echo "--- Actual ---"
    echo "$ACTUAL"
    diff <(echo "$EXPECTED") <(echo "$ACTUAL") || true
    exit 1
fi
