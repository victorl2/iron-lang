# Iron LSP — Neovim configuration

This directory ships the canonical Neovim integration for the Iron language
server (`ironls`). It is a single Lua config file consumed by Neovim's native
`vim.lsp.config()` / `vim.lsp.enable()` API — no plugin-wrapper layer, no
framework dependency.

**Tracks:** Iron v3.0.0-alpha.1 (current main-branch alpha). See the
**Version compatibility** section below for the exact `ironls` range the
config accepts (the hard-refuse constant bumps atomically in a later phase).

## Iron syntax overview

Iron v3 introduces first-class surface features surfaced by `ironls`
diagnostics, hover, completion, and rename — all exercised from Neovim via
the standard LSP client:

- **`init` blocks** — anonymous (`init(x: Int, y: Int) { ... }`) and named
  (`init Named(x: Int) { ... }`) constructors declared as first-class
  object members; replaces the v2 receiver-method initializer pattern.
- **`patch` extensions** — reopen an existing object or primitive type
  (`patch Int { ... }`, `patch Player { ... }`) to add methods.
- **`pub` visibility** — symbol-level export modifier distinguishing
  module-public from module-private decls.
- **`pure` methods** — side-effect-restricted method annotation used by
  the compiler for memoization + reordering safety.
- **`readonly` + `mut` mutation tiers** — transitive-readonly bindings
  and explicit mutable bindings; the type system enforces compatibility
  at call boundaries.

Legacy pre-v3 keywords (`val`, `var`, `object`, `interface`, `impl`, etc.)
continue to work unchanged. The complete roster is 44 keywords,
drift-guarded at build time by `test_grammar_keyword_drift_tree_sitter`.

The same `lsp/ironls.lua` file is byte-compatible with
[nvim-lspconfig][nvim-lspconfig] v2's `lsp/*.lua` contribution shape; a
future PR moves a copy of it upstream. The tracking doc for that PR is
`docs/dev/nvim-lspconfig-upstream.md` (post-v1).

[nvim-lspconfig]: https://github.com/neovim/nvim-lspconfig

---

## Requirements

- **Neovim 0.11.3 or newer.** The native `vim.lsp.config()` API was added
  in 0.11.3. On older Neovim, `lsp/ironls.lua` emits a visible error via
  `vim.notify` and returns an empty config; the server will not start. Use
  [Bob][bob] to manage Neovim versions without touching your OS package
  manager.
- **`ironls` on `PATH`**, _or_ configure an absolute path via
  `vim.lsp.config('ironls', { cmd = { '/full/path/to/ironls' } })` in your
  `init.lua`. Install `ironls` from https://iron-lang.dev/install.
