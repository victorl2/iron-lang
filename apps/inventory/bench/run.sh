#!/bin/bash
# run.sh - benchmark apps/inventory (and the Go baseline) with wrk.
#
#   apps/inventory/bench/run.sh <iron> [targets] [scenarios] [connections]
#
#   targets      comma list of iron, go               (default iron,go)
#   scenarios    comma list of health, rate, get, get_eur, list, stock,
#                create                                (default: all)
#   connections  comma list of wrk connection counts   (default 16,64,256)
#
# Runs inside the iron-service-bench image (wrk, Go, procps). Each target is
# started pinned to SERVICE_CPUS and wrk to WRK_CPUS, against the MySQL
# server of IRON_MYSQL_URL (database inventory_bench, seeded with
# BENCH_ITEMS items) and a local fxstub. For every run it prints requests/s,
# latency percentiles, errors, the service's CPU time per 1000 requests and
# its resident memory, as a Markdown table (also in $BENCH_OUT).
#
# environment: IRON_MYSQL_URL (required), BENCH_ITEMS (10000), BENCH_SECONDS
# (15), WRK_THREADS (4), SERVICE_CPUS (0-3), WRK_CPUS (4-7), BENCH_OUT
# (bench-results.md), BENCH_KEEP=1 to keep the database, BENCH_REUSE=1 to
# reuse a kept one, BENCH_MYSQL_TLS=1 for TLS to MySQL (off by default for
# both targets, as in go-sql-driver), BENCH_POOL (32) for the pool size,
# BENCH_APP=DIR to build the iron target from another copy of apps/inventory
# (an older revision, for before and after numbers) with BENCH_APP_IRON as
# its compiler (default: the one given).
set -u
here=$(cd "$(dirname "$0")" && pwd)
app=$(cd "$here/.." && pwd)
[ $# -ge 1 ] || { sed -n '2,27p' "$0" | sed 's/^# \{0,1\}//'; exit 2; }
IRON=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
IRONC=$(dirname "$IRON")/ironc
targets=${2:-iron,go}
scenarios=${3:-health,rate,get,get_eur,list,stock,create}
conns=${4:-16,64,256}
: "${IRON_MYSQL_URL:?IRON_MYSQL_URL must name a MySQL server}"
ITEMS=${BENCH_ITEMS:-10000}
SECONDS_PER_RUN=${BENCH_SECONDS:-15}
WRK_THREADS=${WRK_THREADS:-4}
SERVICE_CPUS=${SERVICE_CPUS:-0-3}
WRK_CPUS=${WRK_CPUS:-4-7}
OUT=${BENCH_OUT:-bench-results.md}
PORT=18080
export BENCH_ITEMS=$ITEMS

work=$(mktemp -d "${TMPDIR:-/tmp}/inventory-bench.XXXXXX")
cleanup() {
    [ -n "${svc_pid:-}" ] && kill "$svc_pid" 2>/dev/null
    [ -n "${stub_pid:-}" ] && kill "$stub_pid" 2>/dev/null
    wait 2>/dev/null
    rm -rf "$work"
}
trap cleanup EXIT

# Build: the service and the stub with --release, the database tool, Go.
svc_app=${BENCH_APP:-$app}
svc_iron=${BENCH_APP_IRON:-$IRON}
cp -R "$svc_app/src" "$svc_app/iron.toml" "$work/" && mkdir -p "$work/stub" &&
    cp -R "$app/stub/src" "$app/stub/iron.toml" "$work/stub/" || exit 1
(cd "$work" && "$svc_iron" build --release >build.log 2>&1) || { cat "$work/build.log"; exit 1; }
(cd "$work/stub" && "$IRON" build --release >build.log 2>&1) || { cat "$work/stub/build.log"; exit 1; }
"$IRONC" build "$app/tests/dbtool.iron" -o "$work/dbtool" >/dev/null 2>&1 || exit 1
if [[ ,$targets, == *,go,* ]]; then
    (cd "$here/go-baseline" && go build -o "$work/go-baseline" .) || exit 1
fi

# TLS to MySQL is off by default for both targets (go-sql-driver's default;
# Iron's client prefers TLS when the server offers it). BENCH_MYSQL_TLS=1
# turns it on for both.
db_url=$(python3 -c 'import sys,urllib.parse as u; p=u.urlsplit(sys.argv[1]); print(u.urlunsplit((p.scheme,p.netloc,"/inventory_bench",p.query,"")))' "$IRON_MYSQL_URL")
go_tls=""
if [ "${BENCH_MYSQL_TLS:-0}" = 1 ]; then
    go_tls="&tls=skip-verify"
else
    case $db_url in *\?*) db_url="$db_url&tls=disabled" ;; *) db_url="$db_url?tls=disabled" ;; esac
