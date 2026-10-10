#!/usr/bin/env bash
# Copy the tree-sitter queries from grammars/tree-sitter/iron/queries/ into
# the editor integrations that read them:
#
#   editors/neovim/queries/iron/   byte-identical copies (Neovim reads
#                                  queries/<lang>/*.scm from the runtimepath)
#   editors/zed/languages/iron/    highlights.scm, with the captures renamed
#                                  to Zed's theme keys
#
# The Zed-only queries (brackets, indents, outline, textobjects) live in
# editors/zed/languages/iron/ and are not generated.
#
# Usage:
#   scripts/sync-editor-queries.sh          write the copies
#   scripts/sync-editor-queries.sh --check  exit 1 when a copy is stale
set -euo pipefail

REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
SRC="$REPO/grammars/tree-sitter/iron/queries"
NVIM="$REPO/editors/neovim/queries/iron"
ZED="$REPO/editors/zed/languages/iron"
CHECK=0
[ "${1:-}" = "--check" ] && CHECK=1

NVIM_QUERIES="highlights.scm folds.scm locals.scm indents.scm textobjects.scm"

# nvim-treesitter capture names that Zed themes spell differently. Captures
# not listed here resolve through Zed's dotted-prefix fallback
# (@keyword.conditional -> keyword, @function.method.call -> function).
zed_highlights() {
    printf '; Generated from grammars/tree-sitter/iron/queries/highlights.scm by\n'
    printf '; scripts/sync-editor-queries.sh. Edit the source and rerun the script.\n\n'
    sed -e 's/@variable\.member/@property/g' \
        -e 's/@comment\.documentation/@comment.doc/g' \
        -e 's/@character\.special/@variable.special/g' \
        -e 's/@type\.definition/@type/g' \
        "$SRC/highlights.scm"
}

stale=0
emit() {  # emit <dest> ; content on stdin
    local dest="$1" tmp
    tmp=$(mktemp)
    cat > "$tmp"
    if [ "$CHECK" = 1 ]; then
        if ! cmp -s "$tmp" "$dest"; then
            echo "stale: ${dest#"$REPO"/} (run scripts/sync-editor-queries.sh)" >&2
            stale=1
        fi
        rm -f "$tmp"
    else
        mkdir -p "$(dirname "$dest")"
        mv "$tmp" "$dest"
    fi
}

for q in $NVIM_QUERIES; do
    emit "$NVIM/$q" < "$SRC/$q"
done
zed_highlights | emit "$ZED/highlights.scm"

if [ "$CHECK" = 1 ]; then
    [ "$stale" = 0 ] && echo "editor queries in sync with grammars/tree-sitter/iron/queries"
    exit "$stale"
fi
echo "editor queries written"
