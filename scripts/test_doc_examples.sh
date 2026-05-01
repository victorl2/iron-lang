#!/usr/bin/env bash
# Phase 99 DOC-01 / DOC-02: extract every fenced ```iron block from
# docs/language_definition.md and feed each one to `iron check`. The
# durable lock against future spec-vs-impl drift in the language
# specification document.
#
# DOC-01: extract every ```iron block, optionally wrap statement-only
#         blocks in func main() { ... }, and run iron check per block.
#         Exit non-zero on any failure.
# DOC-02: recognize <!-- doctest-skip: <reason> --> on the line
#         IMMEDIATELY before a ```iron fence (no blank line between)
#         and skip the block with a SKIP log line.
#
# Usage: test_doc_examples.sh [IRON_BIN]
#   IRON_BIN: path to the iron binary (default: ./build/iron).
#
# Override the doc-test target via env:
#   MD_FILE=path/to/other.md scripts/test_doc_examples.sh
# Default MD_FILE is docs/language_definition.md (the only spec doc
# under doc-test in v3.2; broader markdown coverage is deferred to v3.3+).
#
# Statement-block wrapping heuristic:
#   if the FIRST non-blank line of a block does NOT start with one of
#     func, object, enum, interface, import, pub, patch, --
#   the entire body is wrapped in `func main() {\n<body>\n}` before
#   the temp file is fed to `iron check`. Otherwise the body is used
#   verbatim.
#
# On block failure, the diagnostic block prints THREE pieces for
# reproducibility (locked CONTEXT decision):
#   1. the source markdown line range (e.g., "lines 543-553"),
#   2. the wrapped source actually fed to iron check,
#   3. the iron check stderr.
#
# Final marker on success: `test_doc_examples OK`.

set -euo pipefail

IRON_BIN_ARG="${1:-./build/iron}"
IRON_BIN="$(cd "$(dirname "${IRON_BIN_ARG}")" && pwd)/$(basename "${IRON_BIN_ARG}")"

[ -x "${IRON_BIN}" ] || { echo "FAIL: iron not executable: ${IRON_BIN}" >&2; exit 1; }

MD_FILE="${MD_FILE:-docs/language_definition.md}"
[ -r "${MD_FILE}" ] || { echo "FAIL: cannot read MD_FILE: ${MD_FILE}" >&2; exit 1; }

WORK="$(mktemp -d -t iron-doctest-XXXXXX)"
trap 'rm -rf "${WORK}"' EXIT

# Walk MD_FILE line by line in a single awk pass and emit one
# extracted block per file:
#   ${WORK}/blocks/block_<N>.body  — raw block body
#   ${WORK}/blocks/block_<N>.meta  — "<start_lineno> <end_lineno> <skip_reason_or_empty>"
# The skip-reason field is empty when no <!-- doctest-skip: ... --> comment
# precedes the fence; otherwise it carries the reason string.
mkdir -p "${WORK}/blocks"

awk -v workdir="${WORK}/blocks" '
    BEGIN {
        in_block = 0
        block_num = 0
        # Last non-blank line seen OUTSIDE a fence — used to detect
        # the doctest-skip directive on the line immediately before a
        # ```iron fence open (no blank line between).
        prev_nonblank = ""
        prev_nonblank_was_skip = 0
        skip_reason = ""
    }
    {
        # Trim trailing whitespace for fence detection only; pass body
        # lines through verbatim.
        line = $0
        trimmed = line
        sub(/[[:space:]]+$/, "", trimmed)

        if (in_block == 0) {
            if (trimmed == "```iron") {
                in_block = 1
                start_lineno = NR
                block_num++
                body_path = workdir "/block_" block_num ".body"
                meta_path = workdir "/block_" block_num ".meta"
                # Reset body file for this block.
                printf "" > body_path
                # Carry the skip reason if the immediately preceding
                # non-blank line was a doctest-skip directive.
                if (prev_nonblank_was_skip) {
                    skip_reason = prev_skip_reason
                } else {
                    skip_reason = ""
                }
                next
            }

            # Track previous non-blank line for skip-comment detection.
            # A blank line between the comment and the fence breaks
            # the link (locked CONTEXT decision: must be IMMEDIATELY
            # preceding, no blank line between).
            if (trimmed == "") {
                prev_nonblank = ""
                prev_nonblank_was_skip = 0
                prev_skip_reason = ""
            } else {
                prev_nonblank = trimmed
                # Match: <!-- doctest-skip: <reason> -->
                # Reason is captured greedy-up-to the trailing -->.
                if (match(trimmed, /^<!--[[:space:]]*doctest-skip:[[:space:]]*.*-->[[:space:]]*$/)) {
                    reason = trimmed
                    sub(/^<!--[[:space:]]*doctest-skip:[[:space:]]*/, "", reason)
                    sub(/[[:space:]]*-->[[:space:]]*$/, "", reason)
                    prev_nonblank_was_skip = 1
                    prev_skip_reason = reason
                } else {
                    prev_nonblank_was_skip = 0
                    prev_skip_reason = ""
                }
            }
            next
        }

        # Inside a block: close on a bare ``` line (trimmed match).
        if (trimmed == "```") {
            end_lineno = NR
            print start_lineno " " end_lineno " " skip_reason > meta_path
            close(meta_path)
            close(body_path)
            in_block = 0
            # Reset skip tracking — a fresh skip directive must
            # immediately precede the NEXT fence.
            prev_nonblank = ""
            prev_nonblank_was_skip = 0
            prev_skip_reason = ""
            next
        }
        # Append body line verbatim.
        print line >> body_path
    }
    END {
        if (in_block) {
            # Unclosed fence — report so the dev can find the typo.
            err_path = workdir "/_error"
            print "UNCLOSED_FENCE " start_lineno > err_path
        }
        count_path = workdir "/_count"
        print block_num > count_path
    }