fi
go_dsn=$(python3 -c 'import sys,urllib.parse as u; p=u.urlsplit(sys.argv[1]); print(f"{u.unquote(p.username or "")}:{u.unquote(p.password or "")}@tcp({p.hostname}:{p.port or 3306})/inventory_bench?parseTime=true")' "$IRON_MYSQL_URL")$go_tls
if [ "${BENCH_REUSE:-0}" != 1 ]; then
    "$work/dbtool" drop "$IRON_MYSQL_URL" inventory_bench
fi
"$work/dbtool" create "$IRON_MYSQL_URL" inventory_bench || exit 1

"$work/stub/target/fxstub" 18090 >"$work/stub.log" 2>&1 &
stub_pid=$!

start_service() {   # $1 = iron | go
    if [ "$1" = iron ]; then
        INVENTORY_PORT=$PORT INVENTORY_HOST=127.0.0.1 INVENTORY_MYSQL_URL=$db_url \
        INVENTORY_RATES_URL=http://127.0.0.1:18090 INVENTORY_DB_POOL_MAX=${BENCH_POOL:-32} \
        INVENTORY_LOG_LEVEL=warn taskset -c "$SERVICE_CPUS" "$work/target/inventory" >"$work/svc-$1.log" 2>&1 &
    else
        INVENTORY_PORT=$PORT INVENTORY_DSN=$go_dsn INVENTORY_RATES_URL=http://127.0.0.1:18090 \
        INVENTORY_DB_POOL_MAX=${BENCH_POOL:-32} GOMAXPROCS=$(taskset -c "$SERVICE_CPUS" nproc) \
        taskset -c "$SERVICE_CPUS" "$work/go-baseline" >"$work/svc-$1.log" 2>&1 &
    fi
    svc_pid=$!
    for _ in $(seq 1 200); do
        curl -sf "http://127.0.0.1:$PORT/v1/rates/EUR" >/dev/null 2>&1 &&
            curl -sf "http://127.0.0.1:$PORT/v1/items?limit=1" >/dev/null 2>&1 && return 0
        sleep 0.1
    done
    echo "service $1 did not start:"; cat "$work/svc-$1.log"; exit 1
}

stop_service() {
    kill "$svc_pid" 2>/dev/null
    wait "$svc_pid" 2>/dev/null
    svc_pid=""
}

cpu_ticks() { awk '{print $14 + $15}' "/proc/$svc_pid/stat"; }
rss_kb() { awk '/^VmRSS/{print $2}' "/proc/$svc_pid/status"; }
hwm_kb() { awk '/^VmHWM/{print $2}' "/proc/$svc_pid/status"; }
threads() { awk '/^Threads/{print $2}' "/proc/$svc_pid/status"; }
hz=$(getconf CLK_TCK)

seeded=0
seed() {
    [ $seeded = 1 ] && return
    local have
    have=$("$work/dbtool" scalar "$db_url" "SELECT COUNT(*) FROM items WHERE sku LIKE 'SEED-%'")
    if [ "${have:-0}" -ge "$ITEMS" ]; then seeded=1; return; fi
    # 10 * 10 * 10 * 10 rows from a cross join of digits, then more blocks.
    local blocks=$(( (ITEMS + 9999) / 10000 )) b
    for b in $(seq 0 $((blocks - 1))); do
        "$work/dbtool" scalar "$db_url" "INSERT INTO items (sku, name, category, price_cents, quantity)
            SELECT CONCAT('SEED-', n), CONCAT('Seeded item ', n), CONCAT('cat-', n % 20), 100 + (n * 37) % 50000, 1000000
            FROM (SELECT $b * 10000 + a.d + b.d * 10 + c.d * 100 + e.d * 1000 AS n
                  FROM (SELECT 0 d UNION ALL SELECT 1 UNION ALL SELECT 2 UNION ALL SELECT 3 UNION ALL SELECT 4
                        UNION ALL SELECT 5 UNION ALL SELECT 6 UNION ALL SELECT 7 UNION ALL SELECT 8 UNION ALL SELECT 9) a,
                       (SELECT 0 d UNION ALL SELECT 1 UNION ALL SELECT 2 UNION ALL SELECT 3 UNION ALL SELECT 4
                        UNION ALL SELECT 5 UNION ALL SELECT 6 UNION ALL SELECT 7 UNION ALL SELECT 8 UNION ALL SELECT 9) b,
                       (SELECT 0 d UNION ALL SELECT 1 UNION ALL SELECT 2 UNION ALL SELECT 3 UNION ALL SELECT 4
                        UNION ALL SELECT 5 UNION ALL SELECT 6 UNION ALL SELECT 7 UNION ALL SELECT 8 UNION ALL SELECT 9) c,
                       (SELECT 0 d UNION ALL SELECT 1 UNION ALL SELECT 2 UNION ALL SELECT 3 UNION ALL SELECT 4
                        UNION ALL SELECT 5 UNION ALL SELECT 6 UNION ALL SELECT 7 UNION ALL SELECT 8 UNION ALL SELECT 9) e) s
            WHERE n < $ITEMS" || exit 1
    done
    seeded=1
}

