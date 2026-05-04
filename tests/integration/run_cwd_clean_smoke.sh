#!/usr/bin/env bash
# Phase 96 TEST-06: assert `iron run` leaves no binaries in cwd or any
# ancestor for both the outside-package (RUN-01) and inside-package
# (RUN-02) paths. Mirrors the Phase 94 lib_init_smoke.sh shape.
#
# Usage: run_cwd_clean_smoke.sh [IRON_BIN]
#   IRON_BIN: path to the iron binary (default: ./build/iron)
#
# Asserts:
#   Scenario A (outside-package, RUN-01):
#     1. iron run t.iron in a sandbox tmpdir produces no binary in cwd.
#     2. BEFORE == AFTER ls -A diff (modulo the test's own files).
#     3. Stdout contains the expected program output.
#
#   Scenario B (inside-package, RUN-02):
#     1. iron init creates a default bin package.
#     2. iron run produces target/run/<pkg> (NOT target/<pkg>).
#     3. Package root stays free of stray executables.

set -euo pipefail

IRON_BIN="${1:-./build/iron}"
IRON_BIN_ABS="$(cd "$(dirname "${IRON_BIN}")" && pwd)/$(basename "${IRON_BIN}")"
IRON_BIN_DIR="$(dirname "${IRON_BIN_ABS}")"

# CTest invokes run_tests.sh with ${CMAKE_BINARY_DIR}/ironc as IRON_BIN,
# but `iron init` and `iron run` (no source-file arg) are package-mode
# commands implemented only in the `iron` binary, NOT in `ironc`. When
# the supplied binary is `ironc`, prefer the sibling `iron` if it exists
# (matches the CTest build layout). Documented baseline issue carried
# from Phase 95 deferred-items.md (the CTest runner passes ironc instead
# of iron to handlers that need package commands).
case "$(basename "${IRON_BIN_ABS}")" in
    ironc)
        if [ -x "${IRON_BIN_DIR}/iron" ]; then
            IRON_BIN_ABS="${IRON_BIN_DIR}/iron"
        fi
        ;;
esac

# Ensure the recursive iron build invoked by pkg_build (and any path-dep
# resolver) finds the build-tree iron, not a stale system install on PATH.
export PATH="${IRON_BIN_DIR}:${PATH}"

WORK="$(mktemp -d -t iron-cwd-clean-smoke-XXXXXX)"
trap 'rm -rf "${WORK}"' EXIT

# ── Scenario A: outside-package (RUN-01) ─────────────────────────────────
mkdir -p "${WORK}/A"
cd "${WORK}/A"
printf 'func main() {\n  println("ok")\n}\n' > t.iron
BEFORE_A="$(ls -A | sort)"
"${IRON_BIN_ABS}" run t.iron > out.log 2>&1
grep -q '^ok$' out.log || {
    echo "FAIL A: missing 'ok' in stdout"
    cat out.log
    exit 1
}
AFTER_A="$(ls -A | sort)"
EXPECTED_A="$(printf 'out.log\nt.iron\n' | sort)"
if [ "${AFTER_A}" != "${EXPECTED_A}" ]; then
    echo "FAIL A: stray files in cwd after iron run t.iron"
    echo "  expected:"
    echo "${EXPECTED_A}" | sed 's/^/    /'
    echo "  actual:"
    echo "${AFTER_A}" | sed 's/^/    /'
    exit 1
fi

# ── Scenario B: inside-package (RUN-02) ──────────────────────────────────
mkdir -p "${WORK}/B"
cd "${WORK}/B"
"${IRON_BIN_ABS}" init > /dev/null  # default bin template
# iron init names the package after the directory; read the actual name
# from iron.toml so the test stays robust if mktemp picks a different
# basename across platforms.
PKG_NAME="$(grep -m1 '^name' iron.toml | sed -E 's/^name *= *"([^"]+)"/\1/')"
[ -n "${PKG_NAME}" ] || {
    echo "FAIL B: could not parse package name from iron.toml"
    cat iron.toml
    exit 1
}

"${IRON_BIN_ABS}" run > out.log 2>&1 || {
    echo "FAIL B: iron run failed"
    cat out.log
    exit 1
}

# RUN-02 invariant: binary lives at target/run/<pkg>, NOT target/<pkg>.
test -f "target/run/${PKG_NAME}" || {
    echo "FAIL B: target/run/${PKG_NAME} missing after iron run"
    ls -lR target 2>/dev/null
    exit 1
}
if [ -f "target/${PKG_NAME}" ]; then
    echo "FAIL B: target/${PKG_NAME} should NOT exist on iron run (only on iron build)"
    ls -lR target
    exit 1
fi

# Package root stays clean: no stray executable files (directories and
# .iron sources are fine; iron.toml and other regular files are not
# expected to be executable).
for f in *; do
    [ -d "${f}" ] && continue
    [ -x "${f}" ] || continue
    echo "FAIL B: stray executable in package root: ${f}"
    exit 1
done

echo "run_cwd_clean_smoke OK"
