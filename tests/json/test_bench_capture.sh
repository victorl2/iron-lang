#!/bin/bash
set -eu
root=$(cd "$(dirname "$0")/../.." && pwd)
capture="$root/bench/json/capture.sh"
work=$(mktemp -d "${TMPDIR:-/tmp}/iron_json_capture_XXXXXX")
trap 'rm -rf "$work"' EXIT
out="$work/results.txt"

"$capture" "$out" default iron parse "" sh -c \
    'echo "RESULT iron parse input.json 10 20,21,22"'
grep -qx 'RESULT default iron parse input.json 10 20,21,22' "$out"

"$capture" "$out" native rapidjson parse _cold sh -c \
    'echo "RESULT rapidjson parse input.json 10 30,31,32"'
grep -qx 'RESULT native rapidjson parse_cold input.json 10 30,31,32' "$out"

"$capture" "$out" default simdjson_od parse "" sh -c \
    'echo "SKIP simdjson_od parse"'

if "$capture" "$out" default iron parse "" sh -c 'exit 7' 2>/dev/null; then
    echo "capture accepted a failed benchmark process" >&2
    exit 1
fi
if "$capture" "$out" default iron parse "" sh -c 'echo no-result' 2>/dev/null; then
    echo "capture accepted missing benchmark output" >&2
    exit 1
fi
if "$capture" "$out" default iron parse "" sh -c \
    'echo "RESULT iron wrong input.json 10 20"' 2>/dev/null; then
    echo "capture accepted a result for the wrong mode" >&2
    exit 1
fi
