# Iron LSP — VSCode extension changelog

All notable changes to the `iron-lsp` VSCode extension are documented
in this file.

## 4.0.0-alpha — 2026-05-31

Iron LSP extension updated for Iron v4 (memory-model overhaul). Requires
`ironls >= 4.0.0, < 5.0.0` (range enforced — older `ironls` is structurally
refused).

### New in this release

- Hover surfaces the new v4 memory-model regime (`val`/`var`, `heap`/`rc`/`weak rc`, `*T`/`*var T`, `readonly`, `nocopy`) on every binding.
- Completion proposes the 10 new v4 keywords (`heap`, `rc`, `weak`, `unchecked`, `defer`, `drop`, `copy`, `nocopy`, `leak`, `in`) with context-aware visibility.
- `defer free <binding>` snippet auto-emitted when the cursor is at the end of a `heap` allocation.
- 5 new quickfixes for v4 memory-model errors (add `val`, insert `defer free`, drop `var` modifier, use `weak rc`, remove `readonly` + extract helper).
- TextMate grammar updated for every v4 surface construct.

### Compatibility

- `ironls` server: `>= 4.0.0, < 5.0.0`. Older servers (3.x) are
  structurally refused at extension start (Phase 7 HARD-22 / D-10 gate).
- VSCode: `^1.92.0` (unchanged).

### Migration

See the [v3 → v4 migration guide](https://github.com/iron-lang/iron-lang/blob/main/docs/site/migration-v3-to-v4.md)
if your code stops compiling against the new `ironc`.

### Full release notes

The full v4.0.0-alpha release notes are
[in the iron-lang repo](https://github.com/iron-lang/iron-lang/blob/main/docs/release/v4.0.0-alpha.md).

### Additional changes in this release

- Bumped `ironLspCompatibleIronlsRange` to `">= 4.0.0, < 5.0.0"`; v3
  `ironls` binaries now hard-refused by the Phase 7 D-10 gate.
- Added snippet pack `snippets/iron.code-snippets` for Iron v4 idioms:
  `defer free <binding>` (Phase 21), `heap T(...)` constructor
  (Phase 21), `in arena { ... }` block (Phase 28). Triggers match
  Phase 34 LSP completion snippets for cross-surface consistency.
- Compatible with the Iron v4 memory model: `val`/`var` discipline,
  `heap`/`rc`/`weak rc` policies, `*T` / `*var T` / `*unchecked T`
  pointers, `[T; <=N]` bounded vectors, `drop`/`copy`/`nocopy` resource
  types, `defer` statement, arena allocation.
- Extension version bumped to `4.0.0-alpha` for v4 LSP compatibility.

## 0.1.0 — initial release

- LSP 3.17 client via `vscode-languageclient@9.0.1`; spawns `ironls`
  from `$PATH` or the `iron.languageServer.path` setting.
- TextMate syntax highlighting via
  `grammars/textmate/iron.tmLanguage.json` (drift-guarded against
  `src/lexer/lexer.c` keyword table; 10-fixture
  `vscode-tmgrammar-test` snapshot suite; `safe-regex` ReDoS lint).
- `grammars/textmate/language-configuration.json` consumed for
  comment markers, bracket auto-closing, indent rules, `onEnter`
  handling (rustfmt-style `///` doc-comment continuation).
- Grammar + language-config copied into `editors/vscode/` at
  `npm run prepackage` time — single source of truth stays in
  `grammars/textmate/`.
- Activation events: `onLanguage:iron` and
  `workspaceContains:**/iron.toml` only (never unconditional).
- Binary discovery (UI-SPEC S1): `iron.languageServer.path` setting →
  `$PATH` lookup via `which` / `where` → error toast with
  **Open Settings** action. `spawnSync` argv form;
  `fs.accessSync(X_OK)` pre-check (T-06-03-01 mitigation).
- Output channels (UI-SPEC S2): `Iron Language Server` (always) +
  `Iron Language Server (trace)` (lazy when
  `iron.languageServer.trace.server != "off"`).
- Command (UI-SPEC S3): `iron-lsp.diagnose` — opens a self-contained
  9-section bug-report payload in an untitled plaintext document
  (server binary, server version, client version, workspace root,
  active document, compatibility check, recent log events, recent
  log tail, platform).
- Settings (UI-SPEC S4): `iron.languageServer.path`,
  `iron.languageServer.trace.server`,
  `iron.languageServer.logLevel` (`error`/`warn`/`info`/`debug`).
- Structured JSON logging (UI-SPEC S5) with locked event vocabulary:
  `ext.activate`, `ironls.discovered`, `ironls.spawn.ok`,
  `lsp.initialize.ok`, `lsp.shutdown`. `src: 'vscode-ext'`.
- Debounced (500 ms) restart on `iron.languageServer.*`
  configuration changes (PITFALLS §2); `deactivate()` awaits
  `client.stop()` to prevent orphan server processes (PITFALLS §11).
- Version-range compatibility via the non-standard
  `ironLspCompatibleIronlsRange` field in `package.json`
  (UI-SPEC S9) — warning toast on mismatch. Phase 7 HARD-22
  promotes this to a hard refuse-to-load gate.
- `@vscode/test-electron` end-to-end harness asserting the shared
  `tests/editors/fixtures/diag_error.iron` fixture surfaces at least
  one Error-severity diagnostic within 10 s (CI matrix:
  `ubuntu-latest` + `macos-latest`; Linux runs under `xvfb`).
- Bundle: `esbuild` single-file `dist/extension.js` (~778 KB; CI
  gates under the 5 MB Marketplace ceiling per RESEARCH Pitfall 10).
- Packaging: `vsce package --no-dependencies` dry-run in CI every
  PR touching `editors/vscode/**` or `grammars/textmate/**`
  (`vscode-package-dryrun` job).
- Publisher: `iron-lang.iron-lsp` (EXT-05; manual publish per
  CONTEXT D-11 — see
  `docs/dev/publisher-namespace-checklist.md`).
- Version-range compatibility: `ironls 1.2.x`.
