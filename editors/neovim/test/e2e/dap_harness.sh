#!/usr/bin/env bash
# editors/neovim/test/e2e/dap_harness.sh
# Headless Neovim + nvim-dap check of lua/iron_dap.lua and `iron dap`
# (#312, #347).
#
# Usage: dap_harness.sh <iron>
#   NVIM_DAP_DIR  nvim-dap checkout (default:
#                 ~/.local/share/nvim/site/pack/test/start/nvim-dap)
#
# Exit codes: 0 pass, 1 fail, 77 skipped (nvim, nvim-dap or a DAP
# debugger missing).
set -euo pipefail

IRON="$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"
REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../../.." && pwd)
NVIM_DAP_DIR="${NVIM_DAP_DIR:-$HOME/.local/share/nvim/site/pack/test/start/nvim-dap}"

command -v nvim >/dev/null 2>&1 || { echo "nvim not found: skipped"; exit 77; }
[ -d "$NVIM_DAP_DIR" ] || {
    echo "nvim-dap not found at $NVIM_DAP_DIR: skipped"
    echo "  git clone --depth 1 https://github.com/mfussenegger/nvim-dap \"$NVIM_DAP_DIR\""
    exit 77
}

nvim --headless -u NONE \
    --cmd "set rtp^=$NVIM_DAP_DIR" \
    --cmd "set rtp^=$REPO/editors/neovim" \
    -l "$REPO/editors/neovim/test/e2e/dap_test.lua" \
    "$REPO/tests/integration/debug/stepping.iron" "$IRON"
