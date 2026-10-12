#!/usr/bin/env bash
# The tree-sitter grammar against the compiler's parser (tests/fuzz/ts_diff.py)
# on generated programs, corpus programs and mutants, with a fixed seed: fails
# when the grammar marks a program the compiler accepts as ERROR.
#
#   ts_differential.sh PYTHON IRONC
#
# Exit codes: 0 agree, 1 disagreement, 77 tree-sitter-cli not available.
set -euo pipefail

REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)
GRAMMAR_DIR="$REPO/grammars/tree-sitter/iron"
PY=$1
IRONC=$2

if [ -n "${TREE_SITTER:-}" ]; then
    TS="$TREE_SITTER"
elif command -v tree-sitter >/dev/null 2>&1; then
    TS=$(command -v tree-sitter)
elif [ -x "$GRAMMAR_DIR/node_modules/.bin/tree-sitter" ]; then
    TS="$GRAMMAR_DIR/node_modules/.bin/tree-sitter"
else
    echo "ts_differential: tree-sitter-cli not found (skipping)" >&2
    exit 77
fi

out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
# tree-sitter caches the compiled parser by language name in a directory
# shared by every checkout and rebuilds it only when src/parser.c is newer,
# so a parser built from another checkout of the grammar would be used
# here. Compile this checkout's parser into a directory of its own.
export TREE_SITTER_LIBDIR="$out/ts-lib"
mkdir -p "$TREE_SITTER_LIBDIR"
"$PY" "$REPO/tests/fuzz/ts_diff.py" --ironc "$IRONC" --tree-sitter "$TS" \
    --seed 1 --count 200 --corpus 100 --mutants 2 -j 4 --out "$out"
