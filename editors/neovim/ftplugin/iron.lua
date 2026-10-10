-- editors/neovim/ftplugin/iron.lua
-- Buffer settings for Iron source files (filetype 'iron').

if vim.b.did_ftplugin then
  return
end
vim.b.did_ftplugin = 1

-- Iron has `--` line comments and `///` doc comments; no block comments.
-- `gc` (Neovim 0.10+ commenting) and `gq` / `o` continuation read these.
vim.bo.commentstring = '-- %s'
vim.bo.comments = ':///,:--'
vim.opt_local.formatoptions:remove('t')
vim.opt_local.formatoptions:append('croql')

-- Four-space indent, no tabs (what the ironls formatter produces).
vim.bo.expandtab = true
vim.bo.shiftwidth = 4
vim.bo.tabstop = 4
vim.bo.softtabstop = 4

-- Brace-based indentation without a tree-sitter indent plugin: indent after
-- `{`, dedent on `}`. nvim-treesitter's indent module, when enabled, sets
-- its own indentexpr from queries/iron/indents.scm.
vim.bo.autoindent = true
vim.bo.smartindent = true
vim.bo.cinwords = ''

-- `%` jumps between brackets; `-` is not part of a word.
vim.bo.matchpairs = '(:),{:},[:]'
vim.bo.iskeyword = '@,48-57,_,192-255'

vim.b.undo_ftplugin = table.concat({
  'setlocal commentstring< comments< formatoptions<',
  'setlocal expandtab< shiftwidth< tabstop< softtabstop<',
  'setlocal autoindent< smartindent< cinwords< matchpairs< iskeyword<',
}, ' | ')

-- Tree-sitter highlighting starts by itself when the `iron` parser is
-- installed (nvim-treesitter, or iron.so under a `parser/` directory on the
-- runtimepath). Without it, syntax/iron.vim and the semantic tokens from
-- ironls highlight the buffer.
if vim.g.iron_treesitter ~= false then
  -- language.add returns true when a parser loads (nil plus an error
  -- message otherwise, or throws on older builds).
  local ok, loaded = pcall(vim.treesitter.language.add, 'iron')
  -- Without highlight queries tree-sitter would only switch syntax off.
  local has_queries = #vim.api.nvim_get_runtime_file('queries/iron/highlights.scm', false) > 0
  if ok and loaded and has_queries then
    pcall(vim.treesitter.start, 0, 'iron')
  end
end
