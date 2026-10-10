#!/usr/bin/env bash
# `ironc build --debug` (#312): debug info that maps to the .iron source.
#
# Usage: debug_info_smoke.sh <ironc> <source dir>
#
#   1. The --debug build prints what the normal build prints.
#   2. The generated C maps each statement to its Iron line (#line) and
#      calls area() instead of inlining it (a breakpoint in area is hit).
#   3. With gdb installed: a breakpoint on stepping.iron:3 stops in
#      area(w, h), called from main at stepping.iron:9, and the locals
#      show under their Iron names (product, total, i, names).
#   4. With gdb, and with lldb: the `locals` command of lib/debug lists
#      names.iron's bindings without the compiler's temporaries, a var a
#      closure captures included, inside the closure and outside it.
set -euo pipefail
# Checks read gdb's output from a here-string: `echo "$out" | grep -q` let
# grep exit at the first match while echo was still writing a long output
# (a runtime thread per core), and pipefail failed the check on SIGPIPE.

IRONC="$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"
SRC_ROOT="$(cd "$2" && pwd)"
SRC="$SRC_ROOT/tests/integration/debug/stepping.iron"
NAMES="$SRC_ROOT/tests/integration/debug/names.iron"

# check_locals <debugger> <output of `locals` at names.iron:12, then :15>
check_locals() {
    for want in "x = 3" "doubled = 6" "count = 42" "step = 2" 'greeting = "hi"' "r = 48"; do
        grep -q "^$want\$" <<< "$2" ||
            { echo "FAIL: $1 locals: no '$want'"; echo "$2"; exit 1; }
    done
    if grep -qE '^(_v[0-9]|_ref_|_e |_env)' <<< "$2"; then
        echo "FAIL: $1 locals shows compiler temporaries"; echo "$2"; exit 1
    fi
    echo "$1: locals under Iron names, temporaries hidden"
}

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
    out="$(gdb -q -batch -ex "break stepping.iron:3" -ex run -ex bt -ex "info locals" \
              -ex up -ex "info locals" ./dbg 2>&1 || true)"
    grep -q "Iron_area (w=0, h=2) at .*stepping.iron:3" <<< "$out" ||
        { echo "FAIL: breakpoint on stepping.iron:3 not hit in area(w, h)"; echo "$out"; exit 1; }
    grep -q "Iron_main () at .*stepping.iron:9" <<< "$out" ||
        { echo "FAIL: backtrace does not show main at stepping.iron:9"; echo "$out"; exit 1; }
    for name in "product = 0" "total = 0" "i = 0" "names = "; do
        grep -q "^$name" <<< "$out" ||
            { echo "FAIL: local '$name' not shown under its Iron name"; echo "$out"; exit 1; }
    done
    echo "gdb: breakpoint, backtrace and locals under Iron names"

    # The value printers shipped in lib/debug: a list of strings reads
    # as its elements.
    out="$(gdb -q -batch -ex "source $SRC_ROOT/src/debug/iron_gdb.py" \
              -ex "break stepping.iron:12" -ex run -ex "print names" ./dbg 2>&1 || true)"
    grep -q '= \[2\] = {"a", "b"}' <<< "$out" ||
        { echo "FAIL: iron_gdb.py does not print names as its elements"; echo "$out"; exit 1; }
    echo "gdb: value printers"

    # `iron debug` builds with --debug and starts gdb with the printers.
    IRON="$(dirname "$IRONC")/iron"
    if [ -x "$IRON" ]; then
        out="$(printf 'break stepping.iron:12\nrun\nprint names\ncontinue\n' |
               "$IRON" debug "$SRC" --gdb 2>&1 || true)"
        grep -q '= \[2\] = {"a", "b"}' <<< "$out" ||
            { echo "FAIL: iron debug did not load the printers"; echo "$out"; exit 1; }
        echo "iron debug: gdb with the printers"
    fi

    # `locals` (iron_gdb.py) lists the Iron bindings only: no compiler
    # temporaries, and a var a closure captures under its own name, inside
    # the closure and in the function that declares it.
    "$IRONC" build "$NAMES" --debug -o names >/dev/null 2>&1
    out="$(gdb -q -batch -ex "source $SRC_ROOT/src/debug/iron_gdb.py" \
              -ex "break names.iron:12" -ex "break names.iron:15" -ex run -ex locals \
              -ex continue -ex locals ./names 2>&1 || true)"
    check_locals gdb "$out"
else
    echo "gdb not installed: skipped the gdb check"
fi

if command -v lldb >/dev/null 2>&1 && lldb --version >/dev/null 2>&1; then
    "$IRONC" build "$NAMES" --debug -o names >/dev/null 2>&1
    out="$(lldb -b -o "command script import $SRC_ROOT/src/debug/iron_lldb.py" \
                -o "b names.iron:12" -o "b names.iron:15" -o run -o locals \
                -o continue -o locals ./names 2>&1 || true)"
    if ! grep -q "stop reason = breakpoint" <<< "$out"; then
        # No debugserver or no permission to debug (a locked-down CI host).
        echo "lldb cannot run the program here: skipped the lldb check"; echo "$out" | tail -5
    else
        # lldb prints "(type) name = value"; keep "name = value".
        check_locals lldb "$(sed -E 's/^\([^)]*\) //' <<< "$out")"
    fi
else
    echo "lldb not installed: skipped the lldb check"
fi
echo "PASS"
