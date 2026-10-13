#!/usr/bin/env bash
# Check every Iron fence in the user-facing Markdown documentation.
#
# A document may opt into compile-only checking with a file-level marker:
#   <!-- doctest-build-only: <reason> -->
# Individual fences that cannot be compiled use the existing immediately
# preceding `doctest-skip` marker understood by test_doc_examples.sh.

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
IRON_BIN_ARG="${1:-${ROOT}/build/iron}"
IRON_BIN="$(cd "$(dirname "${IRON_BIN_ARG}")" && pwd)/$(basename "${IRON_BIN_ARG}")"

cd "${ROOT}"

files=(README.md docs/*.md docs/llms/*.md)
checked_files=0
checked_blocks=0

for file in "${files[@]}"; do
    [ -f "${file}" ] || continue
    blocks="$(grep -Ec '^```iron[[:space:]]*$' "${file}" || true)"
    [ "${blocks}" -gt 0 ] || continue

    build_only=0
    if grep -Eq '^<!--[[:space:]]*doctest-build-only:[[:space:]].+-->[[:space:]]*$' "${file}"; then
        build_only=1
    fi

    echo "=== ${file} (${blocks} Iron blocks) ==="
    DOCTEST_BUILD_ONLY="${build_only}" MD_FILE="${file}" \
        bash scripts/test_doc_examples.sh "${IRON_BIN}"
    checked_files=$((checked_files + 1))
    checked_blocks=$((checked_blocks + blocks))
done

echo "check_doc_examples OK: files=${checked_files} blocks=${checked_blocks}"
