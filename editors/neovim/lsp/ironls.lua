-- editors/neovim/lsp/ironls.lua
-- Phase 6 Plan 06-04 Task 1 (EXT-06, EXT-07 foundation): canonical Iron LSP config.
--
-- Consumed by BOTH:
--   * Neovim 0.11.3+ native `vim.lsp.config()` / `vim.lsp.enable()` auto-discovery
--     (this file placed under any `&runtimepath/lsp/ironls.lua` is registered
--     automatically when `vim.lsp.enable('ironls')` is called).
--   * nvim-lspconfig v2 upstream — the shape below is byte-compatible with
--     sibling files under github.com/neovim/nvim-lspconfig/blob/master/lsp/
--     (e.g. `gopls.lua`, `lua_ls.lua`, `rust_analyzer.lua`) — a future PR
--     moves a copy of this file into that tree without edits. Plan 06-06
--     lands the post-v1 tracking doc `docs/dev/nvim-lspconfig-upstream.md`.
--
-- Source of truth: CONTEXT.md D-05 (decisions locked 2026-04-19).
--
-- Activation (documented in editors/neovim/README.md):
--   -- user's init.lua, after this file is on the runtimepath:
--   vim.lsp.enable('ironls')

-- PITFALLS §11 — Neovim 0.11 vs 0.11.3 confusion. `vim.lsp.config()` was
-- introduced in 0.11.3; `has('nvim-0.11')` returns 1 on 0.11.0/0.11.1/0.11.2
-- where the API is still absent. Match the pattern used by
-- nvim-lspconfig/lsp/lua_ls.lua (`vim.fn.has('nvim-0.11.3')`).
if vim.fn.has('nvim-0.11.3') ~= 1 then
  local v = vim.version and vim.version() or { major = 0, minor = 0, patch = 0 }
  local actual = string.format('%d.%d.%d', v.major or 0, v.minor or 0, v.patch or 0)
  vim.notify(
    '[iron-lsp] Neovim 0.11.3+ is required (found ' .. actual .. '). '
      .. 'Upgrade or use the compat config from editors/neovim/README.md.',
    vim.log.levels.ERROR
  )
  -- Empty-table return matches nvim-lspconfig "server unavailable" convention.
  return {}
end

---@type vim.lsp.Config
return {
  cmd = { 'ironls' },
  filetypes = { 'iron' },
  root_markers = { 'iron.toml', '.git' },
  settings = {
    -- reserved for future iron.languageServer.* options (UI-SPEC S4).
  },
  -- initializationOptions mirror the VSCode extension for cross-editor parity.
  init_options = { clientName = 'neovim' },
  -- UI-SPEC S9 — version compatibility range (Phase 7 HARD-22 promotes to
  -- hard refuse). Non-standard field; harmless to vim.lsp.Config consumers;
  -- read by plugin/iron_lsp.lua's diagnose command + future version probes.
  compatible_ironls = "1.2.0..<1.3.0",
  -- S5 log emit on successful initialize; tolerates the helper module being
  -- absent (plugin/iron_lsp.lua is a plugin file, loaded automatically on
  -- startup under packages on runtimepath — pcall keeps the config valid
  -- even if the plugin directory is missing from a user's custom layout).
  on_attach = function(client, _bufnr)
    local ok, log = pcall(require, 'iron_lsp_log')
    if ok and type(log) == 'table' and type(log.event) == 'function' then
      log.event('info', 'lsp.initialize.ok', {
        server_name = client.name,
        server_id = client.id,
      })
    end
  end,
}
