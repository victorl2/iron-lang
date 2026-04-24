#!/usr/bin/env bash
# Phase 7 Plan 07-07 Task 01 (HARD-22, D-10) — version stamp coherence.
#
# Asserts that iron + ironc + ironls all print the SAME IRON_VERSION_FULL
# string via their --version flag. The single source of truth is
# CMakeLists.txt's IRON_VERSION_FULL variable, injected into every binary
# via the IRON_VERSION_STRING compile-time define.
#
# Failure mode: if any one binary drifts (hardcoded literal, missing
# define, stale build) the semver strings won't match and this test
# fires, blocking the merge.
#
# CTest registration: tests/lsp/invariant/CMakeLists.txt passes
# $<TARGET_FILE:...> for each binary so in-tree builds work without
# install(). Labels: integration;phase-m6-invariant.

set -euo pipefail

IRON_BIN="${1:-}"
IRONC_BIN="${2:-}"
IRONLS_BIN="${3:-}"

if [[ -z "$IRON_BIN" || -z "$IRONC_BIN" || -z "$IRONLS_BIN" ]]; then
    echo "usage: $0 <iron> <ironc> <ironls>" >&2
    exit 2
fi

for bin in "$IRON_BIN" "$IRONC_BIN" "$IRONLS_BIN"; do
    if [[ ! -x "$bin" ]]; then
        echo "FAIL: $bin is not executable" >&2
        exit 1
    fi
done

extract_semver() {
    # The three --version formats are slightly different but every one
    # begins with "<binary-name> <semver>" on the first whitespace
    # boundary. The semver token may include a pre-release suffix like
    # "-alpha" or "-alpha.7", so match greedily up to the first space.
    "$1" --version 2>&1 | head -1 | awk '{print $2}'
}

IRON_VER=$(extract_semver "$IRON_BIN")
IRONC_VER=$(extract_semver "$IRONC_BIN")
IRONLS_VER=$(extract_semver "$IRONLS_BIN")

if [[ -z "$IRON_VER" || -z "$IRONC_VER" || -z "$IRONLS_VER" ]]; then
    echo "FAIL: at least one binary did not print a version string" >&2
    echo "  iron:   '$IRON_VER'" >&2
    echo "  ironc:  '$IRONC_VER'" >&2
    echo "  ironls: '$IRONLS_VER'" >&2
    exit 1
fi

if [[ "$IRON_VER" != "$IRONC_VER" || "$IRONC_VER" != "$IRONLS_VER" ]]; then
    echo "FAIL: version stamps diverge across binaries" >&2
    echo "  iron:   $IRON_VER" >&2
    echo "  ironc:  $IRONC_VER" >&2
    echo "  ironls: $IRONLS_VER" >&2
    exit 1
fi

# Shape check — must look like semver (major.minor.patch with optional
# pre-release). Protects against silent breakage where all 3 binaries
# fall back to a sentinel like "unknown".
if ! [[ "$IRON_VER" =~ ^[0-9]+\.[0-9]+\.[0-9]+(-[a-zA-Z0-9.]+)?$ ]]; then
    echo "FAIL: version $IRON_VER does not match semver shape" >&2
    exit 1
fi

echo "OK: all 3 binaries report $IRON_VER"
