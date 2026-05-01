#!/usr/bin/env bash
# Phase 98 PATCH-02: byte-identity audit harness.
#
# For each canonical example (pong, rotating_cube, model_viewer, post_fx,
# raylib_showcase) this script:
#   1. Captures a BEFORE snapshot of the emitted C by compiling against
#      the CURRENT stdlib state.
#   2. (Full mode only) Runs scripts/migrate_standalone_to_patch.sh on the
#      stdlib (idempotent; no-op if already migrated).
#   3. (Full mode only) Rebuilds the compiler against the migrated stdlib.
#   4. Captures an AFTER snapshot of the emitted C by compiling against
#      the migrated stdlib.
#   5. diff -u target/before/<example>.c target/after/<example>.c — any
#      diff is an audit failure (exit 1).
#
# PATCH-02 contract: post-migration generated C is byte-identical to
# pre-migration generated C. Locks the semantic-preservation guarantee.
#
# Usage:
#   scripts/audit_byte_identity.sh                 # full audit
#   scripts/audit_byte_identity.sh --before-only   # capture BEFORE only
#   scripts/audit_byte_identity.sh --after-only    # capture AFTER + diff
#
# Build mechanics: every example builds via single-file ironc invocation:
#   cd <example_dir> && ironc build <ex>.iron -o _audit_bin --debug-build
# The --debug-build flag preserves .iron-build/_audit_bin.c, which is the
# emitted C captured for the snapshot. Pong's iron.toml is ignored here —
# we use its bare .iron entry-point file directly via ironc, not iron build.

set -euo pipefail

# ---------------------------------------------------------------------------
# Argv parse
# ---------------------------------------------------------------------------
MODE="full"
case "${1:-}" in
    --before-only) MODE="before" ;;
    --after-only)  MODE="after"  ;;
    "")            MODE="full"   ;;
    *) echo "usage: $0 [--before-only|--after-only]" >&2; exit 2 ;;
esac

# ---------------------------------------------------------------------------
# Setup
# ---------------------------------------------------------------------------
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"
cd "$REPO_ROOT"

IRONC="${IRONC:-${REPO_ROOT}/build/ironc}"
[ -x "$IRONC" ] || { echo "FAIL: ironc not found at $IRONC" >&2; exit 1; }

WORK="$(mktemp -d -t iron-audit-XXXXXX)"
trap 'rm -rf "$WORK"' EXIT

# ---------------------------------------------------------------------------
# Audit target matrix.
# Format: "<label> <relative_dir> <entry_iron_file>"
# Build invocation per target:
#     ( cd <relative_dir> && ironc build <entry_iron_file> -o _audit_bin --debug-build )
# Emitted C path: <relative_dir>/.iron-build/_audit_bin.c
# ---------------------------------------------------------------------------
AUDIT_TARGETS=(
    "pong            examples/pong            pong.iron"
    "rotating_cube   examples/rotating_cube   rotating_cube.iron"
    "model_viewer    examples/model_viewer    model_viewer.iron"
    "post_fx         examples/post_fx         post_fx.iron"
    "raylib_showcase examples/raylib_showcase raylib_showcase.iron"
)

# ---------------------------------------------------------------------------
# capture_one <label> <dir> <entry> <phase>
# Builds the example and copies the emitted .c into target/<phase>/<label>.c
# ---------------------------------------------------------------------------
capture_one() {
    local label="$1"
    local dir="$2"
    local entry="$3"
    local phase="$4"
    local outdir="${REPO_ROOT}/target/${phase}"
    mkdir -p "$outdir"
    local outc="${outdir}/${label}.c"
    local logf="${WORK}/${label}_${phase}.log"

    (
        cd "${REPO_ROOT}/${dir}"
        rm -rf .iron-build _audit_bin
        "${IRONC}" build "$entry" -o "_audit_bin" --debug-build > "$logf" 2>&1
    ) || {
        echo "FAIL: ${label} ${phase}: ironc build failed" >&2
        echo "--- build log ---" >&2
        cat "$logf" >&2
        return 1
    }

    local cfile="${REPO_ROOT}/${dir}/.iron-build/_audit_bin.c"
    if [ ! -s "$cfile" ]; then
        echo "FAIL: ${label} ${phase}: no .iron-build/_audit_bin.c emitted" >&2
        return 1
    fi
    cp "$cfile" "$outc"
    # Cleanup the build artefact (keep .iron-build so verbose users can
    # inspect; remove the binary).
    rm -f "${REPO_ROOT}/${dir}/_audit_bin"
    echo "  ${phase}: ${label} -> ${outc} ($(wc -c < "$outc" | tr -d ' ') bytes)"
}