scenario_args() {   # $1 = scenario -> wrk script and path
    case $1 in
        health)  echo "- /healthz" ;;
        rate)    echo "- /v1/rates/EUR" ;;
        get)     echo "$here/lua/get_item.lua /" ;;
        get_eur) echo "$here/lua/get_item.lua /" ;;
        list)    echo "$here/lua/list.lua /" ;;
        stock)   echo "$here/lua/stock.lua /" ;;
        create)  echo "$here/lua/create.lua /" ;;
        *) echo "unknown scenario $1" >&2; exit 2 ;;
    esac
}

printf '| target | scenario | conns | req/s | p50 ms | p90 ms | p99 ms | errors | CPU ms/1k req | RSS MB | peak RSS MB | threads |\n' | tee "$OUT"
printf '|---|---|---|---|---|---|---|---|---|---|---|---|\n' | tee -a "$OUT"
# The schema is the Iron service's: let it migrate before a Go-only run.
if [[ $targets != iron* ]]; then start_service iron; stop_service; fi
IFS=, read -ra T <<<"$targets"
IFS=, read -ra S <<<"$scenarios"
IFS=, read -ra C <<<"$conns"
for target in "${T[@]}"; do
    start_service "$target"
    seed
    for sc in "${S[@]}"; do
        read -r script path <<<"$(scenario_args "$sc")"
        export BENCH_QUERY=""
        [ "$sc" = get_eur ] && export BENCH_QUERY="?currency=EUR"
        for c in "${C[@]}"; do
            # Warm up (connections, statement caches), then measure.
            wargs=(-t"$WRK_THREADS" -c"$c" --latency --timeout 5s)
            [ "$script" != - ] && wargs+=(-s "$script")
            taskset -c "$WRK_CPUS" wrk "${wargs[@]}" -d2s "http://127.0.0.1:$PORT$path" >/dev/null 2>&1
            t0=$(cpu_ticks)
            taskset -c "$WRK_CPUS" wrk "${wargs[@]}" -d"${SECONDS_PER_RUN}s" "http://127.0.0.1:$PORT$path" >"$work/wrk.out" 2>&1
            t1=$(cpu_ticks)
            python3 - "$work/wrk.out" "$target" "$sc" "$c" "$((t1 - t0))" "$hz" "$(rss_kb)" "$(hwm_kb)" "$(threads)" <<'EOF' | tee -a "$OUT"
import re, sys
text = open(sys.argv[1]).read()
target, sc, c, ticks, hz, rss, hwm, thr = sys.argv[2:]
def ms(label):
    m = re.search(r"^\s*%s%%\s+([\d.]+)(us|ms|s)\s*$" % label, text, re.M)
    if not m:
        return float("nan")
    v = float(m.group(1))
    return v / 1000 if m.group(2) == "us" else v * 1000 if m.group(2) == "s" else v
rps = float(re.search(r"Requests/sec:\s+([\d.]+)", text).group(1))
total = int(re.search(r"(\d+) requests in", text).group(1))
errs = 0
m = re.search(r"Non-2xx or 3xx responses: (\d+)", text)
if m: errs += int(m.group(1))
m = re.search(r"Socket errors: connect (\d+), read (\d+), write (\d+), timeout (\d+)", text)
if m: errs += sum(int(x) for x in m.groups())
cpu_ms = int(ticks) * 1000 / int(hz)
per_k = cpu_ms / (total / 1000) if total else float("nan")
print(f"| {target} | {sc} | {c} | {rps:,.0f} | {ms('50'):.2f} | {ms('90'):.2f} | {ms('99'):.2f} | {errs} | {per_k:.1f} | {int(rss)/1024:.1f} | {int(hwm)/1024:.1f} | {thr} |")
EOF
        done
    done
    stop_service
done
[ "${BENCH_KEEP:-0}" = 1 ] || "$work/dbtool" drop "$IRON_MYSQL_URL" inventory_bench
