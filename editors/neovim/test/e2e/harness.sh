#!/usr/bin/env bash
# editors/neovim/test/e2e/harness.sh
# Phase 6 Plan 06-04 Task 2 (EXT-06 + EXT-10): Neovim e2e harness driver.
#
# Preconditions:
#   - nvim 0.11.3+ on PATH
#   - plenary.nvim cloned (override path via PLENARY_DIR env; defaults to
#     ~/.local/share/nvim/site/pack/test/start/plenary.nvim)
#   - ironls on PATH (CI injects via PATH; local devs build ironls + export PATH)
#
# Exit codes:
#   0  all tests passed
#   1  a test failed, or ironls / plenary is not available
#   77 ctest SKIP (nvim not installed) — convention for skipped tests

set -euo pipefail

REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../../.." && pwd)
PLENARY_DIR="${PLENARY_DIR:-$HOME/.local/share/nvim/site/pack/test/start/plenary.nvim}"
E2E_DIR="$REPO/editors/neovim/test/e2e"

if ! command -v nvim >/dev/null 2>&1; then
    echo "neovim-e2e: nvim not found on PATH (skipping)" >&2
    exit 77
fi

if ! command -v ironls >/dev/null 2>&1; then
    echo "neovim-e2e: ironls not found on PATH" >&2
    echo "  CI must set PATH to include the build dir; local devs export PATH=\$PWD/build:\$PATH" >&2
    exit 1
fi

if [[ ! -d "$PLENARY_DIR" ]]; then
    echo "neovim-e2e: plenary.nvim not found at $PLENARY_DIR" >&2
    echo "  Set PLENARY_DIR or clone into the default location:" >&2
    echo "    git clone --depth 1 https://github.com/nvim-lua/plenary.nvim \"$PLENARY_DIR\"" >&2
    exit 1
fi

# Report what we resolved — invaluable when CI logs show a failure.
echo "neovim-e2e: REPO=$REPO"
echo "neovim-e2e: nvim=$(command -v nvim) ($(nvim --version | head -1))"
echo "neovim-e2e: ironls=$(command -v ironls)"
echo "neovim-e2e: PLENARY_DIR=$PLENARY_DIR"

cd "$REPO"

# Build the iron tree-sitter parser into a scratch runtimepath directory
# when tree-sitter-cli is available, so features_spec.lua can check
# tree-sitter highlighting too (it is marked pending otherwise).
TS_CLI="${TREE_SITTER:-$(command -v tree-sitter || true)}"
if [[ -z "$TS_CLI" && -x "$REPO/grammars/tree-sitter/iron/node_modules/.bin/tree-sitter" ]]; then
    TS_CLI="$REPO/grammars/tree-sitter/iron/node_modules/.bin/tree-sitter"
fi
if [[ -n "$TS_CLI" ]]; then
    IRON_TS_RTP=$(mktemp -d)
    trap 'rm -rf "$IRON_TS_RTP"' EXIT
    mkdir -p "$IRON_TS_RTP/parser"
    if (cd "$REPO/grammars/tree-sitter/iron" &&
        { [[ -f src/parser.c ]] || "$TS_CLI" generate >/dev/null; } &&
        "$TS_CLI" build -o "$IRON_TS_RTP/parser/iron.so" >/dev/null 2>&1); then
        export IRON_TS_RTP
        echo "neovim-e2e: tree-sitter parser built into $IRON_TS_RTP/parser"
    else
        echo "neovim-e2e: tree-sitter parser build failed; skipping tree-sitter checks"
        unset IRON_TS_RTP
    fi
fi

# -u NONE: skip the user's init.lua (we are not testing the user config; we
# are testing the shipped in-tree config). The plenary runtimepath is added
# inline via `set rtp+=...` so the harness has no other dependency.
#
# `filetype on`: -u NONE also disables Neovim's built-in filetype-detection
# autocmds. Without it, `:edit *.iron` does not set filetype to "iron", so
# `vim.lsp.enable('ironls')` (filetype-gated) never attaches the client and
# no diagnostics are published. The shipped editors/neovim/ftdetect/iron.lua
# maps the extension via vim.filetype.add(), but that mapping only fires
# inside filetype-detection autocmds.
nvim --headless -u NONE \
    -c "filetype on" \
    -c "set rtp+=$PLENARY_DIR" \
    -c "lua vim.env.IRONLS_E2E = '1'" \
    -c "lua require('plenary.test_harness').test_directory('$E2E_DIR', { minimal_init = 'NONE' })" \
    -c qa
