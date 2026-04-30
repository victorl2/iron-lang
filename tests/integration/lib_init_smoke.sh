#!/usr/bin/env bash
# Phase 94 TEST-03 smoke: end-to-end `iron init --lib` + `iron build` test.
# Mirrors the Phase 92 install-smoke pattern (.github/workflows/install-smoke.yml).
#
# Usage: lib_init_smoke.sh [IRON_BIN]
#   IRON_BIN: path to the iron binary (default: ./build/iron)
#
# Asserts:
#   1. iron init --lib creates iron.toml + src/lib.iron with the locked template.
#   2. iron build succeeds and produces target/libmylib.a (no warnings).
#   3. nm <archive> shows the `hello` symbol (platform-agnostic substring grep).
#   4. ar t <archive> lists exactly one .o member (filter on .o suffix because
#      macOS BSD ar emits a leading `__.SYMDEF SORTED` symbol-table entry that
#      counts as a "member" in `ar t` output).
#   5. iron run on the lib package errors with the LIB-06 locked message.

set -euo pipefail

IRON_BIN="${1:-./build/iron}"
IRON_BIN_ABS="$(cd "$(dirname "${IRON_BIN}")" && pwd)/$(basename "${IRON_BIN}")"
IRON_BIN_DIR="$(dirname "${IRON_BIN_ABS}")"

WORK="$(mktemp -d -t iron-lib-init-smoke-XXXXXX)"
trap 'rm -rf "${WORK}"' EXIT

cd "${WORK}"
mkdir mylib
cd mylib

"${IRON_BIN_ABS}" init --lib
test -f iron.toml      || { echo "FAIL: iron.toml not created"; exit 1; }
test -f src/lib.iron   || { echo "FAIL: src/lib.iron not created"; exit 1; }
grep -q 'type = "lib"' iron.toml || { echo "FAIL: iron.toml missing type=lib"; exit 1; }
grep -q 'pub func hello' src/lib.iron || { echo "FAIL: scaffold missing 'pub func hello'"; exit 1; }
grep -q 'Hello from mylib!' src/lib.iron || { echo "FAIL: scaffold missing greeting"; exit 1; }

# Ensure the recursive iron build invoked by pkg_build (and the path-dep
# resolver in any future smoke variants) finds the build-tree iron, not a
# stale system install on PATH.
export PATH="${IRON_BIN_DIR}:${PATH}"

build_log="$(mktemp)"
"${IRON_BIN_ABS}" build 2>&1 | tee "${build_log}"
test -f target/libmylib.a || { echo "FAIL: target/libmylib.a not produced"; exit 1; }
test -f target/mylib.iron-stub || { echo "FAIL: target/mylib.iron-stub not produced"; exit 1; }

# No empty-pub warning on a freshly init'd lib (the template has pub already)
if grep -qE 'no .pub. symbols' "${build_log}"; then
    echo "FAIL: empty-pub warning fired on freshly init'd lib"
    cat "${build_log}"
    exit 1
fi

# Platform-agnostic symbol grep (macOS adds a leading _ to symbols, and `_`
# is a word character so a `\bhello\b` match would silently fail on
# `_Iron_hello`. Use a plain substring grep.)
nm target/libmylib.a 2>/dev/null | grep -q hello \
    || { echo "FAIL: 'hello' symbol not in archive"; nm target/libmylib.a; exit 1; }

# Archive contains exactly one .o (TEST-03 ar t check). Filter on `.o$`
# because macOS BSD ar lists `__.SYMDEF SORTED` as a phantom member.
member_count=$(ar t target/libmylib.a | grep -c '\.o$' || true)
[ "${member_count}" = "1" ] || { echo "FAIL: expected 1 .o member, got ${member_count}"; ar t target/libmylib.a; exit 1; }

# LIB-06 run-rejection check: iron run on the lib must fail with the locked
# message and exit 1.
run_log="$(mktemp)"
set +e
"${IRON_BIN_ABS}" run > "${run_log}" 2>&1
run_exit=$?
set -e
[ "${run_exit}" = "1" ] || { echo "FAIL: iron run on lib should exit 1, got ${run_exit}"; cat "${run_log}"; exit 1; }
grep -q 'libraries are not directly runnable' "${run_log}" \
    || { echo "FAIL: iron run on lib missing locked LIB-06 message"; cat "${run_log}"; exit 1; }

echo "lib_init_smoke OK"
