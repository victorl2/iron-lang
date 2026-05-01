-- editors/neovim/test/e2e/diag_error_spec.lua
-- Phase 6 Plan 06-04 Task 2 (EXT-06 + EXT-10): Neovim end-to-end test.
--
-- Drives a headless Neovim (launched by editors/neovim/test/e2e/harness.sh)
-- against tests/editors/fixtures/diag_error.iron and asserts that the
-- attached ironls client publishes at least one Error-severity diagnostic.
--
-- Sources:
--   - RESEARCH.md §Pattern 6 (lines 783-826)
--   - zignar.net/2022/10/26 "Testing Neovim LSP plugins"
--   - plenary.nvim test_harness README
--
-- Invoked via `require('plenary.test_harness').test_directory(dir)` from
-- harness.sh. Plenary wraps `busted`-style describe/it; assertions come
-- from plenary's luassert.

local function repo_root()
  -- harness.sh runs `cd "$REPO"` before spawning Neovim, so cwd is the
  -- checkout root.
  return vim.fn.getcwd()
end

describe('iron-lsp e2e', function()
  it('publishes at least one Error-severity diagnostic on diag_error.iron within 5s', function()
    local root = repo_root()

    -- Plenary's test_directory spawns a child nvim with minimal_init='NONE',
    -- which leaves filetype-detection autocmds OFF. Without `filetype on`,
    -- `:edit *.iron` does not assign the "iron" filetype, so
    -- `vim.lsp.enable('ironls')` (filetype-gated) never attaches the client.
    -- Enable detection explicitly so the test mirrors what real users have.
    vim.cmd('filetype on')

    -- Source the shipped editor files.
    dofile(root .. '/editors/neovim/ftdetect/iron.lua')
    dofile(root .. '/editors/neovim/plugin/iron_lsp.lua')

    local cfg = dofile(root .. '/editors/neovim/lsp/ironls.lua')
    assert.is_table(cfg,
      'lsp/ironls.lua must return a config table (not empty {} — '
      .. 'empty indicates the 0.11.3 version guard fired; upgrade Neovim)')
    assert.is_table(cfg.cmd, 'cfg.cmd must be a table')
    assert.are.equal('ironls', cfg.cmd[1])
    assert.is_table(cfg.filetypes)
    assert.are.equal('iron', cfg.filetypes[1])

    -- Register the server per Neovim 0.11+ native API.
    vim.lsp.config('ironls', cfg)
    vim.lsp.enable('ironls')

    -- Open the fixture.
    local fixture = root .. '/tests/editors/fixtures/diag_error.iron'
    vim.cmd('edit ' .. fixture)
    local bufnr = vim.api.nvim_get_current_buf()

    -- PITFALLS §6 — vim.wait with predicate, not sleep. Budget 5 s.
    local ok = vim.wait(5000, function()
      local d = vim.diagnostic.get(bufnr)
      return d and #d > 0
    end, 50)
    assert.is_true(ok,
      'LSP did not publish diagnostics within 5 s; '
      .. 'check that ironls is on PATH (harness.sh responsibility)')

    -- Assert we received at least one ERROR-severity diagnostic.
    local diags = vim.diagnostic.get(bufnr)
    assert.is_true(#diags > 0, 'expected >= 1 diagnostic on diag_error.iron')
    local has_error = false
    for _, d in ipairs(diags) do
      if d.severity == vim.diagnostic.severity.ERROR then
        has_error = true
      end
    end
    assert.is_true(has_error,
      'expected at least one ERROR-severity diagnostic '
      .. '(diag_error.iron contains an IRON_ERR_UNDEFINED_VAR)')

    -- Assert an ironls client is attached to the buffer — catches the
    -- case where a non-ironls diagnostic source (linter plugin) publishes.
    local get_clients = vim.lsp.get_clients or vim.lsp.get_active_clients
    local clients = get_clients({ bufnr = bufnr, name = 'ironls' })
    assert.is_true(#clients > 0,
      'expected an ironls client attached to the fixture buffer')
  end)
end)
