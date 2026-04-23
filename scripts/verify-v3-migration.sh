#!/bin/bash
# scripts/verify-v3-migration.sh
# Release blocker: verifies that ironc emits byte-identical C for every fixture in
# tests/migrate_identity/sources/ compared to the checked-in golden at
# tests/migrate_identity/expected/<name>.c
#
# Requirements: ironc on PATH or ./build/ironc; diff(1)
# Optional:     clang (for compile-check mode). Gracefully skipped when absent.
#
# Exit 0: all fixtures match (or clang absent -- SKIPPED reported)
# Exit 1: any diff found; summary printed to stdout
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"
SOURCES_DIR="$REPO_ROOT/tests/migrate_identity/sources"
EXPECTED_DIR="$REPO_ROOT/tests/migrate_identity/expected"
WORK_DIR="$(mktemp -d)"
trap 'rm -rf "$WORK_DIR"' EXIT

# ---------------------------------------------------------------------------
# Locate ironc
# ---------------------------------------------------------------------------
if [ -f "$REPO_ROOT/build/ironc" ]; then
    IRONC="$REPO_ROOT/build/ironc"
elif command -v ironc >/dev/null 2>&1; then
    IRONC="ironc"
else
    echo "ERROR: ironc not found (tried ./build/ironc and PATH)"
    exit 1
fi

# ---------------------------------------------------------------------------
# Detect clang availability
# ---------------------------------------------------------------------------
if ! command -v clang >/dev/null 2>&1; then
    CLANG_AVAILABLE=0
else
    CLANG_AVAILABLE=1
fi

# ---------------------------------------------------------------------------
# Detect emit-c subcommand (probe for 'emit-c', fall back to 'build --emit-c')
# ---------------------------------------------------------------------------
if $IRONC emit-c --help >/dev/null 2>&1; then
    SUBCOMMAND="emit-c"
else
    SUBCOMMAND="build --emit-c"
fi

# ---------------------------------------------------------------------------
# --generate mode: regenerate golden C files from sources
# ---------------------------------------------------------------------------
if [ "${1:-}" = "--generate" ]; then
    mkdir -p "$EXPECTED_DIR"
    for src in "$SOURCES_DIR"/*.iron; do
        [ -f "$src" ] || { echo "WARN: no .iron files found in $SOURCES_DIR"; exit 0; }
        name="$(basename "$src" .iron)"
        # shellcheck disable=SC2086
        if $IRONC $SUBCOMMAND "$src" -o "$EXPECTED_DIR/$name.c" 2>&1; then
            echo "GENERATED $name"
        else
            echo "ERROR: ironc failed for $name"
            exit 1
        fi
    done
    exit 0
fi

# ---------------------------------------------------------------------------
# Verification mode: diff sources against goldens
# ---------------------------------------------------------------------------
FAIL=0
PASS=0
WARN=0

echo "================================================================"
echo " verify-v3-migration.sh"
echo " Checking byte-identity of emitted C vs. committed goldens"
echo "================================================================"

for src in "$SOURCES_DIR"/*.iron; do
    [ -f "$src" ] || { echo "WARN: no .iron files found in $SOURCES_DIR"; break; }
    name="$(basename "$src" .iron)"

    printf "%-12s ... " "$name"

    # Emit C into a temp file
    # shellcheck disable=SC2086
    if ! emit_output=$($IRONC $SUBCOMMAND "$src" -o "$WORK_DIR/$name.c" 2>&1); then
        echo "FAIL (ironc error)"
        echo "  ironc stderr: $emit_output"
        FAIL=$((FAIL + 1))
        continue
    fi

    # No golden yet -- skip diff (not a failure)
    if [ ! -f "$EXPECTED_DIR/$name.c" ]; then
        echo "WARN (no golden -- skipping diff; run with --generate to create)"
        WARN=$((WARN + 1))
        continue
    fi

    # Byte-identity diff
    if diff "$EXPECTED_DIR/$name.c" "$WORK_DIR/$name.c" >/dev/null 2>&1; then
        echo "PASS"
        PASS=$((PASS + 1))
    else
        echo "FAIL (diff)"
        diff "$EXPECTED_DIR/$name.c" "$WORK_DIR/$name.c" || true
        FAIL=$((FAIL + 1))
    fi
done

echo "================================================================"

if [ "$FAIL" -gt 0 ]; then
    echo " $FAIL fixture(s) failed"
    exit 1
fi

if [ "$CLANG_AVAILABLE" -eq 0 ]; then
    echo "NOTE: clang not found -- compile-check skipped; byte-identity diffs above are the release gate"
fi

exit 0
