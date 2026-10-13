#!/usr/bin/env bash
# Build and run the in-memory MySQL protocol benchmark, then print markdown.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
build=${1:-$root/build}
shift || true
cpu="" warmup=3 samples=15 out=""
while [ $# -gt 0 ]; do
    case $1 in
        --cpu) cpu=$2; shift ;;
        --warmup) warmup=$2; shift ;;
        --samples) samples=$2; shift ;;
        --out) out=$2; shift ;;
        *) echo "run.sh: unknown option $1" >&2; exit 2 ;;
    esac
    shift
done
cmake --build "$build" -j4 --target mysql_protocol_bench >/dev/null
run=("$build/tests/unit/mysql_protocol_bench" "$warmup" "$samples")
if [ -n "$cpu" ] && command -v taskset >/dev/null; then run=(taskset -c "$cpu" "${run[@]}"); fi
tmp=$(mktemp "${TMPDIR:-/tmp}/iron-mysql-bench.XXXXXX")
trap 'rm -f "$tmp"' EXIT
{
    echo "# $(date -u +%Y-%m-%dT%H:%M:%SZ) $(uname -srm) cpu=${cpu:-unpinned} warmup=$warmup samples=$samples"
    "${run[@]}"
} | tee "$tmp"
if [ -n "$out" ]; then cp "$tmp" "$out"; fi
"$here/table.py" < "$tmp"
