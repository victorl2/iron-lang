-- editors/neovim/test/e2e/features_spec.lua
--
-- Drives the shipped Neovim integration (lsp/ironls.lua, ftdetect,
-- ftplugin, plugin, syntax) against a real ironls on
-- tests/editors/fixtures/features.iron and checks each editing feature the
-- way Neovim uses it: requests go through the attached client, edits are
-- applied with Neovim's own helpers, and highlights are read back from the
-- buffer.
--
-- Run by harness.sh (plenary.nvim, headless).

local root = vim.fn.getcwd() -- harness.sh cds to the checkout root
local fixture = root .. '/tests/editors/fixtures/features.iron'

local state = {}

local function setup_once()
  if state.client then return end
  vim.o.swapfile = false
  vim.opt.rtp:prepend(root .. '/editors/neovim')
  if vim.env.IRON_TS_RTP then
    vim.opt.rtp:append(vim.env.IRON_TS_RTP) -- parser/iron.so built by harness.sh
  end
  vim.cmd('filetype plugin on')
  vim.cmd('syntax on')
  dofile(root .. '/editors/neovim/ftdetect/iron.lua')
  dofile(root .. '/editors/neovim/plugin/iron_lsp.lua')
  local cfg = dofile(root .. '/editors/neovim/lsp/ironls.lua')
  vim.lsp.config('ironls', cfg)
  vim.lsp.enable('ironls')
  vim.cmd('edit ' .. fixture)
  state.buf = vim.api.nvim_get_current_buf()
  vim.wait(5000, function()
    state.client = vim.lsp.get_clients({ bufnr = state.buf, name = 'ironls' })[1]
    return state.client ~= nil and state.client.initialized
  end, 20)
  assert(state.client, 'ironls did not attach to features.iron')
  state.uri = vim.uri_from_bufnr(state.buf)
end

-- Reload the fixture from disk (undoes edits; Neovim sends the change).
local function reset()
  vim.api.nvim_set_current_buf(state.buf)
  vim.cmd('silent edit!')
  vim.wait(300)
end

-- 0-based (line, col) of the first occurrence of `text`, plus `offset`.
local function find(text, offset)
  for i, l in ipairs(vim.api.nvim_buf_get_lines(state.buf, 0, -1, false)) do
    local s = l:find(text, 1, true)
    if s then return i - 1, s - 1 + (offset or 0) end
  end
  error('fixture text not found: ' .. text)
end

local function request(method, params)
  local r = state.client:request_sync(method, params, 5000, state.buf)
  assert(r, method .. ' timed out')
  assert(not r.err, method .. ' failed: ' .. vim.inspect(r.err))
  return r.result
end

local function at(line, col)
  return { textDocument = { uri = state.uri }, position = { line = line, character = col } }
end

local function whole_file()
  return { start = { line = 0, character = 0 },
           ['end'] = { line = vim.api.nvim_buf_line_count(state.buf), character = 0 } }
end

local function errors()
  return vim.diagnostic.get(state.buf, { severity = vim.diagnostic.severity.ERROR })
end

-- Replace line `line` (0-based) and wait for ironls to see the change.
local function set_line(line, text)
  vim.api.nvim_buf_set_lines(state.buf, line, line + 1, false, { text })
  vim.wait(400)
end

local function inlay_labels()
  local hints = request('textDocument/inlayHint',
    { textDocument = { uri = state.uri }, range = whole_file() }) or {}
  local labels = {}
  for _, h in ipairs(hints) do
    local label = h.label
    if type(label) == 'table' then
      label = table.concat(vim.tbl_map(function(p) return p.value end, label))
    end
    labels[label] = true
  end
  return labels
end

