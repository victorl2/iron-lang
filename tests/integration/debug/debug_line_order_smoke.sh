#!/usr/bin/env bash
# A breakpoint in a --debug build stops when the program reaches its line,
# not earlier (#595): a constant the optimizer placed ahead of its
# statement kept that statement's line (line_order.iron:11 stopped before
# the loop), and the implicit return had no line of its own, so the last
# line of a function could stop in the middle of an earlier statement.
#
# Usage: debug_line_order_smoke.sh <ironc> <source dir>
# Runs with lldb and with gdb, each skipped when missing or unable to run.
set -euo pipefail

IRONC="$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"
SRC_ROOT="$(cd "$2" && pwd)"
SRC="$SRC_ROOT/tests/integration/debug/line_order.iron"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
cd "$work"
"$IRONC" build "$SRC" --debug -o line_order >/dev/null 2>&1

# At line 11, n has been through the loop (2 * 27^3); at line 18, seen is
# (20 + 1) * 2.
check() {  # check <debugger> <output>
    grep -qE '\bn = 39366\b' <<< "$2" ||
        { echo "FAIL: $1: breakpoint on line_order.iron:11 stopped before the loop ended"; echo "$2"; exit 1; }
    grep -qE '\bseen = 42\b' <<< "$2" ||
        { echo "FAIL: $1: breakpoint on line_order.iron:18 stopped before line 17 ran"; echo "$2"; exit 1; }
    echo "$1: breakpoints stop on their lines"
}

if command -v lldb >/dev/null 2>&1 && lldb --version >/dev/null 2>&1; then
    out="$(lldb -b -o "b line_order.iron:11" -o "b line_order.iron:18" -o run \
                -o "frame variable n" -o continue -o "frame variable seen" ./line_order 2>&1 || true)"
    if grep -q "stop reason" <<< "$out"; then check lldb "$out"; else echo "lldb cannot run the program here: skipped"; fi
else
    echo "lldb not installed: skipped"
fi

if command -v gdb >/dev/null 2>&1; then
    out="$(gdb -q -batch -ex "break line_order.iron:11" -ex "break line_order.iron:18" -ex run \
              -ex "print n" -ex continue -ex "print seen" ./line_order 2>&1 || true)"
    if grep -q "line_order.iron:11" <<< "$out"; then
        out="$(sed -E 's/^\$1 = /n = /; s/^\$2 = /seen = /' <<< "$out")"
        check gdb "$out"
    else
        echo "gdb cannot run the program here: skipped"
    fi
else
    echo "gdb not installed: skipped"
fi
echo "PASS"
