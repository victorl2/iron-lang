# Iron Editor Extensions — Architecture

Developer-facing reference for the three editor extensions landed in
Phase 6 (M5 Grammars & Editor Extensions): VSCode, Neovim, and Zed.
All three share a single language-intelligence source (`ironls`) and
converge through the same LSP 3.17 wire protocol.

This document answers three questions:

1. **How do the editors talk to the compiler?** — the pipeline diagram
   and its invariants.
2. **How do I dev-reload each extension locally?** — per-editor
   workflows.
3. **How do I trace LSP wire messages when something goes wrong?** —
   per-editor troubleshooting hooks.

See also:
- [`editors/vscode/README.md`](../../editors/vscode/README.md) — user-facing VSCode install
- [`editors/neovim/README.md`](../../editors/neovim/README.md) — user-facing Neovim setup
- [`editors/zed/README.md`](../../editors/zed/README.md) — user-facing Zed install
- [`grammars/textmate/README.md`](../../grammars/textmate/README.md) — TextMate scope map + ReDoS rules
- [`grammars/tree-sitter/iron/README.md`](../../grammars/tree-sitter/iron/README.md) — tree-sitter build + WASM
- [`.planning/phases/06-m5-grammars-editor-extensions/06-UI-SPEC.md`](../../.planning/phases/06-m5-grammars-editor-extensions/06-UI-SPEC.md)
  (local-only) — cross-editor consistency invariants S1..S9

---

## Pipeline

```
User in editor (.iron file)
      │
      ▼
Editor-specific client  (TypeScript / Lua / Rust→WASM)
      │   stdio (LSP 3.17)
      ▼
ironls                 (Phase 2 binary, single source of LSP semantics)
      │   in-process (C17 linkage, static-lib)
      ▼
iron_compiler          (Phase 1 hardened static lib)
      │
      ▼
Diagnostics / hover / completion / definition / references / ... back over LSP wire
```

### Invariants enforced across all three editors

- **No editor extension contains `iron_compiler` logic.** Every semantic
  answer (diagnostic, hover, definition, rename...) flows through
  `ironls` — the C binary built from `src/lsp/`. This is the Core Value
  guarantee: `ironls` output matches `ironc` output byte-for-byte where
  their APIs overlap.
- **All three editors speak standard LSP 3.17.** No custom JSON-RPC
  methods. If a feature needs an LSP method we don't have, it lands in
  `src/lsp/handlers/` first; editors consume it automatically via
  `vscode-languageclient` / `vim.lsp` / `zed_extension_api`. This is
  how EXT-04 (VSCode), EXT-06 (Neovim), and EXT-08 (Zed) all get the
  same capability surface from one handler-table update.
- **tree-sitter runs editor-side, never in `ironls`.** The
  `grammars/tree-sitter/iron/iron.wasm` artifact drives highlighting
  + locals + folds inside the editor process. It never talks to
  `ironls`. (Phase 6 explicitly does NOT introduce a
  `textDocument/semanticTokens` handler — editor-side tree-sitter
  covers that surface for v1.)
- **TextMate grammar runs in VSCode's Oniguruma regex engine.** Same
  editor-side-only posture as tree-sitter; VSCode's sole consumer of
  `grammars/textmate/iron.tmLanguage.json`.
- **Keyword parity is enforced at build time via a CMake
  `configure_file` drift-guard.** See Plan 06-01:
  `test_grammar_keyword_drift_{textmate,tree_sitter}` run under
  `phase-m5-invariant` + `phase-m1-invariant` dual labels. Adding a
  keyword to `src/lexer/lexer.c` kw_table without running
  `scripts/regenerate-grammars.sh` fails CI.

---

## Per-editor surface

| Editor | Directory          | Client language        | Transport        | Grammar |
|--------|--------------------|------------------------|------------------|---------|
| VSCode | `editors/vscode/`  | TypeScript (bundled)   | `vscode-languageclient@9.0.1` over stdio | TextMate (copied at `npm run prepackage`) |
| Neovim | `editors/neovim/`  | Lua (native `vim.lsp`) | native stdio     | tree-sitter via `nvim-treesitter` (user-managed) |
| Zed    | `editors/zed/`     | Rust → `wasm32-wasip2` | native stdio     | tree-sitter via `iron.wasm` (fetched from GitHub Release) |

### VSCode

