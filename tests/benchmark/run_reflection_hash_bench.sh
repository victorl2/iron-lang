#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
IRONC=${1:-$SCRIPT_DIR/../../build/ironc}
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

cd "$WORK"
"$IRONC" build --release "$SCRIPT_DIR/reflection_hash_bench.iron"
BEST_RATIO=
for trial in 1 2 3 4 5; do
    OUTPUT=$(./reflection_hash_bench)

    DERIVED_HASH=$(printf '%s\n' "$OUTPUT" | sed -n 's/^derived_hash=//p')
    MANUAL_HASH=$(printf '%s\n' "$OUTPUT" | sed -n 's/^manual_hash=//p')
    DERIVED_SUM=$(printf '%s\n' "$OUTPUT" | sed -n 's/^derived_sum=//p')
    MANUAL_SUM=$(printf '%s\n' "$OUTPUT" | sed -n 's/^manual_sum=//p')
    DERIVED_NS=$(printf '%s\n' "$OUTPUT" | sed -n 's/^derived_ns=//p')
    MANUAL_NS=$(printf '%s\n' "$OUTPUT" | sed -n 's/^manual_ns=//p')

    test "$DERIVED_HASH" = "$MANUAL_HASH"
    test "$DERIVED_SUM" = "$MANUAL_SUM"
    test "$DERIVED_NS" -gt 0
    test "$MANUAL_NS" -gt 0

    RATIO=$(awk -v d="$DERIVED_NS" -v m="$MANUAL_NS" 'BEGIN { if (d > m) print d / m; else print m / d }')
    if [ -z "$BEST_RATIO" ] || awk -v ratio="$RATIO" -v best="$BEST_RATIO" 'BEGIN { exit !(ratio < best) }'; then
        BEST_RATIO=$RATIO
    fi
done

awk -v ratio="$BEST_RATIO" 'BEGIN { exit !(ratio <= 2.0) }'

echo "reflection hash matches manual hash: $DERIVED_HASH"
echo "best derived/manual slower-side ratio from 5 trials: $BEST_RATIO"
