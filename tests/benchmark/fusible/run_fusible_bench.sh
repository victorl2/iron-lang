#!/bin/bash
# Usage: run_fusible_bench.sh path/to/iron [runs]
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
iron="$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"
ironc="$(dirname "${iron}")/ironc"
runs="${2:-3}"
work=$(mktemp -d "${TMPDIR:-/tmp}/iron_fusible_bench_XXXXXX")
trap 'rm -rf "${work}"' EXIT
cp "${here}/fusible_bench.iron" "${work}/"
cd "${work}"
"${iron}" build --release fusible_bench.iron -o bench > build.log 2>&1 || {
    cat build.log
    exit 1
}
for i in $(seq "${runs}"); do ./bench > "run${i}.txt"; done
grep -h '^check ' run1.txt | grep -q '^check true 0 ' || {
    echo "FAIL: fused and materialized results differ"
    cat run1.txt
    exit 1
}
fused=$(grep -h '^fused_us ' run*.txt | awk '{print $2}' | sort -n | head -1)
materialized=$(grep -h '^fused_us ' run*.txt | awk '{print $4}' | sort -n | head -1)
ratio=$(awk -v a="${materialized}" -v f="${fused}" \
    'BEGIN { if (f > 0) printf "%.2f", a / f; else printf "-" }')

"${ironc}" build --emit-c fusible_bench.iron > /dev/null
c=.iron-build/fusible_bench.c
body() {
    awk -v f="Iron_$1(" '
        !on && index($0, f) && index($0, "{") { on = 1 }
        on {
            print
            line = $0
            opens = gsub(/\{/, "{", line)
            line = $0
            closes = gsub(/\}/, "}", line)
            depth += opens - closes
            if (depth == 0) exit
        }' "${c}"
}
fused_allocs=$(body fused | grep -c '_create' || true)
materialized_allocs=$(body materialized | grep -c '_create' || true)
if [ "${fused_allocs}" -ne 0 ] || [ "${materialized_allocs}" -ne 2 ]; then
    echo "FAIL: expected 0 fused and 2 materialized allocation sites, got ${fused_allocs} and ${materialized_allocs}"
    exit 1
fi
printf 'fused_us %s materialized_us %s materialized/fused %s\n' \
    "${fused}" "${materialized}" "${ratio}"
echo "intermediate allocation sites per iteration: fused 0, materialized 2"