- Bundle format: `esbuild` → single `dist/extension.js` (~778 KB, well
  under the Marketplace 5 MB ceiling per RESEARCH Pitfall 10).
- Entry: `editors/vscode/src/extension.ts` activates on
  `onLanguage:iron` or `workspaceContains:**/iron.toml`.
- Transport: `vscode-languageclient@9.0.1` spawns `ironls` via
  `spawnSync` (argv form; never shell). Binary discovery:
  `iron.languageServer.path` setting → `PATH` lookup → error toast
  with **Open Settings** action (UI-SPEC S1).
- Commands: `iron-lsp.diagnose` (UI-SPEC S3) opens the 9-section
  bug-report payload in an untitled plaintext document.
- Settings: `iron.languageServer.path`,
  `iron.languageServer.trace.server`, `iron.languageServer.logLevel`
  (UI-SPEC S4).

### Neovim

- Floor: Neovim **0.11.3+** (native `vim.lsp.config()` API shipped in
  0.11.3). `editors/neovim/lsp/ironls.lua` checks `vim.fn.has('nvim-0.11.3')`
  at load time and emits a clear `vim.notify` ERROR otherwise (PITFALLS
  §11 — `has('nvim-0.11')` returns 1 on 0.11.0/0.11.1/0.11.2 where the
  API is still absent).
- Client: native `vim.lsp` (no plugin dep). User adds
  `editors/neovim/{lsp,ftdetect,plugin}` to `runtimepath` and calls
  `vim.lsp.enable('ironls')` from `init.lua`.
- Filetype registration: `editors/neovim/ftdetect/iron.lua` maps
  `*.iron` → filetype `iron` + registers the tree-sitter parser via
  `vim.treesitter.language.register('iron', 'iron')`.
- User commands: `:IronLspDiagnose` (UI-SPEC S3) writes the payload to
  a scratch buffer and hints at `"y` yank.
- Upstream path: `editors/neovim/lsp/ironls.lua` is byte-compatible
  with nvim-lspconfig v2 `lsp/*.lua` shape (Assumption A4 verified in
  Plan 06-04). The post-v1 upstream PR is tracked in
  [`docs/dev/nvim-lspconfig-upstream.md`](nvim-lspconfig-upstream.md).

### Zed

- Floor: Zed **0.200+** (for `zed_extension_api@0.7` / wasm32-wasip2).
- Extension target: `wasm32-wasip2` (Rust `cdylib` crate).
- Binary acquisition: downloads `ironls-v<version>-<os>-<arch>.tar.gz`
  from the matching `iron-lang/iron-lang` GitHub Release, verifies
  SHA-256 via a hand-rolled `sha2` digest **before** extraction
  (T-06-05-01 mitigation; `zed_extension_api::download_file` has no
  built-in verify — see RESEARCH Code Pattern 4 and Zed issue
  `zed-industries/zed#16732` closed not-planned).
- Cache: work_dir stores the extracted binary across sessions until
  version range changes.
- User override: `iron_lsp_path` extension setting (UI-SPEC S8).

---

## Dev-reload steps

### VSCode

```bash
cd editors/vscode
npm install
npm run prepackage       # copies grammars/textmate/*.json into editors/vscode/syntaxes
npm run bundle           # esbuild → dist/extension.js
```

Press **F5** in VSCode with `editors/vscode/` as the workspace — launches
an **Extension Development Host** with the in-progress extension
pre-loaded. Open any `.iron` file in the Development Host to trigger
activation; logs appear in `View → Output → Iron Language Server`.

Reload after code edits: `Developer: Reload Window` in the Development
Host command palette.

### Neovim

1. Ensure `editors/neovim/{lsp,ftdetect,plugin}` is on your runtimepath.
   Quickest option from the repo root:
   ```bash
   nvim -u NONE --cmd "set rtp+=$PWD/editors/neovim" -c "lua vim.lsp.enable('ironls')" path/to/file.iron
   ```
   Production integration lives in each user's `init.lua` or plugin
   manager (`lazy.nvim` / `pckr.nvim` — snippets in
   `editors/neovim/README.md`).
2. Reload after edits: `:source %` (when editing a loaded Lua file) or
   restart Neovim.
3. Verify: `:edit some.iron` then `:LspInfo` / `:checkhealth lsp` — the
   `ironls` server should show **attached** and **running**.

