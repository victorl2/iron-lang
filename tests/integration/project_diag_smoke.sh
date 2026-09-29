#!/usr/bin/env bash
# Multi-file package diagnostics: errors point at the real source file and
# its own line numbers (not target/combined.iron), and non-pub top-level
# declarations are private to their file.
#
# Usage: project_diag_smoke.sh [IRON_BIN]

set -euo pipefail

IRON_BIN="${1:-./build/iron}"
IRON_BIN_ABS="$(cd "$(dirname "${IRON_BIN}")" && pwd)/$(basename "${IRON_BIN}")"
IRON="$(dirname "${IRON_BIN_ABS}")/iron"

SANDBOX="$(mktemp -d)"
trap 'rm -rf "${SANDBOX}"' EXIT
fail() { echo "FAIL: $*"; exit 1; }

new_pkg() {
    rm -rf "${SANDBOX}/pkg"
    mkdir -p "${SANDBOX}/pkg/src"
    printf '[package]\nname = "pkg"\nversion = "0.1.0"\ntype = "bin"\n' > "${SANDBOX}/pkg/iron.toml"
}

# 1. Location of an error in the second file.
new_pkg
printf 'import lib\nfunc main() {\n    println("{helper()}")\n}\n' > "${SANDBOX}/pkg/src/main.iron"
printf 'pub func helper() -> Int {\n    return "not an int"\n}\n' > "${SANDBOX}/pkg/src/lib.iron"
out=$(cd "${SANDBOX}/pkg" && "${IRON}" check 2>&1 || true)
echo "${out}" | grep -qF -- "--> src/lib.iron:2:5" || fail "wrong location: ${out}"
echo "${out}" | grep -q "combined.iron" && fail "combined.iron in output: ${out}"

# 2. A private function used from another file.
new_pkg
printf 'import lib\nfunc main() {\n    println("{helper()}")\n}\n' > "${SANDBOX}/pkg/src/main.iron"
printf 'func helper() -> Int {\n    return 42\n}\n' > "${SANDBOX}/pkg/src/lib.iron"
out=$(cd "${SANDBOX}/pkg" && "${IRON}" check 2>&1 || true)
echo "${out}" | grep -q "E0320" || fail "private helper visible across files: ${out}"

# 3. The pub version builds and runs.
printf 'pub func helper() -> Int {\n    return 42\n}\n' > "${SANDBOX}/pkg/src/lib.iron"
out=$(cd "${SANDBOX}/pkg" && "${IRON}" run 2>&1 | tail -1)
[ "${out}" = "42" ] || fail "pub helper: ${out}"

echo "project_diag_smoke OK"
