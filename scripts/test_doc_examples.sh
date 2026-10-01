#!/usr/bin/env bash
# Build and run every fenced ```iron block of a markdown file.
#
# For each ```iron block:
#   1. the body is written to a temp .iron file (blocks without a
#      `func main()` are wrapped in one, see MAIN_RE below);
#   2. the file is compiled with `iron build`;
#   3. the binary is run with a timeout; a non-zero exit fails the block;
#   4. if a ```output block follows the ```iron block (only blank lines
#      between them), the program's stdout must match it exactly.
#
# Directives on the line IMMEDIATELY before a ```iron fence:
#   <!-- doctest-skip: <reason> -->          skip the block (logged as SKIP)
#   <!-- doctest-expect-error: E0123 -->     the block must FAIL to compile
#                                            and the compiler output must
#                                            mention the given code
#
# Every fenced code block in the file must carry a language tag; an
# untagged ``` fence fails the test so new examples cannot bypass it.
#
# Usage: test_doc_examples.sh [IRON_BIN]
#   IRON_BIN: path to the iron binary (default: ./build/iron).
#   MD_FILE=path/to/other.md overrides the markdown file
#   (default: docs/language_definition.md).
#   DOCTEST_TIMEOUT=seconds caps each program run (default: 60).
#
# Statement-block wrapping heuristic: a block that declares `func main()`
# is compiled verbatim; any other block is wrapped in
# `func main() {\n<body>\n}` so a few bare statements can be shown.
#
# Final marker on success: `test_doc_examples OK`.

set -euo pipefail

IRON_BIN_ARG="${1:-./build/iron}"
IRON_BIN="$(cd "$(dirname "${IRON_BIN_ARG}")" && pwd)/$(basename "${IRON_BIN_ARG}")"

[ -x "${IRON_BIN}" ] || { echo "FAIL: iron not executable: ${IRON_BIN}" >&2; exit 1; }

MD_FILE="${MD_FILE:-docs/language_definition.md}"
[ -r "${MD_FILE}" ] || { echo "FAIL: cannot read MD_FILE: ${MD_FILE}" >&2; exit 1; }
DOCTEST_TIMEOUT="${DOCTEST_TIMEOUT:-60}"

WORK="$(mktemp -d -t iron-doctest-XXXXXX)"
trap 'rm -rf "${WORK}"' EXIT
mkdir -p "${WORK}/blocks"

