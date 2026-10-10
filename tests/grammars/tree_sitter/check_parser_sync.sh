#!/usr/bin/env bash
# The generated parser (grammars/tree-sitter/iron/src/) is committed because
# Zed builds the grammar straight from the repository at a pinned commit
# and does not run `tree-sitter generate`. This gate regenerates it from
# grammar.js in a scratch copy and fails when the committed files differ,
# i.e. when grammar.js changed without rerunning `tree-sitter generate`.
#
# Generated output can differ between tree-sitter-cli minor versions, so
# the comparison only runs with the version the committed files were
# generated with (PINNED below, the CI tree-sitter-wasm job's version);
# other versions SKIP.
#
# Exit codes: 0 in sync, 1 stale, 77 tree-sitter-cli missing or other version.
set -euo pipefail

PINNED="0.26"

REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)
GRAMMAR_DIR="$REPO/grammars/tree-sitter/iron"

if [ -n "${TREE_SITTER:-}" ]; then
    TS="$TREE_SITTER"
elif command -v tree-sitter >/dev/null 2>&1; then
    TS=$(command -v tree-sitter)
elif [ -x "$GRAMMAR_DIR/node_modules/.bin/tree-sitter" ]; then
    TS="$GRAMMAR_DIR/node_modules/.bin/tree-sitter"
else
    echo "check_parser_sync: tree-sitter-cli not found (skipping)" >&2
    exit 77
fi

version=$("$TS" --version | awk '{print $2}')
case "$version" in
    "$PINNED".*) ;;
    *)
        echo "check_parser_sync: tree-sitter $version is not $PINNED.x (skipping)" >&2
        exit 77
        ;;
esac

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
cp "$GRAMMAR_DIR/grammar.js" "$GRAMMAR_DIR/tree-sitter.json" "$GRAMMAR_DIR/package.json" "$work/"
(cd "$work" && "$TS" generate >/dev/null)

stale=0
for f in parser.c grammar.json node-types.json tree_sitter/parser.h; do
    if ! cmp -s "$work/src/$f" "$GRAMMAR_DIR/src/$f"; then
        echo "check_parser_sync: grammars/tree-sitter/iron/src/$f is stale;" \
             "run 'tree-sitter generate' in grammars/tree-sitter/iron" >&2
        stale=1
    fi
done
[ "$stale" = 0 ] && echo "check_parser_sync: committed parser matches grammar.js (tree-sitter $version)"
exit "$stale"
