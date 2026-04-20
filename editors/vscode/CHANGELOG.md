# Changelog

All notable changes to the `iron-lsp` VSCode extension are documented in
this file.

## 0.1.0 — initial release

- TextMate syntax highlighting for `.iron` files (grammar copied from
  `grammars/textmate/` at prepackage time; drift-guarded against the
  compiler lexer in `src/lexer/lexer.c`).
- LSP 3.17 client (`vscode-languageclient@9.0.1`) connecting to `ironls`
  over stdio.
- Binary discovery: `iron.languageServer.path` setting →
  `which`/`where` on `$PATH` → error toast with **Open Settings**
  action (UI-SPEC S1).
- Activation events: `onLanguage:iron` and
  `workspaceContains:**/iron.toml` only (never unconditional).
- Settings: `iron.languageServer.path`,
  `iron.languageServer.trace.server`,
  `iron.languageServer.logLevel` (UI-SPEC S4).
- Output channels: `Iron Language Server` (always) +
  `Iron Language Server (trace)` (lazy when `trace.server != off`)
  (UI-SPEC S2).
- Command: `Iron LSP: Diagnose` (UI-SPEC S3) — opens a self-contained
  report in an untitled document.
- Structured JSON logging with `ext.activate`, `ironls.discovered`,
  `ironls.spawn.ok`, `lsp.initialize.ok`, `lsp.shutdown` event
  vocabulary (UI-SPEC S5).
- Debounced (500 ms) restart on `iron.languageServer.*` configuration
  changes; `deactivate()` awaits `client.stop()` to prevent orphan
  server processes.
- Version-compatibility range check via the non-standard
  `ironLspCompatibleIronlsRange` field in `package.json` (UI-SPEC S9) —
  warning on mismatch.
- `@vscode/test-electron` end-to-end harness asserting the shared
  `tests/editors/fixtures/diag_error.iron` fixture surfaces at least
  one Error-severity diagnostic within 10 s.
