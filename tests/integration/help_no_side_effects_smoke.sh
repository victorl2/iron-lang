#!/usr/bin/env bash
# Phase 97 TEST-05: assert iron + ironc <sub> --help invocations
# produce non-empty stdout, exit 0, and create zero side effects in
# the cwd. Mirrors the lib_init_smoke.sh shape (mktemp sandbox,
# cleanup trap, OK marker for run_tests.sh dual-check).
#
# Usage: help_no_side_effects_smoke.sh [IRON_BIN]
#   IRON_BIN: path to iron OR ironc (default: ./build/iron). When
#             invoked with the ironc basename (CTest quirk per Phase
#             95 deferred-items.md baseline), the script swaps in the
#             sibling iron from the same build directory.
#
# Asserts, for every iron + ironc subcommand --help invocation:
#   1. Exit code is 0.
#   2. Stdout is non-empty.
#   3. Sandbox cwd contents BEFORE == sandbox cwd contents AFTER
#      (no scaffolded files, no target/, no temp output, nothing).
#
# The init invocation (`iron init --help`) is the highest-stakes
# assertion: it must NOT scaffold a new package in the sandbox.

set -euo pipefail

IRON_BIN_ARG="${1:-./build/iron}"
IRON_BIN_ABS="$(cd "$(dirname "${IRON_BIN_ARG}")" && pwd)/$(basename "${IRON_BIN_ARG}")"
IRON_BIN_DIR="$(dirname "${IRON_BIN_ABS}")"

# Sibling-binary swap: if invoked with the ironc basename (CTest
# baseline quirk per Phase 95 deferred-items.md), pick up the iron
# sibling so we can exercise BOTH binaries from one entry point.
case "$(basename "${IRON_BIN_ABS}")" in
    ironc)
        IRONC_BIN="${IRON_BIN_ABS}"
        IRON_BIN="${IRON_BIN_DIR}/iron"
        ;;
    iron)
        IRON_BIN="${IRON_BIN_ABS}"
        IRONC_BIN="${IRON_BIN_DIR}/ironc"
        ;;
    *)
        echo "FAIL: unexpected binary basename: $(basename "${IRON_BIN_ABS}")" >&2
        exit 1
        ;;
esac

[ -x "${IRON_BIN}" ]  || { echo "FAIL: iron not executable: ${IRON_BIN}" >&2; exit 1; }
[ -x "${IRONC_BIN}" ] || { echo "FAIL: ironc not executable: ${IRONC_BIN}" >&2; exit 1; }

WORK="$(mktemp -d -t iron-help-smoke-XXXXXX)"
trap 'rm -rf "${WORK}"' EXIT
STDOUT_LOG="${WORK}/stdout.log"

# check_help <bin_path> <sub_or_empty> <label>
#   Runs <bin> <sub> --help (or <bin> --help when sub is empty) inside
#   a fresh sandbox subdir; asserts exit 0, non-empty stdout, and
#   byte-identical sandbox before/after.
#
#   stdout MUST be redirected OUTSIDE the sandbox so the AFTER snapshot
#   stays clean. ls -A is portable across BSD ls (macOS) and GNU ls
#   (Linux) and excludes . and .. but includes dotfiles.
check_help() {
    local bin="$1"
    local sub="$2"
    local label="$3"

    local sandbox="${WORK}/sandbox_${label}"
    mkdir -p "${sandbox}"
    cd "${sandbox}"

    local before
    before=$(ls -A | sort)

    set +e
    if [ -z "${sub}" ]; then
        "${bin}" --help > "${STDOUT_LOG}" 2>&1
    else
        "${bin}" "${sub}" --help > "${STDOUT_LOG}" 2>&1
    fi
    local rc=$?
    set -e

    local after
    after=$(ls -A | sort)

    if [ "${rc}" -ne 0 ]; then
        echo "FAIL: ${label}: exit ${rc} (expected 0)"
        echo "--- stdout: ---"
        cat "${STDOUT_LOG}"
        exit 1
    fi
    if [ ! -s "${STDOUT_LOG}" ]; then
        echo "FAIL: ${label}: stdout was empty"
        exit 1
    fi
    if [ "${before}" != "${after}" ]; then
        echo "FAIL: ${label}: side effect detected"
        echo "--- before: ---"
        echo "${before}"
        echo "--- after:  ---"
        echo "${after}"
        exit 1
    fi

    cd - > /dev/null
}

# iron: top-level + 7 subcommands (build, run, check, fmt, test, init, migrate).
check_help "${IRON_BIN}" ""        "iron_top"
check_help "${IRON_BIN}" "build"   "iron_build"
check_help "${IRON_BIN}" "run"     "iron_run"
check_help "${IRON_BIN}" "check"   "iron_check"
check_help "${IRON_BIN}" "fmt"     "iron_fmt"
check_help "${IRON_BIN}" "test"    "iron_test"
check_help "${IRON_BIN}" "init"    "iron_init"
check_help "${IRON_BIN}" "migrate" "iron_migrate"

# ironc: top-level + 6 subcommands (build, run, check, fmt, test, migrate).
# init is iron-only; ironc init --help falls through to the existing
# argv loop and is intentionally NOT exercised here.
check_help "${IRONC_BIN}" ""        "ironc_top"
check_help "${IRONC_BIN}" "build"   "ironc_build"
check_help "${IRONC_BIN}" "run"     "ironc_run"
check_help "${IRONC_BIN}" "check"   "ironc_check"
check_help "${IRONC_BIN}" "fmt"     "ironc_fmt"
check_help "${IRONC_BIN}" "test"    "ironc_test"
check_help "${IRONC_BIN}" "migrate" "ironc_migrate"

echo "help_no_side_effects_smoke OK"
