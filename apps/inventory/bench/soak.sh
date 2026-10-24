#!/bin/bash
# soak.sh - sustained mixed load with memory sampling.
#
#   apps/inventory/bench/soak.sh <iron> [minutes] [connections]
#
# Starts the service like bench/run.sh (same environment; the database must
# have been seeded, e.g. BENCH_KEEP=1 bench/run.sh first), runs the mixed
# workload (bench/lua/mixed.lua) for the given minutes (default 10) and
# samples RSS, threads, requests, pool and cache sizes every 10 s. On glibc,
# SOAK_MALLOC_STATS=FILE also records live heap, retained free heap and arena
# count through malloc_probe.c. Prints the samples and a summary, and fails
# when RSS in the last third exceeds the first third's maximum by more than
# SOAK_GROWTH_MB (default 8).
set -u
here=$(cd "$(dirname "$0")" && pwd)
app=$(cd "$here/.." && pwd)
[ $# -ge 1 ] || { sed -n '2,12p' "$0" | sed 's/^# \{0,1\}//'; exit 2; }
IRON=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
minutes=${2:-10}
c=${3:-128}
: "${IRON_MYSQL_URL:?}"
export BENCH_ITEMS=${BENCH_ITEMS:-10000}
work=$(mktemp -d "${TMPDIR:-/tmp}/inventory-soak.XXXXXX")
trap 'kill $svc_pid $stub_pid $wrk_pid 2>/dev/null; wait 2>/dev/null; rm -rf "$work"' EXIT
cp -R "$app/src" "$app/iron.toml" "$work/" && mkdir -p "$work/stub" &&
    cp -R "$app/stub/src" "$app/stub/iron.toml" "$work/stub/" || exit 1
(cd "$work" && "$IRON" build --release >/dev/null 2>&1) || exit 1
# SOAK_SANITIZE=1: an AddressSanitizer build; leaks are reported at exit.
if [ "${SOAK_SANITIZE:-0}" = 1 ]; then
    (cd "$work" && IRON_CFLAGS="-g -O1 -fno-omit-frame-pointer -fsanitize=address" "$IRON" build >/dev/null 2>&1) || exit 1
    export ASAN_OPTIONS=detect_leaks=1
    for s in llvm-symbolizer llvm-symbolizer-18; do command -v $s >/dev/null && export ASAN_SYMBOLIZER_PATH=$(command -v $s) && break; done
fi
(cd "$work/stub" && "$IRON" build --release >/dev/null 2>&1) || exit 1
malloc_env=()
if [ -n "${SOAK_MALLOC_STATS:-}" ]; then
    [ "$(uname -s)" = Linux ] || { echo "SOAK_MALLOC_STATS requires Linux glibc" >&2; exit 2; }
    command -v "${CC:-cc}" >/dev/null || { echo "SOAK_MALLOC_STATS requires a C compiler" >&2; exit 2; }
    stats_dir=$(cd "$(dirname "$SOAK_MALLOC_STATS")" && pwd) || exit 2
    stats_file="$stats_dir/$(basename "$SOAK_MALLOC_STATS")"
    "${CC:-cc}" -shared -fPIC -O2 -pthread "$here/malloc_probe.c" -o "$work/malloc_probe.so" || exit 1
    malloc_env=(LD_PRELOAD="$work/malloc_probe.so" IRON_MALLOC_STATS_FILE="$stats_file")
fi
db_url=$(python3 -c 'import sys,urllib.parse as u; p=u.urlsplit(sys.argv[1]); print(u.urlunsplit((p.scheme,p.netloc,"/inventory_bench","tls=disabled","")))' "$IRON_MYSQL_URL")
"$work/stub/target/fxstub" 18090 >/dev/null 2>&1 &
stub_pid=$!
env "${malloc_env[@]}" INVENTORY_PORT=18080 INVENTORY_HOST=127.0.0.1 INVENTORY_MYSQL_URL=$db_url \
INVENTORY_RATES_URL=http://127.0.0.1:18090 INVENTORY_LOG_LEVEL=warn INVENTORY_RATES_TTL_MS=5000 \
    taskset -c "${SERVICE_CPUS:-0-3}" "$work/target/inventory" >"$work/svc.log" 2>&1 &
svc_pid=$!
for _ in $(seq 1 200); do curl -sf http://127.0.0.1:18080/readyz >/dev/null 2>&1 && break; sleep 0.1; done
secs=$((minutes * 60))
taskset -c "${WRK_CPUS:-4-7}" wrk -t4 -c"$c" -d"${secs}s" --latency -s "$here/lua/mixed.lua" \
    http://127.0.0.1:18080/ >"$work/wrk.out" 2>&1 &
wrk_pid=$!
echo "seconds,rss_mb,threads,requests_total,pool_idle,pool_in_use,rates,report_cache_rows,parked"
samples=()
# Sample while the load runs, by the clock: a sample takes a moment of its
# own, and counting samples instead ran past the end of the load.
started=$(date +%s)
while kill -0 "$wrk_pid" 2>/dev/null; do
    sleep 10
    kill -0 "$wrk_pid" 2>/dev/null || break
    t=$(( $(date +%s) - started ))
    rss=$(awk '/^VmRSS/{printf "%.1f", $2/1024}' "/proc/$svc_pid/status")
    thr=$(awk '/^Threads/{print $2}' "/proc/$svc_pid/status")
    metrics=$(curl -s -m 8 http://127.0.0.1:18080/metrics)
    metric() { awk -v pattern="$1" '$0 ~ pattern {s += $NF} END {print s + 0}' <<<"$metrics"; }
    total=$(metric '^inventory_http_requests_total')
    idle=$(metric '^inventory_db_pool_connections[{]state="idle"[}]')
    in_use=$(metric '^inventory_db_pool_connections[{]state="in_use"[}]')
    rates=$(metric '^inventory_rates_currencies ')
    reports=$(metric '^inventory_report_cache_rows ')
    parked=$(metric '^inventory_http_connections_parked ')
    echo "$t,$rss,$thr,$total,$idle,$in_use,$rates,$reports,$parked"
    samples+=("$rss")
done
wait $wrk_pid
grep -E "Requests/sec|Latency|requests in|Non-2xx" "$work/wrk.out"
echo "requests by route and status:"
curl -s http://127.0.0.1:18080/metrics | grep '^inventory_http_requests_total' | sed 's/inventory_http_requests_total//'
echo "warnings and errors logged:"
grep -v '"level":"info"' "$work/svc.log" | sed 's/"ts":"[^"]*",//' | cut -c1-200 | sort | uniq -c | sort -rn | head -5
if [ "${SOAK_SANITIZE:-0}" = 1 ]; then
    kill -TERM $svc_pid; wait $svc_pid
    grep -A30 "ERROR: LeakSanitizer\|ERROR: AddressSanitizer" "$work/svc.log" | head -150
fi
python3 - "${SOAK_GROWTH_MB:-8}" "${samples[@]}" <<'PY'
import sys
limit = float(sys.argv[1]); s = [float(x) for x in sys.argv[2:]]
third = max(1, len(s) // 3)
first, last = max(s[:third]), max(s[-third:])
print(f"RSS first third max {first:.1f} MB, last third max {last:.1f} MB, overall max {max(s):.1f} MB")
if last - first > limit:
    print(f"FAIL: RSS grew {last - first:.1f} MB"); sys.exit(1)
print("PASS: memory flat")
PY
