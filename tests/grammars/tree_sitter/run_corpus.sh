#!/usr/bin/env bash
# Phase 6 Plan 06-02 Task 2 (EXT-02). Tree-sitter corpus runner wrapping
# `tree-sitter test` for CTest consumption.
#
# Exit codes:
#   0   — all corpus fixtures produce the expected parse trees
#   1   — at least one fixture diverges
#   77  — tree-sitter-cli not available (CTest SKIP code)
set -euo pipefail

REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)
GRAMMAR_DIR="$REPO/grammars/tree-sitter/iron"

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

cd "$GRAMMAR_DIR"
if [ ! -f "src/parser.c" ]; then
    "$TS" generate >/dev/null
fi
exec "$TS" test
