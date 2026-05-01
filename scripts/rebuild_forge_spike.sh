#!/usr/bin/env bash
# Phase 99 TEST-08 acceptance check: rebuild the Forge framework Phase 1
# spike against THIS repo's v3.2 compiler and confirm that the workarounds
# for issues #47 / #48 / #52 introduced during the Phase 1 spike are no
# longer needed.
#
# This is a developer-machine acceptance check, NOT a recurring CI gate.
# CI runners do not have the spike checked out, so the script must skip
# cleanly with exit 0 when ${FORGE_DIR} is absent.
#
# Cross-reference: .planning/phases/99-doc-test-release/99-CONTEXT.md TEST-08.
#
# Usage: rebuild_forge_spike.sh [IRON_BIN]
#   IRON_BIN: path to the iron binary (default: ./build/iron)
#
# Environment overrides:
#   FORGE_DIR: path to the forge spike checkout (default: $HOME/code/forge)
#
# Behavior contract:
#   1. Skip cleanly with `rebuild_forge_spike SKIP` when FORGE_DIR is absent.
#   2. When FORGE_DIR exists:
#        - Capture original git state (SHA + branch); fetch + ff-pull
#          (best-effort, tolerates non-ff).
#        - Detect presence of #47 / #48 / #52 workarounds.
#        - Apply removable workarounds (#47 CI curl-fetch, #48 type=bin
#          stub main); log #52 as manual follow-up if no `pub` markers.
#        - Set PATH so this repo's iron is first.
#        - Run `iron build` and assert exit 0.
#        - Optionally run target binary --help, tolerating absence.
#        - Print machine-grep-able report block.
#        - Restore the spike to its original SHA via EXIT trap regardless
#          of success/failure (idempotent; never pushes commits).
#   3. Final marker on full success: `rebuild_forge_spike OK`.

set -euo pipefail

IRON_BIN="${1:-./build/iron}"
IRON_BIN_ABS="$(cd "$(dirname "${IRON_BIN}")" && pwd)/$(basename "${IRON_BIN}")"
IRON_BIN_DIR="$(dirname "${IRON_BIN_ABS}")"

if [ ! -x "${IRON_BIN_ABS}" ]; then
    echo "FAIL: iron binary not executable: ${IRON_BIN_ABS}"
    exit 1
fi

FORGE_DIR="${FORGE_DIR:-$HOME/code/forge}"

if [ ! -d "${FORGE_DIR}" ]; then
    echo "SKIP: ${FORGE_DIR} not checked out"
    echo "      (TEST-08 is a developer-machine acceptance check, not a CI gate)"
    echo "rebuild_forge_spike SKIP"
    exit 0
fi

# Capture original git state up front so the EXIT trap can always restore.
ORIG_SHA="$(git -C "${FORGE_DIR}" rev-parse HEAD)"
ORIG_BRANCH="$(git -C "${FORGE_DIR}" rev-parse --abbrev-ref HEAD)"

# Restore-on-exit trap: reset the spike back to its original SHA + branch
# so this script never pollutes the spike checkout, regardless of outcome.
restore_forge() {
    local rc=$?
    git -C "${FORGE_DIR}" reset --hard "${ORIG_SHA}" > /dev/null 2>&1 || true
    if [ "${ORIG_BRANCH}" != "HEAD" ]; then
        git -C "${FORGE_DIR}" checkout "${ORIG_BRANCH}" > /dev/null 2>&1 || true
    fi
    exit "${rc}"
}
trap restore_forge EXIT

git -C "${FORGE_DIR}" fetch --quiet origin > /dev/null 2>&1 || true
git -C "${FORGE_DIR}" pull --ff-only --quiet > /dev/null 2>&1 || true
POST_PULL_SHA="$(git -C "${FORGE_DIR}" rev-parse HEAD)"

echo "forge spike: ${ORIG_SHA} -> ${POST_PULL_SHA}"
echo "iron binary: ${IRON_BIN_ABS}"

# Detect workaround presence (defensive: each is best-effort).
#   WA_47: spike CI workflow defensively curl-fetches lib/diagnostics
#          (Phase 92 fixed; once forge install hits a v3.2+ runtime, the
#          curl-patch step is dead code).
#   WA_48: spike ships as type="bin" with a stub main because Phase 1
#          predated type="lib" (Phase 94 fixed).
#   WA_52: spike has no `pub` modifiers because top-level pub was rejected
#          (Phase 93 fixed). Detected by ABSENCE of pub.
WA_47=0
WA_48=0
WA_52=1   # default: assume workaround present until we find a `pub` marker

if grep -rqlE 'curl.*diagnostics' "${FORGE_DIR}/.github" 2>/dev/null; then
    WA_47=1
