-- editors/neovim/ftdetect/iron.lua
-- Phase 6 Plan 06-04 Task 1 (EXT-06, CONTEXT D-05): filetype + tree-sitter
-- registration for .iron source files.
--
-- When this directory is on Neovim's &runtimepath, Neovim automatically
-- `source`s every `ftdetect/*.lua` file once at startup (see :help ftdetect).
-- No user-side config required beyond adding the directory to runtimepath.

-- Map *.iron -> filetype 'iron'. Uses vim.filetype.add (Neovim 0.8+), the
-- successor to `:au BufRead,BufNewFile *.iron set filetype=iron`.
vim.filetype.add({
  extension = {
    iron = 'iron',
  },
})

-- Register the tree-sitter parser for the 'iron' filetype. Because we use
-- 'iron' as BOTH the filetype AND the parser name (see
-- grammars/tree-sitter/iron/tree-sitter.json), either install path —
-- nvim-treesitter `:TSInstall iron` OR local build from
-- grammars/tree-sitter/iron/ — works without additional aliasing.
--
-- pcall tolerates absent tree-sitter runtime: users without any tree-sitter
-- install still get ftdetect + LSP diagnostics; they miss only
-- tree-sitter-powered highlighting.
pcall(vim.treesitter.language.register, 'iron', 'iron')
