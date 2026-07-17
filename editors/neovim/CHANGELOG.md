# Iron LSP — Neovim configuration changelog

All notable changes to the `iron-lsp` Neovim configuration are
documented in this file.

## v4.0.0-alpha

- Bumped `IRON_LSP_COMPATIBLE_VERSION_RANGE` to
  `{ ">= 4.0.0", "< 5.0.0", min = "4.0.0", max_exclusive = "5.0.0" }`
  (lines 49-54); pre-v4 ironls binaries now hard-refused by the
  Phase 7 D-10 / HARD-22 gate via the `on_attach` detach path.
- Bumped `compatible_ironls` (line 106) and the diagnostic-event
  `range` field (line 133) from `">= 3.0.0, < 4.0.0"` to
  `">= 4.0.0, < 5.0.0"`.
- Added snippet pack `snippets/iron.snippets` (SnipMate-compatible;
  loadable via LuaSnip's `from_snipmate` loader or vim-snipmate /
  UltiSnips) for Iron v4 idioms: `defer free <binding>` (Phase 21),
  `heap T(...)` constructor (Phase 21), `in arena { ... }` block
  (Phase 28). Triggers match the Phase 34 LSP completion snippets
  and the VSCode + Zed packs.

## 0.1.0 — initial release

- Native `vim.lsp.config('ironls')` / `vim.lsp.enable('ironls')`
  integration (Neovim **0.11.3+**; the API landed in 0.11.3, not
  earlier 0.11.x — PITFALLS §11).
- `editors/neovim/lsp/ironls.lua` — canonical server config; returns a
  `vim.lsp.Config` table consumable by both the Neovim native API
  **and** nvim-lspconfig v2 (byte-identical shape; Assumption A4
  verified in Plan 06-04). The post-v1 upstream PR to
  `neovim/nvim-lspconfig` is tracked in
  `docs/dev/nvim-lspconfig-upstream.md`.
- Version guard: `editors/neovim/lsp/ironls.lua` calls
  `vim.fn.has('nvim-0.11.3')` at load time and emits a clear
  `vim.notify` ERROR (plus empty-table return) on older Neovim —
  matches nvim-lspconfig "server unavailable" convention.
- Root-directory resolution: `root_markers = { 'iron.toml', '.git' }`
  (`iron.toml` priority matches `ironc` / `iron` semantics per
  `src/pkg/*`).
- `editors/neovim/ftdetect/iron.lua`: maps `*.iron` → filetype
  `iron` + registers the tree-sitter parser via
  `vim.treesitter.language.register('iron', 'iron')`.
- `editors/neovim/plugin/iron_lsp.lua`: `:IronLspDiagnose` user
  command (UI-SPEC S3) writes the 9-section payload to a scratch
  buffer and hints at `"y` yank for bug reports.
- Structured JSON logging (UI-SPEC S5) to
  `$XDG_STATE_HOME/iron-lsp/client-nvim.log` with `src: 'neovim-ext'`
  and event vocabulary matching VSCode + Zed.
- Settings (UI-SPEC S4) reserved via the `settings = {}` field on
  the config table; populated server-side on initialize.
- `initializationOptions`: `{ clientName = 'neovim' }` (parity with
  VSCode extension).
- Version-range compatibility (UI-SPEC S9): non-standard
  `compatible_ironls = "1.2.0..<1.3.0"` field on the config table;
  plugin/diagnose consumes for warning. Phase 7 HARD-22 promotes
  to hard refuse.
- `plenary.nvim` end-to-end harness
  (`editors/neovim/test/e2e/diag_error_spec.lua`) asserts the shared
  `tests/editors/fixtures/diag_error.iron` fixture surfaces >= 1
  Error-severity diagnostic within 5 s (PITFALLS §6) and that
  `vim.lsp.get_active_clients({ bufnr = 0 })` is non-empty.
- CI `neovim-e2e` job (ubuntu + macos matrix): builds `ironls`,
  clones `plenary.nvim` depth-1, installs Neovim 0.11.3 (Linux via
  GitHub release tarball; macOS via `brew install neovim`), runs
  `editors/neovim/test/e2e/harness.sh`.
- Install documentation in `editors/neovim/README.md` covers three
  install flows: manual (runtimepath prepend), `lazy.nvim`,
  `pckr.nvim`. Includes tree-sitter parser install notes and
  `:checkhealth lsp` troubleshooting section.
- Version-range compatibility: `ironls 1.2.x`.
