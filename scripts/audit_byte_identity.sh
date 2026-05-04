#!/usr/bin/env bash
# Phase 98 PATCH-02: behavioral-equivalence audit harness.
#
# For each canonical example (pong, rotating_cube, model_viewer, post_fx,
# raylib_showcase) this script:
#   1. Captures a BEFORE snapshot of the build outcome by compiling against
#      the CURRENT stdlib state. Records build exit code; if the example
#      emits stdout when run with a brief timeout, captures it; otherwise
#      records a build-only equivalence marker.
#   2. (Full mode only) Runs scripts/migrate_standalone_to_patch.sh on the
#      stdlib (idempotent; no-op if already migrated).
#   3. (Full mode only) Rebuilds the compiler against the migrated stdlib.
#   4. Captures an AFTER snapshot the same way.
#   5. Diffs target/before/<example>.out against target/after/<example>.out.
#      Any diff is a behavioral-equivalence failure (exit 1).
#
# PATCH-02 contract (revised 2026-05-01): post-migration build outcomes and
# program output remain identical to pre-migration. Locks the
# semantic-preservation guarantee at the user-observable level.
#
# Note: byte-identity at the C-emission level was dropped because the
# patch-body parser synthesizes a `self: TYPE` first parameter that the
# standalone form lacks (parser.c:4102-4128); even with the Phase 98 HIR
# stub-self-stripping fix the emitted C signatures may differ in metadata
# the codegen weaves around the call. Behavioral equivalence is the
# user-facing guarantee that actually matters.
#
# Usage:
#   scripts/audit_byte_identity.sh                 # full audit
#   scripts/audit_byte_identity.sh --before-only   # capture BEFORE only
#   scripts/audit_byte_identity.sh --after-only    # capture AFTER + diff
#
# Build mechanics: every example builds via single-file ironc invocation:
#   cd <example_dir> && ironc build <ex>.iron -o _audit_bin
# The runtime check is best-effort: each binary runs with a 2s SIGTERM
# timeout (raylib examples block on Window.should_close so they would
# otherwise loop forever). For examples that print to stdout in their
# first 2 seconds, the captured stdout is compared. For windowed examples
# that produce no stdout, the snapshot records BUILD_OK and any non-fatal
# exit signal (SIGTERM = 124 from `timeout`); equivalence is then the
# build-success outcome.

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

# Detect a portable timeout binary. macOS ships `gtimeout` via coreutils;
# Linux ships `timeout` natively. If neither is present, fall back to
# build-only equivalence (no run step).
TIMEOUT_BIN=""
if command -v timeout >/dev/null 2>&1; then
    TIMEOUT_BIN="timeout"
elif command -v gtimeout >/dev/null 2>&1; then
    TIMEOUT_BIN="gtimeout"
fi

WORK="$(mktemp -d -t iron-audit-XXXXXX)"
trap 'rm -rf "$WORK"' EXIT

