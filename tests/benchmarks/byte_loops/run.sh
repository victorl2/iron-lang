#!/usr/bin/env bash
# Build the byte-loop microbenchmark, pin it to one Linux CPU, and print the
# hot functions' disassembly. Usage: run.sh [ironc] [MB] [runs] [CPU].
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../.." && pwd)
ironc=${1:-$root/build-release/ironc}
mb=${2:-32}
runs=${3:-11}
cpu=${4:-0}

if ! command -v taskset >/dev/null 2>&1; then
    echo "byte_loops: taskset is required to pin the benchmark" >&2
    exit 1
fi
if ! command -v objdump >/dev/null 2>&1; then
    echo "byte_loops: objdump is required for the assembly report" >&2
    exit 1
fi

case "$ironc" in
    /*) ;;
    *) ironc=$(cd "$(dirname "$ironc")" && pwd)/$(basename "$ironc") ;;
esac

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
binary=$work/byte_loops

cd "$work"
"$ironc" build --release "$here/byte_loops.iron" -o "$binary"
echo "byte_loops: $(uname -m), CPU $cpu, $mb MB, best of $runs"
taskset -c "$cpu" "$binary" "$mb" "$runs"

for symbol in Iron_sum_bytes Iron_count_quotes Iron_str_quotes Iron_classify Iron_tokenize; do
    echo
    echo "$symbol"
    objdump -d --no-show-raw-insn --disassemble="$symbol" "$binary"
done