# One awk pass over MD_FILE. For every fence it emits:
#   block_<N>.body    the raw body of a ```iron block
#   block_<N>.meta    "<start> <end> <mode> <arg>" where mode is one of
#                     run | skip | error, and arg is the skip reason or the
#                     expected error code
#   block_<N>.out     the body of the ```output block that follows block N
#   _untagged         start lines of fences without a language tag
#   _error            UNCLOSED_FENCE <line>
awk -v workdir="${WORK}/blocks" '
    function flush_directive() { pending_mode = ""; pending_arg = "" }
    BEGIN {
        in_block = 0; block_num = 0; kind = ""
        pending_mode = ""; pending_arg = ""
        last_iron_block = 0; last_iron_end = 0
    }
    {
        line = $0
        trimmed = line
        sub(/[[:space:]]+$/, "", trimmed)

        if (in_block == 0) {
            if (trimmed ~ /^```/) {
                tag = trimmed
                sub(/^```/, "", tag)
                if (tag == "") {
                    print NR >> (workdir "/_untagged")
                    kind = "other"
                } else if (tag == "iron") {
                    kind = "iron"
                    block_num++
                    start_lineno = NR
                    body_path = workdir "/block_" block_num ".body"
                    printf "" > body_path
                    mode = (pending_mode == "") ? "run" : pending_mode
                    arg = pending_arg
                } else if (tag == "output") {
                    # An output block belongs to the iron block that ended
                    # just before it (blank lines only in between).
                    kind = "output"
                    if (last_iron_block > 0 && only_blank_since_iron) {
                        out_path = workdir "/block_" last_iron_block ".out"
                        printf "" > out_path
                    } else {
                        print "STRAY_OUTPUT " NR >> (workdir "/_error")
                        out_path = ""
                    }
                } else {
                    kind = "other"
                }
                in_block = 1
                flush_directive()
                next
            }
            if (trimmed == "") {
                # A blank line keeps a pending directive only if it is
                # directly followed by the fence; directives must be on the
                # line immediately before the fence, so drop it here.
                flush_directive()
                next
            }
            only_blank_since_iron = 0
            if (match(trimmed, /^<!--[[:space:]]*doctest-skip:[[:space:]]*.*-->[[:space:]]*$/)) {
                reason = trimmed
                sub(/^<!--[[:space:]]*doctest-skip:[[:space:]]*/, "", reason)
                sub(/[[:space:]]*-->[[:space:]]*$/, "", reason)
                pending_mode = "skip"; pending_arg = reason
            } else if (match(trimmed, /^<!--[[:space:]]*doctest-expect-error:[[:space:]]*[EW][0-9]+[[:space:]]*-->[[:space:]]*$/)) {
                code = trimmed
                sub(/^<!--[[:space:]]*doctest-expect-error:[[:space:]]*/, "", code)
                sub(/[[:space:]]*-->[[:space:]]*$/, "", code)
                pending_mode = "error"; pending_arg = code
            } else {
                flush_directive()
            }
            next
        }

        # Inside a fence: close on a bare ``` line.
        if (trimmed == "```") {
            if (kind == "iron") {
                meta_path = workdir "/block_" block_num ".meta"
                print start_lineno " " NR " " mode " " arg > meta_path
                close(meta_path)
                close(body_path)
                last_iron_block = block_num
                only_blank_since_iron = 1
            } else if (kind == "output") {
                if (out_path != "") close(out_path)
                only_blank_since_iron = 0
            } else {
                only_blank_since_iron = 0
            }
            in_block = 0
            kind = ""
            next
        }
        if (kind == "iron") print line >> body_path
        else if (kind == "output" && out_path != "") print line >> out_path
    }
    END {
        if (in_block) print "UNCLOSED_FENCE " start_lineno >> (workdir "/_error")
        print block_num > (workdir "/_count")
    }
' "${MD_FILE}"

FAIL=0

if [ -f "${WORK}/blocks/_error" ]; then
    while read -r err; do
        echo "FAIL: ${MD_FILE}: ${err}" >&2
    done < "${WORK}/blocks/_error"
    FAIL=1
fi

if [ -f "${WORK}/blocks/_untagged" ]; then
    while read -r lineno; do
        echo "FAIL: ${MD_FILE}:${lineno}: code fence without a language tag (use iron, output, toml, sh, text or ebnf)" >&2
    done < "${WORK}/blocks/_untagged"
    FAIL=1
fi

TOTAL="$(cat "${WORK}/blocks/_count")"
PASS=0
SKIP=0

# A block that declares `func main()` is a complete program; anything else
# is wrapped in a main function.
MAIN_RE='^[[:space:]]*(pub[[:space:]]+)?func[[:space:]]+main[[:space:]]*\('

# Run a command with a timeout, portably (macOS has no `timeout`).
run_with_timeout() {
    local secs="$1"; shift
    "$@" &
    local pid=$!
    local waited=0
    while kill -0 "${pid}" 2>/dev/null; do
        if [ "${waited}" -ge "${secs}" ]; then
            kill -9 "${pid}" 2>/dev/null || true
            wait "${pid}" 2>/dev/null || true
            return 124
        fi
        sleep 1
        waited=$((waited + 1))
    done
    wait "${pid}"
}