# ---------------------------------------------------------------------------
# Audit target matrix.
# Format: "<label> <relative_dir> <entry_iron_file>"
# Build invocation per target:
#     ( cd <relative_dir> && ironc build <entry_iron_file> -o _audit_bin )
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
# Builds the example and records build outcome + (optional) stdout to
# target/<phase>/<label>.out. The .out file format is:
#   line 1: BUILD=<ok|fail>
#   line 2: RUN_AVAILABLE=<yes|no>
#   line 3 onward: STDOUT_BEGIN ... STDOUT_END (if RUN_AVAILABLE=yes)
# ---------------------------------------------------------------------------
capture_one() {
    local label="$1"
    local dir="$2"
    local entry="$3"
    local phase="$4"
    local outdir="${REPO_ROOT}/target/${phase}"
    mkdir -p "$outdir"
    local outf="${outdir}/${label}.out"
    local logf="${WORK}/${label}_${phase}.log"

    local build_ok=1
    (
        cd "${REPO_ROOT}/${dir}"
        rm -rf .iron-build _audit_bin
        "${IRONC}" build "$entry" -o "_audit_bin" > "$logf" 2>&1
    ) || build_ok=0

    if [ "$build_ok" -eq 0 ]; then
        {
            echo "BUILD=fail"
            echo "RUN_AVAILABLE=no"
            echo "BUILD_LOG_BEGIN"
            cat "$logf"
            echo "BUILD_LOG_END"
        } > "$outf"
        echo "  ${phase}: ${label} -> BUILD=fail (see $outf)"
        # Cleanup the (likely missing) binary.
        rm -f "${REPO_ROOT}/${dir}/_audit_bin"
        return 0
    fi

    # Optional run step: only if the timeout binary exists. Headless raylib
    # examples typically block on Window.should_close; the timeout kills
    # the binary after 2s. We accept BOTH normal exit (0) and timeout-kill
    # (124 from `timeout`) as success markers - both indicate the program
    # started without immediately crashing (segfault, missing symbol, etc.).
    local stdout_file="${WORK}/${label}_${phase}.stdout"
    : > "$stdout_file"
    local run_status=""
    if [ -n "$TIMEOUT_BIN" ] && [ -x "${REPO_ROOT}/${dir}/_audit_bin" ]; then
        # Run with detached stdin, suppress display (DISPLAY="") and audio,
        # capture stdout. Stderr is redirected to /dev/null because raylib
        # logs informational messages there that vary across runs.
        local rc=0
        (
            cd "${REPO_ROOT}/${dir}"
            "${TIMEOUT_BIN}" --signal=TERM 2 ./_audit_bin > "$stdout_file" 2>/dev/null
        ) || rc=$?
        # Map exit codes:
        #   0     - clean exit
        #   124   - timeout fired (expected for windowed loops)
        #   139   - segfault (real failure)
        #   130   - SIGINT
        #   other - report verbatim
        case "$rc" in
            0)   run_status="exit_0" ;;
            124) run_status="timeout_2s" ;;
            *)   run_status="exit_${rc}" ;;
        esac
    else
        run_status="run_skipped"
    fi

    {
        echo "BUILD=ok"
        if [ "$run_status" = "run_skipped" ]; then
            echo "RUN_AVAILABLE=no"
        else
            echo "RUN_AVAILABLE=yes"
            echo "RUN_STATUS=${run_status}"
            echo "STDOUT_BEGIN"
            cat "$stdout_file"
            echo "STDOUT_END"
        fi
    } > "$outf"

    rm -f "${REPO_ROOT}/${dir}/_audit_bin"
    echo "  ${phase}: ${label} -> BUILD=ok RUN=${run_status} ($(wc -c < "$outf" | tr -d ' ') bytes)"
}

# ---------------------------------------------------------------------------
# diff_all - compares target/before/<label>.out vs target/after/<label>.out
# ---------------------------------------------------------------------------
diff_all() {
    local fail=0
    for entry in "${AUDIT_TARGETS[@]}"; do
        # shellcheck disable=SC2086
        set -- $entry
        local label="$1"
        local before="${REPO_ROOT}/target/before/${label}.out"
        local after="${REPO_ROOT}/target/after/${label}.out"
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
        # The build log contents may differ (filenames, mtime in clang
        # output, etc.). For the equivalence check we compare only the
        # BUILD= and RUN_AVAILABLE / RUN_STATUS markers plus the captured
        # STDOUT body. Build-log bodies are excluded.
        extract_canonical "$before" > "${WORK}/${label}.before.canon"
        extract_canonical "$after"  > "${WORK}/${label}.after.canon"
        if diff -u "${WORK}/${label}.before.canon" "${WORK}/${label}.after.canon" \
            > "${WORK}/${label}.diff"; then
            local marker
            marker=$(awk -F= '/^BUILD=/{b=$2} /^RUN_STATUS=/{r=$2} END{print "BUILD="b" RUN="r}' \
                "${WORK}/${label}.before.canon")
            echo "OK:   ${label}: behavior-identical (${marker})"
        else
            echo "FAIL: ${label}: behavioral-equivalence audit failed" >&2
            echo "--- diff (truncated to 50 lines) ---" >&2
            head -50 "${WORK}/${label}.diff" >&2
            cp "${WORK}/${label}.diff" "${REPO_ROOT}/target/${label}.diff"
            echo "    full diff saved to target/${label}.diff" >&2
            fail=1
        fi
    done
    return $fail
}

# Extract the canonical equivalence-check fields from a .out snapshot.
# Drops BUILD_LOG_* blocks (build-log bodies are noise) but preserves
# BUILD/RUN/STDOUT markers and contents.
extract_canonical() {
    awk '
    /^BUILD_LOG_BEGIN$/ { skip=1; next }
    /^BUILD_LOG_END$/   { skip=0; next }
    skip               { next }
                       { print }
    ' "$1"
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
            echo "audit_byte_identity OK (5/5 examples behavior-identical)"
        else
            echo "audit_byte_identity FAILED" >&2
            exit 1
        fi
        ;;
esac

exit 0
