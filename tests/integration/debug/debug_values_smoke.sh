#!/usr/bin/env bash
# lib/debug's value formatters (#312): with gdb and with lldb, values.iron's
# bindings print as Iron values: enums with payloads, rc and weak rc,
# rc lists, closures with their captures, optionals.
#
# Usage: debug_values_smoke.sh <ironc> <source dir>
#
# Each debugger is skipped when it is not installed, or when it cannot run
# a program on this host (no debugserver, no ptrace permission).
set -euo pipefail

IRONC="$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"
SRC_ROOT="$(cd "$2" && pwd)"
SRC="$SRC_ROOT/tests/integration/debug/values.iron"
LINE=$(grep -n 'println(' "$SRC" | head -1 | cut -d: -f1)
VARS="maybe nothing color rect empty ok err shared root child letters add"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
cd "$work"
"$IRONC" build "$SRC" --debug -o values >/dev/null 2>&1

# expect <debugger> <output> <fixed string>...
expect() {
    local dbg="$1" out="$2"
    shift 2
    for want in "$@"; do
        grep -qF -- "$want" <<< "$out" ||
            { echo "FAIL: $dbg does not show '$want'"; echo "$out"; exit 1; }
    done
}

common=('maybe = 7' 'nothing = null' 'rect = Rect(2, 3)' 'empty = Empty'
        'ok = Ok(5)' 'err = Err("negative")'
        'rc Point {x = 3, y = 4} (strong=2, weak=0)'
        'rc Node {name = "root", parent = null} (strong=1, weak=1)'
        'func __lambda_0 at values.iron:'
        'k = 10')

if command -v gdb >/dev/null 2>&1; then
    out="$(gdb -q -batch -ex "source $SRC_ROOT/src/debug/iron_gdb.py" \
              -ex "break values.iron:$LINE" -ex run -ex "info locals" ./values 2>&1 || true)"
    if ! grep -q "values.iron:$LINE" <<< "$out"; then
        echo "gdb cannot run the program here: skipped"
    else
        expect gdb "$out" "${common[@]}" 'color = Green' \
            'rc [2] = {"a", "b"} (strong=2, weak=0)'
        echo "gdb: values"
    fi
else
    echo "gdb not installed: skipped"
fi

if command -v lldb >/dev/null 2>&1 && lldb --version >/dev/null 2>&1; then
    out="$(lldb -b -o "command script import $SRC_ROOT/src/debug/iron_lldb.py" \
                -o "b values.iron:$LINE" -o run -o "frame variable $VARS" ./values 2>&1 || true)"
    if ! grep -q "stop reason = breakpoint" <<< "$out"; then
        echo "lldb cannot run the program here: skipped"
    else
        expect lldb "$out" "${common[@]}" 'color = Iron_Color_Green' \
            'letters = 0x' 'rc size=2 (strong=2, weak=0)'
        echo "lldb: values"
    fi
else
    echo "lldb not installed: skipped"
fi
echo "PASS"
