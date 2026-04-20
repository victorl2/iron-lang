# tree-sitter-iron

Tree-sitter grammar for the [Iron](https://iron-lang.dev) programming
language, consumed by Neovim (via `nvim-treesitter`) and Zed (via the
`zed_extension_api` tree-sitter integration). VSCode does NOT consume
this grammar — it uses TextMate via `editors/vscode/syntaxes/iron.tmLanguage.json`
(Plan 06-03).

Phase 6 Plan 06-02 delivers the full parseable grammar covering every
construct in `tests/integration/*.iron` (376 / 377 files clean; one
documented v1 gap below). Plan 06-01 shipped the Wave 0 skeleton
(keyword-drift wiring + minimal `_top` alternation); this plan
replaces that with a real structural parse tree.

## Files

| File | Purpose |
|------|---------|
| `grammar.js.in` | `configure_file` template with `@IRON_KEYWORD_LIST_JS@` substitution |
| `grammar.js` | Committed generated grammar; drift-gated via `test_grammar_keyword_drift_tree_sitter` |
| `tree-sitter.json` | Grammar metadata + ABI 15 activation per tree-sitter 0.26.x contract |
| `package.json` | npm manifest pinning `tree-sitter-cli` |
| `queries/highlights.scm` | Syntax-highlight captures (28+ rows, UI-SPEC S7, nvim-treesitter canonical names) |
| `queries/locals.scm` | Local-scope / definition / reference captures — enables declaration-vs-use differentiation |
| `queries/folds.scm` | Code-folding captures for all container rules |
| `test/corpus/*.txt` | 42 hand-curated parse-tree fixtures across 7 topic files |

## Building

Prerequisites: Node 20 LTS (tree-sitter-cli pulls its CLI binary via
npm; npm must be available at install time).

```bash
cd grammars/tree-sitter/iron
npm install                    # installs tree-sitter-cli locally
npx tree-sitter generate       # produces src/parser.c (gitignored)
npx tree-sitter test           # runs the 42-fixture corpus
```

Generated `src/parser.c` is **not committed** — it is a reproducible
artifact from `grammar.js`. The committed `grammar.js` is drift-gated
against the lexer kw_table (see [Regenerating](#regenerating-after-lexer-changes)).

## WASM Build

```bash
npx tree-sitter build --wasm
```

Produces `iron.wasm` consumed by Neovim's `nvim-treesitter` (via
`parser.install-info`) and Zed's tree-sitter runtime. On
`tree-sitter-cli@0.26.x`, `build --wasm` **auto-downloads `wasi-sdk`
on first run** (requires internet; cached in the user data dir).
If the first run is slow this is expected — it's a one-time 150 MB
wasi-sdk fetch.

CI (Plan 06-02 `tree-sitter-wasm` job) runs this step on every PR
touching `grammars/tree-sitter/**` and uploads `iron.wasm` as an
artifact named `iron-wasm-<sha>` with 7-day retention. Plan 06-06
will attach the Release-tagged artifact to the iron-lang GitHub
Releases for downstream consumer pinning.

## ABI Contract

- **ABI 15** — current default for tree-sitter-cli 0.26.x. Activation
  requires `tree-sitter.json` alongside `grammar.js` (older CLIs fall
  back to ABI 14 silently).
- **Minimum Neovim:** 0.11.3 — supports ABI 14+ in the core runtime;
  0.11.3 is the first release with the `vim.treesitter.language.register`
  API Plan 06-04 uses.
- **Minimum Zed:** 0.200 — bundles a tree-sitter runtime compatible
  with ABI 15 via `zed_extension_api@0.7`.
- **CLI pins:** The local-dev `package.json` pins `tree-sitter-cli@0.25.10`
  (glibc 2.34+ compatible; ABI 15 supported). CI pins
  `tree-sitter-cli@0.26.3` exactly — avoids accidental ABI 16 upgrade if
  tree-sitter bumps its default. Both produce byte-identical grammar
  artifacts because `grammar.js` is the source of truth; the CLI is only
  a code generator.

## Consumers

- **Neovim** — Plan 06-04 ships `editors/neovim/lsp/ironls.lua` which
  registers the grammar via `vim.treesitter.language.register('iron',
  'iron')`. Users install the parser via `nvim-treesitter`'s
  `:TSInstall iron` (once the upstream PR lands — see CONTEXT D-05;
  post-v1 per the plan), or by pointing at the Release artifact
  directly.
- **Zed** — Plan 06-05 ships `editors/zed/languages/iron/config.toml`
  pointing at `iron.wasm` fetched from the matching iron-lang GitHub
  Release per CONTEXT D-06 + D-11.

Both consumers use the highlight / locals / folds queries verbatim —
the capture names follow nvim-treesitter's canonical list so default
themes style Iron code out of the box with no user theme hacking.

## Regenerating After Lexer Changes

If `src/lexer/lexer.c kw_table` gains or loses an entry:

```bash
./scripts/regenerate-grammars.sh
```

This runs the CMake `regenerate-grammars` target which:
1. Re-runs `configure_file` over `grammar.js.in` with the updated
   keyword list substituted into `@IRON_KEYWORD_LIST_JS@`.
2. Copies the result back over the committed `grammar.js` via
   `cmake -E copy_if_different`.

Then rebuild the parser locally:

```bash
cd grammars/tree-sitter/iron
npx tree-sitter generate       # rebuilds src/parser.c (gitignored)
```

Commit `grammar.js` (drift-gated); do NOT commit `src/parser.c`.
The CTest `test_grammar_keyword_drift_tree_sitter` diffs
committed-vs-generated; drift is a hard CI failure.

## Testing

```bash
# Local — from repo root
ctest --test-dir build -R test_tree_sitter -j4
# corpus fixtures:
ctest --test-dir build -R test_tree_sitter_corpus --output-on-failure
# integration-corpus parse gate (D-12 structural parity fence):
ctest --test-dir build -R test_tree_sitter_parses_integration_corpus --output-on-failure
```

Both tests are registered under the `phase-m5-invariant` CTest label
(CONTEXT D-10) and run by default in CI.

## Troubleshooting

- **"Parse ERROR in test X.iron"** — the grammar does not yet cover a
  construct the Iron parser accepts. File an issue citing the specific
  `.iron` file; the structural-parity fence (`test_tree_sitter_parses_integration_corpus`)
  is designed to surface exactly this. Plan 06-02 covers the v1
  syntactic surface; future phases expand.
- **"tree-sitter build --wasm fails to download wasi-sdk"** — typically
  CI network hiccup or corporate proxy. Retry the workflow step.
  CONTEXT D-11 notes this as a known first-run slow path; the wasi-sdk
  is cached after the first fetch so subsequent builds are fast.
- **"Invalid node type break" in queries/highlights.scm** — anonymous
  keyword tokens that are the full body of a rule (`break_statement: $
  => 'break'`) surface only as node kinds (`(break_statement)`), not
  as anonymous terminals reachable via `"break" @capture`. Use the
  node-kind form when the keyword has no other parse context.

## Known Gaps (v1)

- `tests/integration/url_parse_basic.iron` — contains
  `"{Url.default_port(\"http\")}"`: backslash-escaped double quotes
  inside a string interpolation. Iron's lexer handles this in
  `IRON_TOK_INTERP_STRING`'s single-token escape decoding; tree-sitter
  would need an **external scanner** in `bindings/src/scanner.c` to
  match the same shape. Deferred to post-v1 (tracked in the Plan
  06-02 summary under "Known Gaps"). Currently skipped via the
  `parse_fixtures.sh` allowlist — this is 1 of 377 integration
  fixtures (99.7% coverage).
