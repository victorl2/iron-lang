#!/bin/bash
# profile.sh - sample the service with perf while wrk drives one scenario.
#
#   apps/inventory/bench/profile.sh <iron> <scenario> [connections] [seconds]
#
# Builds the service with frame pointers, starts it like bench/run.sh does
# (same environment variables), runs wrk and records `perf record -g` on the
# service for the measured part, then prints the hottest functions (self and
# children). Needs perf_event_open: in podman add
# --security-opt seccomp=unconfined. The perf.data file is left in
# $PROFILE_OUT (default ./perf-<scenario>.data).
set -u
here=$(cd "$(dirname "$0")" && pwd)
app=$(cd "$here/.." && pwd)
[ $# -ge 2 ] || { sed -n '2,12p' "$0" | sed 's/^# \{0,1\}//'; exit 2; }
IRON=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
IRONC=$(dirname "$IRON")/ironc
sc=$2
c=${3:-64}
secs=${4:-10}
: "${IRON_MYSQL_URL:?}"
SERVICE_CPUS=${SERVICE_CPUS:-0-3}
WRK_CPUS=${WRK_CPUS:-4-7}
OUT=${PROFILE_OUT:-$PWD/perf-$sc.data}
PERF=${PERF:-$(ls /usr/lib/linux-tools/*/perf 2>/dev/null | head -1)}
export BENCH_ITEMS=${BENCH_ITEMS:-10000}

work=$(mktemp -d "${TMPDIR:-/tmp}/inventory-prof.XXXXXX")
trap 'kill $svc_pid $stub_pid 2>/dev/null; wait 2>/dev/null; rm -rf "$work"' EXIT
cp -R "$app/src" "$app/iron.toml" "$work/" && mkdir -p "$work/stub" &&
    cp -R "$app/stub/src" "$app/stub/iron.toml" "$work/stub/" || exit 1
(cd "$work" && IRON_CFLAGS="-g -fno-omit-frame-pointer" "$IRON" build --release >build.log 2>&1) || { cat "$work/build.log"; exit 1; }
(cd "$work/stub" && "$IRON" build --release >/dev/null 2>&1) || exit 1
"$IRONC" build "$app/tests/dbtool.iron" -o "$work/dbtool" >/dev/null 2>&1 || exit 1
db_url=$(python3 -c 'import sys,urllib.parse as u; p=u.urlsplit(sys.argv[1]); print(u.urlunsplit((p.scheme,p.netloc,"/inventory_bench",p.query,"")))' "$IRON_MYSQL_URL")
"$work/dbtool" create "$IRON_MYSQL_URL" inventory_bench || exit 1
"$work/stub/target/fxstub" 18090 >/dev/null 2>&1 &
stub_pid=$!
INVENTORY_PORT=18080 INVENTORY_HOST=127.0.0.1 INVENTORY_MYSQL_URL=$db_url \
INVENTORY_RATES_URL=http://127.0.0.1:18090 INVENTORY_LOG_LEVEL=warn \
    taskset -c "$SERVICE_CPUS" "$work/target/inventory" >"$work/svc.log" 2>&1 &
svc_pid=$!
for _ in $(seq 1 200); do
    curl -sf http://127.0.0.1:18080/readyz >/dev/null 2>&1 && break
    sleep 0.1
done
n=$("$work/dbtool" scalar "$db_url" "SELECT COUNT(*) FROM items")
if [ "${n:-0}" -lt "$BENCH_ITEMS" ]; then
    echo "profile.sh: seed the database first (bench/run.sh with BENCH_KEEP=1)" >&2
fi
case $sc in
    health) args=(http://127.0.0.1:18080/healthz) ;;
    rate) args=(http://127.0.0.1:18080/v1/rates/EUR) ;;
    *) args=(-s "$here/lua/$sc.lua" http://127.0.0.1:18080/) ;;
esac
[ "$sc" = get ] && args=(-s "$here/lua/get_item.lua" http://127.0.0.1:18080/)
taskset -c "$WRK_CPUS" wrk -t4 -c"$c" -d2s "${args[@]}" >/dev/null 2>&1
taskset -c "$WRK_CPUS" wrk -t4 -c"$c" -d"$((secs + 2))s" --latency "${args[@]}" >"$work/wrk.out" 2>&1 &
wrk_pid=$!
sleep 1
"$PERF" record -F 999 -g -p "$svc_pid" -o "$OUT" -- sleep "$secs" >/dev/null 2>&1
wait $wrk_pid
grep -E "Requests/sec|Latency  " "$work/wrk.out"
"$PERF" report -i "$OUT" --no-children --stdio --sort symbol 2>/dev/null | grep -E '^ +[0-9]' | head -40
echo "--- with children ---"
"$PERF" report -i "$OUT" --children --stdio --sort symbol 2>/dev/null | grep -E '^ +[0-9]' | head -40