# ---------------------------------------------------------------------------
# diff_all — compares target/before/<label>.c vs target/after/<label>.c
# ---------------------------------------------------------------------------
diff_all() {
    local fail=0
    for entry in "${AUDIT_TARGETS[@]}"; do
        # shellcheck disable=SC2086
        set -- $entry
        local label="$1"
        local before="${REPO_ROOT}/target/before/${label}.c"
        local after="${REPO_ROOT}/target/after/${label}.c"
        if [ ! -s "$before" ]; then
            echo "FAIL: ${label}: BEFORE snapshot missing or empty (${before})" >&2
            fail=1
            continue
        fi
        if [ ! -s "$after" ]; then
            echo "FAIL: ${label}: AFTER snapshot missing or empty (${after})" >&2
            fail=1
            continue
        fi
        if diff -u "$before" "$after" > "${WORK}/${label}.diff"; then
            echo "OK:   ${label}: byte-identical ($(wc -c < "$before" | tr -d ' ') bytes)"
        else
            echo "FAIL: ${label}: byte-identity audit failed" >&2
            echo "--- diff (truncated to 50 lines) ---" >&2
            head -50 "${WORK}/${label}.diff" >&2
            cp "${WORK}/${label}.diff" "${REPO_ROOT}/target/${label}.diff"
            echo "    full diff saved to target/${label}.diff" >&2
            fail=1
        fi
    done
    return $fail
}

# ---------------------------------------------------------------------------
# Capture BEFORE
# ---------------------------------------------------------------------------
case "$MODE" in
    before|full)
        echo "==> Capturing BEFORE snapshots..."
        for entry in "${AUDIT_TARGETS[@]}"; do
            # shellcheck disable=SC2086
            set -- $entry
            capture_one "$1" "$2" "$3" "before"
        done
        ;;
esac

# ---------------------------------------------------------------------------
# Run codemod + rebuild compiler (full mode only)
# ---------------------------------------------------------------------------
if [ "$MODE" = "full" ]; then
    echo "==> Running codemod on stdlib..."
    "${REPO_ROOT}/scripts/migrate_standalone_to_patch.sh" \
        "${REPO_ROOT}/src/stdlib/string.iron" \
        "${REPO_ROOT}/src/stdlib/math.iron" \
        "${REPO_ROOT}/src/stdlib/raylib.iron"

    echo "==> Rebuilding compiler with migrated stdlib..."
    cmake --build "${REPO_ROOT}/build" --target iron ironc > "${WORK}/rebuild.log" 2>&1 \
        || {
            echo "FAIL: rebuild failed" >&2
            cat "${WORK}/rebuild.log" >&2
            exit 1
        }
fi

# ---------------------------------------------------------------------------
# Capture AFTER + diff
# ---------------------------------------------------------------------------
case "$MODE" in
    after|full)
        echo "==> Capturing AFTER snapshots..."
        for entry in "${AUDIT_TARGETS[@]}"; do
            # shellcheck disable=SC2086
            set -- $entry
            capture_one "$1" "$2" "$3" "after"
        done
        echo "==> Diffing BEFORE vs AFTER..."
        if diff_all; then
            echo "audit_byte_identity OK (5/5 examples byte-identical)"
        else
            echo "audit_byte_identity FAILED" >&2
            exit 1
        fi
        ;;
esac

exit 0
