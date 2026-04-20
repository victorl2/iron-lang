#!/usr/bin/env bash
# Phase 6 Plan 06-02 Task 2 (EXT-02). Integration-corpus tree-sitter parse
# gate. Iterates every tests/integration/*.iron through `tree-sitter parse`
# and fails the build when any file produces ERROR or MISSING nodes.
#
# This is the structural-parity fence between the Iron parser
# (src/parser/parser.c) and the tree-sitter grammar: any new integration
# fixture that uses a syntactic construct the grammar does not cover will
# turn this test red, prompting the developer to either (a) add the rule
# to grammar.js.in + regenerate, or (b) land an explicit
# known-skip entry in this script with a reason and a follow-up tracking ID.
#
# Exit codes:
#   0   — every integration fixture parses with zero ERROR/MISSING nodes
#   1   — at least one fixture failed; stderr lists the offenders
#   77  — tree-sitter-cli not available (CTest SKIP code)
#
# Environment:
#   TREE_SITTER — optional override for the tree-sitter executable.
#                 Otherwise resolved from PATH, then from the grammar
#                 directory's node_modules/.bin.
set -euo pipefail

REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)
GRAMMAR_DIR="$REPO/grammars/tree-sitter/iron"
INT_DIR="$REPO/tests/integration"

# Tooling discovery — global install, then local node_modules, then skip.
if [ -n "${TREE_SITTER:-}" ]; then
    TS="$TREE_SITTER"
elif command -v tree-sitter >/dev/null 2>&1; then
    TS=$(command -v tree-sitter)
elif [ -x "$GRAMMAR_DIR/node_modules/.bin/tree-sitter" ]; then
    TS="$GRAMMAR_DIR/node_modules/.bin/tree-sitter"
else
    echo "iron-lsp: tree-sitter-cli not found. Run 'npm install' in $GRAMMAR_DIR." >&2
    exit 77  # CTest SKIP
fi

# Known-skip list: fixtures with syntactic constructs the grammar does
# not cover in v1. Each entry MUST document the reason + tracking ID.
# When adding a rule that fixes a fixture, delete its line here.
KNOWN_SKIPS=(
    # url_parse_basic.iron — contains backslash-escaped quotes inside
    # interpolations (e.g. `"{Url.default_port(\"http\")}"`). Iron's lexer
    # handles this via IRON_TOK_INTERP_STRING single-token escape decoding;
    # tree-sitter would need an external scanner to match. v1 gap;
    # tracked in Plan 06-02 SUMMARY "Known Gaps" — post-v1 external-scanner
    # upgrade will close it.
    url_parse_basic.iron
)

contains_skip() {
    local name="$1"
    local s
    for s in "${KNOWN_SKIPS[@]}"; do
        if [ "$s" = "$name" ]; then return 0; fi
    done
    return 1
}

# Regenerate parser.c if absent (first run after clean checkout).
cd "$GRAMMAR_DIR"
if [ ! -f "src/parser.c" ]; then
    "$TS" generate >/dev/null
fi

fail=0
total=0
skipped=0
for f in $(find "$INT_DIR" -maxdepth 1 -name '*.iron' | sort); do
    total=$((total+1))
    base=$(basename "$f")
    if contains_skip "$base"; then
        skipped=$((skipped+1))
        continue
    fi
    if ! out=$("$TS" parse "$f" 2>&1); then
        echo "iron-lsp: PARSE FAILURE in $f" >&2
        echo "$out" | head -3 >&2
        fail=$((fail+1))
        continue
    fi
    if echo "$out" | grep -qE '(ERROR|MISSING)'; then
        echo "iron-lsp: ERROR/MISSING nodes in $f" >&2
        echo "$out" | grep -E '(ERROR|MISSING)' | head -3 >&2
        fail=$((fail+1))
    fi
done

if [ "$fail" -gt 0 ]; then
    echo "iron-lsp: $fail / $total integration fixtures failed the tree-sitter parse gate (skipped=$skipped)" >&2
    exit 1
fi

echo "iron-lsp: all $((total - skipped)) / $total integration fixtures parsed cleanly (skipped=$skipped)"
