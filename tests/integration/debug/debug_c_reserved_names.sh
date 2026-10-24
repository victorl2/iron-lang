#!/usr/bin/env bash
# C-only spellings used in debug information are decoded by the gdb and LLDB
# formatters so users continue to see the original Iron names (#725).
set -euo pipefail

IRONC="$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"
SRC_ROOT="$(cd "$2" && pwd)"
SRC="$SRC_ROOT/tests/integration/debug/c_reserved_names.iron"
LINE=$(grep -n 'println(' "$SRC" | head -1 | cut -d: -f1)
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
cd "$work"
"$IRONC" build "$SRC" --debug -o reserved >/dev/null 2>&1

expect_names() {
    local dbg="$1" out="$2"
    for want in 'default = 3' 'register = 4' 'default = 5' 'choice = default' 'payload = register(7)'; do
        grep -qF -- "$want" <<< "$out" || {
            echo "FAIL: $dbg does not show '$want'"
            echo "$out"
            exit 1
        }
    done
    local values
    values=$(grep -E '^(default|register|item|choice|payload) =' <<< "$out" || true)
    if grep -q 'Iron__id_' <<< "$values"; then
        echo "FAIL: $dbg exposes an encoded C name in the Iron locals view"
        echo "$out"
        exit 1
    fi
}

if command -v gdb >/dev/null 2>&1; then
    out="$(gdb -q -batch -ex "source $SRC_ROOT/src/debug/iron_gdb.py" \
              -ex "break c_reserved_names.iron:$LINE" -ex run \
              -ex "locals" ./reserved 2>&1 || true)"
    if grep -q "c_reserved_names.iron:$LINE" <<< "$out"; then
        expect_names gdb "$out"
        echo "gdb: reserved names"
    else
        echo "gdb cannot run the program here: skipped"
    fi
else
    echo "gdb not installed: skipped"
fi

if command -v lldb >/dev/null 2>&1 && lldb --version >/dev/null 2>&1; then
    out="$(lldb -b -o "command script import $SRC_ROOT/src/debug/iron_lldb.py" \
                -o "b c_reserved_names.iron:$LINE" -o run -o locals \
                ./reserved 2>&1 || true)"
    if grep -q "stop reason = breakpoint" <<< "$out"; then
        expect_names lldb "$out"
        echo "lldb: reserved names"
    else
        echo "lldb cannot run the program here: skipped"
    fi
else
    echo "lldb not installed: skipped"
fi
echo "PASS"
