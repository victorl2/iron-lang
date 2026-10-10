-- editors/neovim/test/e2e/dap_test.lua
-- Headless nvim-dap check of the Iron debug adapter (#312, #347): run by
-- dap_harness.sh as
--   nvim --headless -u NONE -l dap_test.lua <stepping.iron> <iron>
-- Sets a breakpoint on line 3 of stepping.iron, starts the "Iron: debug
-- this file" configuration from lua/iron_dap.lua, and checks the stop in
-- area(w, h) and its locals (Iron names, no compiler temporaries).

local src, iron = arg[1], arg[2]

local function finish(code, msg)
  io.stdout:write(msg .. '\n')
  pcall(function() require('dap').terminate() end)
  vim.wait(1000)
  os.exit(code)
end

vim.cmd('filetype on')
assert(require('iron_dap').setup({ iron = iron }), 'nvim-dap missing')
local dap = require('dap')

local stopped, output = nil, {}
dap.listeners.after.event_stopped['iron-test'] = function(session, body)
  stopped = { session = session, body = body }
end
dap.listeners.after.event_output['iron-test'] = function(_, body)
  table.insert(output, body.output or '')
end

vim.cmd('edit ' .. vim.fn.fnameescape(src))
vim.api.nvim_win_set_cursor(0, { 3, 0 })
dap.toggle_breakpoint()
dap.run(require('iron_dap').configurations()[1])

if not vim.wait(180000, function() return stopped ~= nil end, 100) then
  local log = table.concat(output)
  if log:find('no debugger found') then finish(77, 'no DAP debugger (lldb-dap, gdb 14+): skipped') end
  finish(1, 'FAIL: no stop on stepping.iron:3\n' .. log)
end

local session = stopped.session
local function request(cmd, args)
  local done, result, err = false, nil, nil
  session:request(cmd, args, function(e, r) err, result, done = e, r, true end)
  vim.wait(20000, function() return done end, 20)
  if err or not result then finish(1, 'FAIL: ' .. cmd .. ': ' .. vim.inspect(err)) end
  return result
end

local frames = request('stackTrace', { threadId = stopped.body.threadId }).stackFrames
if not frames[1] or not frames[1].name:match('^area') then
  finish(1, 'FAIL: top frame is ' .. vim.inspect(frames[1] and frames[1].name))
end
local locals = {}
for _, scope in ipairs(request('scopes', { frameId = frames[1].id }).scopes) do
  if scope.name:lower() == 'locals' or scope.name:lower() == 'arguments' then
    for _, v in ipairs(request('variables', { variablesReference = scope.variablesReference }).variables) do
      locals[v.name] = v.value
    end
  end
end
if locals.w ~= '0' or locals.h ~= '2' or locals.product ~= '0' then
  finish(1, 'FAIL: locals ' .. vim.inspect(locals))
end
for name in pairs(locals) do
  if name:sub(1, 1) == '_' then finish(1, 'FAIL: temporary ' .. name .. ' listed') end
end
finish(0, 'nvim-dap: stopped in area at stepping.iron:3 with w = 0, h = 2, product = 0\nPASS')
