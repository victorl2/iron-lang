#!/bin/bash
# run.sh - parse and serialize throughput of the json module against
# Python's json and yyjson on the same machine.
#
# usage: tests/json/bench/run.sh <ironc> [iterations]
# Downloads twitter.json, citm_catalog.json, canada.json and yyjson into
# $JSON_BENCH_DIR (default /tmp/iron_json_bench) on first use and generates
# api.json (a ~1 MB REST response) and small.json with gen_payloads.py.
# Each figure is the best of the iterations.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
ironc=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
iters=${2:-20}
dir=${JSON_BENCH_DIR:-/tmp/iron_json_bench}
mkdir -p "$dir" && cd "$dir"
for f in twitter citm_catalog; do
    [ -f $f.json ] || curl -fsSLo $f.json https://raw.githubusercontent.com/simdjson/simdjson/master/jsonexamples/$f.json
done
[ -f canada.json ] || curl -fsSLo canada.json https://raw.githubusercontent.com/miloyip/nativejson-benchmark/master/data/canada.json
[ -f api.json ] || python3 "$here/gen_payloads.py" >/dev/null
if [ ! -f yyjson.c ]; then
    curl -fsSLo yyjson.c https://raw.githubusercontent.com/ibireme/yyjson/master/src/yyjson.c
    curl -fsSLo yyjson.h https://raw.githubusercontent.com/ibireme/yyjson/master/src/yyjson.h
fi
"$ironc" build "$here/bench_json.iron" -o bench_json --release >/dev/null 2>&1
${CC:-cc} -O2 -I. "$here/bench_yy.c" yyjson.c -o bench_yy
for f in api twitter citm_catalog canada; do
    echo "== $f.json"
    echo -n "iron     "; ./bench_json $f.json "$iters"
    echo -n "python   "; python3 "$here/bench_py.py" $f.json "$iters"
    echo -n "yyjson   "; ./bench_yy $f.json "$iters"
done
"$ironc" build "$here/bench_small.iron" -o bench_small --release >/dev/null 2>&1
echo "== small.json x 200000"
./bench_small small.json 200000
python3 "$here/bench_small.py" small.json 200000
