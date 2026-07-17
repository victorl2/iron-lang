#!/usr/bin/env bash
# Phase 35 Plan 35-01 Task 3 (GRM-11). Tree-sitter v4-rule coverage runner.
# Wraps `tree-sitter test` and asserts the v4_* corpus fixtures (added by
# Plan 35-01) are present in the suite AND every one of them passes. This
# is a structural gate that catches:
#   1. someone deleting v4_*.txt fixtures (count drops below 7)
#   2. a grammar.js.in change breaking weak_rc_expression, pointer_type,
#      bounded_vector_type, arena_block, drop_block, copy_block,
#      nocopy_modifier, or leak_statement parse trees
#
# Sibling to run_corpus.sh — that script runs the whole corpus; this one
# narrows the failure to v4 surface for faster diagnosis when GRM-06..11
# regressions land.
#
# Exit codes:
#   0   — all 7 v4_* fixture files present and every v4_* test green
#   1   — at least one v4_* test failed, or fewer than 7 v4_* files present
#   77  — tree-sitter-cli not available (CTest SKIP code)
set -euo pipefail

REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)
GRAMMAR_DIR="$REPO/grammars/tree-sitter/iron"
CORPUS_DIR="$GRAMMAR_DIR/test/corpus"

if [ -n "${TREE_SITTER:-}" ]; then
    TS="$TREE_SITTER"
elif command -v tree-sitter >/dev/null 2>&1; then
    TS=$(command -v tree-sitter)
elif [ -x "$GRAMMAR_DIR/node_modules/.bin/tree-sitter" ]; then
    TS="$GRAMMAR_DIR/node_modules/.bin/tree-sitter"
else
    echo "iron-lsp: tree-sitter-cli not found. Run 'npm install' in $GRAMMAR_DIR." >&2
    exit 77
fi

# Step 1: confirm all 7 v4_* fixture files exist (plan's <files_modified>).
EXPECTED_V4_FILES=(
    v4_pointers.txt
    v4_bounded_vector.txt
    v4_weak_rc.txt
    v4_arena.txt
    v4_drop_copy.txt
    v4_leak.txt
    v4_nocopy.txt
)
MISSING=0
for f in "${EXPECTED_V4_FILES[@]}"; do
    if [ ! -f "$CORPUS_DIR/$f" ]; then
        echo "FAIL: missing v4 corpus fixture: $CORPUS_DIR/$f" >&2
        MISSING=$((MISSING + 1))
    fi
done
if [ $MISSING -gt 0 ]; then
    echo "FAIL: $MISSING required v4_* corpus fixture(s) missing" >&2
    exit 1
fi

# Step 2: run the full corpus (tree-sitter test does not have a per-file
# selector that handles our naming convention robustly). Capture output and
# scan for v4_* test failures.
cd "$GRAMMAR_DIR"
if [ ! -f "src/parser.c" ]; then
    "$TS" generate >/dev/null
fi

LOG=$(mktemp)
trap 'rm -f "$LOG"' EXIT

if ! "$TS" test 2>&1 | tee "$LOG" >/dev/null; then
    # tree-sitter test exit nonzero — show output and fail.
    cat "$LOG" >&2
    echo "FAIL: tree-sitter test exited nonzero" >&2
    exit 1
fi

# Step 3: confirm at least one v4_* test fired (the group header lines
# `  v4_pointers:` etc. appear in tree-sitter test output).
V4_GROUPS=$(grep -cE '^[[:space:]]*v4_(pointers|bounded_vector|weak_rc|arena|drop_copy|leak|nocopy):' "$LOG" || true)
if [ "$V4_GROUPS" -lt 7 ]; then
    cat "$LOG" >&2
    echo "FAIL: expected >=7 v4_* test groups in tree-sitter test output, saw $V4_GROUPS" >&2
    exit 1
fi

# Step 4: confirm no failures appear under any v4_* group. The tree-sitter
# test output marks failures with the literal substring "failed" or a red ✗.
# Scan for the canonical "failed parses: N" tail and assert N == 0.
if grep -qE '^[[:space:]]*[0-9]+\.[[:space:]]+✗[[:space:]]' "$LOG"; then
    cat "$LOG" >&2
    echo "FAIL: at least one corpus test failed (✗ marker present in output)" >&2
    exit 1
fi

echo "PASS: $V4_GROUPS v4_* test group(s) all green"
exit 0
