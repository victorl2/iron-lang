-- editors/neovim/plugin/iron_lsp.lua
-- Phase 6 Plan 06-04 Task 1 (EXT-06, UI-SPEC S3 + S5):
--   * `:IronLspDiagnose` user command — produces the UI-SPEC S3
--     9-section bug-report payload, prints to :messages, copies to
--     the `+` register (system clipboard).
--   * S5 structured-JSON log helper (src:"neovim-ext"), appended to
--     $XDG_STATE_HOME/iron-lsp/client-nvim.log.
--
-- This is the Neovim counterpart of editors/vscode/src/{diagnose.ts, log.ts}
-- (Plan 06-03). Event names are byte-identical across editors per UI-SPEC S5.
--
-- When this directory is on Neovim's &runtimepath, Neovim automatically
-- `source`s every `plugin/*.lua` file at startup (see :help plugin-script).

local M = {}

-- ---------------------------------------------------------------------------
-- S5 log helper
-- ---------------------------------------------------------------------------

local function xdg_state_home()
  return vim.env.XDG_STATE_HOME or (vim.env.HOME .. '/.local/state')
end

local function client_log_path()
  local dir = xdg_state_home() .. '/iron-lsp'
  vim.fn.mkdir(dir, 'p')
  return dir .. '/client-nvim.log'
end
M._client_log_path = client_log_path

--- Emit a single UI-SPEC S5 structured log record. Never throws.
--- @param lvl string one of "error" | "warn" | "info" | "debug"
--- @param evt string event name from the S5 event vocabulary
--- @param extra table|nil additional structured fields
function M.event(lvl, evt, extra)
  local rec = {
    ts = os.date('!%Y-%m-%dT%H:%M:%SZ'),
    lvl = lvl,
    src = 'neovim-ext',
    evt = evt,
  }
  if type(extra) == 'table' then
    for k, v in pairs(extra) do
      rec[k] = v
    end
  end
  local ok, line = pcall(vim.json.encode, rec)
  if not ok then return end
  local f = io.open(client_log_path(), 'a')
  if f then
    f:write(line .. '\n')
    f:close()
  end
end

-- Expose the helper to `require('iron_lsp_log')` so lsp/ironls.lua's
-- on_attach can emit `lsp.initialize.ok` without a circular dependency
-- on the plugin/ file path.
package.loaded['iron_lsp_log'] = M

-- ---------------------------------------------------------------------------
-- Diagnose payload (UI-SPEC S3)
-- ---------------------------------------------------------------------------

local function path_exists(p)
  local stat = vim.uv and vim.uv.fs_stat(p) or vim.loop.fs_stat(p)
  return stat ~= nil
end

local function get_clients()
  -- Neovim 0.11+ deprecates get_active_clients in favor of get_clients.
  if vim.lsp.get_clients then
    return vim.lsp.get_clients({ name = 'ironls' })
  end
  return vim.lsp.get_active_clients({ name = 'ironls' })
end

local function probe_ironls()
  -- Honor cmd[1] from vim.lsp.config('ironls') if the user overrode.
  local clients = get_clients()
  if #clients > 0 and clients[1].config and clients[1].config.cmd then
    return clients[1].config.cmd[1], 'iron.languageServer.path setting'
  end
  -- Fall back to PATH resolution.
  local r = vim.fn.exepath('ironls')
  if r and #r > 0 then return r, 'PATH' end
  return nil, nil
end

local function probe_version(ironls_path)
  if not ironls_path or ironls_path == '' then return nil end
  local out = vim.fn.system({ ironls_path, '--version' })
  if vim.v.shell_error ~= 0 then return nil end
  local ver = out:match('(%d+%.%d+%.%d+[%w%-%.]*)')
  return ver
end

local function editor_version_string()
  local v = vim.version and vim.version() or nil
  if v then
    return string.format('Neovim %d.%d.%d', v.major or 0, v.minor or 0, v.patch or 0)
  end
  return 'Neovim (version unknown)'
end

local function platform_string()
  local uname = (vim.uv and vim.uv.os_uname and vim.uv.os_uname())
    or (vim.loop and vim.loop.os_uname and vim.loop.os_uname())
    or { sysname = 'unknown', machine = 'unknown' }
  return (uname.sysname or 'unknown'):lower() .. '-' .. (uname.machine or 'unknown')
end

