#!/usr/bin/env bash
# Toolchain lookup: ironc compiles with the pinned bundle and nothing else.
# Builds fake bundles around the clang of the toolchain the test itself runs
# with and checks the lookup order, the manifest check, the developer
# override and what --version reports.
#
# Usage: toolchain_smoke.sh [IRON_BIN]

set -euo pipefail

IRON_BIN="${1:-./build/iron}"
IRON_BIN_ABS="$(cd "$(dirname "${IRON_BIN}")" && pwd)/$(basename "${IRON_BIN}")"
IRON_BIN_DIR="$(dirname "${IRON_BIN_ABS}")"
IRON="${IRON_BIN_DIR}/iron"
IRONC="${IRON_BIN_DIR}/ironc"

SANDBOX="$(mktemp -d)"
trap 'rm -rf "${SANDBOX}"' EXIT
cd "${SANDBOX}"

fail() { echo "toolchain_smoke: $*" >&2; exit 1; }

# The real toolchain, found the normal way: the version line and the path.
real="$("${IRONC}" toolchain path)" || fail "no toolchain for the test itself"
[ -f "${real}/toolchain.txt" ] || fail "${real} has no manifest"
llvm=$(sed -n 's/^llvm //p' "${real}/toolchain.txt")
bundle=$(sed -n 's/^bundle //p' "${real}/toolchain.txt")
"${IRONC}" --version | grep -q "^toolchain llvm ${llvm} bundle ${bundle} (" || fail "--version does not report the toolchain"
"${IRON}" --version | grep -q "^toolchain llvm ${llvm} bundle ${bundle} (" || fail "iron --version does not report the toolchain"

# A bundle is a directory with a manifest and bin/clang.
make_bundle() {   # make_bundle <dir> <llvm> <bundle>
    mkdir -p "$1/bin"
    ln -sf "${real}/bin/clang" "$1/bin/clang"
    printf 'iron-toolchain 1\nllvm %s\nbundle %s\nhost test\n' "$2" "$3" > "$1/toolchain.txt"
}

cat > hello.iron <<'EOF'
func main() {
    println("toolchain ok")
}
EOF

# 1. IRON_TOOLCHAIN wins and is reported as such.
make_bundle "${SANDBOX}/override" "${llvm}" "${bundle}"
out=$(IRON_TOOLCHAIN="${SANDBOX}/override" "${IRONC}" toolchain path)
[ "${out}" = "${SANDBOX}/override" ] || fail "IRON_TOOLCHAIN not used: ${out}"
IRON_TOOLCHAIN="${SANDBOX}/override" "${IRONC}" --version | grep -q "(IRON_TOOLCHAIN, ${SANDBOX}/override)" || fail "--version does not name the override"
IRON_TOOLCHAIN="${SANDBOX}/override" "${IRONC}" run hello.iron | grep -q "toolchain ok" || fail "run through the override failed"

# 2. A mismatched override warns but compiles.
make_bundle "${SANDBOX}/old" "0.0.1" "${bundle}"
err=$(IRON_TOOLCHAIN="${SANDBOX}/old" "${IRONC}" run hello.iron 2>&1 >/dev/null) || fail "mismatched override refused"
grep -q "warning: IRON_TOOLCHAIN=.* is llvm 0.0.1 bundle" <<<"${err}" || fail "no mismatch warning: ${err}"

# 3. An override without a manifest, or pointing nowhere, is an error.
mkdir -p "${SANDBOX}/empty"
out=$(IRON_TOOLCHAIN="${SANDBOX}/empty" "${IRONC}" toolchain path 2>&1 || true)
grep -q "has no toolchain.txt" <<<"${out}" || fail "manifest-less override accepted: ${out}"
out=$(IRON_TOOLCHAIN="${SANDBOX}/missing" "${IRONC}" toolchain path 2>&1 || true)
grep -q "is not a directory" <<<"${out}" || fail "missing override accepted: ${out}"

# 4. Without the override, the per-user location is used and a mismatched
#    bundle there is refused (no download: the pins have no test bundle).
export HOME="${SANDBOX}/home"
unset IRON_TOOLCHAIN
make_bundle "${HOME}/.iron/toolchain/${llvm}-${bundle}" "${llvm}" "${bundle}"
out=$("${IRONC}" toolchain path) || fail "per-user bundle not found"
case "${out}" in "${HOME}/.iron/toolchain/${llvm}-${bundle}") ;; *) fail "unexpected path ${out}";; esac
"${IRONC}" --version | grep -q "(user, ${HOME}/.iron/toolchain/${llvm}-${bundle})" || fail "--version does not report the user bundle"
make_bundle "${HOME}/.iron/toolchain/${llvm}-${bundle}" "0.0.1" "${bundle}"
out=$("${IRONC}" toolchain path 2>&1 || true)
grep -q "is llvm 0.0.1 bundle .*; this compiler needs llvm ${llvm}" <<<"${out}" || fail "mismatched user bundle accepted: ${out}"
"${IRONC}" --version | grep -q "(refused)" || fail "--version does not say refused"

echo "toolchain_smoke OK"
