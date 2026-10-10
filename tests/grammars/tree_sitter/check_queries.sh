#!/usr/bin/env bash
# Compile every tree-sitter query file the editors ship against the current
# grammar, so a renamed or removed node type fails here instead of silently
# turning off highlighting in Neovim or Zed:
#
#   grammars/tree-sitter/iron/queries/*.scm   canonical (nvim-treesitter names)
#   editors/neovim/queries/iron/*.scm         copies read by Neovim
#   editors/zed/languages/iron/*.scm          read by the Zed extension
#
# Also checks that the editor copies match the canonical queries
# (scripts/sync-editor-queries.sh --check).
#
# Exit codes: 0 pass, 1 failure, 77 tree-sitter-cli not available (SKIP).
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
    echo "check_queries: tree-sitter-cli not found (skipping)" >&2
    exit 77
fi

fail=0
bash "$REPO/scripts/sync-editor-queries.sh" --check || fail=1

cd "$GRAMMAR_DIR"
if [ ! -f "src/parser.c" ]; then
    "$TS" generate >/dev/null
fi

SAMPLES=(
    "$REPO/tests/integration/v4/regressions/syntax_forms.iron"
    "$REPO/tests/integration/v4/regressions/iface_type_match.iron"
    "$REPO/src/stdlib/map.iron"
)

count=0
for q in "$GRAMMAR_DIR"/queries/*.scm \
         "$REPO"/editors/neovim/queries/iron/*.scm \
         "$REPO"/editors/zed/languages/iron/*.scm; do
    [ -f "$q" ] || continue
    count=$((count+1))
    if ! out=$("$TS" query --quiet "$q" "${SAMPLES[@]}" 2>&1); then
        echo "check_queries: ${q#"$REPO"/} does not compile against the grammar:" >&2
        echo "$out" | head -5 >&2
        fail=1
    fi
done

if [ "$fail" = 0 ]; then
    echo "check_queries: $count query files compile against grammars/tree-sitter/iron"
fi
exit "$fail"