### Zed

```bash
cd editors/zed
cargo build --target wasm32-wasip2 --release
zed --dev-extension editors/zed/
```

Open a `.iron` file in the launched Zed instance — the extension
acquires `ironls` (from setting override or GitHub Release) and
spawns the language server.

Reload after edits: in Zed, open the command palette (`cmd-shift-p`
/ `ctrl-shift-p`) → **zed: reload**. Rebuild first if Rust sources
changed.

---

## LSP wire tracing

All three editors can emit the raw LSP wire log for debugging.

### VSCode

Set the workspace/user setting:

```json
"iron.languageServer.trace.server": "verbose"
```

Raw messages appear in `View → Output → Iron Language Server (trace)`.
The main output channel (`Iron Language Server`) shows structured
client-side events (UI-SPEC S5 vocabulary: `ext.activate`,
`ironls.discovered`, `ironls.spawn.ok`, `lsp.initialize.ok`,
`lsp.shutdown`).

### Neovim

```vim
:lua vim.lsp.set_log_level('debug')
```

Raw wire logs land in `$XDG_STATE_HOME/nvim/lsp.log` (typically
`~/.local/state/nvim/lsp.log`). Structured client-side events
(UI-SPEC S5) go to `$XDG_STATE_HOME/iron-lsp/client-nvim.log`.

Tail both in parallel when diagnosing:

```bash
tail -F ~/.local/state/nvim/lsp.log ~/.local/state/iron-lsp/client-nvim.log
```

### Zed

Open `View → Developer → Open Log` (or the command palette entry **zed:
open log**). Filter for `iron-lsp` to surface extension events. Raw
LSP wire messages flow through Zed's built-in language server channel
(logged at Zed's chosen verbosity — see `dev: logging` settings).

---

## Cross-editor consistency invariants

Reference:
`.planning/phases/06-m5-grammars-editor-extensions/06-UI-SPEC.md`
§"Cross-Editor Consistency Invariants" (planning doc, local-only).

Invariants enforced by review + CI spot-checks during Phase 6 closeout:

- **S3 Diagnose command** — same 9-section payload across
  `iron-lsp.diagnose` (VSCode), `:IronLspDiagnose` (Neovim), and
  `iron-lsp: diagnose` (Zed). Section ordering is locked; sections are
  `server binary`, `server version`, `client version`, `workspace root`,
  `active document`, `compatibility check`, `recent log events`,
  `recent log tail`, `platform`.
- **S4 Settings** — same `logLevel` enum
  (`error` / `warn` / `info` / `debug`) across editors.
- **S5 Log events** — same event vocabulary across editors
  (`ext.activate`, `ironls.discovered`, `ironls.spawn.ok`, ...). Only
  the `src` field changes per editor
  (`vscode-ext` / `neovim-ext` / `zed-ext`).
- **S1 Error wording** — `"{tool}: {problem}. {next-step}."` pattern
  (e.g. `"iron-lsp: ironls not found on PATH. Set
  iron.languageServer.path in Settings."`).

---

## Relation to the compiler

Phase 6 is **purely client-side**:

- Zero changes to `ironls`, `iron_compiler`, `iron_runtime`, `iron_stdlib`.
- Zero new `iron_analyze_buffer` call sites (CORE-22 parity invariant —
  single call site preserved at 1 across all Phase 2..6 plans).
- Zero new `iron_format_source` call sites (Plan 05-02 single-call-site
  invariant preserved).
- Zero new LSP request handlers.

The extensions consume the LSP 3.17 surface that landed in Phase 2
(transport + document sync + lifecycle), Phase 3 (navigation +
workspace), Phase 4 (editing assistance + quickfix + rename), and
Phase 5 (formatting). Phase 6's delta is entirely grammars + editor
clients + CI harnesses + publish infrastructure.

See `.planning/phases/06-m5-grammars-editor-extensions/06-CONTEXT.md`
D-13 for the formal "no new server-side contracts" statement.

---

## Further reading

- Phase 2 architecture: `src/lsp/README.md` (if present) or
  `.planning/phases/02-*/02-CONTEXT.md`
- LSP 3.17 spec:
  https://microsoft.github.io/language-server-protocol/specifications/lsp/3.17/specification/
- CMake grammar drift-guard:
  `CMakeLists.txt` (search for `IRON_BUILD_GRAMMARS`)
