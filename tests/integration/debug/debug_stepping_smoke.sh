#!/usr/bin/env bash
# Stepping through stepping.iron's main in a --debug build (#312) visits
# its lines in source order: 7, 8, 9, 8, 9, ... The locals declared up
# front take the function's first line, and the copies that end the
# entry block used to take it too, so stepping went 7, 8, 7, 8.
#
# Usage: debug_stepping_smoke.sh <ironc> <source dir>
# Runs with gdb and with lldb, each skipped when missing or when it
# cannot run a program on this host.
set -euo pipefail

IRONC="$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"
SRC_ROOT="$(cd "$2" && pwd)"
SRC="$SRC_ROOT/tests/integration/debug/stepping.iron"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
cd "$work"
"$IRONC" build "$SRC" --debug -o stepping >/dev/null 2>&1
want="7 8 9 8 9 8"

check() {  # check <debugger> <lines visited>
    local got
    got="$(head -6 <<< "$2" | tr '\n' ' ' | sed 's/ $//')"
    [ "$got" = "$want" ] || { echo "FAIL: $1 steps through main as '$got', want '$want'"; exit 1; }
    echo "$1: stepping visits $want"
}

if command -v gdb >/dev/null 2>&1; then
    out="$(gdb -q -batch -ex "break stepping.iron:7" -ex run \
              -ex next -ex next -ex next -ex next -ex next ./stepping 2>&1 || true)"
    if grep -q "stepping.iron:7" <<< "$out"; then
        # The stop prints "Iron_main () at .../stepping.iron:7" then
        # "7<TAB>..."; each `next` prints "<line><TAB><source>".
        check gdb "$(grep -E $'^[0-9]+\t' <<< "$out" | cut -f1)"
    else
        echo "gdb cannot run the program here: skipped"
    fi
else
    echo "gdb not installed: skipped"
fi

if command -v lldb >/dev/null 2>&1 && lldb --version >/dev/null 2>&1; then
    out="$(lldb -b -o "b stepping.iron:7" -o run -o next -o next -o next -o next -o next \
                ./stepping 2>&1 || true)"
    if grep -q "stop reason" <<< "$out"; then
        check lldb "$(grep -oE 'frame #0: .* at stepping.iron:[0-9]+' <<< "$out" |
                      sed -E 's/.*stepping.iron://')"
    else
        echo "lldb cannot run the program here: skipped"
    fi
else
    echo "lldb not installed: skipped"
fi
echo "PASS"
