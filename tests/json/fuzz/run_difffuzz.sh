#!/bin/bash
# run_difffuzz.sh - differential fuzzing of the json module against Python's
# json (strict) and yyjson (strict, src/vendor/yyjson), and simdjson when
# SIMDJSON_DIR points at a directory with simdjson.h and simdjson.cpp
# (bench/json/fetch.sh downloads the pinned release).
#
# usage: tests/json/fuzz/run_difffuzz.sh <ironc> <ref_yyjson> [count] [seed]
# ctest runs a short fixed-seed pass; run longer passes by hand, e.g.
#   tests/json/fuzz/run_difffuzz.sh build/ironc build/tests/json/ref_yyjson 200000 $RANDOM
set -u
here=$(cd "$(dirname "$0")" && pwd)
ironc=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
yy=$(cd "$(dirname "$2")" && pwd)/$(basename "$2")
count=${3:-3000}
seed=${4:-20261011}
work=$(mktemp -d "${TMPDIR:-/tmp}/iron_json_fuzz_XXXXXX")
trap 'rm -rf "$work"' EXIT
(cd "$work" && "$ironc" build "$here/jcanon.iron" -o "$work/jcanon" >"$work/build.log" 2>&1) || {
    echo "difffuzz: building jcanon.iron failed"; cat "$work/build.log"; exit 1; }
impls=(--impl "iron=$work/jcanon" --impl "python=python3 $here/ref_python.py" --impl "yyjson=$yy")
if [ -n "${SIMDJSON_DIR:-}" ]; then
    ${CXX:-c++} -O2 -std=c++17 -I"$SIMDJSON_DIR" "$here/ref_simdjson.cpp" "$SIMDJSON_DIR/simdjson.cpp" \
        -o "$work/ref_simdjson" || { echo "difffuzz: building ref_simdjson failed"; exit 1; }
    impls+=(--impl "simdjson=$work/ref_simdjson")
fi
python3 "$here/difffuzz.py" "${impls[@]}" --count "$count" --seed "$seed" --out "${DIFFFUZZ_OUT:-$work/out}"
