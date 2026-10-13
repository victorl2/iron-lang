#!/bin/bash
# A fusible hint is a performance hint, so removing it or disabling the LIR
# optimizer must not change a program's output, exit status, or panic text.
# Each fixture also declares how many fused loops its optimized generated C
# must contain. This keeps semantic parity from passing when fusion is dead.
set -u

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
iron="$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"
work=$(mktemp -d "${TMPDIR:-/tmp}/iron_fusible_parity_XXXXXX")
trap 'rm -rf "${work}"' EXIT
pass=0
fail=0

run_variant() {
    variant=$1
    src=$2
    extra=$3
    dir="${work}/${name}/${variant}"
    mkdir -p "${dir}"
    cp "${src}" "${dir}/${name}.iron"
    # shellcheck disable=SC2086
    (cd "${dir}" && "${iron}" build ${extra} --emit-c "${name}.iron" -o prog \
        > build.out 2> build.err)
    build_rc=$?
    printf '%s\n' "${build_rc}" > "${dir}/build.rc"
    if [ "${build_rc}" -eq 0 ]; then
        (cd "${dir}" && ./prog > run.out 2> run.err)
        run_rc=$?
    else
        : > "${dir}/run.out"
        : > "${dir}/run.err"
        run_rc=125
    fi
    printf '%s\n' "${run_rc}" > "${dir}/run.rc"
    sed -i "s#${work}#<work>#g" "${dir}/build.out" "${dir}/build.err" \
        "${dir}/run.out" "${dir}/run.err"
}

same_outcome() {
    left=$1
    right=$2
    for part in build.rc build.out build.err run.rc run.out run.err; do
        if ! diff -u "${left}/${part}" "${right}/${part}" > "${work}/diff"; then
            echo "[FAIL] ${name}: ${part} differs between $(basename "${left}") and $(basename "${right}")"
            head -80 "${work}/diff"
            return 1
        fi
    done
}

for original in "${here}"/v4/fusible/*.iron; do
    name=$(basename "${original}" .iron)
    fixture="${work}/${name}"
    mkdir -p "${fixture}/source"
    # Keep blank lines so panic locations stay identical.
    sed -E 's@^([[:space:]]*)//iron:fusible[[:space:]]*$@\1@; s#^([[:space:]]*)@fusible[[:space:]]*$#\1#' \
        "${original}" > "${fixture}/source/${name}.iron"
    if cmp -s "${original}" "${fixture}/source/${name}.iron"; then
        echo "[FAIL] ${name}: fixture has no fusible hint"
        fail=$((fail + 1))
        continue
    fi

    run_variant hinted "${original}" ""
    run_variant plain "${fixture}/source/${name}.iron" ""
    run_variant noopt "${original}" "--no-optimize"

    if ! same_outcome "${fixture}/hinted" "${fixture}/plain" ||
       ! same_outcome "${fixture}/hinted" "${fixture}/noopt"; then
        fail=$((fail + 1))
        continue
    fi

    expected=$(sed -nE 's#^[[:space:]]*//[[:space:]]*@fusion-loops:[[:space:]]*([0-9]+).*#\1#p' \
        "${original}" | head -1)
    if [ -z "${expected}" ]; then
        echo "[FAIL] ${name}: missing // @fusion-loops marker"
        fail=$((fail + 1))
        continue
    fi
    generated="${fixture}/hinted/.iron-build/prog.c"
    if [ ! -f "${generated}" ]; then
        echo "[FAIL] ${name}: optimized build did not leave generated C"
        fail=$((fail + 1))
        continue
    fi
    actual=$(grep -c 'Phase 49: Emit fused loop' "${generated}" || true)
    if [ "${actual}" -ne "${expected}" ]; then
        echo "[FAIL] ${name}: expected ${expected} fused loops, found ${actual}"
        fail=$((fail + 1))
        continue
    fi
    pass=$((pass + 1))
done

echo "fusible parity: ${pass} passed, ${fail} failed"
[ "${fail}" -eq 0 ] && [ "${pass}" -gt 0 ]
