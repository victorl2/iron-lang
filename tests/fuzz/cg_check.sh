#!/bin/sh
# cg_check.sh IRONC SRC EXPECTED OUTDIR [ironc build flags...]
#
# Builds SRC (in the environment given, e.g. IRON_LIR_SKIP or IRON_CC_FLAGS),
# fails when the C compiler reports a warning or an error on the generated C,
# runs the program and compares its output with EXPECTED. With
# CG_CHECK_DIAG=text the build output must also contain that text.
ironc=$1 src=$2 expected=$3 out=$4
shift 4
mkdir -p "$out" || exit 1
exe="$out/$(basename "$src" .iron).bin"
rm -f "$exe"
log=$("$ironc" build "$@" "$src" -o "$exe" 2>&1)
rc=$?
if printf '%s\n' "$log" | grep -E '^[^ ]+\.[ch]:[0-9]+:[0-9]+: (warning|error):' >/dev/null; then
    printf '%s\n' "$log"
    echo "cg_check: the C compiler reported on the generated C"
    exit 1
fi
if [ -n "$CG_CHECK_DIAG" ] && ! printf '%s\n' "$log" | grep -F "$CG_CHECK_DIAG" >/dev/null; then
    printf '%s\n' "$log"
    echo "cg_check: the build output does not contain: $CG_CHECK_DIAG"
    exit 1
fi
if [ $rc -ne 0 ]; then
    printf '%s\n' "$log"
    echo "cg_check: build failed"
    exit 1
fi
"$exe" > "$out/got.txt" 2>&1
if ! diff "$expected" "$out/got.txt"; then
    echo "cg_check: output differs from $expected"
    exit 1
fi
exit 0
