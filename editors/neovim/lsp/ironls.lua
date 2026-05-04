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

-- Phase 7 Plan 07-07 Task 02 (HARD-22, D-10, UI-SPEC S9) — compatible
-- ironls version range. The on_attach hook below reads
-- `client.server_info.version` (Neovim 0.11+ populates it from the
-- LSP initialize response's serverInfo.version) and HARD-REFUSES the
-- attach on mismatch: notifies the user with ERROR severity + one-line
-- install command, then detaches the client via
-- `vim.lsp.buf_detach_client` so no language features fire.
--
-- The tuple shape matches the plan spec literally: an array of the two
-- bounds plus an auxiliary table with parsed min/max for the
-- comparator. Callers should treat this constant as read-only; any
-- future relaxation of the range updates both the min/max and the
-- array shape together.
local IRON_LSP_COMPATIBLE_VERSION_RANGE = {
  ">= 3.0.0",
  "< 4.0.0",
  min = "3.0.0",
  max_exclusive = "4.0.0",
}

-- Parse a dotted semver prefix "X.Y.Z" (with optional "-preX.Y" suffix)
-- into { major, minor, patch }. Returns nil on parse failure so the
-- caller can surface a clear error rather than silently accepting a
-- malformed version.
local function parse_semver(s)
  if type(s) ~= 'string' then return nil end
  local major, minor, patch = s:match("^(%d+)%.(%d+)%.(%d+)")
  if not major then return nil end
  return { tonumber(major), tonumber(minor), tonumber(patch) }
end

-- Inclusive lower bound, exclusive upper bound. Pre-release suffixes
-- within the major range are accepted (D-10 policy); a next-major
-- release will require an extension update.
local function version_in_range(version, range)
  local v = parse_semver(version)
  local lo = parse_semver(range.min)
  local hi = parse_semver(range.max_exclusive)
  if not v or not lo or not hi then return false end
  -- v >= lo
  for i = 1, 3 do
    if v[i] ~= lo[i] then
      if v[i] < lo[i] then return false end
      break
    end
  end
  -- v < hi
  for i = 1, 3 do
    if v[i] ~= hi[i] then
      return v[i] < hi[i]
    end
  end
  -- equal to upper bound -- rejected (exclusive)
  return false
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
  -- UI-SPEC S9 — version compatibility range. Phase 7 HARD-22 tightened
  -- this to hard-refuse; the on_attach hook below enforces it.
  -- Non-standard field; harmless to vim.lsp.Config consumers; read by
  -- plugin/iron_lsp.lua's diagnose command.
  compatible_ironls = ">= 3.0.0, < 4.0.0",
  -- S5 log emit on successful initialize; tolerates the helper module being
  -- absent (plugin/iron_lsp.lua is a plugin file, loaded automatically on
  -- startup under packages on runtimepath — pcall keeps the config valid
  -- even if the plugin directory is missing from a user's custom layout).
  --
  -- Phase 7 HARD-22 / D-10: HARD-REFUSE on version mismatch. We read
  -- client.server_info.version (populated by vim.lsp core from the
  -- initialize response); if missing or outside range, we vim.notify
  -- ERROR + buf_detach_client so the buffer has no active language
  -- features. A detached client is effectively refused for this buffer.
  on_attach = function(client, bufnr)
    local ok, log = pcall(require, 'iron_lsp_log')
    local emit = (ok and type(log) == 'table' and type(log.event) == 'function')
      and log.event or function() end

    -- Neovim 0.11+ exposes the initialize response's serverInfo on the
    -- client at on_attach time. Fall back to server_capabilities.serverInfo
    -- for older / non-standard builds.
    local server_info = client.server_info
      or (client.server_capabilities and client.server_capabilities.serverInfo)
      or nil
    local server_version = server_info and server_info.version or nil

    if not server_version or not version_in_range(server_version, IRON_LSP_COMPATIBLE_VERSION_RANGE) then
      emit('error', 'ironls.version_mismatch', {
        detected = server_version or 'unknown',
        range = ">= 3.0.0, < 4.0.0",
        action = 'detach-client',
      })
      vim.notify(
        string.format(
          '[iron-lsp] detected ironls %s, but this config requires >= %s, < %s. '
            .. 'Install latest: https://github.com/iron-lang/iron-lang/releases/latest '
            .. '(or: cargo install --path . from the iron-lang repo). Detaching.',
          server_version or 'unknown',
          IRON_LSP_COMPATIBLE_VERSION_RANGE.min,
          IRON_LSP_COMPATIBLE_VERSION_RANGE.max_exclusive
        ),
        vim.log.levels.ERROR,
        { title = 'Iron LSP' }
      )
      -- buf_detach_client is the Neovim 0.11+ surface. Wrap in pcall so
      -- older builds (where the API shape drifts) still exit cleanly.
      pcall(vim.lsp.buf_detach_client, bufnr, client.id)
      return
    end

    emit('info', 'lsp.initialize.ok', {
      server_name = client.name,
      server_id = client.id,
      server_version = server_version,
    })
  end,
}
