#!/usr/bin/env bash
# The compiler's temporaries carry no debug info in a --debug build (#388):
# gdb and lldb used directly (and the Visual Studio debugger, which has no
# rule to hide them) list only the program's own names.
#
# Usage: debug_temporaries_smoke.sh <ironc> <source dir>
#
#   1. The generated C marks temporaries `__attribute__((nodebug))` in a
#      --debug build only; without --debug the C has no such attribute.
#   2. gdb `info locals` and lldb `frame variable`, in main, inside a
#      closure, in a function and in a test block, show no `_vN`, no
#      `iron__...` and no other name starting with `_` but `_ref_<name>`
#      (a var a closure captures, which `iron dap` shows as <name>). The
#      list kept as a C array is still `xs` plus its length `xs_len`.
#   3. IRON_DEBUG_TEMPORARIES=1 brings the temporaries back.
#
# Each debugger is skipped when it is not installed, or when it cannot run
# a program on this host (no debugserver, no ptrace permission).
set -euo pipefail

IRONC="$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"
SRC_ROOT="$(cd "$2" && pwd)"
SRC="$SRC_ROOT/tests/integration/debug/temporaries.iron"
MAIN_LINE=20      # println in main
CLOSURE_LINE=17   # return doubled, inside the closure
SCALE_LINE=3      # return scaled, in scale(v, by)
TEST_LINE=28      # assert_eq in the test block
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
cd "$work"

fail() { echo "FAIL: $1"; [ $# -lt 2 ] || echo "$2"; exit 1; }

# 1. The attribute is in the --debug C only.
"$IRONC" build "$SRC" --emit-c -o plain >/dev/null 2>&1
"$IRONC" build "$SRC" --debug --emit-c -o dbg >/dev/null 2>&1
if grep -q 'nodebug' .iron-build/plain.c; then
    fail "the C of a build without --debug has nodebug attributes"
fi
grep -q '__attribute__((nodebug)) int64_t _v' .iron-build/dbg.c ||
    fail "the --debug C does not mark temporaries nodebug"
if grep -qE '__attribute__\(\(nodebug\)\)[^;]*_ref_' .iron-build/dbg.c; then
    fail "a _ref_ pointer is marked nodebug"
fi
if grep -qE '__attribute__\(\(nodebug\)\)[^;=]* xs(_len)?( =|;)' .iron-build/dbg.c; then
    fail "the Iron list xs or its length xs_len is marked nodebug"
fi
echo "C: temporaries marked nodebug in the --debug build only"

"$IRONC" build "$SRC" --debug -o prog >/dev/null 2>&1
"$IRONC" build "$SRC" --debug --test -o prog_test >/dev/null 2>&1
IRON_DEBUG_TEMPORARIES=1 "$IRONC" build "$SRC" --debug -o prog_temps >/dev/null 2>&1
[ "$(./prog)" = "$(./prog_temps)" ] ||
    fail "IRON_DEBUG_TEMPORARIES=1 changed the program's output"

# names <output>: the variable names a debugger listed, one per line.
# gdb prints "name = value", lldb "(type) name = value".
names() { sed -nE 's/^(\([^)]*\) )?([A-Za-z_][A-Za-z0-9_]*) = .*/\2/p' <<< "$1" | sort -u; }

# check <debugger> <where> <output> <names that must be there...>
check() {
    local dbg="$1" where="$2" out="$3"
    shift 3
    local got bad
    got="$(names "$out")"
    for want in "$@"; do
        grep -qx -- "$want" <<< "$got" ||
            fail "$dbg in $where does not list '$want'" "$out"
    done
    bad="$(grep -E '^(_|iron__|vron__)' <<< "$got" | grep -v '^_ref_' || true)"
    [ -z "$bad" ] || fail "$dbg in $where lists compiler temporaries: $(echo $bad)" "$out"
}

# gdb_locals <program> <line>, lldb_locals <program> <line> [flags]: what
# the debugger lists at a breakpoint on that line.
gdb_locals() {
    gdb -q -batch -ex "break temporaries.iron:$2" -ex run -ex "info locals" "./$1" 2>&1 || true
}
lldb_locals() {
    lldb -b -o "b temporaries.iron:$2" -o run -o "frame variable ${3:-}" "./$1" 2>&1 || true
}

if command -v gdb >/dev/null 2>&1; then
    out="$(gdb_locals prog $MAIN_LINE)"
    if ! grep -q "temporaries.iron:$MAIN_LINE" <<< "$out"; then
        echo "gdb cannot run the program here: skipped"
    else
        check gdb "main" "$out" total xs xs_len names i _ref_count bump r
        check gdb "scale" "$(gdb_locals prog $SCALE_LINE)" scaled
        check gdb "the closure" "$(gdb_locals prog $CLOSURE_LINE)" doubled _ref_count
        check gdb "the test block" "$(gdb_locals prog_test $TEST_LINE)" t i
        out="$(gdb_locals prog_temps $MAIN_LINE)"
        grep -qE '^_v[0-9]+ = ' <<< "$out" ||
            fail "gdb: IRON_DEBUG_TEMPORARIES=1 did not keep the temporaries" "$out"
        echo "gdb: info locals lists only Iron names (temporaries back with IRON_DEBUG_TEMPORARIES=1)"
    fi
else
    echo "gdb not installed: skipped"
fi

if command -v lldb >/dev/null 2>&1 && lldb --version >/dev/null 2>&1; then
    out="$(lldb_locals prog $MAIN_LINE)"
    if ! grep -q "stop reason = breakpoint" <<< "$out"; then
        echo "lldb cannot run the program here: skipped"; echo "$out" | tail -5
    else
        check lldb "main" "$out" total xs xs_len names i _ref_count bump r
        check lldb "scale" "$(lldb_locals prog $SCALE_LINE)" v by scaled
        # A closure's environment comes in as a parameter, which C cannot
        # mark nodebug: inside the closure the check leaves parameters out.
        check lldb "the closure" "$(lldb_locals prog $CLOSURE_LINE --no-args)" doubled _ref_count
        check lldb "the test block" "$(lldb_locals prog_test $TEST_LINE)" t i
        out="$(lldb_locals prog_temps $MAIN_LINE)"
        grep -qE '^\([^)]*\) _v[0-9]+ = ' <<< "$out" ||
            fail "lldb: IRON_DEBUG_TEMPORARIES=1 did not keep the temporaries" "$out"
        echo "lldb: frame variable lists only Iron names (temporaries back with IRON_DEBUG_TEMPORARIES=1)"
    fi
else
    echo "lldb not installed: skipped"
fi
echo "PASS"
