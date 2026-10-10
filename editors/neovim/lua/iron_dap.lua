-- editors/neovim/lua/iron_dap.lua
-- Debugging Iron programs with nvim-dap (#312, #347).
--
-- The adapter is `iron dap`, the Iron debug adapter shared with VS Code
-- and Zed: it builds the program (a .iron file or a package directory)
-- with `iron build --debug`, runs lldb-dap or gdb 14+ with the value
-- formatters, shows locals under their Iron names and stops on the Iron
-- line when the program panics.
--
--   require('iron_dap').setup()                 -- defaults
--   require('iron_dap').setup({ iron = '/path/to/iron' })
--
-- plugin/iron_dap.lua calls setup() at startup when nvim-dap is loaded.

local M = {}

--- Launch configurations offered for `iron` buffers.
function M.configurations()
  return {
    {
      type = 'iron',
      request = 'launch',
      name = 'Iron: debug this file',
      program = '${file}',
    },
    {
      type = 'iron',
      request = 'launch',
      name = 'Iron: debug the package',
      program = '${workspaceFolder}',
      args = function()
        local line = vim.fn.input('Arguments: ')
        return vim.split(line, ' ', { trimempty = true })
      end,
    },
    {
      type = 'iron',
      request = 'launch',
      name = 'Iron: debug the test under the cursor',
      program = '${file}',
      test = function()
        return M.test_at_cursor() or vim.fn.input('Test name: ')
      end,
    },
  }
end

--- The name of the `test "..."` block the cursor is in (the nearest one
--- at or above the cursor), or nil.
function M.test_at_cursor()
  local row = vim.api.nvim_win_get_cursor(0)[1]
  for i = row, 1, -1 do
    local line = vim.api.nvim_buf_get_lines(0, i - 1, i, false)[1] or ''
    local name = line:match('^%s*test%s+"(.-)"%s*{')
    if name then
      return (name:gsub('\\(.)', '%1'))
    end
  end
  return nil
end

--- Register the `iron` adapter and configurations with nvim-dap.
--- @param opts table|nil { iron = path to the iron CLI (default: "iron" on PATH) }
--- The iron CLI (setup's opts.iron, else "iron" on PATH).
M.iron = 'iron'

--- Run `iron <args...>` in a terminal split below.
function M.run_in_terminal(args)
  local cmd = { M.iron }
  vim.list_extend(cmd, args)
  vim.cmd('botright 15split')
  vim.cmd('enew')
  if vim.fn.has('nvim-0.11') == 1 then
    vim.fn.jobstart(cmd, { term = true })
  else
    vim.fn.termopen(cmd)
  end
  vim.cmd('startinsert')
end

local function current_file()
  local file = vim.api.nvim_buf_get_name(0)
  if file == '' or not file:match('%.iron$') then
    vim.notify('iron: open a .iron file first', vim.log.levels.WARN)
    return nil
  end
  return file
end

--- :IronRun - build and run the current file.
function M.run_file()
  local file = current_file()
  if file then M.run_in_terminal({ 'run', file }) end
end

--- :IronTest [name] - one test (the name, else the test under the cursor),
--- or every test of the file when there is neither.
function M.test(name)
  local file = current_file()
  if not file then return end
  name = (name and name ~= '') and name or M.test_at_cursor()
  M.run_in_terminal(name and { 'test', file, name } or { 'test', file })
end

local function start_debugging(config)
  local ok, dap = pcall(require, 'dap')
  if not ok then
    vim.notify('iron: debugging needs nvim-dap', vim.log.levels.WARN)
    return
  end
  dap.run(vim.tbl_extend('force', { type = 'iron', request = 'launch' }, config))
end

--- :IronDebug - debug the current file.
function M.debug_file()
  local file = current_file()
  if file then
    start_debugging({ name = 'Iron: debug ' .. vim.fn.fnamemodify(file, ':t'), program = file })
  end
end

--- :IronDebugTest [name] - debug one test alone (the name, else the test
--- under the cursor).
function M.debug_test(name)
  local file = current_file()
  if not file then return end
  name = (name and name ~= '') and name or M.test_at_cursor()
  if not name then
    vim.notify('iron: no test block at or above the cursor', vim.log.levels.WARN)
    return
  end
  start_debugging({ name = 'Iron: debug test "' .. name .. '"', program = file, test = name })
end

function M.setup(opts)
  opts = opts or {}
  if opts.iron then M.iron = opts.iron end
  local ok, dap = pcall(require, 'dap')
  if not ok then
    vim.notify('iron_dap: nvim-dap is not installed', vim.log.levels.WARN)
    return false
  end
  dap.adapters.iron = {
    type = 'executable',
    command = opts.iron or 'iron',
    args = { 'dap' },
  }
  dap.configurations.iron = dap.configurations.iron or M.configurations()
  return true
end

return M
