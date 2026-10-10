-- editors/neovim/test/e2e/dap_test_block.lua
-- Headless check of the run / debug commands (#367): run by dap_harness.sh
-- as
--   nvim --headless -u NONE -l dap_test_block.lua <conditions.iron> <iron>
-- With the cursor inside the test block "sums the first ten", the test
-- under the cursor is found, the :Iron* commands exist, and :IronDebugTest
-- stops on the block's breakpoint (line 29) with total = 45.

local src, iron = arg[1], arg[2]

local function finish(code, msg)
  io.stdout:write(msg .. '\n')
  pcall(function() require('dap').terminate() end)
  vim.wait(1000)
  os.exit(code)
end

vim.cmd('filetype on')
vim.cmd('runtime plugin/iron_run.lua')
local iron_dap = require('iron_dap')
assert(iron_dap.setup({ iron = iron }), 'nvim-dap missing')
local dap = require('dap')

for _, name in ipairs({ 'IronRun', 'IronTest', 'IronTestFile', 'IronDebug', 'IronDebugTest' }) do
  if vim.fn.exists(':' .. name) ~= 2 then finish(1, 'FAIL: :' .. name .. ' is not defined') end
end

local stopped, output = nil, {}
dap.listeners.after.event_stopped['iron-test'] = function(session, body)
  stopped = { session = session, body = body }
end
dap.listeners.after.event_output['iron-test'] = function(_, body)
  table.insert(output, body.output or '')
end

vim.cmd('edit ' .. vim.fn.fnameescape(src))
vim.api.nvim_win_set_cursor(0, { 29, 0 })
dap.toggle_breakpoint()
vim.api.nvim_win_set_cursor(0, { 27, 0 })
local name = iron_dap.test_at_cursor()
if name ~= 'sums the first ten' then
  finish(1, 'FAIL: test under the cursor is ' .. vim.inspect(name))
end
vim.cmd('IronDebugTest')

if not vim.wait(180000, function() return stopped ~= nil end, 100) then
  local log = table.concat(output)
  if log:find('no debugger found') then finish(77, 'no DAP debugger (lldb-dap, gdb 14+): skipped') end
  finish(1, 'FAIL: :IronDebugTest did not stop on conditions.iron:29\n' .. log)
end

local session = stopped.session
local function request(cmd, args)
  local done, result, err = false, nil, nil
  session:request(cmd, args, function(e, r) err, result, done = e, r, true end)
  vim.wait(20000, function() return done end, 20)
  if err or not result then finish(1, 'FAIL: ' .. cmd .. ': ' .. vim.inspect(err)) end
  return result
end

local frame = request('stackTrace', { threadId = stopped.body.threadId }).stackFrames[1]
if not frame or frame.line ~= 29 then
  finish(1, 'FAIL: stopped at ' .. vim.inspect(frame and frame.line) .. ', want line 29')
end
local total = request('evaluate', { expression = 'total', frameId = frame.id, context = 'watch' })
if total.result ~= '45' then finish(1, 'FAIL: total = ' .. vim.inspect(total.result)) end
finish(0, 'nvim-dap: :IronDebugTest stopped in "sums the first ten" at line 29 with total = 45\nPASS')
