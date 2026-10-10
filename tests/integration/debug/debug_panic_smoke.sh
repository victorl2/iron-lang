#!/usr/bin/env bash
# Break on panic (#312): the failed assert in panic.iron stops the
# debugger with the Iron frame that panicked selected (check at line 6),
# in `iron debug` with gdb and with the commands `iron debug` gives LLDB,
# and through `iron dap` (dap_client.py --panic).
#
# Usage: debug_panic_smoke.sh <ironc> <source dir>
# Each debugger is skipped when missing or unable to run a program here.
set -euo pipefail

IRONC="$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"
IRON="$(dirname "$IRONC")/iron"
SRC_ROOT="$(cd "$2" && pwd)"
SRC="$SRC_ROOT/tests/integration/debug/panic.iron"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
cd "$work"

want="Iron panic at panic.iron:6"

if command -v gdb >/dev/null 2>&1 && [ -x "$IRON" ]; then
    out="$(printf 'run\nframe\nprint total\nkill\n' | "$IRON" debug "$SRC" --gdb 2>&1 || true)"
    if ! grep -q "assertion failed: total too large" <<< "$out"; then
        echo "gdb cannot run the program here: skipped"; tail -5 <<< "$out"
    else
        grep -q "$want" <<< "$out" || { echo "FAIL: gdb: no '$want'"; echo "$out"; exit 1; }
        # `frame` prints "#2  0x... in Iron_check (total=12)" and the
        # location on the next line.
        grep -qE "#[0-9]+ .*Iron_check \(total=12\)" <<< "$out" ||
            { echo "FAIL: gdb: check's frame is not selected"; echo "$out"; exit 1; }
        grep -qE '\$1 = 12' <<< "$out" || { echo "FAIL: gdb: total is not 12"; echo "$out"; exit 1; }
        echo "gdb: iron debug stops on the panic at panic.iron:6"
    fi
else
    echo "gdb not installed: skipped"
fi

if command -v lldb >/dev/null 2>&1 && lldb --version >/dev/null 2>&1; then
    "$IRONC" build "$SRC" --debug -o panic >/dev/null 2>&1
    # The commands iron debug passes to LLDB.
    out="$(lldb -b -o "command script import $SRC_ROOT/src/debug/iron_lldb.py" \
                -o iron-panic-stop -o run -o "frame variable total" ./panic 2>&1 || true)"
    if ! grep -q "stop reason" <<< "$out"; then
        echo "lldb cannot run the program here: skipped"
    else
        grep -q "$want" <<< "$out" || { echo "FAIL: lldb: no '$want'"; echo "$out"; exit 1; }
        grep -qE "frame #[0-9]+: .*Iron_check\(total=12\) at panic.iron:6" <<< "$out" ||
            { echo "FAIL: lldb: check's frame is not selected"; echo "$out"; exit 1; }
        grep -q "total = 12" <<< "$out" || { echo "FAIL: lldb: total is not 12"; echo "$out"; exit 1; }
        echo "lldb: stops on the panic at panic.iron:6"
    fi
else
    echo "lldb not installed: skipped"
fi

if command -v python3 >/dev/null 2>&1 && [ -x "$IRON" ]; then
    set +e
    python3 "$SRC_ROOT/tests/integration/debug/dap_client.py" --panic "$SRC" "$IRON" dap
    rc=$?
    set -e
    [ $rc -eq 0 ] || [ $rc -eq 77 ] || { echo "FAIL: iron dap does not stop on the panic"; exit 1; }
fi
echo "PASS"
