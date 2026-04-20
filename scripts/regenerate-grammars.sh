#!/usr/bin/env bash
# Phase 6 Plan 06-01 (EXT-03): regenerate TextMate + tree-sitter grammars.
#
# Wraps the `regenerate-grammars` CMake custom target, which runs the two
# `configure_file` invocations and copies the generated outputs back to the
# committed paths under grammars/textmate/ and grammars/tree-sitter/iron/.
#
# The committed grammars are drift-gated by CTests
# test_grammar_keyword_drift_textmate + test_grammar_keyword_drift_tree_sitter
# (dual-labelled phase-m5-invariant + phase-m1-invariant). This script is the
# developer entry point when src/lexer/lexer.c kw_table gains/loses entries.
#
# Usage:
#   ./scripts/regenerate-grammars.sh
#   IRON_BUILD_DIR=build-debug ./scripts/regenerate-grammars.sh
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${IRON_BUILD_DIR:-${REPO_ROOT}/build}"

if [ ! -d "${BUILD_DIR}" ]; then
    echo "error: build directory '${BUILD_DIR}' not found." >&2
    echo "       Run 'cmake -B \"${BUILD_DIR}\" -DIRON_BUILD_GRAMMARS=ON' first," >&2
    echo "       or set IRON_BUILD_DIR to an existing configured directory." >&2
    exit 1
fi

cmake --build "${BUILD_DIR}" --target regenerate-grammars

echo "Phase 6 Plan 06-01: grammars regenerated from src/lexer/lexer.c kw_table."
echo "Verify with: ctest --test-dir \"${BUILD_DIR}\" -L phase-m5-invariant"