' "${MD_FILE}"

if [ -f "${WORK}/blocks/_error" ]; then
    err="$(cat "${WORK}/blocks/_error")"
    echo "FAIL: ${MD_FILE}: ${err} (fence opened but never closed)" >&2
    exit 1
fi

TOTAL="$(cat "${WORK}/blocks/_count")"
PASS=0
SKIP=0
FAIL=0

# First-non-blank starter regex: statement-block heuristic. If the
# first non-blank body line starts with one of these keywords (or
# Iron's `--` line comment), the body is used verbatim. Otherwise the
# body is wrapped in a `func main() { ... }` harness so bare
# statements compile.
STARTER_RE='^[[:space:]]*(func|object|enum|interface|import|pub|patch|--)'

i=0
while [ "$i" -lt "$TOTAL" ]; do
    i=$((i + 1))
    body_path="${WORK}/blocks/block_${i}.body"
    meta_path="${WORK}/blocks/block_${i}.meta"

    # meta line: "<start> <end> <skip_reason_or_empty>"
    meta="$(cat "${meta_path}")"
    start_lineno="$(echo "${meta}" | awk '{print $1}')"
    end_lineno="$(echo "${meta}" | awk '{print $2}')"
    skip_reason="$(echo "${meta}" | cut -d' ' -f3-)"

    if [ -n "${skip_reason}" ]; then
        echo "SKIP lines ${start_lineno}-${end_lineno}: ${skip_reason}"
        SKIP=$((SKIP + 1))
        continue
    fi

    # Statement-block detection: examine the FIRST non-blank line.
    first_nonblank="$(awk 'NF { print; exit }' "${body_path}" || true)"

    src_path="${WORK}/blocks/block_${i}.iron"
    if echo "${first_nonblank}" | grep -Eq "${STARTER_RE}"; then
        cp "${body_path}" "${src_path}"
    else
        {
            echo "func main() {"
            cat "${body_path}"
            echo "}"
        } > "${src_path}"
    fi

    set +e
    "${IRON_BIN}" check "${src_path}" > "${WORK}/blocks/block_${i}.stderr" 2>&1
    rc=$?
    set -e

    if [ "${rc}" -eq 0 ]; then
        echo "PASS lines ${start_lineno}-${end_lineno}"
        PASS=$((PASS + 1))
    else
        echo "FAIL lines ${start_lineno}-${end_lineno}"
        echo "--- wrapped source (fed to iron check) ---"
        cat "${src_path}"
        echo "--- iron check stderr ---"
        cat "${WORK}/blocks/block_${i}.stderr"
        echo "--- end of failure ${start_lineno}-${end_lineno} ---"
        FAIL=$((FAIL + 1))
    fi
done

echo ""
echo "Results: total=${TOTAL} pass=${PASS} skip=${SKIP} fail=${FAIL}"

if [ "${FAIL}" -gt 0 ]; then
    exit 1
fi

echo "test_doc_examples OK"
