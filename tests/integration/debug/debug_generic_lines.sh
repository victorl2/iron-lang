#!/usr/bin/env bash
# The statements of a generic function's instance, and of a generic
# method's, carry their own source lines in a --debug build (#722): they
# all took the declaration's line, so a debugger stepped through the
# instance on one line and a breakpoint inside it never bound.
#
# Usage: debug_generic_lines.sh <ironc> <repo>
set -euo pipefail
IRONC="$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"
SRC="$(cd "$2" && pwd)/tests/integration/debug/generic_lines.iron"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
cd "$work"
"$IRONC" build "$SRC" --debug --emit-c -o prog >/dev/null
fail=0
# lines_of <C function>: the #line numbers inside its definition.
lines_of() {
    sed -n "/^[A-Za-z_0-9 \*]* $1(.*) {\$/,/^}/p" .iron-build/prog.c |
        sed -n 's/^#line \([0-9]*\) .*generic_lines\.iron.*/\1/p' | sort -un | tr '\n' ' '
}
want() {   # <C function> <line>...
    local fn=$1 got; shift
    got=" $(lines_of "$fn")"
    for n in "$@"; do
        case "$got" in *" $n "*) ;; *) echo "FAIL: $fn has no line $n (has:$got)"; fail=1 ;; esac
    done
}
want Iron_gid__Int 2
want Iron_sum_to__String 8 10 11
want Iron_holder__Int_map__Int 17 18 19
"$IRONC" build "$SRC" --debug -o prog >/dev/null
out="$(./prog | tr '\n' ' ')"
[ "$out" = "mapped 6 3 6 5 " ] || { echo "FAIL: output is '$out'"; fail=1; }
[ $fail -eq 0 ] && echo "PASS"
exit $fail
