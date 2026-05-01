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

# Phase 93 multi-file integration fixtures: directory-shaped fixtures under
# tests/integration/multi_file/<case>/ with N >= 1 .iron files plus a sibling
# `expected` file (no leading dot — there is no <name>.iron to pair against).
# The runner concatenates the .iron files in lexicographic order (so lib.iron
# precedes main.iron alphabetically) with a synthetic `-- @file: <basename>`
# directive between them. The lexer recognizes that directive and re-tags
# subsequent tokens with the named filename so the resolver's cross-module
# check (E0320) sees real per-file source identity.
#
# A fixture directory without an `expected` sibling is treated as compile-only.
if [ "${CATEGORY}" = "integration" ] && [ -d "${TEST_DIR}/multi_file" ]; then
    for case_dir in "${TEST_DIR}/multi_file"/*/; do
        [ -d "${case_dir}" ] || continue
        case_name="$(basename "${case_dir}")"
        TOTAL=$((TOTAL + 1))

        # Collect .iron files in lexicographic (sorted) order.
        iron_files=()
        for f in "${case_dir}"*.iron; do
            [ -f "${f}" ] || continue
            iron_files+=("${f}")
        done
        if [ "${#iron_files[@]}" -eq 0 ]; then
            echo "[FAIL] multi_file/${case_name} (no .iron files in directory)"
            FAIL=$((FAIL + 1))
            continue
        fi

        # Sort the array (bash arrays don't sort intrinsically; portable sort).
        IFS=$'\n' iron_files=($(printf '%s\n' "${iron_files[@]}" | sort))
        unset IFS

        # Build a combined source file with @file directives.
        build_dir="${WORK_DIR}/multi_file_${case_name}"
        mkdir -p "${build_dir}"
        combined="${build_dir}/${case_name}.iron"
        : > "${combined}"
        for f in "${iron_files[@]}"; do
            printf -- '-- @file: %s\n' "$(basename "${f}")" >> "${combined}"
            cat "${f}" >> "${combined}"
            printf '\n' >> "${combined}"
        done

        echo -n "[RUN ] multi_file/${case_name} ... "

        build_stderr="${build_dir}/build.err"
        if ! (cd "${build_dir}" && "${IRON_BIN}" build "${combined}") 2>"${build_stderr}"; then
            echo "[FAIL] (build failed)"
            cat "${build_stderr}" >&2
            FAIL=$((FAIL + 1))
            continue
        fi

        output_bin="${build_dir}/${case_name}"
        if [ ! -x "${output_bin}" ]; then
            echo "[FAIL] (binary not found at ${output_bin})"
            FAIL=$((FAIL + 1))
            continue
        fi

        expected_file="${case_dir}expected"
        if [ ! -f "${expected_file}" ]; then
            # No `expected` sibling: compile-only multi-file fixture.
            echo "[PASS] (compile-only)"
            PASS=$((PASS + 1))
            continue
        fi

        actual=$("${output_bin}" 2>&1) || true
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
fi

# Phase 94 LIB-03 lib_consume fixtures: directory-shaped fixtures under
# tests/integration/lib_consume/ with two sibling subdirs: a `mylib`
# (type="lib") and a `consumer` (type="bin") that path-deps on the lib via
# `[dependencies] mylib = { path = "../mylib" }`. The runner builds the
# consumer (which recursively builds the lib via the path-dep resolver),
# runs the produced binary, compares stdout to the consumer's `expected`
# file, and then asserts TEST-03's three archive checks:
#   1. nm <archive> shows the lib's pub symbol (substring grep, platform-
#      agnostic; macOS prepends `_` and `_` is a word character so a `\b`
#      anchor would silently fail; see Plan 94-01 deviation #4).
#   2. ar t <archive> lists exactly one .o member (filtered on `\.o$` to
#      skip the macOS BSD `__.SYMDEF SORTED` phantom entry).
#   3. Consumer binary stdout matches the locked `expected` file.
#
# PATH is exported with the iron binary's directory at the front so the
# resolver's `execlp("iron","iron","build")` recursive invocation finds
# the build-tree iron rather than a stale ~/.iron/bin/iron from a prior
# system install.
if [ "${CATEGORY}" = "integration" ] && [ -d "${TEST_DIR}/lib_consume" ]; then
    IRON_BIN_DIR="$(dirname "${IRON_BIN}")"
    for case_dir in "${TEST_DIR}/lib_consume"/*/; do
        [ -f "${case_dir}iron.toml" ] || continue

        # Skip lib-only dirs: they're consumed transitively by their
        # paired consumer dir and should not run as standalone integration
        # tests.
        if grep -qE 'type *= *"lib"' "${case_dir}iron.toml"; then continue; fi

        case_name="$(basename "${case_dir%/}")"
        TOTAL=$((TOTAL + 1))
        echo -n "[RUN ] lib_consume/${case_name} ... "

        consumer_abs="$(cd "${case_dir}" && pwd)"
        # Pull the path-dep relative path out of the consumer's iron.toml.
        lib_dir_rel="$(grep -oE 'path *= *"[^"]+"' "${case_dir}iron.toml" | head -1 \
                       | sed -E 's/path *= *"([^"]+)"/\1/')"
        if [ -z "${lib_dir_rel}" ]; then
            echo "[FAIL] (no path-dep in iron.toml)"
            FAIL=$((FAIL + 1))
            continue
        fi
        lib_abs="$(cd "${consumer_abs}/${lib_dir_rel}" 2>/dev/null && pwd || true)"
        if [ -z "${lib_abs}" ]; then
            echo "[FAIL] (path-dep ${lib_dir_rel} does not resolve)"
            FAIL=$((FAIL + 1))
            continue
        fi

        # Clean prior artifacts so the freshness check in the resolver
        # exercises the recursive build path on every run.
        rm -rf "${consumer_abs}/target" "${lib_abs}/target"

        build_log="${WORK_DIR}/lib_consume_${case_name}_build.log"
        set +e
        (cd "${consumer_abs}" && PATH="${IRON_BIN_DIR}:${PATH}" "${IRON_BIN}" build) \
            > "${build_log}" 2>&1
        build_rc=$?
        set -e
        if [ "${build_rc}" -ne 0 ]; then
            echo "[FAIL] (build exit ${build_rc})"
            cat "${build_log}" >&2
            FAIL=$((FAIL + 1))
            continue
        fi

        # Resolve the lib name + consumer name from their iron.tomls. The
        # archive lives at <lib>/target/lib<name>.a; the binary at
        # <consumer>/target/<name>.
        lib_name=$(grep -oE 'name *= *"[^"]+"' "${lib_abs}/iron.toml" | head -1 \
                   | sed -E 's/name *= *"([^"]+)"/\1/')
        consumer_name=$(grep -oE 'name *= *"[^"]+"' "${case_dir}iron.toml" | head -1 \
                        | sed -E 's/name *= *"([^"]+)"/\1/')
        archive="${lib_abs}/target/lib${lib_name}.a"
        binary="${consumer_abs}/target/${consumer_name}"

        # TEST-03 check 1: archive exists + nm shows pub symbol. Substring
        # grep covers either `greet` (lib_consume fixture) or `hello`
        # (any future fixture using the iron init --lib template).
        if [ ! -f "${archive}" ]; then
            echo "[FAIL] (archive ${archive} not produced)"
            FAIL=$((FAIL + 1))
            continue
        fi
        if ! nm "${archive}" 2>/dev/null | grep -qE 'greet|hello'; then
            echo "[FAIL] (nm ${archive} missing pub symbol)"
            FAIL=$((FAIL + 1))
            continue
        fi

        # TEST-03 check 2: ar t lists exactly one .o member.
        member_count=$(ar t "${archive}" | grep -c '\.o$' || true)
        if [ "${member_count}" != "1" ]; then
            echo "[FAIL] (expected 1 .o member, got ${member_count})"
            ar t "${archive}" >&2
            FAIL=$((FAIL + 1))
            continue
        fi

        # TEST-03 check 3: consumer binary stdout matches expected.
        if [ ! -x "${binary}" ]; then
            echo "[FAIL] (consumer binary ${binary} not produced)"
            FAIL=$((FAIL + 1))
            continue
        fi
        actual="$("${binary}" 2>&1 || true)"
        expected_file="${case_dir}expected"
        if [ -f "${expected_file}" ]; then
            expected="$(cat "${expected_file}")"
            expected="${expected%$'\n'}"
            if [ "${actual}" != "${expected}" ]; then
                echo "[FAIL] (stdout mismatch)"
                echo "  Expected: ${expected}"
                echo "  Actual:   ${actual}"
                FAIL=$((FAIL + 1))
                continue
            fi
        fi

        # Clean up artifacts after success so the next run exercises the
        # full pipeline (resolver freshness check + recursive build).
        rm -rf "${consumer_abs}/target" "${lib_abs}/target"
        echo "[PASS]"
        PASS=$((PASS + 1))
    done
