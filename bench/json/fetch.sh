#!/bin/bash
# fetch.sh - download the pinned benchmark corpora and reference libraries
# into $JSON_BENCH_DIR (default ~/.cache/iron-json-bench) and check their
# SHA-256 sums. Nothing here is vendored: the corpora are large and the
# libraries are only needed to compare against.
#
#   corpora    simdjson/simdjson-data @ 4197c425 (twitter, citm_catalog,
#              canada, gsoc-2018, mesh, numbers), Apache-2.0 repository
#   simdjson   v5.0.3 single-header amalgamation (Apache-2.0 / MIT)
#   yyjson     0.13.0 (MIT)
#   rapidjson  Tencent/rapidjson @ 24b5e7a8 (MIT)
#
# usage: bench/json/fetch.sh            prints the directory
set -eu
here=$(cd "$(dirname "$0")" && pwd)
dir=${JSON_BENCH_DIR:-$HOME/.cache/iron-json-bench}
mkdir -p "$dir/data" "$dir/deps"
DATA=https://raw.githubusercontent.com/simdjson/simdjson-data/4197c425e857f0ec38e89822fdd0bd9ea21f4daf/jsonexamples
SIMD=https://raw.githubusercontent.com/simdjson/simdjson/v5.0.3/singleheader
YY=https://raw.githubusercontent.com/ibireme/yyjson/0.13.0/src
RJ=https://github.com/Tencent/rapidjson/archive/24b5e7a8b27f42fa16b96fc70aade9106cf7102f.tar.gz

get() {   # url dest
    if [ ! -f "$2" ]; then
        curl -fsSL --retry 3 -o "$2.part" "$1"
        mv "$2.part" "$2"
    fi
}
for f in twitter citm_catalog canada gsoc-2018 mesh numbers; do get "$DATA/$f.json" "$dir/data/$f.json"; done
get "$SIMD/simdjson.h" "$dir/deps/simdjson.h"
get "$SIMD/simdjson.cpp" "$dir/deps/simdjson.cpp"
get "$YY/yyjson.h" "$dir/deps/yyjson.h"
get "$YY/yyjson.c" "$dir/deps/yyjson.c"
if [ ! -d "$dir/deps/rapidjson" ]; then
    get "$RJ" "$dir/deps/rapidjson.tar.gz"
    tar -xzf "$dir/deps/rapidjson.tar.gz" -C "$dir/deps"
    mv "$dir/deps"/rapidjson-24b5e7a8b27f42fa16b96fc70aade9106cf7102f/include/rapidjson "$dir/deps/rapidjson"
    rm -rf "$dir/deps"/rapidjson-24b5e7a8b27f42fa16b96fc70aade9106cf7102f
fi
[ -f "$dir/data/small.ndjson" ] || python3 "$here/gen_small.py" "$dir/data/small.ndjson"
(cd "$dir" && if command -v sha256sum >/dev/null; then sha256sum -c --quiet "$here/SHA256SUMS"; else shasum -a 256 -c --quiet "$here/SHA256SUMS"; fi) ||
    { echo "fetch.sh: checksum mismatch (see $here/SHA256SUMS)" >&2; exit 1; }
echo "$dir"
