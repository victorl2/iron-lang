#!/usr/bin/env bash
# lib/debug's list formatters on a `layout: soa` list: with gdb and with
# lldb, soa.iron's `cols` (stored column by column) shows the same elements
# as `rows` (the same list with the default layout), and an empty soa list
# shows as empty.
#
# Usage: debug_soa_smoke.sh <ironc> <source dir>
#
# Each debugger is skipped when it is not installed, or when it cannot run
# a program on this host (no debugserver, no ptrace permission).
set -euo pipefail

IRONC="$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"
SRC_ROOT="$(cd "$2" && pwd)"
SRC="$SRC_ROOT/tests/integration/debug/soa.iron"
LINE=$(grep -n 'println(' "$SRC" | head -1 | cut -d: -f1)
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
cd "$work"
"$IRONC" build "$SRC" --debug -o soa >/dev/null 2>&1

# The text the debugger printed for one list: from the line that names it
# up to the line that names the next one, without the names, addresses and
# spaces. `cols` and `rows` must agree.
elements() {
    awk -v from=" $2 = " -v to=" $3 = " \
        'index(" " $0, to) { on = 0 } index(" " $0, from) { on = 1 } on' <<< "$1" |
        sed -E "s/(^| )$2 = //; s/0x[0-9a-f]+//g" | tr -d ' \n'
}

check() {
    local dbg="$1" out="$2"
    for want in 'id = 99' 'name = "mote2"' 'x = 2.5' 'kind = 100000' 'alive = true'; do
        [ "$(grep -cF -- "$want" <<< "$out")" -ge 2 ] ||
            { echo "FAIL: $dbg does not show '$want' for both lists"; echo "$out"; exit 1; }
    done
    local c r
    c="$(elements "$out" cols rows)"
    r="$(elements "$out" rows none)"
    [ -n "$c" ] && [ "$c" = "$r" ] ||
        { echo "FAIL: $dbg shows cols and rows differently"; echo "cols: $c"; echo "rows: $r"; echo "$out"; exit 1; }
}

if command -v gdb >/dev/null 2>&1; then
    out="$(gdb -q -batch -ex "source $SRC_ROOT/src/debug/iron_gdb.py" \
              -ex "break soa.iron:$LINE" -ex run \
              -ex "print cols" -ex "print rows" -ex "print none" ./soa 2>&1 || true)"
    if ! grep -q "soa.iron:$LINE" <<< "$out"; then
        echo "gdb cannot run the program here: skipped"
    else
        out="$(sed -E 's/^\$[0-9]+ = /LIST = /' <<< "$out" | awk '/^LIST = /{n++; sub(/^LIST/, n==1?"cols":n==2?"rows":"none")} {print}')"
        check gdb "$out"
        grep -qE '^none = \[0\]' <<< "$out" || { echo "FAIL: gdb: empty soa list"; echo "$out"; exit 1; }
        echo "gdb: soa lists"
    fi
else
    echo "gdb not installed: skipped"
fi

if command -v lldb >/dev/null 2>&1 && lldb --version >/dev/null 2>&1; then
    out="$(lldb -b -o "command script import $SRC_ROOT/src/debug/iron_lldb.py" \
                -o "b soa.iron:$LINE" -o run -o "frame variable cols rows none" ./soa 2>&1 || true)"
    if ! grep -q "stop reason = breakpoint" <<< "$out"; then
        echo "lldb cannot run the program here: skipped"
    else
        check lldb "$out"
        echo "lldb: soa lists"
    fi
else
    echo "lldb not installed: skipped"
fi
echo "PASS"