local function truncate(s, max)
  if not s then return 'UNAVAILABLE' end
  if #s <= max then return s end
  return s:sub(1, max - 3) .. '...'
end

local function caps_summary(client)
  if not client or not client.server_capabilities then return 'UNAVAILABLE' end
  local ok, enc = pcall(vim.inspect, client.server_capabilities)
  if not ok then return 'UNAVAILABLE' end
  -- Collapse whitespace + newlines so the one-line summary stays readable.
  enc = enc:gsub('%s+', ' ')
  return truncate(enc, 120)
end

--- Build the UI-SPEC S3 payload. Order-locked: every field always present.
--- @return string payload multi-line UTF-8 text
function M.diagnose_payload()
  local now = os.date('!%Y-%m-%dT%H:%M:%SZ')
  local ironls_path, ironls_method = probe_ironls()
  local resolved_path = ironls_path or 'NOT FOUND'
  local resolved_method = ironls_method or 'NONE'
  local ironls_ver = ironls_path and (probe_version(ironls_path) or 'UNAVAILABLE') or 'UNAVAILABLE'

  local workspace = vim.fn.getcwd()
  local iron_toml = path_exists(workspace .. '/iron.toml') and (workspace .. '/iron.toml') or 'NO'
  local iron_lock = path_exists(workspace .. '/iron.lock') and (workspace .. '/iron.lock') or 'NO'

  local clients = get_clients()
  local status = 'not-started'
  local last_init = 'NOT INITIALIZED'
  local caps = 'UNAVAILABLE'
  if #clients > 0 then
    local c = clients[1]
    if c.initialized or (c.server_capabilities and next(c.server_capabilities) ~= nil) then
      status = 'running'
      last_init = now -- Neovim does not expose the original initialize ts; payload records present-tense.
      caps = caps_summary(c)
    else
      status = 'not-started'
    end
  end

  local lines = {
    'Iron LSP Diagnose Report',
    '========================',
    'Timestamp:           ' .. now,
    'Extension version:   editors/neovim (in-tree v0.1.0)',
    'Editor:              ' .. editor_version_string(),
    'Platform:            ' .. platform_string(),
    '',
    'ironls binary',
    '-------------',
    'Resolved path:       ' .. resolved_path,
    'Resolution method:   ' .. resolved_method,
    'Version reported:    ' .. ironls_ver,
    'SHA-256 (Zed only):  n/a',
    '',
    'Workspace',
    '---------',
    'Root:                ' .. workspace,
    'iron.toml found:     ' .. iron_toml,
    'iron.lock found:     ' .. iron_lock,
    '',
    'LSP session',
    '-----------',
    'Server capabilities: ' .. caps,
    'Last initialize:     ' .. last_init,
    'Status:              ' .. status,
    '',
    'Recent log tail (last 20 lines from editor-side log)',
    '----------------------------------------------------',
  }

  -- Append editor-side log tail (client-nvim.log).
  local fh = io.open(client_log_path(), 'r')
  if fh then
    local all = {}
    for line in fh:lines() do
      table.insert(all, line)
    end
    fh:close()
    if #all == 0 then
      table.insert(lines, 'no log data')
    else
      local start = math.max(1, #all - 19)
      for i = start, #all do
        table.insert(lines, all[i])
      end
    end
  else
    table.insert(lines, 'no log data')
  end

  return table.concat(lines, '\n')
end

-- ---------------------------------------------------------------------------
-- :IronLspDiagnose user command
-- ---------------------------------------------------------------------------

vim.api.nvim_create_user_command('IronLspDiagnose', function()
  local payload = M.diagnose_payload()
  -- Print to :messages.
  for _, line in ipairs(vim.split(payload, '\n', { plain = true })) do
    vim.api.nvim_echo({ { line } }, true, {})
  end
  -- Copy to the + register (system clipboard); tolerate clipboard-less envs.
  local ok = pcall(vim.fn.setreg, '+', payload)
  if ok then
    vim.notify('[iron-lsp] diagnose payload copied to + register', vim.log.levels.INFO)
  else
    vim.notify('[iron-lsp] diagnose payload printed to :messages (no clipboard)', vim.log.levels.INFO)
  end
end, { desc = 'Iron LSP: Diagnose (print + copy UI-SPEC S3 bug-report payload)' })

-- Fire ext.activate event at plugin load (UI-SPEC S5 vocabulary).
M.event('info', 'ext.activate', { editor_version = editor_version_string() })

return M
