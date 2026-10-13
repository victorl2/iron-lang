#!/bin/bash
# run.sh - build and run the JSON benchmarks (see README.md).
#
# usage: bench/json/run.sh <ironc> [options]
#   --out FILE        results file (default bench-json-results.txt); RESULT lines
#                     prefixed with the build variant
#   --native          also build every library with -march=native (-mcpu=native on arm64)
#   --cpu N           pin to CPU N with taskset (Linux; ignored elsewhere)
#   --repeat R        run the whole sweep R times (default 1), to see run-to-run noise
#   --warmup W        warmup iterations per measurement (default 5)
#   --samples S       timed iterations per measurement (default 25)
#   --cold            also measure parse with a 128 MiB cache sweep before each sample
#   --quick           twitter and small only, warmup 2, samples 5 (smoke test)
#
# Every library is compiled by the Iron toolchain's clang (the one ironc
# uses): the references at -O3, the Iron program as `ironc build --release`
# would (-O2) and at -O3, through a wrapper toolchain that appends flags.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
ironc=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
shift
out=bench-json-results.txt native=0 cpu="" repeat=1 warmup=5 samples=25 cold=0 quick=0
while [ $# -gt 0 ]; do
    case $1 in
    --out) out=$2; shift ;;
    --native) native=1 ;;
    --cpu) cpu=$2; shift ;;
    --repeat) repeat=$2; shift ;;
    --warmup) warmup=$2; shift ;;
    --samples) samples=$2; shift ;;
    --cold) cold=1 ;;
    --quick) quick=1; warmup=2; samples=5 ;;
    *) echo "run.sh: unknown option $1" >&2; exit 2 ;;
    esac
    shift
done
data=$("$here/fetch.sh")
deps=$data/deps
work=$(mktemp -d "${TMPDIR:-/tmp}/iron_json_bench_XXXXXX")
trap 'rm -rf "$work"' EXIT

# The Iron toolchain (ironc toolchain info prints its root).
tcroot=$("$ironc" toolchain info 2>&1 | sed -n 's/^toolchain .*(.*, \(.*\))$/\1/p')
[ -d "$tcroot" ] || tcroot=$(ls -d "$HOME"/.iron/toolchain/*/ | tail -1)
tcroot=${tcroot%/}
clang="$tcroot/bin/clang"
sysroot=()
if [ "$(uname)" = Darwin ]; then sysroot=(-isysroot "$(xcrun --show-sdk-path)"); fi
case $(uname -m) in arm64|aarch64) natflag=-mcpu=native ;; *) natflag=-march=native ;; esac

# wrapper <dir> <extra flags...>: a toolchain directory whose clang appends flags.
wrapper() {
    local d=$1; shift
    mkdir -p "$d/bin"
    cp "$tcroot/toolchain.txt" "$d/"
    ln -sf "$tcroot/lib" "$d/lib"
    for t in "$tcroot"/bin/*; do ln -sf "$t" "$d/bin/$(basename "$t")"; done
    rm -f "$d/bin/clang"
    printf '#!/bin/sh\nexec "%s" "$@" %s\n' "$clang" "$*" > "$d/bin/clang"
    chmod +x "$d/bin/clang"
}

build() {   # variant cflags...
    local v=$1; shift
    local o="$work/$v"
    mkdir -p "$o"
    "$clang" -x c -O3 "$@" ${sysroot[@]+"${sysroot[@]}"} -c "$deps/yyjson.c" -o "$o/yyjson.o"
    "$clang" --driver-mode=g++ -std=c++17 -O3 "$@" ${sysroot[@]+"${sysroot[@]}"} -c "$deps/simdjson.cpp" -o "$o/simdjson.o"
    "$clang" --driver-mode=g++ -std=c++17 -O3 "$@" ${sysroot[@]+"${sysroot[@]}"} -I"$deps" -c "$here/bench_ref.cpp" -o "$o/bench_ref.o"
    "$clang" --driver-mode=g++ ${sysroot[@]+"${sysroot[@]}"} "$o/bench_ref.o" "$o/simdjson.o" "$o/yyjson.o" -o "$o/bench_ref"
    wrapper "$o/tc_o2" "$@"
    wrapper "$o/tc_o3" -O3 "$@"
    (cd "$o" && IRON_TOOLCHAIN="$o/tc_o2" "$ironc" build --release "$here/bench_iron.iron" -o "$o/bench_iron" >"$o/iron.log" 2>&1) ||
        { cat "$o/iron.log"; exit 1; }
    (cd "$o" && IRON_TOOLCHAIN="$o/tc_o3" "$ironc" build --release "$here/bench_iron.iron" -o "$o/bench_iron_o3" >"$o/iron.log" 2>&1) ||
        { cat "$o/iron.log"; exit 1; }
}
variants=(default)
build default
if [ $native -eq 1 ]; then build native "$natflag"; variants+=(native); fi

pin=()
if [ -n "$cpu" ] && command -v taskset >/dev/null; then pin=(taskset -c "$cpu"); fi
corpora="twitter citm_catalog canada gsoc-2018 mesh numbers"
[ $quick -eq 1 ] && corpora=twitter
{
    echo "# $(date -u +%Y-%m-%dT%H:%M:%SZ) $(uname -srm) cpu=${cpu:-unpinned} warmup=$warmup samples=$samples"
    echo "# clang: $("$clang" --version | head -1)"
} >> "$out"
measure() {   # variant lib mode file [cold]
    local v=$1 lib=$2 mode=$3 file=$4 extra=${5:-}
    local bin="$work/$v/bench_ref" args=("$lib")
    case $lib in
    iron) bin="$work/$v/bench_iron"; args=() ;;
    iron_o3) bin="$work/$v/bench_iron_o3"; args=() ;;
    esac
    "$here/capture.sh" "$out" "$v" "$lib" "$mode" "${extra:+_cold}" \
        ${pin[@]+"${pin[@]}"} "$bin" ${args[@]+"${args[@]}"} \
        "$mode" "$file" "$warmup" "$samples" $extra
}
libs="iron iron_o3 simdjson_dom simdjson_od yyjson rapidjson"
for r in $(seq 1 "$repeat"); do
    for v in "${variants[@]}"; do
        for c in $corpora; do
            for m in parse validate serialize traverse; do
                for lib in $libs; do measure "$v" "$lib" "$m" "$data/data/$c.json"; done
            done
            if [ $cold -eq 1 ]; then
                for lib in $libs; do measure "$v" "$lib" parse "$data/data/$c.json" cold; done
            fi
        done
        for m in small_parse small_validate small_select small_serialize; do
            for lib in $libs; do measure "$v" "$lib" "$m" "$data/data/small.ndjson"; done
        done
        echo "run.sh: pass $r/$repeat variant $v done" >&2
    done
done
echo "run.sh: results appended to $out; bench/json/table.py $out prints the tables" >&2