fi

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

# Phase 95 PIN: directory-shaped iron.toml-rooted fixtures under
# tests/integration/pin_*/. Three cases lock the satisfied / mismatch /
# absent paths of the version-pin check (PIN-01..04 + TEST-04).
#
#   pin_satisfied/  iron = ">= 0.1.0" -> build + run + stdout-compare
#   pin_mismatch/   iron = ">= 99.0.0" -> iron build MUST exit non-zero;
#                   stderr MUST contain the substring in
#                   expected_stderr_substring (TEST-04 lock); target/
#                   MUST NOT have been created (fail-fast invariant).
#   pin_absent/     no iron field -> build + run + stdout-compare
if [ "${CATEGORY}" = "integration" ]; then
    for case_dir in "${TEST_DIR}"/pin_*/; do
        [ -d "${case_dir}" ] || continue
        [ -f "${case_dir}iron.toml" ] || continue
        case_name="$(basename "${case_dir%/}")"
        TOTAL=$((TOTAL + 1))
        echo -n "[RUN ] ${case_name} ... "

        build_log="${WORK_DIR}/${case_name}_build.log"
        set +e
        (cd "${case_dir}" && rm -rf target && "${IRON_BIN}" build) > "${build_log}" 2>&1
        build_rc=$?
        set -e

        if [ "${case_name}" = "pin_mismatch" ]; then
            if [ "${build_rc}" -eq 0 ]; then
                echo "[FAIL] (expected build to fail; exit 0)"
                cat "${build_log}" >&2
                FAIL=$((FAIL + 1))
                continue
            fi
            expected_substr=$(cat "${case_dir}expected_stderr_substring")
            if ! grep -qF "${expected_substr}" "${build_log}"; then
                echo "[FAIL] (stderr missing locked substring: ${expected_substr})"
                cat "${build_log}" >&2
                FAIL=$((FAIL + 1))
                continue
            fi
            # Fail-fast invariant: target/ MUST NOT have been created.
            if [ -d "${case_dir}target" ]; then
                echo "[FAIL] (fail-fast violated: target/ created on mismatch)"
                FAIL=$((FAIL + 1))
                continue
            fi
            echo "[PASS]"
            PASS=$((PASS + 1))
            continue
        fi

        # pin_satisfied / pin_absent: build + run + stdout-compare.
        if [ "${build_rc}" -ne 0 ]; then
            echo "[FAIL] (build exited ${build_rc})"
            cat "${build_log}" >&2
            FAIL=$((FAIL + 1))
            continue
        fi
        bin="${case_dir}target/${case_name}"
        if [ ! -x "${bin}" ]; then
            echo "[FAIL] (binary not produced at ${bin})"
            FAIL=$((FAIL + 1))
            continue
        fi
        actual=$("${bin}" 2>&1) || true
        expected=$(cat "${case_dir}expected")
        expected="${expected%$'\n'}"
        if [ "${actual}" = "${expected}" ]; then
            # Clean up so the next run exercises a fresh build.
            rm -rf "${case_dir}target"
            echo "[PASS]"
            PASS=$((PASS + 1))
        else
            echo "[FAIL]"
            echo "  Expected: ${expected}"
            echo "  Actual:   ${actual}"
            FAIL=$((FAIL + 1))
        fi
    done
