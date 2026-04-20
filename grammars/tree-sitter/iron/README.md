# tree-sitter-iron

Phase 6 Plan 06-01 Task 3 (EXT-02, CONTEXT D-03). Tree-sitter grammar
for the [Iron](https://iron-lang.dev) programming language, consumed by
Neovim (via `nvim-treesitter`) and Zed (via the `zed_extension_api`
tree-sitter integration).

The grammar shape landed by Plan 06-01 is a **v0.1 permissive skeleton**
— it recognises comments, literals, strings, interpolated strings, and
all 38 Iron keywords (generated from `src/lexer/lexer.c kw_table`). Plan
06-02 replaces the `_top` permissive choice with `_declaration` + full
statement / expression / pattern rules transcribed from
`src/parser/parser.c`.

## Files

| File | Purpose |
|------|---------|
| `grammar.js.in` | `configure_file` template with `@IRON_KEYWORD_LIST_JS@` substitution |
| `grammar.js` | Committed generated grammar; drift-gated via `test_grammar_keyword_drift_tree_sitter` |
| `package.json` | npm manifest pinning `tree-sitter-cli@0.26.1` |
| `queries/highlights.scm` | Syntax-highlight captures (conventional names per UI-SPEC S7) |
| `queries/locals.scm` | Local-scope queries (skeleton in v0.1; full tree in 06-02) |
| `queries/folds.scm` | Code-folding queries (skeleton in v0.1) |
| `test/corpus/*.txt` | Hand-curated parse-tree fixtures (3 smoke cases; ≥ 20 in 06-02) |

## Build

Requires `tree-sitter-cli@0.26.x`. Install once:

```bash
cd grammars/tree-sitter/iron
npm install
```

Then:

```bash
npx tree-sitter generate      # regenerate src/parser.c from grammar.js
npx tree-sitter test           # run the corpus under test/corpus/
```

Generated `src/parser.c` is **not committed** — it is a reproducible
artifact from `grammar.js`. The committed `grammar.js` is drift-gated
against the lexer kw_table (see Regenerating below).

## Regenerating Keywords

If `src/lexer/lexer.c kw_table` gains or loses an entry, regenerate the
grammar:

```bash
./scripts/regenerate-grammars.sh
```

This runs the CMake `regenerate-grammars` target which re-runs the
`configure_file` over `grammar.js.in` and copies the output back over
`grammar.js`. The CTest `test_grammar_keyword_drift_tree_sitter` diffs
committed vs generated; drift is a hard CI failure.

## WASM Build (for Neovim / Zed)

Plan 06-02 adds the CI step that runs:

```bash
npx tree-sitter build --wasm
```

This produces `iron.wasm` consumed by Neovim's `nvim-treesitter` (via
`parser.install-info`) and Zed's tree-sitter runtime. On `tree-sitter-cli`
0.26.1+, `build --wasm` auto-downloads `wasi-sdk` on first run. The
`iron.wasm` artifact is published to the iron-lang GitHub Release
alongside `ironls` binaries.

## Consumers

- **Neovim** — Plan 06-04 ships `editors/neovim/lsp/ironls.lua` which
  registers the grammar via `vim.treesitter.language.register('iron',
  'iron')`. Users install the parser via `nvim-treesitter`'s
  `:TSInstall iron` once the upstream PR lands (post-v1 per CONTEXT
  D-05).
- **Zed** — Plan 06-05 ships `editors/zed/languages/iron/config.toml`
  pointing at `iron.wasm` fetched from the GitHub Release.

Both consumers use the highlight / locals / folds queries verbatim —
the capture names follow nvim-treesitter's canonical list so default
themes style Iron code out of the box.

## ABI Version

Tree-sitter ABI 15 (current default in 0.26.x). Documented here so
Neovim + Zed integrators can confirm compatibility.

## Minimum Versions

- Neovim 0.11.3+ (native `vim.treesitter.language.register` API)
- Zed 0.200+ (`zed_extension_api@0.7`)
- tree-sitter-cli 0.26.1 (pinned exact; see `package.json`)
