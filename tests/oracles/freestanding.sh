#!/bin/bash
# freestanding.sh - the generated C must compile without platform headers.
#
# For every fixture in the given directories, emit its C (iron build
# --emit-c writes .iron-build/<name>.c and skips clang) and compile that file with
# -ffreestanding -nostdinc, seeing only clang's own resource headers and the
# Iron source tree. Generated code may include iron_runtime.h and the stdlib
# headers; those may include only the freestanding headers (stdint.h,
# stddef.h, stdbool.h, stdarg.h, stdatomic.h) and must declare everything
# else themselves (#235). Prints the undeclared symbols and missing headers
# per fixture so the remaining work is visible.
#
# usage: tests/oracles/freestanding.sh <iron-binary> <fixture-dir>...
#   FREESTANDING_REPORT=1 prints a symbol histogram instead of failing fast.
set -u
IRON=$(cd "$(dirname "$1")" && pwd)/$(basename "$1"); shift
SRC=$(cd "$(dirname "$0")/../.." && pwd)/src
# The same clang ironc compiles with (the pinned toolchain bundle).
CLANG="$("$IRON" toolchain path)/bin/clang" || exit 1
RES=$("$CLANG" -print-resource-dir)
work=$(mktemp -d "${TMPDIR:-/tmp}/iron-freestanding.XXXXXX")
trap 'rm -rf "$work"' EXIT
pass=0; fail=0; skipped=0
: > "$work/symbols"
# Report mode: stub every libc header out so the compile gets past the first
# missing include and lists every symbol the code takes from the platform.
extra=(-I "$SRC")   # placeholder member: bash 3 rejects "${extra[@]}" on an empty array under set -u
if [ "${FREESTANDING_REPORT:-0}" = 1 ]; then
    for h in string.h stdlib.h stdio.h pthread.h time.h math.h unistd.h errno.h signal.h assert.h \
             ctype.h limits.h inttypes.h fcntl.h sys/types.h sys/stat.h sys/time.h windows.h winsock2.h; do
        mkdir -p "$work/stubs/$(dirname "$h")"; echo "/* stubbed by freestanding.sh */" > "$work/stubs/$h"
    done
    extra=(-isystem "$work/stubs")
fi
for dir in "$@"; do
    for src in "$dir"/*.iron; do
        [ -e "$src" ] || continue
        name=$(basename "$src" .iron)
        abs=$(cd "$(dirname "$src")" && pwd)/$(basename "$src")
        if head -n 10 "$src" | grep -qE '^[[:space:]]*--[[:space:]]*@(compile-only|expected-pass-after)'; then continue; fi
        if ! mkdir -p "$work/$name" || ! (cd "$work/$name" && "$IRON" build --emit-c "$abs" >/dev/null 2>"$work/$name.build"); then
            # A fixture that does not build (a @posix-only path, a missing library) is not this oracle's concern.
            skipped=$((skipped+1)); continue
        fi
        cfile="$work/$name/.iron-build/$name.c"
        [ -f "$cfile" ] || continue
        if "$CLANG" -std=gnu17 -fsyntax-only -ferror-limit=0 -Werror=implicit-function-declaration -ffreestanding -nostdinc \
                 -isystem "$RES/include" "${extra[@]}" -I "$SRC" "$cfile" > "$work/$name.log" 2>&1; then
            pass=$((pass+1))
        else
            fail=$((fail+1))
            grep -oE "undeclared (function|identifier) '[A-Za-z_0-9]+'|unknown type name '[A-Za-z_0-9]+'|'[A-Za-z_0-9/.]+' file not found" "$work/$name.log" \
                | sed -E "s/.*'(.*)'.*/\1/" | sort -u >> "$work/symbols"
            [ "${FREESTANDING_REPORT:-0}" = 1 ] || echo "[FAIL] $name: $(sort -u "$work/symbols" | tr '\n' ' ' | cut -c1-200)"
        fi
    done
done
if [ -s "$work/symbols" ]; then
    echo "symbols the generated C still needs from the platform (fixtures using each):"
    sort "$work/symbols" | uniq -c | sort -rn | head -40
fi
echo "freestanding: PASS=$pass FAIL=$fail SKIPPED=$skipped (did not build)"
[ "$fail" -eq 0 ]