describe('iron-lsp in Neovim', function()
  before_each(setup_once)

  it('detects the filetype and applies the ftplugin', function()
    assert.are.equal('iron', vim.bo[state.buf].filetype)
    assert.are.equal('-- %s', vim.bo[state.buf].commentstring)
    assert.are.equal(4, vim.bo[state.buf].shiftwidth)
    assert.is_true(vim.bo[state.buf].expandtab)
  end)

  it('advertises every editing feature the client uses', function()
    local caps = state.client.server_capabilities
    for _, k in ipairs({
      'hoverProvider', 'completionProvider', 'signatureHelpProvider',
      'definitionProvider', 'referencesProvider', 'renameProvider',
      'semanticTokensProvider', 'inlayHintProvider', 'documentFormattingProvider',
      'codeActionProvider', 'foldingRangeProvider', 'documentSymbolProvider',
    }) do
      assert.is_truthy(caps[k], 'missing capability ' .. k)
    end
  end)

  it('starts with no diagnostics and reports an error once, while typing', function()
    reset()
    vim.wait(3000, function() return #errors() == 0 end, 50)
    assert.are.equal(0, #errors(), vim.inspect(errors()))
    local l = find('val total')
    set_line(l, '    val total: Int = undefined_thing')
    assert.is_true(vim.wait(3000, function() return #errors() > 0 end, 50),
      'no diagnostic after typing an undefined name')
    -- Exactly one: push and pull diagnostics must not both be shown.
    vim.wait(800)
    assert.are.equal(1, #errors(), vim.inspect(errors()))
    reset()
    assert.is_true(vim.wait(3000, function() return #errors() == 0 end, 50),
      'diagnostic did not clear after undoing the edit')
  end)

  it('hovers a call with its signature', function()
    reset()
    local hover = request('textDocument/hover', at(find('area(2.0', 1)))
    local text = type(hover.contents) == 'table' and (hover.contents.value or '') or hover.contents
    assert.matches('func area%(width: Float, height: Float%) %-> Float', text)
  end)

  it('jumps to the definition of a function', function()
    reset()
    local res = request('textDocument/definition', at(find('area(2.0', 1)))
    local loc = res[1] or res
    local range = loc.targetSelectionRange or loc.range
    assert.are.equal((find('func area')), range.start.line)
  end)

  it('completes locals, members on a stdlib type and prelude functions', function()
    reset()
    local l = find('println(name.replace')
    local function labels(line_text, col)
      set_line(l, line_text)
      local res = request('textDocument/completion', at(l, col))
      local items = res.items or res
      local set = {}
      for _, it in ipairs(items) do set[it.label] = true end
      return set
    end
    local locals = labels('        na', 10)
    assert.is_true(locals.name and locals.names, 'loop variable / local missing')
    local members = labels('        name.re', 15)
    assert.is_true(members.replace and members['repeat'], 'String members missing')
    local prelude = labels('        printl', 14)
    assert.is_true(prelude.println, 'prelude println missing')
  end)

  it('shows signature help while arguments are typed', function()
    reset()
    local l = find('println(name.replace')
    set_line(l, '        name.replace("a", ')
    local sig = request('textDocument/signatureHelp', at(l, 26))
    assert.matches('replace%(old: String, new: String%)', sig.signatures[1].label)
    assert.are.equal(1, sig.activeParameter or sig.signatures[1].activeParameter)
  end)

  it('finds references to a field across bodies', function()
    reset()
    local l, c = find('val x: Float', 4)
    local p = at(l, c)
    p.context = { includeDeclaration = true }
    local refs = request('textDocument/references', p)
    -- declaration, self.x twice, origin.x
    assert.are.equal(4, #refs)
  end)

  it('renames a field everywhere and leaves no errors', function()
    reset()
    local l, c = find('val x: Float', 4)
    local p = at(l, c)
    p.newName = 'px'
    local edit = request('textDocument/rename', p)
    vim.lsp.util.apply_workspace_edit(edit, state.client.offset_encoding)
    vim.wait(300)
    local text = table.concat(vim.api.nvim_buf_get_lines(state.buf, 0, -1, false), '\n')
    assert.matches('val px: Float', text)
    assert.matches('self%.px %* self%.px', text)
    assert.matches('{origin%.px}', text)
    assert.is_nil(text:find('%f[%w_]x%f[^%w_]%s*:'), 'a declaration of x survived')
    vim.wait(1500)
    assert.are.equal(0, #errors(), vim.inspect(errors()))
    reset()
  end)

  it('turns inlay hints on, and each kind off with :IronInlayHints', function()
    reset()
    assert.is_true(vim.lsp.inlay_hint.is_enabled({ bufnr = state.buf }),
      'inlay hints should be enabled in Iron buffers')
    local labels = inlay_labels()
    assert.is_true(labels['width:'] and labels['height:'], 'parameter hints missing')
    assert.is_true(labels['x:'], 'construction parameter hint missing')
    assert.is_true(labels[': Float'], 'binding type hint missing')

    vim.cmd('IronInlayHints parameterNames off')
    vim.wait(300)
    labels = inlay_labels()
    assert.is_nil(labels['width:'], 'parameter hints still shown after turning them off')
    assert.is_true(labels[': Float'], 'type hints should stay')

    vim.cmd('IronInlayHints bindingTypes off')
    vim.wait(300)
    labels = inlay_labels()
    assert.is_nil(labels[': Float'], 'type hints still shown after turning them off')

    vim.cmd('IronInlayHints parameterNames on')
    vim.cmd('IronInlayHints bindingTypes on')
    vim.wait(300)
    labels = inlay_labels()
    assert.is_true(labels['width:'] and labels[': Float'], 'hints did not come back')
  end)

  it('passes the inlay hint settings at initialize', function()
    -- before_init copies settings.iron.inlayHints into
    -- initializationOptions so the first response already honors them.
    local cfg = dofile(root .. '/editors/neovim/lsp/ironls.lua')
    local params = {}
    cfg.before_init(params, { settings = { iron = { inlayHints = { parameterNames = false } } } })
    assert.are.same({ parameterNames = false }, params.initializationOptions.inlayHints)
  end)

  it('applies semantic tokens to the buffer', function()
    reset()
    local tokens = request('textDocument/semanticTokens/full', { textDocument = { uri = state.uri } })
    assert.is_true(#tokens.data > 0)
    local l, c = find('area(2.0', 1)
    local found = vim.wait(3000, function()
      for _, t in ipairs(vim.lsp.semantic_tokens.get_at_pos(state.buf, l, c) or {}) do
        if t.type == 'function' then return true end
      end
      return false
    end, 50)
    assert.is_true(found, 'no function token highlighted on the area() call')
  end)

  it('lists document symbols and folding ranges', function()
    reset()
    local symbols = request('textDocument/documentSymbol', { textDocument = { uri = state.uri } })
    local names = {}
    for _, s in ipairs(symbols) do names[s.name] = true end
    assert.is_true(names.Point and names.area and names.main, vim.inspect(vim.tbl_keys(names)))
    local folds = request('textDocument/foldingRange', { textDocument = { uri = state.uri } })
    local point = find('object Point')
    local has = false
    for _, f in ipairs(folds) do
      if f.startLine == point then has = true end
    end
    assert.is_true(has, 'no folding range for object Point')
  end)

  it('formats the buffer', function()
    reset()
    local l = find('val total')
    set_line(l, ' val total = area(2.0, 3.0)')
    vim.lsp.buf.format({ bufnr = state.buf, async = false, timeout_ms = 5000 })
    assert.are.equal('    val total = area(2.0, 3.0)',
      vim.api.nvim_buf_get_lines(state.buf, l, l + 1, false)[1])
    reset()
  end)

  it('offers a quick fix for a misspelled name', function()
    reset()
    local l = find('println("{origin')
    vim.api.nvim_buf_set_lines(state.buf, l, l, false, { '    println(totl)' })
    assert.is_true(vim.wait(3000, function() return #errors() > 0 end, 50))
    local d = errors()[1].user_data.lsp
    local actions = request('textDocument/codeAction', {
      textDocument = { uri = state.uri }, range = d.range,
      context = { diagnostics = { d } },
    })
    local titles = vim.tbl_map(function(a) return a.title end, actions or {})
    local ok = false
    for _, t in ipairs(titles) do
      if t:find('total', 1, true) then ok = true end
    end
    assert.is_true(ok, 'no quick fix naming `total`: ' .. vim.inspect(titles))
    reset()
  end)

  it('highlights keywords without a tree-sitter parser (syntax/iron.vim)', function()
    -- Every keyword of the lexer (mirrored in the tree-sitter grammar's
    -- _keyword rule) appears in syntax/iron.vim.
    local grammar = table.concat(vim.fn.readfile(root .. '/grammars/tree-sitter/iron/grammar.js'), '\n')
    local list = grammar:match('_keyword: %$ => choice%((.-)%),')
    assert.is_truthy(list, '_keyword rule not found in grammar.js')
    local syntax = table.concat(vim.fn.readfile(root .. '/editors/neovim/syntax/iron.vim'), '\n')
    local missing = {}
    for kw in list:gmatch("'([%w_]+)'") do
      if not syntax:find('%f[%w_]' .. kw .. '%f[^%w_]') then table.insert(missing, kw) end
    end
    assert.are.same({}, missing)

    if vim.b[state.buf].ts_highlight then return end -- tree-sitter owns the buffer
    local l, c = find('func area')
    local names = vim.tbl_map(function(id) return vim.fn.synIDattr(id, 'name') end,
      vim.fn.synstack(l + 1, c + 1))
    assert.are.same({ 'ironKeyword' }, names)
  end)

  it('highlights with tree-sitter when the iron parser is installed', function()
    local ok, loaded = pcall(vim.treesitter.language.add, 'iron')
    if not (ok and loaded) then
      pending('iron tree-sitter parser not installed (harness builds it when tree-sitter-cli is available)')
      return
    end
    if #vim.api.nvim_get_runtime_file('queries/iron/highlights.scm', false) == 0 then
      pending('no queries/iron/highlights.scm on the runtimepath')
      return
    end
    reset()
    assert.is_truthy(vim.b[state.buf].ts_highlight, 'ftplugin did not start tree-sitter highlighting')
    local function captures(text, offset)
      local l, c = find(text, offset)
      return vim.tbl_map(function(cap) return cap.capture end,
        vim.treesitter.get_captures_at_pos(state.buf, l, c))
    end
    assert.is_true(vim.tbl_contains(captures('func area'), 'keyword.function'))
    assert.is_true(vim.tbl_contains(captures('object Point', 7), 'type.definition'))
    assert.is_true(vim.tbl_contains(captures('length_sq()}', 0), 'function.method.call'))
    assert.is_true(vim.tbl_contains(captures('-- Shared'), 'comment'))
  end)
end)