fi

# Phase 94 LIB-03 lib_init_smoke: end-to-end iron init --lib + iron build
# + nm + ar t + iron run rejection check. Mirrors the Phase 92 install-smoke
# pattern (.github/workflows/install-smoke.yml). Wired into the integration
# suite so the full TEST-03 surface (init template + archive emit + run
# rejection) is exercised on every run, not only in CI.
if [ "${CATEGORY}" = "integration" ] && [ -x "${TEST_DIR}/lib_init_smoke.sh" ]; then
    TOTAL=$((TOTAL + 1))
    echo -n "[RUN ] lib_init_smoke ... "
    smoke_log="${WORK_DIR}/lib_init_smoke.log"
    set +e
    "${TEST_DIR}/lib_init_smoke.sh" "${IRON_BIN}" > "${smoke_log}" 2>&1
    smoke_rc=$?
    set -e
    if [ "${smoke_rc}" -eq 0 ] && grep -q 'lib_init_smoke OK' "${smoke_log}"; then
        echo "[PASS]"
        PASS=$((PASS + 1))
    else
        echo "[FAIL] (exit ${smoke_rc})"
        cat "${smoke_log}" >&2
        FAIL=$((FAIL + 1))
    fi
fi

# Phase 96 TEST-06 run_cwd_clean_smoke: assert iron run produces no
# stray binaries in cwd or the package root for both the direct-source
# (RUN-01) and inside-package (RUN-02) paths. Mirrors the Phase 94
# lib_init_smoke handler shape.
if [ "${CATEGORY}" = "integration" ] && [ -x "${TEST_DIR}/run_cwd_clean_smoke.sh" ]; then
    TOTAL=$((TOTAL + 1))
    echo -n "[RUN ] run_cwd_clean_smoke ... "
    smoke_log="${WORK_DIR}/run_cwd_clean_smoke.log"
    set +e
    "${TEST_DIR}/run_cwd_clean_smoke.sh" "${IRON_BIN}" > "${smoke_log}" 2>&1
    smoke_rc=$?
    set -e
    if [ "${smoke_rc}" -eq 0 ] && grep -q 'run_cwd_clean_smoke OK' "${smoke_log}"; then
        echo "[PASS]"
        PASS=$((PASS + 1))
    else
        echo "[FAIL] (exit ${smoke_rc})"
        cat "${smoke_log}" >&2
        FAIL=$((FAIL + 1))
    fi
fi

echo ""
echo "Results: ${PASS} passed, ${FAIL} failed, ${TOTAL} total"

if [ "${FAIL}" -gt 0 ]; then
    exit 1
fi
