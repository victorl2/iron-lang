#!/bin/bash
# soa_parity.sh: a list behaves the same with layout: soa and without.
#
# Every fixture of tests/integration/v4/soa is written with layout: soa
# lists and checked against its .expected by the v4 suite. This script
# builds each one again with the annotations removed (the default layout)
# and requires the same output, or the same panic for an @expect-panic
# fixture: the two layouts are one language.
#
# Usage: soa_parity.sh path/to/iron
set -u
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
iron="$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"
work=$(mktemp -d "${TMPDIR:-/tmp}/iron_soa_parity_XXXXXX")
trap 'rm -rf "${work}"' EXIT
pass=0; fail=0
for src in "${here}"/v4/soa/*.iron; do
    name=$(basename "${src}" .iron)
    grep -q 'layout: soa\]' "${src}" || { echo "[FAIL] ${name}: no layout: soa list"; fail=$((fail + 1)); continue; }
    mkdir -p "${work}/${name}"
    sed 's/, layout: soa\]/]/g' "${src}" > "${work}/${name}/${name}.iron"
    if ! (cd "${work}/${name}" && "${iron}" build "${name}.iron" -o prog > build.log 2>&1); then
        echo "[FAIL] ${name}: default layout build failed"; cat "${work}/${name}/build.log"
        fail=$((fail + 1)); continue
    fi
    # (in a shell of its own, so an expected abort is not reported here)
    bash -c 'cd "$1" && ./prog > out.txt 2> err.txt' _ "${work}/${name}" 2> /dev/null; rc=$?
    panic=$(head -n 10 "${src}" \
        | sed -nE 's#^[[:space:]]*(--|//)[[:space:]]*@expect-panic:[[:space:]]*##p' \
        | head -1)
    if [ -n "${panic}" ]; then
        if [ "${rc}" -ne 0 ] && grep -qF -- "${panic}" "${work}/${name}/err.txt"; then
            pass=$((pass + 1))
        else
            echo "[FAIL] ${name}: default layout did not panic with '${panic}' (exit ${rc})"
            fail=$((fail + 1))
        fi
    elif [ "${rc}" -eq 0 ] && diff -u "${here}/v4/soa/${name}.expected" "${work}/${name}/out.txt" > "${work}/${name}/diff.txt"; then
        pass=$((pass + 1))
    else
        echo "[FAIL] ${name}: default layout output differs (exit ${rc})"; head -40 "${work}/${name}/diff.txt"
        fail=$((fail + 1))
    fi
done
echo "soa parity: ${pass} passed, ${fail} failed"
[ "${fail}" -eq 0 ] && [ "${pass}" -gt 0 ]
