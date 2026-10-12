#!/usr/bin/env bash
set -euo pipefail

ORACLE=$1
WORK=$(mktemp -d "${TMPDIR:-/tmp}/iron_parity_timeout_test.XXXXXX")
trap 'rm -rf "$WORK"' EXIT

mkdir -p "$WORK/bin" "$WORK/fixtures"
printf '%s\n' \
    '#!/usr/bin/env bash' \
    'set -euo pipefail' \
    '[ "$1" = build ]' \
    'shift' \
    'delay=0' \
    'if [ "${1:-}" = --no-optimize ]; then delay=2; shift; fi' \
    'name=$(basename "$1" .iron)' \
    'printf '\''#!/usr/bin/env bash\nsleep %s\nprintf "same\\n"\n'\'' "$delay" > "$name"' \
    'chmod +x "$name"' \
    > "$WORK/bin/fake-iron"
chmod +x "$WORK/bin/fake-iron"

printf '%s\n' \
    '-- @parity-timeout: 3' \
    'func main() {}' \
    > "$WORK/fixtures/slow_no_optimize.iron"

output=$(PARITY_JOBS=1 PARITY_TIMEOUT=1 \
    "$ORACLE" "$WORK/bin/fake-iron" "$WORK/fixtures")
printf '%s\n' "$output"
grep -q '=== opt parity: 1 agree, 0 disagree, 0 skipped' <<< "$output"
