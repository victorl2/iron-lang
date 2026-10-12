#!/bin/bash
# run_soa_bench.sh: time [T, layout: soa] against the default layout and
# check that the field loops over a soa list are vectorized.
#
# Usage: run_soa_bench.sh path/to/iron [runs]
#
# Builds soa_bench.iron as `iron build` does, runs it `runs` times (default
# 3) and prints the best time of each kernel in both layouts. Then compiles
# the generated C to assembly with the same flags and looks, in each field
# kernel, for packed vector instructions on the column data (NEON on arm64,
# SSE2 or AVX on x86-64). Exit status 1 when a soa field kernel is not
# vectorized or the two layouts disagree.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
iron="$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"
ironc="$(dirname "${iron}")/ironc"
runs="${2:-3}"
work=$(mktemp -d "${TMPDIR:-/tmp}/iron_soa_bench_XXXXXX")
trap 'rm -rf "${work}"' EXIT
cp "${here}/soa_bench.iron" "${work}/"
cd "${work}"
"${iron}" build soa_bench.iron -o bench > build.log 2>&1 || { cat build.log; exit 1; }
for i in $(seq "${runs}"); do ./bench > "run${i}.txt"; done
grep -h '^check' run1.txt | grep -q '^check 0 true 0 true true true$' ||
    { echo "FAIL: the two layouts computed different results"; cat run1.txt; exit 1; }
printf '%-14s %12s %12s %8s\n' kernel soa_us aos_us aos/soa
for k in push sum_ids integrate count advance whole remove_insert; do
    soa=$(grep -h "^${k} " run*.txt | awk '{print $3}' | sort -n | head -1)
    aos=$(grep -h "^${k} " run*.txt | awk '{print $5}' | sort -n | head -1)
    printf '%-14s %12s %12s %8s\n' "${k}" "${soa}" "${aos}" \
        "$(awk -v a="${aos}" -v s="${soa}" 'BEGIN { if (s > 0) printf "%.2f", a / s; else printf "-" }')"
done

# Assembly of the kernels, with the flags of a default `iron build`.
"${ironc}" build soa_bench.iron --emit-c > /dev/null 2>&1
src="$(cd "${here}/../../../src" && pwd)"
cc="${CC:-clang}"
"${cc}" -O3 -std=gnu17 -fwrapv -fno-strict-aliasing -ffp-contract=off -w \
    -I"${src}" -I"${src}/vendor" -I"${src}/stdlib" -S -o bench.s .iron-build/soa_bench.c
case "$(uname -m)" in
    arm64|aarch64) packed='\.2d|\.4s' ;;
    *)             packed='[[:space:]]v?(p?add|mul|padd)(pd|ps|q|d)[[:space:]].*[xyz]mm' ;;
esac
status=0
for k in sum_ids integrate count advance; do
    n=$(awk -v f="Iron_${k}_soa" '$1 ~ "^_?" f ":" { on = 1 } on && /cfi_endproc/ { on = 0 } on' bench.s |
        grep -cE "${packed}" || true)
    if [ "${n}" -gt 0 ]; then
        echo "vectorized: ${k} (${n} packed instructions)"
    else
        echo "FAIL: ${k} over a soa list is not vectorized"
        status=1
    fi
done
exit "${status}"
