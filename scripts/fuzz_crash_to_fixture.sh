#!/usr/bin/env bash
# scripts/fuzz_crash_to_fixture.sh — FUZZ-06 crash-to-fixture harness.
#
# Called by .github/workflows/fuzz.yml after any libFuzzer run that
# exits non-zero. Also invokable locally for developer reproduction:
#
#   scripts/fuzz_crash_to_fixture.sh \
#     --crash-dir crashes/ \
#     --target parser \
#     --run-id local \
#     --sha "$(git rev-parse HEAD)" \
#     [--open-issue]
#
# Responsibilities:
#   1. For every crash-* file in --crash-dir, run the fuzz target in
#      minimize mode (`-minimize_crash=1 -runs=10000`) to produce a
#      minimal reproducer.
#   2. Replay the minimized input once (-runs=1) to capture the
#      canonical crashing stderr. This becomes the .expected file
#      for the regression fixture.
#   3. Compute a crash signature from the top 3 stack frames of the
#      replayed stderr (sha1 -> first 8 hex chars). Used for issue
#      dedup + fixture naming context.
#   4. (--open-issue only) `gh issue list` for the signature; if no
#      match, `gh issue create` with label fuzz-crash; if match,
#      `gh issue comment` on the existing issue.
#   5. For the parser target ONLY: write the minimized input as
#      tests/integration/fuzz_crash_NNN.iron with the 4-section
#      doc-comment stub (Motivating Incident / Symptom / Protection
#      / Remediation Path TODO). Also write the captured stderr as
#      tests/integration/fuzz_crash_NNN.expected. NNN is the next
#      free 3-digit zero-padded integer. A flock guards concurrent
#      invocations (CI races).
#   6. For typecheck/hir_to_lir targets: the minimized input is a
#      binary blob; run_integration.sh can't run blobs, so we do
#      NOT emit a .iron fixture for those targets. The minimized
#      input is archived as an upload-artifact blob (handled by
#      the workflow's upload-artifact step) and an issue is opened
#      pointing at that artifact.
#
# Exit policy:
#   - Missing required args -> exit 2
#   - Fuzz binary not found -> exit 1
#   - Empty --crash-dir (no crash-* files) -> print "no crashes" and exit 0
#   - Individual minimize/replay failures -> warn and continue (don't
#     let one bad crash file abort the whole batch)
#
# Closed `fuzz-crash` issues are NEVER reopened automatically — a
# resurfaced crash after a close is a human decision (CONTEXT.md).

set -euo pipefail

CRASH_DIR=""
TARGET=""
RUN_ID=""
SHA=""
OPEN_ISSUE=0

usage() {
    cat >&2 <<EOF
usage: $0 --crash-dir DIR --target NAME --run-id ID --sha SHA [--open-issue]
  --crash-dir DIR   directory containing libFuzzer crash-* files
  --target NAME     one of: parser | typecheck | hir_to_lir
  --run-id ID       github.run_id or 'local' for manual runs
  --sha SHA         github.sha or local HEAD
  --open-issue      also open/dedup a fuzz-crash gh issue
EOF
    exit 2
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --crash-dir)  CRASH_DIR="${2:-}"; shift 2;;
        --target)     TARGET="${2:-}";    shift 2;;
        --run-id)     RUN_ID="${2:-}";    shift 2;;
        --sha)        SHA="${2:-}";       shift 2;;
        --open-issue) OPEN_ISSUE=1;       shift;;
        -h|--help)    usage;;
        *) echo "unknown arg: $1" >&2; usage;;
    esac
done

[[ -n "$CRASH_DIR" && -n "$TARGET" && -n "$RUN_ID" && -n "$SHA" ]] || usage
case "$TARGET" in
    parser|typecheck|hir_to_lir) ;;
    *) echo "invalid --target: $TARGET (must be parser|typecheck|hir_to_lir)" >&2; exit 2;;
esac

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
INTEG_DIR="${REPO_ROOT}/tests/integration"
LOCK_FILE="${INTEG_DIR}/.fuzz_crash_lock"
FUZZ_BIN="${REPO_ROOT}/build/tests/fuzz/fuzz_${TARGET}"

