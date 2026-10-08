#!/usr/bin/env bash
# `ironc build --debug` (#312): debug info that maps to the .iron source.
#
# Usage: debug_info_smoke.sh <ironc> <source dir>
#
#   1. The --debug build prints what the normal build prints.
#   2. The generated C maps each statement to its Iron line (#line) and
#      calls area() instead of inlining it (a breakpoint in area is hit).
#   3. With gdb installed: a breakpoint on stepping.iron:2 stops in area,
#      called from main at stepping.iron:9.
set -euo pipefail

IRONC="$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"
SRC="$(cd "$2" && pwd)/tests/integration/debug/stepping.iron"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
cd "$work"

"$IRONC" build "$SRC" -o normal >/dev/null 2>&1
"$IRONC" build "$SRC" --debug -o dbg >/dev/null 2>&1
want="$(./normal)"
got="$(./dbg)"
[ "$want" = "$got" ] || { echo "FAIL: --debug output '$got' != '$want'"; exit 1; }

"$IRONC" build "$SRC" --debug --emit-c -o stepping >/dev/null 2>&1
c=".iron-build/stepping.c"
grep -q "^#line 2 \"$SRC\"" "$c" || { echo "FAIL: no '#line 2' for $SRC in $c"; exit 1; }
grep -q '^#line 1 "<iron-generated>"' "$c" || { echo "FAIL: generated code is not marked"; exit 1; }
calls=$(sed -n '/^void Iron_main(void) {/,/^}/p' "$c" | grep -c 'Iron_area(' || true)
[ "$calls" -ge 1 ] || { echo "FAIL: area() was inlined into main"; exit 1; }

if command -v gdb >/dev/null 2>&1; then
    out="$(gdb -q -batch -ex "break stepping.iron:2" -ex run -ex bt ./dbg 2>&1 || true)"
    echo "$out" | grep -q "Iron_area .* at .*stepping.iron:2" ||
        { echo "FAIL: breakpoint on stepping.iron:2 not hit in area"; echo "$out"; exit 1; }
    echo "$out" | grep -q "Iron_main () at .*stepping.iron:9" ||
        { echo "FAIL: backtrace does not show main at stepping.iron:9"; echo "$out"; exit 1; }
    echo "gdb: breakpoint and backtrace on Iron lines"
else
    echo "gdb not installed: skipped the debugger check"
fi
echo "PASS"