fi
if grep -q 'type = "bin"' "${FORGE_DIR}/iron.toml" 2>/dev/null; then
    WA_48=1
fi
if grep -rqE '^[[:space:]]*pub\b' "${FORGE_DIR}/src" 2>/dev/null; then
    WA_52=0
fi

echo "workarounds detected: 47=${WA_47} 48=${WA_48} 52=${WA_52}"

# Apply removable workarounds in-place. The EXIT trap restores the spike
# regardless, so these edits never persist past the script.
if [ "${WA_47}" = "1" ]; then
    # Remove the curl-fetch diagnostics block from forge CI workflows.
    # The trap reset handles cleanup; this is a transient edit only used
    # to verify the v3.2 install no longer needs the workaround.
    for wf in "${FORGE_DIR}"/.github/workflows/*.yml; do
        [ -f "${wf}" ] || continue
        # Best-effort: delete lines matching the curl diagnostics pattern.
        # The trap will reset HEAD if anything goes sideways.
        sed -i.bak -e '/curl.*diagnostics/d' "${wf}" 2>/dev/null || true
        rm -f "${wf}.bak"
    done
    echo "applied workaround removal: #47 (curl-fetch diagnostics)"
fi

if [ "${WA_48}" = "1" ]; then
    # Flip iron.toml from type="bin" to type="lib" and remove the stub main.
    # Note: forge spike is currently a binary-style scaffold; flipping to
    # lib is a probe of "would this work as a lib in v3.2", not a real
    # migration. The trap restores the original on exit either way.
    sed -i.bak -e 's/type = "bin"/type = "lib"/' "${FORGE_DIR}/iron.toml" 2>/dev/null || true
    rm -f "${FORGE_DIR}/iron.toml.bak"
    if [ -f "${FORGE_DIR}/src/main.iron" ]; then
        # Move stub main aside so the lib build doesn't pick it up. Trap
        # reset restores it.
        rm -f "${FORGE_DIR}/src/main.iron"
    fi
    echo "applied workaround removal: #48 (type=bin stub main)"
fi

if [ "${WA_52}" = "1" ]; then
    echo "manual follow-up needed in spike: add \`pub\` to public-API symbols (#52 closure)"
fi

# Run the build with this repo's iron first on PATH so any recursive
# resolver dispatch picks up the v3.2 binary, not a stale ~/.iron install.
export PATH="${IRON_BIN_DIR}:${PATH}"

BUILD_LOG="$(mktemp -t forge-rebuild-XXXXXX.log)"
BUILD_START="$(date +%s)"

set +e
( cd "${FORGE_DIR}" && "${IRON_BIN_ABS}" build ) > "${BUILD_LOG}" 2>&1
BUILD_RC=$?
set -e

BUILD_END="$(date +%s)"
BUILD_SECONDS=$(( BUILD_END - BUILD_START ))

if [ "${BUILD_RC}" != "0" ]; then
    echo "FAIL: forge spike build failed (rc=${BUILD_RC})"
    echo "build log saved to: ${BUILD_LOG}"
    echo "--- last 40 lines of build log ---"
    tail -n 40 "${BUILD_LOG}" || true
    echo "--- end of build log ---"
    exit 1
fi

# Optional: if the build produced a binary under target/, smoke-test --help.
# Tolerate absence (lib builds emit an archive, not a binary).
BINARY_RAN=0
for candidate in \
    "${FORGE_DIR}/target/forge" \
    "${FORGE_DIR}/target/dev/forge" \
    "${FORGE_DIR}/target/release/forge"; do
    if [ -x "${candidate}" ]; then
        set +e
        "${candidate}" --help > /dev/null 2>&1
        rc=$?
        set -e
        if [ "${rc}" = "0" ] || [ "${rc}" = "1" ]; then
            # Either accepted --help (rc 0) or printed usage and exited
            # nonzero. Both are fine; we only require the binary be
            # invokable without crashing the runtime.
            BINARY_RAN=1
            echo "binary smoke OK: ${candidate} --help (rc=${rc})"
            break
        fi
    fi
done

# Machine-grep-able report block. Plan 99-02 / 99-03-SUMMARY.md quote this.
echo "rebuild_forge_spike report"
echo "  forge_sha:           ${POST_PULL_SHA}"
echo "  workarounds_removed: 47=${WA_47} 48=${WA_48} 52=${WA_52}"
echo "  build_rc:            ${BUILD_RC}"
echo "  build_seconds:       ${BUILD_SECONDS}"
echo "  binary_ran:          ${BINARY_RAN}"
echo "rebuild_forge_spike OK"