i=0
while [ "$i" -lt "$TOTAL" ]; do
    i=$((i + 1))
    body_path="${WORK}/blocks/block_${i}.body"
    meta_path="${WORK}/blocks/block_${i}.meta"
    out_path="${WORK}/blocks/block_${i}.out"

    meta="$(cat "${meta_path}")"
    start_lineno="$(echo "${meta}" | awk '{print $1}')"
    end_lineno="$(echo "${meta}" | awk '{print $2}')"
    mode="$(echo "${meta}" | awk '{print $3}')"
    arg="$(echo "${meta}" | cut -d' ' -f4-)"
    where="lines ${start_lineno}-${end_lineno}"

    if [ "${mode}" = "skip" ]; then
        echo "SKIP ${where}: ${arg}"
        SKIP=$((SKIP + 1))
        continue
    fi

    src_path="${WORK}/blocks/block_${i}.iron"
    if grep -Eq "${MAIN_RE}" "${body_path}"; then
        cp "${body_path}" "${src_path}"
    else
        { echo "func main() {"; cat "${body_path}"; echo "}"; } > "${src_path}"
    fi

    bin_path="${WORK}/blocks/block_${i}.bin"
    build_log="${WORK}/blocks/block_${i}.build"
    set +e
    (cd "${WORK}/blocks" && "${IRON_BIN}" build "${src_path}" -o "${bin_path}") > "${build_log}" 2>&1
    rc=$?
    set -e

    if [ "${mode}" = "error" ]; then
        if [ "${rc}" -ne 0 ] && grep -Fq "${arg}" "${build_log}"; then
            echo "PASS ${where} (rejected with ${arg})"
            PASS=$((PASS + 1))
        else
            echo "FAIL ${where}: expected compile error ${arg}"
            echo "--- source ---"; cat "${src_path}"
            echo "--- iron build output (exit ${rc}) ---"; cat "${build_log}"
            echo "--- end of failure ${where} ---"
            FAIL=$((FAIL + 1))
        fi
        continue
    fi

    if [ "${rc}" -ne 0 ]; then
        echo "FAIL ${where}: iron build failed"
        echo "--- source ---"; cat "${src_path}"
        echo "--- iron build output ---"; cat "${build_log}"
        echo "--- end of failure ${where} ---"
        FAIL=$((FAIL + 1))
        continue
    fi

    stdout_path="${WORK}/blocks/block_${i}.stdout"
    stderr_path="${WORK}/blocks/block_${i}.stderr"
    set +e
    (cd "${WORK}/blocks" && run_with_timeout "${DOCTEST_TIMEOUT}" "${bin_path}" > "${stdout_path}" 2> "${stderr_path}" < /dev/null)
    run_rc=$?
    set -e
    if [ "${run_rc}" -ne 0 ]; then
        if [ "${run_rc}" -eq 124 ]; then
            echo "FAIL ${where}: program timed out after ${DOCTEST_TIMEOUT}s"
        else
            echo "FAIL ${where}: program exited with ${run_rc}"
        fi
        echo "--- source ---"; cat "${src_path}"
        echo "--- stdout ---"; cat "${stdout_path}"
        echo "--- stderr ---"; cat "${stderr_path}"
        echo "--- end of failure ${where} ---"
        FAIL=$((FAIL + 1))
        continue
    fi

    if [ -f "${out_path}" ]; then
        if cmp -s "${stdout_path}" "${out_path}"; then
            echo "PASS ${where} (output matches)"
            PASS=$((PASS + 1))
        else
            echo "FAIL ${where}: stdout differs from the output block"
            echo "--- source ---"; cat "${src_path}"
            echo "--- expected ---"; cat "${out_path}"
            echo "--- actual ---"; cat "${stdout_path}"
            echo "--- diff ---"; diff "${out_path}" "${stdout_path}" || true
            echo "--- end of failure ${where} ---"
            FAIL=$((FAIL + 1))
        fi
    else
        echo "PASS ${where}"
        PASS=$((PASS + 1))
    fi
done

echo ""
echo "Results: total=${TOTAL} pass=${PASS} skip=${SKIP} fail=${FAIL}"

if [ "${FAIL}" -gt 0 ]; then
    exit 1
fi

echo "test_doc_examples OK"