# Empty crash-dir is the common clean-nightly-run case; short-circuit before
# we need the fuzz binary so the harness is also safe to dry-run locally
# without an IRON_ENABLE_FUZZING build present.
shopt -s nullglob
crashes=("${CRASH_DIR}"/crash-*)
if [[ ${#crashes[@]} -eq 0 ]]; then
    echo "no crashes to process"
    exit 0
fi

if [[ ! -x "${FUZZ_BIN}" ]]; then
    echo "fuzz binary not found: ${FUZZ_BIN}" >&2
    exit 1
fi

# Top-3-frame signature extractor. libFuzzer + ASan stack traces look like:
#   #0 0x... in <symbol> /path/to/file.c:LINE
# Awk slices the first 3 lines matching that pattern, then sha1 -> 8 chars.
extract_signature() {
    local stderr_file="$1"
    awk '/^[[:space:]]*#[0-9]+/ { print $0; n++ } n>=3 { exit }' "${stderr_file}" \
        | sha1sum | cut -c1-8
}

next_fixture_num() {
    local max=0 f n
    for f in "${INTEG_DIR}"/fuzz_crash_*.iron; do
        [[ -e "$f" ]] || continue
        n="${f##*fuzz_crash_}"
        n="${n%.iron}"
        # Strip leading zeros for arithmetic (leading zero makes bash treat as octal).
        n="$(printf '%d' "$((10#$n))")"
        (( n > max )) && max=$n
    done
    printf '%03d' $((max + 1))
}

mkdir -p "${INTEG_DIR}"
: > "${LOCK_FILE}"   # ensure lockfile exists
exec 9>"${LOCK_FILE}"
flock 9

for crash in "${crashes[@]}"; do
    echo "processing ${crash}"

    # Step 1: minimize. libFuzzer writes min-* next to crash-*.
    "${FUZZ_BIN}" \
        -minimize_crash=1 \
        -runs=10000 \
        -artifact_prefix="${CRASH_DIR}/min-" \
        "${crash}" \
        2>"${CRASH_DIR}/$(basename "${crash}").min.stderr" \
        || true
    minimized="$(ls "${CRASH_DIR}"/min-* 2>/dev/null | head -n1 || true)"
    if [[ -z "${minimized}" ]]; then
        minimized="${crash}"
    fi

    # Step 2: replay minimized input once to capture crashing stderr.
    replay_stderr="${CRASH_DIR}/$(basename "${crash}").replay.stderr"
    "${FUZZ_BIN}" -runs=1 "${minimized}" 2>"${replay_stderr}" || true

    # Step 3: compute signature.
    sig="$(extract_signature "${replay_stderr}")"
    if [[ -z "${sig}" ]]; then
        sig="nosig"   # fall back if no stack frames were printed
    fi
    echo "  signature: ${sig}"

    # Step 4: issue dedup (opt-in).
    if [[ "${OPEN_ISSUE}" -eq 1 ]]; then
        existing_count="$(gh issue list --label fuzz-crash --state open \
                            --search "fuzz crash ${sig} in:title" \
                            --json number --jq 'length' 2>/dev/null || echo 0)"
        if [[ "${existing_count}" -eq 0 ]]; then
            top3="$(awk '/^[[:space:]]*#[0-9]+/ {print; n++} n>=3 {exit}' "${replay_stderr}")"
            input_hash="$(sha1sum "${minimized}" | cut -c1-12)"
            body=$(printf 'libFuzzer nightly run at %s produced a crash in fuzz_%s.\n\n- **Seed:** 1\n- **Input hash:** %s\n- **Run ID:** %s\n- **Signature:** %s\n\n## Top 3 stack frames\n\n```\n%s\n```\n\nSee artifact `fuzz-crashes-%s-%s` on the workflow run for the full reproducer.\n' \
                "${SHA}" "${TARGET}" "${input_hash}" "${RUN_ID}" "${sig}" "${top3}" "${TARGET}" "${RUN_ID}")
            gh issue create \
                --label fuzz-crash \
                --title "Fuzz crash: ${sig} in fuzz_${TARGET}" \
                --body "${body}" || echo "  warn: gh issue create failed" >&2
        else
            num="$(gh issue list --label fuzz-crash --state open \
                     --search "fuzz crash ${sig} in:title" \
                     --json number --jq '.[0].number' 2>/dev/null || echo "")"
            if [[ -n "${num}" ]]; then
                gh issue comment "${num}" \
                    --body "Resurfaced in run ${RUN_ID} at ${SHA}." \
                    || echo "  warn: gh issue comment failed" >&2
            fi
        fi
    fi

    # Step 5: parser target only — wrap as tests/integration/fuzz_crash_NNN.iron.
    if [[ "${TARGET}" == "parser" ]]; then
        nnn="$(next_fixture_num)"
        fixture="${INTEG_DIR}/fuzz_crash_${nnn}.iron"
        expected="${INTEG_DIR}/fuzz_crash_${nnn}.expected"

        # Build the 4-section doc-comment stub.
        input_hash="$(sha1sum "${minimized}" | cut -c1-12)"
        top5="$(awk '/^[[:space:]]*#[0-9]+/ {print "-- "$0; n++} n>=5 {exit}' "${replay_stderr}")"

        {
            echo "-- Regression: libFuzzer nightly run discovered a crash in iron_${TARGET}."
            echo "--"
            echo "-- **Motivating Incident.** libFuzzer nightly run at commit ${SHA}"
            echo "-- produced this crash (target=${TARGET}, seed=1, signature=${sig},"
            echo "-- input-hash=${input_hash}, run=${RUN_ID})."
            echo "--"
            echo "-- **Symptom.** Top frames from the crash stack trace:"
            if [[ -n "${top5}" ]]; then
                echo "${top5}"
            else
                echo "-- (no stack frames captured — see full stderr in the workflow artifact)"
            fi
            echo "--"
            echo "-- **Protection.** Fixture pins the minimized input verbatim;"
            echo "-- tests/integration/run_integration.sh exits non-zero on regression"
            echo "-- (Phase 66 REG-01 crash-swallow prevention). See the phase 66 Linux"
            echo "-- Release CI job for the upstream guard."
            echo "--"
            echo "-- **Remediation Path.** TODO: fill in after the fix lands."
            echo ""
            cat "${minimized}"
        } > "${fixture}"

        cp "${replay_stderr}" "${expected}"

        echo "  wrote ${fixture}"
        echo "  wrote ${expected}"
    else
        echo "  non-parser target — minimized input archived as artifact only"
    fi
done