- **Tree-sitter parser** (optional but recommended for syntax highlighting)
  — see [Tree-sitter](#tree-sitter) below.

[bob]: https://github.com/MordechaiHadad/bob

---

## Install

Three install flows. Pick one.

### Manual

Copy (or symlink) the `lsp/`, `ftdetect/`, and `plugin/` directories into
your Neovim runtimepath:

```sh
mkdir -p ~/.config/nvim/lsp ~/.config/nvim/ftdetect ~/.config/nvim/plugin
cp -r editors/neovim/lsp/ironls.lua     ~/.config/nvim/lsp/
cp -r editors/neovim/ftdetect/iron.lua  ~/.config/nvim/ftdetect/
cp -r editors/neovim/plugin/iron_lsp.lua ~/.config/nvim/plugin/
```

Then add to `~/.config/nvim/init.lua`:

```lua
vim.lsp.enable('ironls')
```

### lazy.nvim

Add a spec entry pointing at this repo (or any fork). `lazy.nvim` puts the
plugin's root on `&runtimepath`, which makes `lsp/ironls.lua`,
`ftdetect/iron.lua`, and `plugin/iron_lsp.lua` auto-discovered:

```lua
{
  'iron-lang/iron-lang',
  -- Scope to .iron buffers so Neovim does not load the plugin for other filetypes.
  ft = 'iron',
  config = function()
    vim.lsp.enable('ironls')
  end,
}
```

### pckr.nvim

```lua
require('pckr').add({
  { 'iron-lang/iron-lang',
    config = function()
      vim.lsp.enable('ironls')
    end,
  },
})
```

---

## Activate

After one of the install flows, verify the server attaches:

1. Open any `.iron` file.
2. `:checkhealth lsp` — under **Active Clients**, expect an entry named
   `ironls`.
3. Type a deliberate error (`val x: int = undefined_symbol`) — a diagnostic
   should surface within ~1 second.

If step 2 shows no `ironls` client, run `:IronLspDiagnose` (see below) and
copy the output into a bug report.

---

## Tree-sitter

Two install paths. Either works — the ftdetect script calls
`vim.treesitter.language.register('iron', 'iron')`, so the parser name is
just `iron` regardless of install source.

1. **nvim-treesitter** (recommended once the upstream parser registration
   PR lands — tracked post-v1):

   ```lua
   -- After Iron is registered in nvim-treesitter/parsers:
   require('nvim-treesitter.configs').setup({
     ensure_installed = { 'iron' },
     highlight = { enable = true },
   })
   ```

   Until that PR lands, nvim-treesitter users can point at the in-tree
   grammar as a custom parser (see nvim-treesitter's own "Adding parsers"
   docs).

2. **Local build** from the in-tree grammar:

   ```sh
   cd grammars/tree-sitter/iron
   npm install
   npx tree-sitter generate
   npx tree-sitter build --wasm
   ```

   Then copy `iron.wasm` + the `queries/` directory into your
   nvim-treesitter parser directory (usually
   `~/.local/share/nvim/site/parser/` and
   `~/.local/share/nvim/site/queries/iron/`).

---

## `:checkhealth lsp`

Neovim's built-in `:checkhealth lsp` is the smoke gate for the config. A
healthy state looks like:

```
vim.lsp: Active Clients
- ironls (id: 1)
  - Root directory: /abs/path/to/your/project
  - Command: { "ironls" }
  - Settings: vim.empty_dict()
```

If `ironls` is not listed, check: (a) you opened a `.iron` file before
running `:checkhealth`; (b) the server is on `PATH`; (c) your Neovim is
0.11.3+.

---

## `:IronLspDiagnose`

Run `:IronLspDiagnose` in Neovim to produce a bug-report payload (UI-SPEC
S3 — same contract as the VSCode and Zed extensions). The result is:

1. Printed to `:messages` (view with `:messages`).
2. Copied to the `+` register (system clipboard — paste anywhere).

Include the payload verbatim when filing an issue at
https://github.com/iron-lang/iron-lang/issues.

---

## Troubleshooting

### `[iron-lsp] ironls not found on PATH.`

The server binary is not on `PATH`. Either install it
(https://iron-lang.dev/install) or set an absolute path:

```lua
vim.lsp.config('ironls', {
  cmd = { '/full/path/to/ironls' },
})
vim.lsp.enable('ironls')
```

### `[iron-lsp] configured ironls path "..." is not executable.`

The `cmd[1]` you provided does not resolve to an executable file. Check
the path and the file's exec bit (`chmod +x`).

### `[iron-lsp] Neovim 0.11.3+ is required (found X.Y.Z).`

Upgrade via your package manager or use [Bob][bob] to install a newer
Neovim alongside the system one.

### `attempt to call field 'config' (a nil value)`

You are on Neovim < 0.11.3 and something else is calling `vim.lsp.config`
before our version guard gets a chance to `return {}`. Upgrade Neovim.

### Version mismatch (`[iron-lsp] detected ironls X.Y.Z, but this config requires …`)

Phase 7 HARD-22 / D-10 / UI-SPEC S9 — when the attached `ironls` reports
a `serverInfo.version` outside the range `>= 1.2.0, < 2.0.0`
(`IRON_LSP_COMPATIBLE_VERSION_RANGE` in `lsp/ironls.lua`), the
`on_attach` hook refuses the attach:

1. `vim.notify` ERROR surfaces the detected version, the compatible
   range, and a one-line install command.
2. `vim.lsp.buf_detach_client(bufnr, client.id)` removes the client
   from the buffer so no language features fire.

To recover: install the latest `ironls` from
<https://github.com/iron-lang/iron-lang/releases/latest>, then reopen
the `.iron` buffer (or restart Neovim). The detach is buffer-scoped;
the LSP client is not silently left running in a half-active state.

---

## Version compatibility

This config targets `ironls` in `>= 1.2.0, < 2.0.0` per the
`IRON_LSP_COMPATIBLE_VERSION_RANGE` constant in `lsp/ironls.lua`. The
`on_attach` hook enforces the range as a hard refuse (Phase 7 HARD-22
/ D-10). Minor/patch bumps within `1.2.x` / `1.3.x` / `1.999.x` are
compatible by definition; a `2.0.0` release signals breaking LSP
semantics and will require an updated config.

---

## `nvim-lspconfig` upstream

The `lsp/ironls.lua` file in this directory matches the
[nvim-lspconfig v2 CONTRIBUTING][contrib] contribution shape byte-for-byte
(return a `vim.lsp.Config`-typed table with `cmd` / `filetypes` /
`root_markers` / `settings`). After ≥ 1 external user validates this
config, a PR moves a copy to `nvim-lspconfig/lsp/ironls.lua` upstream.
Tracking lives in `docs/dev/nvim-lspconfig-upstream.md` (landed in Plan
06-06; post-v1 follow-up).

[contrib]: https://github.com/neovim/nvim-lspconfig/blob/master/CONTRIBUTING.md

---

## Layout

```
editors/neovim/
├── README.md                       # this file
├── lsp/
│   └── ironls.lua                  # canonical vim.lsp.Config (user entry point)
├── ftdetect/
│   └── iron.lua                    # *.iron -> filetype 'iron' + tree-sitter register
├── plugin/
│   └── iron_lsp.lua                # :IronLspDiagnose + S5 log helper
└── test/
    └── e2e/
        ├── diag_error_spec.lua     # plenary.nvim e2e test
        └── harness.sh              # CI driver (nvim --headless + plenary)
```
