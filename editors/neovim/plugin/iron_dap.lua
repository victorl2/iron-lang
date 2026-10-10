-- editors/neovim/plugin/iron_dap.lua
-- Registers the Iron debug adapter with nvim-dap when nvim-dap is
-- installed (#312, #347). Plugin managers that load nvim-dap lazily can
-- call require('iron_dap').setup() from their nvim-dap config instead.
local ok = pcall(require, 'dap')
if ok then
  require('iron_dap').setup()
end
