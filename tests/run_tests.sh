#!/bin/bash
# run_tests.sh - Shared test runner for Iron compiler .iron test categories.
#
# Two modes of operation per .iron file:
#
#   1. Run-and-compare (default when a <name>.expected sibling exists):
#      Compiles, runs the resulting binary, compares stdout+stderr to the
#      .expected file (trailing newlines trimmed).
#
#   2. Compile-only (file opts in via a `-- @compile-only` marker anywhere
#      in the first 10 lines — typical for fixtures whose runtime needs
#      a GUI/display/raylib window that CI can't provide):
#      Only verifies `iron build` succeeds. No binary execution, no output
#      compare.
#
# A .iron file with neither a .expected sibling nor the @compile-only
# marker is a HARD FAIL, not a silent skip. (Historical rot had stale
# fixtures with neither — caught by CI only after they were migrated; see
# 2026-04-18 commit on feat/v2-raylib-milestone.)
#
# Usage: ./run_tests.sh <category> [path/to/iron]
#   category:  Subdirectory name under the directory containing this script
#              (e.g. integration, algorithms, composite, manual)
#   iron:      Path to iron binary; defaults to ./build/iron relative to
#              the project root (two levels above this script)

set -euo pipefail

CATEGORY="${1:?usage: run_tests.sh <category> [iron-bin]}"
IRON_BIN_ARG="${2:-}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TEST_DIR="${SCRIPT_DIR}/${CATEGORY}"

if [ -z "${IRON_BIN_ARG}" ]; then
    IRON_BIN_ARG="${SCRIPT_DIR}/../build/iron"
fi

# Resolve to absolute path so it works after cd
IRON_BIN="$(cd "$(dirname "${IRON_BIN_ARG}")" && pwd)/$(basename "${IRON_BIN_ARG}")"

if [ ! -x "${IRON_BIN}" ]; then
    echo "error: iron binary not found or not executable: ${IRON_BIN}" >&2
    exit 1
fi

if [ ! -d "${TEST_DIR}" ]; then
    echo "error: test directory not found: ${TEST_DIR}" >&2
    exit 1
fi

PASS=0
FAIL=0
TOTAL=0

echo "=== Iron ${CATEGORY} Tests ==="
echo "Using: ${IRON_BIN}"
echo "Dir:   ${TEST_DIR}"
echo ""

# Create a per-run temp directory for compiled binaries
WORK_DIR=$(mktemp -d /tmp/iron_test_XXXXXX)
trap 'rm -rf "${WORK_DIR}"' EXIT

shopt -s nullglob
for test_file in "${TEST_DIR}"/*.iron; do
    test_name=$(basename "${test_file}" .iron)
    expected_file="${TEST_DIR}/${test_name}.expected"
    TOTAL=$((TOTAL + 1))

    # @compile-only marker: grep the first 10 lines of the .iron source.
    # Files that opt in only need to build successfully — no binary run,
    # no output compare. Used for fixtures that require a GUI/display
    # at runtime (raylib window, audio device, …) which CI can't supply.
    compile_only=0
    if head -n 10 "${test_file}" | grep -qE '^[[:space:]]*(--|//)[[:space:]]*@compile-only\b'; then
        compile_only=1
    fi

    if [ ! -f "${expected_file}" ] && [ "${compile_only}" -eq 0 ]; then
        echo "[FAIL] ${test_name} (missing .expected; add an .expected sibling or an '-- @compile-only' marker)"
        FAIL=$((FAIL + 1))
        continue
    fi

    echo -n "[RUN ] ${test_name} ... "

    # Build the test in a temp directory so the output binary is isolated
    build_dir="${WORK_DIR}/${test_name}"
    mkdir -p "${build_dir}"
    build_stderr="${WORK_DIR}/${test_name}_build.err"

    # iron build derives the output name from the source file (without .iron)
    # and places it in the current directory, so we cd into build_dir first
    if ! (cd "${build_dir}" && "${IRON_BIN}" build "${test_file}") 2>"${build_stderr}"; then
        echo "[FAIL] (build failed)"
        cat "${build_stderr}" >&2
        FAIL=$((FAIL + 1))
        continue
    fi

    output_bin="${build_dir}/${test_name}"
    if [ ! -x "${output_bin}" ]; then
        echo "[FAIL] (binary not found at ${output_bin})"
        FAIL=$((FAIL + 1))
        continue
    fi

    # Compile-only tests end here — we've proved the build succeeds and
    # the binary landed on disk. Running would block on a GUI/display.
    if [ "${compile_only}" -eq 1 ]; then
        echo "[PASS] (compile-only)"
        PASS=$((PASS + 1))
        continue
    fi

    # Run and capture stdout+stderr
    actual=$("${output_bin}" 2>&1) || true

    # Compare output (trim trailing newline for robustness)
    expected=$(cat "${expected_file}")
    expected="${expected%$'\n'}"

    if [ "${actual}" = "${expected}" ]; then
        echo "[PASS]"
        PASS=$((PASS + 1))
    else
        echo "[FAIL]"
        echo "  Expected: $(echo "${expected}" | head -5)"
        echo "  Actual:   $(echo "${actual}" | head -5)"
        FAIL=$((FAIL + 1))
    fi
done

echo ""
echo "Results: ${PASS} passed, ${FAIL} failed, ${TOTAL} total"

if [ "${FAIL}" -gt 0 ]; then
    exit 1
fi
