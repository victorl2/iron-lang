#!/usr/bin/env bash
# Build and run the HTTP parser, serializer and loopback server baseline.
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
build=${1:-build-ci-release}
shift || true
samples=15
warmup=3
quick=0
out=""
cpu=""
server_cpu=""
while [ $# -gt 0 ]; do
    case $1 in
    --samples) samples=$2; shift ;;
    --warmup) warmup=$2; shift ;;
    --cpu) cpu=$2; shift ;;
    --server-cpu) server_cpu=$2; shift ;;
    --out) out=$2; shift ;;
    --quick) quick=1; samples=3; warmup=1 ;;
    *) echo "run.sh: unknown option $1" >&2; exit 2 ;;
    esac
    shift
done

cmake --build "$build" -j4 --target http_benchmark >/dev/null
bin=$(cd "$build/bench/http" && pwd)/http_benchmark
work=$(mktemp -d "${TMPDIR:-/tmp}/iron_http_bench_XXXXXX")
trap 'rm -rf "$work"' EXIT
raw=${out:-$work/results.txt}

if command -v taskset >/dev/null; then
    if [ -z "$cpu" ]; then
        read -r cpu auto_server_cpu < <(python3 -c \
            'import os; c=sorted(os.sched_getaffinity(0)); print(c[0], c[min(1,len(c)-1)])')
        [ -n "$server_cpu" ] || server_cpu=$auto_server_cpu
    fi
    [ -n "$server_cpu" ] || server_cpu=$cpu
    load_pin=(taskset -c "$cpu")
    server_pin=(taskset -c "$server_cpu")
else
    load_pin=()
    server_pin=()
    cpu=unpinned
    server_cpu=unpinned
fi

if [ $quick -eq 1 ]; then
    parse_reps=(2000 500 2 50)
    serialize_reps=(5000 500 2)
    e2e_warmup=20
    e2e_requests=200
else
    parse_reps=(2000000 300000 100 20000)
    serialize_reps=(2000000 200000 100)
    e2e_warmup=1000
    e2e_requests=20000
fi
parse_names=(small browser chunked pipeline)
serialize_names=(small json4k large)

{
    echo "# $(date -u +%Y-%m-%dT%H:%M:%SZ) $(uname -srm) load_cpu=$cpu server_cpu=$server_cpu warmup=$warmup samples=$samples"
    echo "# compiler: $(${CC:-clang} --version | head -1)"
} > "$raw"

measure_command() {
    local mode=$1 impl=$2 name=$3 repetitions=$4
    for _ in $(seq 1 "$warmup"); do
        "${load_pin[@]}" "$bin" "$mode" "$impl" "$name" "$repetitions" >/dev/null
    done
    for _ in $(seq 1 "$samples"); do
        "${load_pin[@]}" "$bin" "$mode" "$impl" "$name" "$repetitions" >> "$raw"
    done
}

for index in "${!parse_names[@]}"; do
    for impl in iron pico; do
        measure_command parse "$impl" "${parse_names[$index]}" "${parse_reps[$index]}"
    done
done
for index in "${!serialize_names[@]}"; do
    for impl in iron c; do
        measure_command serialize "$impl" "${serialize_names[$index]}" "${serialize_reps[$index]}"
    done
done

measure_e2e() {
    local impl=$1
    for phase in warmup samples; do
        local count=$warmup
        [ "$phase" = samples ] && count=$samples
        for iteration in $(seq 1 "$count"); do
            local port=$((24000 + ($$ + iteration) % 10000))
            local ready="$work/$impl-$phase-$iteration.ready"
            "${server_pin[@]}" "$bin" serve "$impl" "$port" \
                $((e2e_warmup + e2e_requests)) >"$ready" 2>"$ready.err" &
            local server_pid=$!
            for _ in $(seq 1 200); do
                grep -q READY "$ready" 2>/dev/null && break
                sleep 0.01
            done
            if [ "$phase" = samples ]; then
                "${load_pin[@]}" "$bin" load "$impl" "$port" \
                    "$e2e_warmup" "$e2e_requests" >> "$raw"
            else
                "${load_pin[@]}" "$bin" load "$impl" "$port" \
                    "$e2e_warmup" "$e2e_requests" >/dev/null
            fi
            wait "$server_pid"
        done
    done
}

measure_e2e iron
measure_e2e pico

python3 "$here/table.py" "$raw"
if [ -n "$out" ]; then
    echo "Raw samples: $out" >&2
fi
