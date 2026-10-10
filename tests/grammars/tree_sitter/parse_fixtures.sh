#!/usr/bin/env bash
# Integration-corpus tree-sitter parse gate. Runs every valid Iron program
# in the repo through `tree-sitter parse` and fails when any file produces
# ERROR or MISSING nodes:
#
#   tests/integration/**/*.iron   except the negative suites (v4-fail,
#                                 diagnostics), which hold invalid code
#   src/stdlib/**/*.iron
#   examples/**/*.iron
#
# This is the structural-parity fence between the Iron parser
# (src/parser/parser.c) and the tree-sitter grammar that Neovim and Zed
# highlight with: a program using a construct the grammar does not cover
# turns this test red, prompting the developer to either (a) add the rule
# to grammar.js.in + grammar.js and regenerate, or (b) add an explicit
# known-skip entry below with a reason.
#
# Exit codes:
#   0   every file parses with zero ERROR/MISSING nodes
#   1   at least one file failed; stderr lists the offenders
#   77  tree-sitter-cli not available (CTest SKIP code)
#
# Environment:
#   TREE_SITTER  optional override for the tree-sitter executable.
#                Otherwise resolved from PATH, then from the grammar
#                directory's node_modules/.bin.
set -euo pipefail

REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)
GRAMMAR_DIR="$REPO/grammars/tree-sitter/iron"

# Tooling discovery: global install, then local node_modules, then skip.
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

KNOWN_SKIPS=(
    # Backslash-escaped quotes inside an interpolation
    # (`"{Url.default_port(\"http\")}"`). Iron's lexer re-lexes the
    # interpolation text; tree-sitter would need an external scanner.
    url_parse_basic.iron
    string_interpolation_lexing.iron
    # Deliberately invalid source used by the vendor test.
    broken_test.iron
)

contains_skip() {
    local name="$1"
    local s
    for s in "${KNOWN_SKIPS[@]}"; do
        if [ "$s" = "$name" ]; then return 0; fi
    done
    return 1
}

cd "$GRAMMAR_DIR"
if [ ! -f "src/parser.c" ]; then
    "$TS" generate >/dev/null
fi

list=$(mktemp)
trap 'rm -f "$list"' EXIT
total=0
skipped=0
while IFS= read -r f; do
    total=$((total+1))
    if contains_skip "$(basename "$f")"; then
        skipped=$((skipped+1))
        continue
    fi
    echo "$f" >> "$list"
done < <(find "$REPO/tests/integration" "$REPO/src/stdlib" "$REPO/examples" \
              -name '*.iron' \
              -not -path '*/v4-fail/*' \
              -not -path '*/diagnostics/*' | sort)

# One process for the whole list; --quiet prints only the files that hold
# ERROR or MISSING nodes.
out=$("$TS" parse --quiet --paths "$list" 2>&1 || true)
bad=$(echo "$out" | grep -E '\((ERROR|MISSING)' || true)

if [ -n "$bad" ]; then
    fail=$(echo "$bad" | wc -l | tr -d ' ')
    echo "$bad" | head -40 >&2
    echo "iron-lsp: $fail / $total Iron programs failed the tree-sitter parse gate (skipped=$skipped)" >&2
    exit 1
fi
if [ "$total" -lt 100 ]; then
    echo "iron-lsp: only $total Iron programs found; the corpus paths look wrong" >&2
    exit 1
fi

echo "iron-lsp: all $((total - skipped)) / $total Iron programs parsed cleanly (skipped=$skipped)"
