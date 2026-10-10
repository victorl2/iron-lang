-- editors/neovim/plugin/iron_run.lua
-- Run and debug commands for Iron files (#367):
--   :IronRun                  build and run the current file in a terminal
--   :IronTest [name]          run one test (the name, else the test under
--                             the cursor), or every test of the file
--   :IronTestFile             run every test of the file
--   :IronDebug                debug the current file (nvim-dap)
--   :IronDebugTest [name]     debug one test alone (nvim-dap)
local function iron() return require('iron_dap') end

vim.api.nvim_create_user_command('IronRun', function() iron().run_file() end,
  { desc = 'Iron: build and run the current file' })
vim.api.nvim_create_user_command('IronTest', function(o) iron().test(o.args) end,
  { nargs = '?', desc = 'Iron: run the test under the cursor (or the named one)' })
vim.api.nvim_create_user_command('IronTestFile', function()
  local file = vim.api.nvim_buf_get_name(0)
  if file:match('%.iron$') then iron().run_in_terminal({ 'test', file }) end
end, { desc = 'Iron: run every test of the current file' })
vim.api.nvim_create_user_command('IronDebug', function() iron().debug_file() end,
  { desc = 'Iron: debug the current file' })
vim.api.nvim_create_user_command('IronDebugTest', function(o) iron().debug_test(o.args) end,
  { nargs = '?', desc = 'Iron: debug the test under the cursor (or the named one)' })
