#!/bin/bash
# memory_bound.sh - short, repeatable steady-state RSS regression check.
#
#   apps/inventory/bench/memory_bound.sh <iron>
#
# Builds the service and upstream stub, creates and seeds a throwaway database,
# warms the mixed workload for 30 seconds, then fits an RSS slope to the
# remaining samples. The test fails above 0.75 MB/minute by default. It skips
# with code 77 when Linux, a database, or wrk is unavailable.
set -u
here=$(cd "$(dirname "$0")" && pwd)
app=$(cd "$here/.." && pwd)
[ $# -eq 1 ] || { sed -n '2,11p' "$0" | sed 's/^# \{0,1\}//'; exit 2; }
IRON=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
IRONC=$(dirname "$IRON")/ironc
if [ "$(uname -s)" != Linux ] || [ -z "${IRON_MYSQL_URL:-}" ]; then
    echo "SKIP: inventory memory bound check needs Linux and IRON_MYSQL_URL"
    exit 77
fi
for command in python3 curl wrk; do
    command -v "$command" >/dev/null || { echo "SKIP: $command is required"; exit 77; }
done

seconds=${INVENTORY_MEMORY_SECONDS:-120}
warmup=${INVENTORY_MEMORY_WARMUP:-30}
limit=${INVENTORY_MEMORY_SLOPE_MB_MIN:-0.75}
connections=${INVENTORY_MEMORY_CONNECTIONS:-32}
items=${INVENTORY_MEMORY_ITEMS:-1000}
if [ "$seconds" -le "$warmup" ] || [ "$warmup" -lt 10 ]; then
    echo "memory_bound.sh: duration must exceed a warmup of at least 10 seconds" >&2
    exit 2
fi

work=$(mktemp -d "${TMPDIR:-/tmp}/inventory-memory.XXXXXX")
db_name="inventory_memory_$$"
svc_pid=""
stub_pid=""
wrk_pid=""
cleanup() {
    [ -n "$wrk_pid" ] && kill "$wrk_pid" 2>/dev/null
    [ -n "$svc_pid" ] && kill "$svc_pid" 2>/dev/null
    [ -n "$stub_pid" ] && kill "$stub_pid" 2>/dev/null
    wait 2>/dev/null
    [ -x "$work/dbtool" ] && "$work/dbtool" drop "$IRON_MYSQL_URL" "$db_name" >/dev/null 2>&1
    rm -rf "$work"
}
trap cleanup EXIT

cp -R "$app/src" "$app/iron.toml" "$work/" && mkdir -p "$work/stub" &&
    cp -R "$app/stub/src" "$app/stub/iron.toml" "$work/stub/" || exit 1
(cd "$work" && "$IRON" build --release >service.build 2>&1) || {
    sed 's/^/    /' "$work/service.build"; exit 1;
}
(cd "$work/stub" && "$IRON" build --release >stub.build 2>&1) || {
    sed 's/^/    /' "$work/stub/stub.build"; exit 1;
}
"$IRONC" build "$app/tests/dbtool.iron" -o "$work/dbtool" >"$work/dbtool.build" 2>&1 || {
    sed 's/^/    /' "$work/dbtool.build"; exit 1;
}
"$work/dbtool" create "$IRON_MYSQL_URL" "$db_name" || exit 1
db_url=$(python3 -c 'import sys,urllib.parse as u; p=u.urlsplit(sys.argv[1]); print(u.urlunsplit((p.scheme,p.netloc,"/"+sys.argv[2],p.query,"")))' "$IRON_MYSQL_URL" "$db_name")
read -r service_port stub_port < <(python3 - <<'PY'
import socket
ports = []
for _ in range(2):
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    ports.append(s.getsockname()[1])
    s.close()
print(*ports)
PY
)

"$work/stub/target/fxstub" "$stub_port" >"$work/stub.log" 2>&1 &
stub_pid=$!
INVENTORY_PORT=$service_port INVENTORY_HOST=127.0.0.1 INVENTORY_MYSQL_URL=$db_url \
INVENTORY_RATES_URL="http://127.0.0.1:$stub_port" INVENTORY_DB_POOL_MAX=8 \
INVENTORY_WORKERS=4 INVENTORY_LOG_LEVEL=warn INVENTORY_RATES_TTL_MS=5000 \
    "$work/target/inventory" >"$work/service.log" 2>&1 &
svc_pid=$!
ready=0
for _ in $(seq 1 300); do
    if curl -sf "http://127.0.0.1:$service_port/readyz" >/dev/null 2>&1; then
        ready=1
        break
    fi
    kill -0 "$svc_pid" 2>/dev/null || break
    sleep 0.1
done
if [ "$ready" != 1 ]; then
    echo "memory_bound.sh: service did not become ready" >&2
    sed 's/^/    /' "$work/service.log" >&2
    exit 1
fi

"$work/dbtool" scalar "$db_url" "INSERT INTO items (sku, name, category, price_cents, quantity)
    SELECT CONCAT('MEM-', n), CONCAT('Memory item ', n), CONCAT('cat-', n % 20), 100 + n, 1000000
    FROM (SELECT a.d + b.d * 10 + c.d * 100 AS n
          FROM (SELECT 0 d UNION ALL SELECT 1 UNION ALL SELECT 2 UNION ALL SELECT 3 UNION ALL SELECT 4
                UNION ALL SELECT 5 UNION ALL SELECT 6 UNION ALL SELECT 7 UNION ALL SELECT 8 UNION ALL SELECT 9) a,
               (SELECT 0 d UNION ALL SELECT 1 UNION ALL SELECT 2 UNION ALL SELECT 3 UNION ALL SELECT 4
                UNION ALL SELECT 5 UNION ALL SELECT 6 UNION ALL SELECT 7 UNION ALL SELECT 8 UNION ALL SELECT 9) b,
               (SELECT 0 d UNION ALL SELECT 1 UNION ALL SELECT 2 UNION ALL SELECT 3 UNION ALL SELECT 4
                UNION ALL SELECT 5 UNION ALL SELECT 6 UNION ALL SELECT 7 UNION ALL SELECT 8 UNION ALL SELECT 9) c) s
    WHERE n < $items" >/dev/null || exit 1

export BENCH_ITEMS=$items
wrk -t2 -c"$connections" -d"${seconds}s" --timeout 5s -s "$here/lua/mixed.lua" \
    "http://127.0.0.1:$service_port/" >"$work/wrk.out" 2>&1 &
wrk_pid=$!
samples="$work/rss.csv"
echo "seconds,rss_mb,threads,pool_idle,pool_in_use,rates,parked,requests" >"$samples"
started=$(date +%s)
while kill -0 "$wrk_pid" 2>/dev/null; do
    sleep 2
    kill -0 "$wrk_pid" 2>/dev/null || break
    elapsed=$(( $(date +%s) - started ))
    status="/proc/$svc_pid/status"
    [ -r "$status" ] || { echo "memory_bound.sh: service exited during load" >&2; exit 1; }
    rss=$(awk '/^VmRSS/{printf "%.3f", $2/1024}' "$status")
    threads=$(awk '/^Threads/{print $2}' "$status")
    # Metrics are diagnostic only. Do not let a saturated worker pool reduce
    # the independent /proc RSS sampling rate.
    metrics=$(curl -sf -m 0.5 "http://127.0.0.1:$service_port/metrics") || metrics=""
    value() { awk -v pattern="$1" '$0 ~ pattern {sum += $NF} END {print sum + 0}' <<<"$metrics"; }
    idle=$(value '^inventory_db_pool_connections[{]state="idle"[}]')
    in_use=$(value '^inventory_db_pool_connections[{]state="in_use"[}]')
    rates=$(value '^inventory_rates_currencies ')
    parked=$(value '^inventory_http_connections_parked ')
    requests=$(value '^inventory_http_requests_total')
    echo "$elapsed,$rss,$threads,$idle,$in_use,$rates,$parked,$requests" >>"$samples"
done
wait "$wrk_pid" || { sed 's/^/    /' "$work/wrk.out"; exit 1; }
wrk_pid=""
grep -E "Requests/sec|requests in|Non-2xx" "$work/wrk.out"
python3 - "$samples" "$work/wrk.out" "$warmup" "$limit" <<'PY'
import csv
import re
import statistics
import sys

path, wrk_path = sys.argv[1], sys.argv[2]
warmup, limit = float(sys.argv[3]), float(sys.argv[4])
rows = list(csv.DictReader(open(path, encoding="utf-8")))
points = [(float(r["seconds"]), float(r["rss_mb"])) for r in rows
          if float(r["seconds"]) >= warmup]
if len(points) < 10:
    print(f"FAIL: only {len(points)} RSS samples after warmup")
    sys.exit(1)
mean_x = sum(x for x, _ in points) / len(points)
mean_y = sum(y for _, y in points) / len(points)
den = sum((x - mean_x) ** 2 for x, _ in points)
slope = sum((x - mean_x) * (y - mean_y) for x, y in points) / den * 60
quarter = max(1, len(points) // 4)
first = statistics.median(y for _, y in points[:quarter])
last = statistics.median(y for _, y in points[-quarter:])
match = re.search(r"(\d+) requests in", open(wrk_path, encoding="utf-8").read())
requests = int(match.group(1)) if match else 0
print(f"RSS after {warmup:.0f}s warmup: {first:.3f} MB first-quarter median, "
      f"{last:.3f} MB last-quarter median, {slope:.3f} MB/min slope, "
      f"{requests} sampled requests")
diagnostic_rows = [r for r in rows if int(float(r["rates"])) > 0]
last_row = diagnostic_rows[-1] if diagnostic_rows else rows[-1]
print("Final bounded state: "
      f"threads={last_row['threads']}, pool_idle={last_row['pool_idle']}, "
      f"pool_in_use={last_row['pool_in_use']}, rates={last_row['rates']}, "
      f"parked={last_row['parked']}")
if requests < 1000:
    print("FAIL: workload completed too few requests")
    sys.exit(1)
if slope > limit:
    print(f"FAIL: RSS slope exceeds {limit:.3f} MB/min")
    sys.exit(1)
print(f"PASS: RSS slope is at most {limit:.3f} MB/min")
PY
