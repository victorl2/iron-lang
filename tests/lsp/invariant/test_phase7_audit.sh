#!/usr/bin/env bash
# HARD-24 meta-gate (Phase 7 Plan 07-08):
#
# Assert every Phase 7 artifact from CONTEXT §D-14 still exists, every
# phase-mN-invariant CTest label (m1..m6) is populated, and both parity
# harnesses (`test_parity_ironc_lsp`, `test_parity_ironc_lsp_fmt`) are
# registered under the CTest graph.
#
# Runs on every PR via `ctest -L phase-m6-invariant`. If somebody deletes
# a Phase 7 critical artifact (for example `parity.yml` itself), this
# audit fires red before branch protection would even get the chance to
# catch the missing `parity` status check.
#
# Argument contract: none. Env:
#   IRON_BUILD_DIR (optional, default "build") — ctest query target dir.
#
# Exit code: 0 on audit clean; 1 on any missing file or label.

set -euo pipefail

# Walk up to the repository root (the script lives at tests/lsp/invariant/).
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
cd "$REPO_ROOT"

FAIL=0

check() {
    local path="$1"
    if [[ ! -e "$path" ]]; then
        echo "MISSING: $path" >&2
        FAIL=1
    fi
}

# --------------------------------------------------------------------------
# Phase 7 Plan 07-01 — supervisor + crash dump + parent-death (HARD-13/14/20)
# --------------------------------------------------------------------------
check src/lsp/supervisor/supervisor.c
check src/lsp/obs/crash_dump.c
check src/lsp/obs/parent_watch.c
check tests/lsp/invariant/test_crash_dump_written.sh
check tests/lsp/invariant/test_parent_death_detection.sh

# --------------------------------------------------------------------------
# Phase 7 Plan 07-02 — RSS cap + soak harness (HARD-15/16)
# --------------------------------------------------------------------------
check src/lsp/obs/rss.c
check tests/lsp/soak/harness.py
check tests/lsp/soak/workload.jsonl
check tests/lsp/soak/short.jsonl
check .github/workflows/soak.yml
check tests/lsp/invariant/test_rss_cap_trips_exit_42.sh

# --------------------------------------------------------------------------
# Phase 7 Plan 07-03 — ThreadSanitizer (HARD-17)
# --------------------------------------------------------------------------
check tests/lsp/tsan/driver.py
check tests/lsp/tsan/workload.jsonl
check .github/workflows/tsan.yml

# --------------------------------------------------------------------------
# Phase 7 Plan 07-04 — SLOs + build-time regression (HARD-18/23)
# --------------------------------------------------------------------------
check tests/lsp/slos/measurement.py
check .github/workflows/slos.yml
check build-time-baseline.json
check .github/workflows/build-time.yml
check scripts/ci/build_time_check.py

# --------------------------------------------------------------------------
# Phase 7 Plan 07-05 — LSP fuzz harnesses (HARD-19)
# --------------------------------------------------------------------------
check tests/fuzz/lsp/frame/fuzz_target.c
check tests/fuzz/lsp/json/fuzz_target.c
check tests/fuzz/lsp/dispatch/fuzz_target.c
check tests/fuzz/lsp/didChange/fuzz_target.c

# --------------------------------------------------------------------------
# Phase 7 Plan 07-06 — macOS signing + notarization (HARD-21)
# --------------------------------------------------------------------------
check scripts/ci/sign_and_notarize_macos.sh
check docs/dev/apple-notarization-setup.md

# --------------------------------------------------------------------------
# Phase 7 Plan 07-07 — version coherence (HARD-22)
# --------------------------------------------------------------------------
check src/lsp/cli/version.c
check src/lsp/cli/version.h
check tests/lsp/invariant/test_version_stamp_coherence.sh

# --------------------------------------------------------------------------
# Phase 7 Plan 07-08 — parity blocking gate + CI-gates doc (HARD-24)
# --------------------------------------------------------------------------
check .github/workflows/parity.yml
check docs/dev/ci-gates.md
check docs/dev/release-runbook.md
check .planning/phases/07-m6-production-hardening/07-PHASE-SUMMARY.md

# --------------------------------------------------------------------------
# Phase label audit — every phase's invariant label must exist in the
# CTest graph. A missing label means a plan's invariants have dropped out
# of the CI sweep, defeating the cross-phase regression fence.
# --------------------------------------------------------------------------
BUILD_DIR="${IRON_BUILD_DIR:-build}"

if [[ ! -d "$BUILD_DIR" ]]; then
    echo "MISSING build dir: $BUILD_DIR (set IRON_BUILD_DIR or run from repo root after configure)" >&2
    FAIL=1
else
    LABELS="$(ctest --test-dir "$BUILD_DIR" --print-labels 2>&1 || true)"
    for m in 1 2 3 4 5 6; do
        if ! echo "$LABELS" | grep -q "phase-m${m}-invariant"; then
            echo "MISSING CTest label: phase-m${m}-invariant" >&2
            FAIL=1
        fi
    done

    # Parity tests must exist by name in the CTest graph. These are the
    # Core-Value enforcement tests and must never silently drop out.
    TESTS="$(ctest --test-dir "$BUILD_DIR" -N 2>&1 || true)"
    if ! echo "$TESTS" | grep -qE "Test[ ]*#[0-9]+:[ ]*test_parity_ironc_lsp\b"; then
        echo "MISSING CTest: test_parity_ironc_lsp" >&2
        FAIL=1
    fi
    if ! echo "$TESTS" | grep -qE "test_parity_ironc_lsp_fmt\b"; then
        echo "MISSING CTest: test_parity_ironc_lsp_fmt" >&2
        FAIL=1
    fi
fi

if [[ $FAIL -ne 0 ]]; then
    echo "" >&2
    echo "test_phase7_audit: FAIL" >&2
    exit 1
fi

echo "test_phase7_audit: PASS"
