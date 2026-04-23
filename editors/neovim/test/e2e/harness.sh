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

# -u NONE: skip the user's init.lua (we are not testing the user config; we
# are testing the shipped in-tree config). The plenary runtimepath is added
# inline via `set rtp+=...` so the harness has no other dependency.
nvim --headless -u NONE \
    -c "set rtp+=$PLENARY_DIR" \
    -c "lua vim.env.IRONLS_E2E = '1'" \
    -c "lua require('plenary.test_harness').test_directory('$E2E_DIR', { minimal_init = 'NONE' })" \
    -c qa
