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
function M.setup(opts)
  opts = opts or {}
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
