#!/usr/bin/env bash
# A tuple destructuring's temporary was named from a stack buffer that
# --debug read after its frame was gone: a stack use after return in ironc
# and nondeterministic --debug C (random names such as `_n____4`). Build
# tuple_names.iron with --debug twice: the C must be identical and the
# program right. Under the ASan build, ctest sets
# detect_stack_use_after_return so the old bug aborts ironc.
#
# Usage: debug_tuple_names.sh <ironc> <repo>
set -euo pipefail
IRONC="$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"
SRC="$(cd "$2" && pwd)/tests/integration/debug/tuple_names.iron"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
cd "$work"
for i in 1 2; do
    "$IRONC" build "$SRC" --debug --emit-c -o "t$i" >/dev/null
    cp .iron-build/t$i.c "run$i.c"
done
# The two runs differ only in the output name.
sed 's/\bt1\b/tN/g' run1.c > a.c
sed 's/\bt2\b/tN/g' run2.c > b.c
if ! cmp -s a.c b.c; then
    echo "FAIL: --debug C differs between two runs"; diff a.c b.c | head -20; exit 1
fi
"$IRONC" build "$SRC" --debug -o prog >/dev/null
out="$(./prog)"
[ "$out" = "3 2" ] || { echo "FAIL: output is '$out', want '3 2'"; exit 1; }
echo "PASS"
