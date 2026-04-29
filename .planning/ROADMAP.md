# Roadmap: iron-lsp

## Overview

iron-lsp ships a full-scope LSP 3.17 server for the Iron language as a second binary (`ironls`) in the iron-lang compiler repository, plus TextMate + tree-sitter grammars and extensions for VSCode, Neovim, and Zed. The journey is seven phases. Phase 1 hardens `iron_compiler` so the frontend is safe for a long-running server — this is the load-bearing prerequisite for everything else. Phases 2–5 layer the LSP vertically: transport & diagnostics (2), navigation (3), editing assistance (4), formatting (5). Phase 6 ships the editor integrations that let real users consume the server. Phase 7 turns "works on my machine" into a shippable product with supervised restarts, soak tests, TSAN CI, SLOs, fuzzers, code signing, and `test_parity_ironc_lsp` as a blocking gate. The Core Value — LSP diagnostics never diverge from `ironc` — runs as a cross-phase thread: the parity fixture is **built in Phase 1**, **wired end-to-end in Phase 2**, and **promoted to a blocking CI gate in Phase 7**.

## Core Value Thread

**`test_parity_ironc_lsp` is the single most important thread across this roadmap.** It is not a new requirement — it is the enforcement mechanism for "LSP output must match `ironc` byte-for-byte on diagnostics."

- **Phase 1** (HARD-12): fixture created, invoked from CLI tests only (LSP does not exist yet); proves the shared `iron_analyze_buffer` API preserves CLI semantics (HARD-11).
- **Phase 2** (CORE-22): the LSP smoke-test drives `ironls` end-to-end; parity harness is now runnable from both CLI **and** LSP call sites.
- **Phases 3–6**: every feature that surfaces a diagnostic must pass parity. Not a new gate, an always-on assertion.
- **Phase 7** (HARD-24): parity is promoted from "test in the suite" to a **blocking CI gate on every PR**. Divergence cannot merge.
- **Phases 8–14** (v2.0 milestone): parity is preserved continuously through the v3 syntax transition — `test_parity_ironc_lsp` + `_fmt` must stay green on every PR; goldens regenerated in Phase 14 against the v3-migrated corpus.

## Risk Register

| Phase | Risk | Rationale |
|-------|------|-----------|
| Phase 1 | **HIGH** | 12 requirements touch the compiler's hottest internal paths (parser, analyzer, arena allocator, `iron_types_init` singleton, `iron_oom_abort`/`IRON_NODE_ASSERT*` surface). A regression here breaks `iron check` and every downstream phase. `/gsd-research-phase` is **strongly recommended** at planning time per research SUMMARY.md §"Research Flags". |
| Phase 6 | **MEDIUM** | Editor APIs move fast — VSCode `vscode-languageclient` is at 9.0.1 with 10.0.0-next in sight, Neovim 0.11.3 switched to native `vim.lsp.config()`, Zed `zed_extension_api` advanced 0.6→0.7. `/gsd-research-phase` is **recommended** at planning time to confirm version floors. |
| Phase 8 | **HIGH** | Rebase gate for the v2.0 milestone. Merge conflicts across `src/lsp/main.c`, `src/lsp/server/dispatch.c`, `CMakeLists.txt`, `.github/workflows/ci.yml`; the v3 compiler surface (new AST node kinds, removed receiver-method syntax, new visibility/tier fields) must build cleanly under `-Werror=switch-enum` before any other v2.0 phase can start. A regression here blocks every downstream phase and every subsequent LSP release. |
| Phase 14 | **MEDIUM** | Absorbs the deferred v1 human-gated release steps (branch protection, Apple secrets, release tag, Marketplace/Zed publish) **plus** the v3-compatible extension cut — first time any of those steps are exercised end-to-end. Fixture corpus migration (MIG-01) + parity-golden regeneration (MIG-02) must both land cleanly or `test_parity_ironc_lsp` flips red. |
| Phases 2–5, 7, 9–13 | LOW–STANDARD | Well-trodden LSP territory (framing, lifecycle, navigation, completion, formatting, hardening, AST adaptation, feature surfacing); ample precedent in clangd/gopls/zls/ols and the v1.0 milestone delivered 93 requirements across the equivalent surface. |

## Phases

**Phase Numbering:**
- Integer phases (1, 2, 3, …, 14): Planned milestone work
- Decimal phases (2.1, 2.2): Urgent insertions (marked with INSERTED) — none at this time

### Milestone v1.0 — Full-scope LSP (Phases 1–7)

- [x] **Phase 1: M0 Compiler Hardening** — Bound abort paths, make analyzer `ErrorNode`-tolerant, add cancellation + per-request arenas + thread-safe `iron_types_init`; build `test_parity_ironc_lsp` fixture (completed 2026-04-18)
- [x] **Phase 2: M1 LSP Core** — `ironls` binary, JSON-RPC transport, LSP 3.17 lifecycle, document sync, push + pull diagnostics, `$/cancelRequest` honored end-to-end (completed 2026-04-18)
- [x] **Phase 3: M2 Navigation & Understanding** — Definition / declaration / typeDefinition / implementation / references / hover / signatureHelp / document + workspace symbols / type hierarchy / workspace diagnostics (completed 2026-04-18)
- [x] **Phase 4: M3 Editing Assistance** — Completion with auto-import, code actions wired to diagnostic suggestions, cross-file rename, documentHighlight / foldingRange / selectionRange (completed 2026-04-18)
- [x] **Phase 5: M4 Formatting** — `textDocument/formatting` via `iron_print_ast` library call; range + on-type formatting; formatting-cleanliness CI gate on every quickfix (completed 2026-04-19)
- [x] **Phase 6: M5 Grammars & Editor Extensions** — TextMate + tree-sitter grammars with lexer-synced keywords, VSCode / Neovim / Zed extensions, end-to-end editor harnesses in CI (engineering complete 2026-04-22; Marketplace/Zed publish deferred into Phase 14)
- [x] **Phase 7: M6 Production Hardening** — Crash telemetry, supervisor, RSS cap, soak test, TSAN CI, SLOs, LSP fuzzers, macOS notarization, version-stamp coherence, parity as blocking gate (engineering complete 2026-04-23; 4 human-gated release steps deferred into Phase 14)

### Milestone v2.0 — LSP support for Iron v3.0 syntax (Phases 8–14)

- [ ] **Phase 8: Rebase + baseline rebuild** — Rebase `gsd/phase-2-m1-lsp-core` onto `origin/main` (v3.0.0-alpha); all three binaries build cleanly against the v3 compiler surface; `IRON_VERSION_FULL` bumped to `3.0.0-alpha.1`; all Phase 1–7 invariant labels remain registered
- [ ] **Phase 9: AST + analyzer surface adaptation** — `src/lsp/facade/` learns the new v3 AST node kinds (`init`, `patch`, visibility, mutation tier); `node_at` walker, symbol identity, and `iron_print_ast` round-trip every v3 construct; parity-fmt green on v3 fixtures
- [x] **Phase 10: Visibility + mutation tier surfacing** — `pub` filters cross-file references / workspace symbols / definition / rename; hover + completion + signatureHelp render `readonly`/`pure` mutation tiers (completed 2026-04-28)
- [ ] **Phase 11: Patch extension support** — `implementation`, `typeHierarchy/subtypes`, completion-at-dot, hover, and `references` all handle `patch object T { … }` open extensions
- [x] **Phase 12: Keyword mirrors + new diagnostic quickfixes** — 6 new keywords (`init`, `mut`, `patch`, `pub`, `pure`, `readonly`) flow through the grammar drift-guard auto-regeneration; 5 new quickfixes handle `E0101` / `E03XA` / `E03XC` / `E03F1` / `E03F2` (completed 2026-04-29)
- [ ] **Phase 13: tree-sitter grammar v3 coverage** — `grammar.js` + `highlights.scm` + `locals.scm` + corpus fixtures cover every v3 construct; `tree-sitter test` + `test_tree_sitter_parses_integration_corpus` green (parallelizable with Phases 9–12)
- [ ] **Phase 14: Migration + codemod command + v3.0.0-alpha.1 release** — Fixture corpus migrated via `ironc migrate`; parity goldens regenerated; editor extension version ranges bumped; optional `workspace/executeCommand: iron.migrate` LSP surface; 4 deferred v1 human-gated release steps executed for the first time with v3.0.0-alpha.1 binaries

## Phase Details

### Phase 1: M0 Compiler Hardening
**Goal**: The `iron_compiler` frontend is safe to run inside a long-lived server — no `abort()` on LSP hot path, analyzer tolerates error-recovery ASTs, compilations are cooperatively cancellable, arenas scope per-request, and `test_parity_ironc_lsp` exists proving CLI semantics are preserved by the API refactor.
**Depends on**: Nothing (first phase; pre-LSP blocker)
**Requirements**: HARD-01, HARD-02, HARD-03, HARD-04, HARD-05, HARD-06, HARD-07, HARD-08, HARD-09, HARD-10, HARD-11, HARD-12
**Success Criteria** (what must be TRUE):
  1. `iron check` produces byte-for-byte identical output on every existing `tests/integration/` fixture after the M0 refactor (HARD-11), proving the `iron_analyze_buffer` API refactor is semantics-preserving
  2. An Iron developer can feed a pathologically malformed source file (deep nesting, half-written functions, `Iron_ErrorNode` in the AST) to `iron_analyze_buffer` in `LSP` mode and the process never aborts, SIGSEGVs, or hangs — it returns partial diagnostics
  3. `test_parity_ironc_lsp` exists, is invoked from the CLI test suite, and passes on every `tests/integration/` fixture (HARD-12) — this is the parity harness the LSP will wire into in Phase 2
  4. A developer can set a `cancel_flag` from another thread during an `iron_analyze_buffer` call and the compile returns early within one pipeline-stage boundary (lex / parse / analyze) — confirmed by a Unity test that measures cancel-to-return latency
  5. Running `iron_analyze_buffer` in a tight loop for 10,000 iterations with per-request arenas leaves resident-set size bounded (not linearly growing) — confirmed by a Unity stress test
**Plans**: 5 plans
- [x] 01-01-PLAN.md — Audit + API skeleton (HARD-01; docs/dev/abort-audit.md; iron_analyze_buffer stub; CLI rewire)
- [x] 01-02-PLAN.md — Analyzer error-tolerance (HARD-03, HARD-04, HARD-10; 7 passes get IRON_NODE_ERROR arms)
- [x] 01-03-PLAN.md — Cancellation + arena lifetime (HARD-05, HARD-06; cancel_flag threaded through lex/parse/8 analyzer passes)
- [x] 01-04-PLAN.md — Thread safety + parser guards (HARD-07, HARD-08, HARD-09; pthread_once + recursion-depth guard + parser.c oom REPLACE)
- [x] 01-05-PLAN.md — Parity fixture + final hardening (HARD-02, HARD-11, HARD-12; LSP-mode comptime FS gating + test_parity_ironc_lsp)
**Risk**: **HIGH** — critical path. `/gsd-research-phase` strongly recommended at planning time.

### Phase 2: M1 LSP Core
**Goal**: A running `ironls` process implementing LSP 3.17 transport, lifecycle, document synchronization, and diagnostics (push + pull). An editor client can `initialize`, open a `.iron` file, type into it, receive live diagnostics within ~250 ms of the last keystroke, and cancel stale requests — and the server survives bad input, dead parents, and broken pipes.
**Depends on**: Phase 1 (needs unified `iron_analyze_buffer`, cancel flag, `ErrorNode`-tolerant passes, thread-safe `iron_types_init`)
**Requirements**: CORE-01, CORE-02, CORE-03, CORE-04, CORE-05, CORE-06, CORE-07, CORE-08, CORE-09, CORE-10, CORE-11, CORE-12, CORE-13, CORE-14, CORE-15, CORE-16, CORE-17, CORE-18, CORE-19, CORE-20, CORE-21, CORE-22
**Success Criteria** (what must be TRUE):
  1. An editor integration-test harness (`tests/lsp/smoke/`) drives a real `ironls` process through `initialize` → `didOpen` → `didChange` (incremental) → `publishDiagnostics` → `shutdown` and asserts the diagnostics match what `iron check` emits on the same buffer — this is `test_parity_ironc_lsp` wired end-to-end for the first time (CORE-22)
  2. An Iron programmer can type fast into a `.iron` file in a client that speaks LSP 3.17, and within 250 ms of the final keystroke of each edit burst sees diagnostics with correct UTF-16 positions on strings containing BOM, surrogate pairs, and combining marks (CORE-09, CORE-11, CORE-16)
  3. An Iron programmer can issue a `$/cancelRequest` against a slow compile and the server actually stops compiling within one pipeline-stage boundary, returning `RequestCancelled` (`-32800`) or dropping the response (CORE-14, CORE-15) — not just receiving the notification and ignoring it
  4. A single malformed document that triggers a compiler `iron_oom_abort` no longer kills the server: the `sigsetjmp` boundary catches `SIGABRT`, the user sees a `window/showMessage` "analysis crashed" notification, and after a second strike on the same document the document is quarantined while the rest of the session continues (CORE-18)
  5. Killing the editor process leaves no `ironls` zombies — the server detects `SIGPIPE`/`EPIPE`/parent-death and exits cleanly with a `shutdown` log line to `$XDG_STATE_HOME/iron-lsp/server-<pid>.log` (CORE-19, CORE-21)
**Plans**: 6 plans
- [x] 02-01-PLAN.md — Build skeleton + yyjson vendor + CI path filter + test subtrees (CORE-01, CORE-20)
- [x] 02-02-PLAN.md — Transport: framing + yyjson-arena + 3-thread I/O + bounded writer queue (CORE-02, CORE-03, CORE-04)
- [x] 02-03-PLAN.md — Dispatch + lifecycle FSM + capabilities + positionEncoding + dyn-register + cancel registry (CORE-05, CORE-06, CORE-07, CORE-08, CORE-14 dispatch side)
- [x] 02-04-PLAN.md — Document store: flat UTF-8 buffer + line_starts + UTF-16/8/byte math + SHA-256 drift + watched-files (CORE-09, CORE-10, CORE-11, CORE-12, CORE-13)
- [x] 02-05-PLAN.md — ASTWorker + mailbox + facade + diagnostics push+pull + SIGABRT quarantine + parity 4th pass (CORE-14 pipeline, CORE-15, CORE-16, CORE-17, CORE-18, CORE-22 parity wire-up)
- [x] 02-06-PLAN.md — Main.c finalization + XDG JSON-line logs + SIGPIPE/EPIPE/EOF + pytest-lsp smoke (CORE-19, CORE-21, CORE-22 smoke)

### Phase 3: M2 Navigation & Understanding
**Goal**: Editors feel like they understand Iron code. An Iron programmer can navigate their codebase and read hover tooltips across files, a workspace index exists and stays fresh under file-watch events, and workspace-level diagnostics surface errors from files not currently open.
**Depends on**: Phase 2 (needs transport, document store, facade, UTF-16 translation)
**Requirements**: NAV-01, NAV-02, NAV-03, NAV-04, NAV-05, NAV-06, NAV-07, NAV-08, NAV-09, NAV-10, NAV-11, NAV-12, NAV-13, NAV-14, NAV-15, NAV-16
**Success Criteria** (what must be TRUE):
  1. An Iron programmer can Ctrl-click (or press `gd` / F12) a symbol in their editor and jump to its definition — same-file, cross-file within the workspace, into `iron.toml` dependencies, and into the stdlib (NAV-02, NAV-01)
  2. An Iron programmer can hover any identifier in VSCode / Neovim / Zed and see a markdown tooltip showing its type, signature, and the author's `///` doc comment from the declaration site (NAV-09, NAV-14)
  3. An Iron programmer can run `workspace/symbol` with a fuzzy query and get ranked results from every `.iron` file in the workspace — and those results stay fresh when a file is edited outside the editor (NAV-08, NAV-13)
  4. An Iron programmer editing an interface-heavy codebase can invoke `typeHierarchy/subtypes` on an interface declaration and see every implementing object declaration across the workspace (NAV-11, NAV-05)
  5. An Iron programmer opens a workspace that contains a file with compile errors they haven't opened yet — the editor's Problems panel / `:Trouble` view shows those errors without the user needing to open that file (NAV-12, NAV-13)
**Plans**: 6 plans
- [x] 03-01-PLAN.md — Foundations: Wave 0 test infra + NAV-14 lexer + NAV-15 sealed + NAV-16 symbol_id + node_at
- [x] 03-02-PLAN.md — Workspace index + stdlib cache + dep map (NAV-01)
- [x] 03-03-PLAN.md — Nav primitives: definition, declaration, typeDefinition, documentSymbol, workspace/symbol + fuzzy (NAV-02, NAV-03, NAV-04, NAV-07, NAV-08)
- [x] 03-04-PLAN.md — References, hover, signatureHelp (NAV-06, NAV-09, NAV-10)
- [x] 03-05-PLAN.md — Interface-aware: iface_workspace aggregator + implementation + type hierarchy (NAV-05, NAV-11)
- [x] 03-06-PLAN.md — workspace/diagnostic pull + refresh + full parity sweep (NAV-12, NAV-13)
**UI hint**: yes

### Phase 4: M3 Editing Assistance
**Goal**: Completion, code actions, and rename make the editor feel collaborative. An Iron programmer gets context-aware completions with package-manager-aware auto-import, can fix common diagnostics with a single keystroke, and can rename a symbol across the whole workspace safely.
**Depends on**: Phase 3 (needs workspace index, cross-file references, symbol resolution)
**Requirements**: EDIT-01, EDIT-02, EDIT-03, EDIT-04, EDIT-05, EDIT-06, EDIT-07, EDIT-08, EDIT-09, EDIT-10, EDIT-11, EDIT-12, EDIT-13, EDIT-14, EDIT-15
**Success Criteria** (what must be TRUE):
  1. An Iron programmer types a stdlib symbol they haven't imported yet (e.g., `println`) and accepting the completion inserts both the call site AND the `use` statement at the top of the file via `additionalTextEdits` — the "auto-import" killer feature (EDIT-02, EDIT-05)
  2. An Iron programmer sees a red squiggle under an undefined symbol, presses the editor's code-action key, and one of the quickfixes resolves the diagnostic — wired to the corresponding `Iron_Diagnostic.suggestion` value with at least 5 quickfixes live (EDIT-07, EDIT-08)
  3. An Iron programmer presses F2 on a top-level symbol, types a new name, and every reference across every `.iron` file in the workspace is renamed atomically — OR the rename is rejected with an explanation when it would collide, target a stdlib/package/`extern` symbol, or break the build (EDIT-10, EDIT-11, EDIT-12)
  4. An Iron programmer invokes `source.organizeImports` on a file and gets back a deduplicated, sorted `use` block with no behavioral change to the code (EDIT-09)
  5. An Iron programmer opens a file that is syntactically broken (missing brace, half-written expression) and can still fold code, expand selection, and see document highlights on the parts the parser recovered — parser-only features keep working when the analyzer can't (EDIT-13, EDIT-14, EDIT-15)
**Plans**: 7 plans
- [x] 04-01-PLAN.md — Diagnostic suggestions surface for quickfix routing (EDIT-07)
- [x] 04-02-PLAN.md — Completion engine: keywords, locals, trigger characters (EDIT-01, EDIT-02, EDIT-03, EDIT-06)
- [x] 04-03-PLAN.md — Stdlib/workspace completion + auto-import via additionalTextEdits (EDIT-04, EDIT-05)
- [x] 04-04-PLAN.md — Code actions: 5 quickfixes wired to Iron_Diagnostic.suggestion (EDIT-07, EDIT-08)
- [x] 04-05-PLAN.md — source.organizeImports (EDIT-09)
- [x] 04-06-PLAN.md — Cross-file rename with collision/stdlib guards (EDIT-10, EDIT-11, EDIT-12)
- [x] 04-07-PLAN.md — documentHighlight + foldingRange + selectionRange on recovered AST (EDIT-13, EDIT-14, EDIT-15)
**UI hint**: yes

### Phase 5: M4 Formatting
**Goal**: `textDocument/formatting` and its siblings return edits produced by `iron_print_ast` called as a library (never as a subprocess), matching `iron fmt` byte-for-byte, with a CI gate proving every quickfix from Phase 4 produces `iron fmt`-clean output.
**Depends on**: Phase 2 (needs LSP transport + facade). **Does NOT depend on Phase 4** — formatting can run in parallel with editing-assistance work.
**Requirements**: FMT-01, FMT-02, FMT-03, FMT-04, FMT-05, FMT-06
**Success Criteria** (what must be TRUE):
  1. An Iron programmer presses "Format Document" in their editor and the file is reformatted to match exactly what `iron fmt` on the CLI would produce on the same source — no subprocess shell-out, no divergence (FMT-01, FMT-02)
  2. An Iron programmer selects a block and presses "Format Selection" and the intersecting top-level construct is reformatted without touching the rest of the file (FMT-03)
  3. An Iron programmer with on-type formatting enabled in their editor types `}` and the enclosing block is instantly re-indented correctly — and users who leave on-type formatting off (the default) see no unsolicited edits (FMT-04)
  4. An Iron programmer with a `[fmt]` section in `iron.toml` (e.g., custom line width) sees that config respected by both `iron fmt` on the CLI AND `textDocument/formatting` from the LSP — single source of truth (FMT-05)
  5. CI fails the build if any of the Phase 4 quickfixes produces output that `iron fmt --check` would reformat — quickfix code is expected to match project style out of the box (FMT-06)
**Plans**: 5 plans
- [x] 05-01-PLAN.md — Compiler-side foundations: src/fmt/ module + iron_format_source + IronFmtOptions + iron_print_ast signature ext + [fmt] in iron.toml + iron fmt --check (FMT-01, FMT-05)
- [x] 05-02-PLAN.md — LSP facade + full-document formatting: single iron_format_source call site + handlers_fmt + 3 dispatch rows + workspace fmt_opts + parity CTest (FMT-02)
- [x] 05-03-PLAN.md — Range formatting: decl-intersection walker + per-decl edits + descending sort (FMT-03)
- [x] 05-04-PLAN.md — On-type formatting: } block finder + per-line indent edits + capability shape override (FMT-04)
- [x] 05-05-PLAN.md — Quickfix CI gate (5 fixtures) + idempotency invariant + Phase 5 closeout (FMT-06)

### Phase 6: M5 Grammars & Editor Extensions
**Goal**: The three target editors (VSCode, Neovim, Zed) ship to their respective distribution channels with working LSP integration, TextMate + tree-sitter grammars handle structural highlighting (we are NOT shipping semantic tokens), and keyword arrays in both grammars are generated from `src/lexer/keywords.h` so a new keyword can never silently miss highlighting.
**Depends on**: Phase 2 (editor extensions need a working LSP server to validate against). Grammars alone **can start as early as Phase 1 completes** — they have zero dependency on the LSP server.
**Requirements**: EXT-01, EXT-02, EXT-03, EXT-04, EXT-05, EXT-06, EXT-07, EXT-08, EXT-09, EXT-10
**Success Criteria** (what must be TRUE):
  1. An Iron programmer can install the VSCode extension from the Visual Studio Marketplace, open a `.iron` file, and get syntax highlighting + live diagnostics + go-to-definition without any manual configuration (EXT-04, EXT-05)
  2. A Neovim user following the `editors/neovim/README.md` gets `ironls` running via the native `vim.lsp.config()` API (Neovim 0.11.3+), passes `:checkhealth lsp`, and sees tree-sitter highlighting on `.iron` files (EXT-06, EXT-02)
  3. A Zed user can install the iron-lsp extension from the Zed extensions registry and the extension downloads a signed `ironls` binary from GitHub Releases (verified by SHA-256) before registering the language server (EXT-08, EXT-09)
  4. A developer adds a new keyword to `src/lexer/keywords.h`; without running `scripts/regenerate-grammars.sh` their CI pre-push gate fails because the committed grammars are stale (EXT-03) — guarantees keyword parity across C parser / TextMate / tree-sitter
  5. CI end-to-end harnesses (`@vscode/test-electron`, `plenary.nvim`, Zed dev-mode load) open a canonical `.iron` fixture in each of the three editors and assert diagnostics actually appear, on every PR touching `editors/**` or `grammars/**` (EXT-10)
**Plans**: 6 plans
- [x] 06-01-PLAN.md — Grammar foundations: IRON_BUILD_GRAMMARS + configure_file extractor + TextMate + tree-sitter skeleton + Wave 0 test infra (EXT-01, EXT-03)
- [x] 06-02-PLAN.md — tree-sitter grammar completion + corpus + WASM build CI (EXT-02)
- [x] 06-03-PLAN.md — VSCode extension + @vscode/test-electron e2e + Marketplace dry-run (EXT-04, EXT-05)
- [x] 06-04-PLAN.md — Neovim config + plenary.nvim e2e + checkhealth gate (EXT-06, EXT-07 foundation)
- [x] 06-05-PLAN.md — Zed extension with SHA-256 hand-roll + dev-load harness + release-assets pipeline (EXT-08, EXT-09)
- [x] 06-06-PLAN.md — E2E harness consolidation + nvim-lspconfig upstream tracking doc + publisher checklist + phase closeout (EXT-05, EXT-07, EXT-09, EXT-10)
**Risk**: **MEDIUM** — editor APIs move fast. `/gsd-research-phase` recommended at planning time to confirm current version floors (VSCode `vscode-languageclient`, Neovim native LSP, Zed `zed_extension_api`).
**UI hint**: yes

### Phase 7: M6 Production Hardening
**Goal**: `ironls` is shippable — not just "works on my machine." Crashes are captured with diagnostic dumps, memory is bounded and measured under 8-hour soak, thread safety is TSAN-verified, per-request SLOs are documented and enforced in CI, LSP-surface fuzz harnesses run continuously, macOS binaries are notarized, and `test_parity_ironc_lsp` graduates from "in the test suite" to a blocking CI gate on every PR.
**Depends on**: Phases 1–6 (hardening is sequenced last because it measures and shields everything that came before)
**Requirements**: HARD-13, HARD-14, HARD-15, HARD-16, HARD-17, HARD-18, HARD-19, HARD-20, HARD-21, HARD-22, HARD-23, HARD-24
**Success Criteria** (what must be TRUE):
  1. An Iron programmer running `ironls --supervised` against a crashy document sees the worker restart transparently — the parent supervisor logs the crash, writes a dump to `$XDG_STATE_HOME/iron-lsp/crashes/<timestamp>.dmp` with `backtrace(3)` + last-N in-flight requests + offending URI, and the session stays alive (HARD-13, HARD-14)
  2. An 8-hour automated soak test (`tests/lsp/soak/`) drives scripted edit sessions against a fixture workspace, measures RSS growth, and fails CI if growth exceeds the calibrated threshold OR if the RSS cap (default 1 GiB) is hit (HARD-15, HARD-16)
  3. A ThreadSanitizer CI job running `ironls` with 2 concurrent document workers under a mixed-request script emits zero TSAN findings (HARD-17)
  4. CI fails the build if p50 hover > 20 ms, p50 completion > 100 ms, or p50 full-file diagnostics > 500 ms on the canonical fixture set — SLOs are documented AND enforced (HARD-18)
  5. A pull request that causes `test_parity_ironc_lsp` to fail on any `tests/integration/` fixture **cannot merge** — parity is a blocking, non-bypassable CI gate on every PR, closing the Core Value loop that started in Phase 1 (HARD-24)
  6. LSP-specific fuzz harnesses (`fuzz_lsp_frame`, `fuzz_lsp_json`, `fuzz_lsp_dispatch`, `fuzz_lsp_didChange`) run on the existing `.github/workflows/fuzz.yml` schedule and find zero undiagnosed crashes between releases (HARD-19)
**Plans**: 8 plans
- [x] 07-01-PLAN.md — Supervisor + crash dumps + parent-death (HARD-13, HARD-14, HARD-20)
- [x] 07-02-PLAN.md — RSS cap + soak test (HARD-15, HARD-16)
- [x] 07-03-PLAN.md — TSAN CI (HARD-17)
- [x] 07-04-PLAN.md — SLOs + build-time regression (HARD-18, HARD-23)
- [x] 07-05-PLAN.md — LSP fuzz harnesses (HARD-19)
- [x] 07-06-PLAN.md — macOS signing + notarization (HARD-21)
- [x] 07-07-PLAN.md — Version coherence + hard-refuse (HARD-22)
- [x] 07-08-PLAN.md — Parity blocking gate + phase closeout (HARD-24)

---

## Milestone v2.0 — LSP support for Iron v3.0 syntax

**Defined:** 2026-04-24
**Milestone goal:** Adapt the feature-complete v1.2 LSP (7 phases, 93 requirements) to Iron v3.0's syntax overhaul (`init` / `mut` / `patch` / `pub` / `pure` / `readonly` + method-in-block + mutation tiers) without regressing any Phase 1–7 LSP capability. Parity gate (HARD-24) stays green throughout.

**Cross-phase threads (v2.0-specific):**

1. **Parity preservation.** `test_parity_ironc_lsp` + `test_parity_ironc_lsp_fmt` (HARD-24 blocking gate from Phase 7) must stay green on every PR across Phases 8–14. During Phases 8–13 they validate against v2 fixtures built with the v3 compiler's analyzer/formatter output on unchanged syntax. In Phase 14 (MIG-02) goldens are regenerated from the v3-migrated corpus and both fixtures flip green under v3 syntax — this is the single atomic parity cutover.

2. **HARD-22 version coherence.** `IRON_VERSION_FULL` is bumped to `3.0.0-alpha.1` in Phase 8 (REC-04); the same version string stamps `iron`, `ironc`, and `ironls` via the Phase 7 HARD-22 coherence machinery. In Phase 14 (MIG-03/04/05/06) the three editor extensions' hard-refuse constants are bumped from `">= 1.2.0, < 2.0.0"` to `">= 3.0.0, < 4.0.0"` — the same enforcement contract that rejected pre-HARD-22 v0 binaries now structurally refuses a v1 ironls against a v3 extension.

3. **Grammar drift-guard.** The 6 new keywords (`init`, `mut`, `patch`, `pub`, `pure`, `readonly`) flow through the Phase 4 D-01 `keyword_mirror.h.in` + Phase 6 D-01 grammar-regeneration `configure_file` pattern automatically — no manual grammar edit is required for the keyword roster itself. Phase 12 (KW-01/02) re-runs the generator and re-asserts `test_grammar_keyword_drift` / completion-mirror drift-guards; Phase 13 (GRM-01..05) adds the structural rules that go beyond keyword highlighting.

### Phase 8: Rebase + Baseline Rebuild
**Goal**: `gsd/phase-2-m1-lsp-core` is reconciled with `origin/main` (v3.0.0-alpha); `ironls` + `ironc` + `iron` all build cleanly against the v3 compiler surface; `IRON_VERSION_FULL` is bumped to `3.0.0-alpha.1`; every Phase 1–7 invariant label remains registered so downstream phases can lean on them. This is the gate for the entire v2.0 milestone — no other phase starts until this is green.
**Depends on**: Milestone v1.0 engineering complete (Phase 7)
**Requirements**: REC-01, REC-02, REC-03, REC-04, REC-05
**Success Criteria** (what must be TRUE):
  1. `git status` on `gsd/phase-2-m1-lsp-core` shows zero conflict markers across `src/lsp/main.c`, `src/lsp/server/dispatch.c`, `CMakeLists.txt`, `.github/workflows/ci.yml`; `git log --graph` shows a linear history on top of `origin/main` (REC-01)
  2. A clean build (`cmake --build build --target ironls ironc iron`) succeeds on Linux + macOS under `-Wall -Wextra -Werror -Wpedantic -Werror=switch-enum` against the v3 compiler's AST surface — no switch-enum holes where new `IRON_NODE_INIT_DECL` / `IRON_NODE_PATCH_DECL` cases were added (REC-02)
  3. Running `ctest -L phase-m1-invariant -L phase-m2-invariant -L phase-m3-invariant -L phase-m4-invariant -L phase-m5-invariant -L phase-m6-invariant` registers and exercises every Phase 1–7 invariant; any label that went missing in the rebase is restored (REC-03)
  4. `ironls --version`, `ironc --version`, and `iron --version` all print `3.0.0-alpha.1` from the same `IRON_VERSION_FULL` compile-time define; the Phase 7 `test_version_stamp_coherence` harness remains green (REC-04)
  5. `CLAUDE.md`'s "Technology Stack" section, `INSTALL.md`, and each of the three editor extensions' README files correctly document the v3 compiler surface Iron programmers interact with via ironls (REC-05)
**Plans**: 2 plans
- [ ] 08-01-PLAN.md — Wave 0 prep + 134-commit rebase execution + 3-layer build verification (REC-01, REC-02, REC-04)
- [ ] 08-02-PLAN.md — Invariant label diff + REC-05 docs sweep + sacrificial-CI push + promote-on-green (REC-03, REC-05; cross-thread D-09 parity, D-10 hard-refuse)
**Risk**: **HIGH** — rebase gate; blocks all downstream v2.0 work. `/gsd-research-phase` recommended at planning time to pre-stage the conflict map.

### Phase 9: AST + Analyzer Surface Adaptation
**Goal**: The LSP facade (`src/lsp/facade/`) and navigation walkers understand every v3 AST node kind and annotated analyzer field. Features from Phases 1–7 that walked AST switch statements or queried analyzer output are updated in place; `iron_print_ast` round-trips every v3 construct; `test_parity_ironc_lsp_fmt` green on v3-migrated integration corpus snippets. This is the foundation that Phases 10–12 build features on top of.
**Depends on**: Phase 8 (needs a clean v3-rebased build)
**Requirements**: AST-01, AST-02, AST-03, AST-04, AST-05, AST-06
**Success Criteria** (what must be TRUE):
  1. An Iron programmer places the cursor anywhere inside an `init(...) { ... }` body or a `patch object T { ... }` declaration and the LSP's `node_at` walker (from NAV-16) descends into the construct and returns a locatable `Iron_Node *` — no navigation feature returns "no symbol here" when the cursor is inside a new v3 construct (AST-02)
  2. A compile of the LSP subtree under `-Werror=switch-enum` succeeds: every `switch (node->kind)` in `src/lsp/` handles `IRON_NODE_INIT_DECL` and `IRON_NODE_PATCH_DECL` (and any new expression forms) explicitly — the compiler structurally forbids the "forgot a case" regression (AST-01)
  3. An Iron programmer defines a symbol inside an `init` or a `patch` and then renames / finds references / jumps to definition on it; the symbol identity triple (canonical_path, symbol_name_path, kind) stays stable across re-analysis and FNV-1a hashes remain collision-free on both v2 and v3 fixture corpora (AST-03)
  4. An Iron programmer runs `iron fmt` on a v3-syntax file AND the LSP's `textDocument/formatting` on the same buffer — byte-for-byte identical output on every v3 construct (init, patch, visibility, mutation tier); `test_parity_ironc_lsp_fmt` green (AST-05)
  5. `docs/dev/AST_CONTRACT.md` documents the sealed-tree invariant covering `init_decl` + `patch_decl` nodes so future contributors know the v3 AST is read-only after `iron_analyze_buffer` returns — exactly the discipline the v2 LSP already relied on (AST-04)
**Plans**: 3 plans
- [x] 09-01-PLAN.md — Walker verify + symbol_name_path v3 routing + strict_v3 plumbing (AST-01, AST-02, AST-03; D-11 XXX_PHASE_9 marker resolved)
- [x] 09-02-PLAN.md — iron_print_ast v3 parity: modifier prefixes + two-pass method-merge for OBJECT_DECL (AST-05; D-10 byte-identical fmt)
- [x] 09-03-PLAN.md — Facade audit: hover modifier prefix + patch-skip guards + AST_CONTRACT.md v3 subsection + pytest-lsp v3 smoke (AST-01, AST-04, AST-06; D-12 case 4)

### Phase 10: Visibility + Mutation Tier Surfacing
**Goal**: `pub` visibility and `readonly`/`pure` mutation tiers show up everywhere an Iron programmer asks the LSP "what is this symbol and who can see it?". Cross-file references, workspace symbol search, definition-jump, rename, hover, completion, and signatureHelp all honor the new language-level semantics — and same-file private access continues to work exactly as v2 developers expect.
**Depends on**: Phase 9 (needs analyzer `visibility` + `mutation_tier` fields wired into the LSP facade)
**Requirements**: VIS-01, VIS-02, VIS-03, VIS-04, VIS-05, TIER-01, TIER-02, TIER-03, TIER-04
**Success Criteria** (what must be TRUE):
  1. An Iron programmer searches for a private symbol via `workspace/symbol` fuzzy query from a file outside the symbol's module and the symbol does not appear in results; the same query from a file inside that module surfaces the symbol normally (VIS-02)
  2. An Iron programmer invokes `textDocument/references` on a private symbol and the results include only same-file usages even when other modules import the module; on a `pub` symbol the results include every module that imports it (VIS-01)
  3. An Iron programmer invokes F2 rename on a private symbol whose usages span modules the rename cannot legally reach; the LSP refuses the rename with an `E03PV: symbol not visible` equivalent surfaced via `window/showMessage` — the rename never produces a broken edit (VIS-04)
  4. An Iron programmer hovers a method and the first line of the hover markdown signature block renders its mutation tier exactly as declared — `func`, `readonly func`, or `pure func` — and for `pub` members the `pub` modifier is rendered explicitly (TIER-01, VIS-05)
  5. An Iron programmer typing a function signature sees `readonly` and `pure` proposed only as `func` modifiers (directly before the `func` token), never as standalone identifiers; completion entries for methods render the callee's tier in the detail field so mutation behaviour is visible at a glance (TIER-02, TIER-03)
**Plans**: 3 plans
- [x] 10-01-PLAN.md — Foundations: visibility predicate + path_is_stdlib lift + symbol identity drift-guard + Wave 0 stubs (D-02, D-08, D-12; predicate prerequisite for VIS-01..04)
- [x] 10-02-PLAN.md — VIS fan-out: 7 call sites consume the predicate (VIS-01..04 closed via references / workspace_symbol / definition / declaration / type_definition / implementation / rename apply.c — RESEARCH Conflict 2 resolution adds implementation.c)
- [x] 10-03-PLAN.md — TIER + VIS-05 hover extension + closeout: pub prefix on hover funcs/methods/objects + readonly/pure on signature_help label + completion detail tier prefix + phase closeout (VIS-05, TIER-01 verify, TIER-03, TIER-04; TIER-02 deferred to Phase 12 KW-03)
**UI hint**: yes

### Phase 11: Patch Extension Support
**Goal**: Every LSP feature that answers "where else does this type have methods?" understands `patch object T { … }` open extensions. An Iron programmer who patches a primitive or library type can discover, complete, hover, jump into, and find references to the patched methods from anywhere in the workspace with cross-module visibility respecting `pub` on the patch itself.
**Depends on**: Phase 9 (needs `IRON_SYMBOL_PATCH_METHOD` exposed through the LSP facade + symbol identity from AST-03)
**Requirements**: PATCH-01, PATCH-02, PATCH-03, PATCH-04, PATCH-05
**Success Criteria** (what must be TRUE):
  1. An Iron programmer invokes `textDocument/implementation` on an interface or type declaration and the results include every `patch object T { ... }` method that extends it — not just the natively-declared implementors (PATCH-01)
  2. An Iron programmer types `someString.` (with a `patch object String { ... }` in scope) and the completion proposals include the patched methods alongside String's native methods; both are ranked equivalently when equally relevant (PATCH-03)
  3. An Iron programmer hovers a patch-added method and the hover markdown surfaces the enclosing `patch object T { ... }` context, so the reader immediately knows which patch declared the method and which module it lives in (PATCH-04)
  4. An Iron programmer invokes `typeHierarchy/subtypes` on a primitive or library type and the result tree includes patched methods as virtual "methods of T" entries — rendered distinctly from subtype children because patches do not subtype (PATCH-02)
  5. An Iron programmer invokes `textDocument/references` on a patch-added method and the results include every call site across every module that imports the patching module; cross-module visibility respects `pub` on the patch declaration (PATCH-05)
**Plans**: 3 plans
- [ ] 09-01-PLAN.md — Walker verify + symbol_name_path v3 routing + strict_v3 plumbing (AST-01, AST-02, AST-03; D-11 XXX_PHASE_9 marker resolved)
- [ ] 09-02-PLAN.md — iron_print_ast v3 parity: modifier prefixes + two-pass method-merge for OBJECT_DECL (AST-05; D-10 byte-identical fmt)
- [ ] 09-03-PLAN.md — Facade audit: hover modifier prefix + patch-skip guards + AST_CONTRACT.md v3 subsection + pytest-lsp v3 smoke (AST-01, AST-04, AST-06; D-12 case 4)
**UI hint**: yes

### Phase 12: Keyword Mirrors + New Diagnostic Quickfixes
**Goal**: The 6 new v3 keywords (`init`, `mut`, `patch`, `pub`, `pure`, `readonly`) flow through the Phase 4 D-01 + Phase 6 D-01 drift-guard auto-regeneration pattern, and completion filters them context-awarely. Five new diagnostic quickfixes let an Iron programmer fix v3-specific errors (receiver-method syntax, missing `init`, inline field defaults, `readonly` violations) with a single keystroke.
**Depends on**: Phase 9 (needs v3 AST adaptation so quickfix codegen targets the right tree)
**Requirements**: KW-01, KW-02, KW-03, QF-01, QF-02, QF-03, QF-04, QF-05
**Success Criteria** (what must be TRUE):
  1. A developer adds or removes a keyword in `src/lexer/keywords.h` and both `test_grammar_keyword_drift` (Phase 6 drift-guard) and the completion keyword-mirror drift-guard fail until the regenerated artifacts are committed — 6 new v3 keywords flow through the existing machinery with zero hand-editing of grammar or completion tables (KW-01, KW-02)
  2. An Iron programmer types `pub` at the start of a declaration line inside a module or object body and the completion engine proposes it; typing `pub` inside an expression body never proposes it — context-aware filtering matches the v3 grammar exactly (KW-03)
  3. An Iron programmer opens a v2 file containing `func (p: Player) take_damage(n: Int)` receiver-method syntax, sees an `E0101` red squiggle, and presses the code-action key; a quickfix "Run `ironc migrate --from v2 --to v3`" appears and runs the codemod via `workspace/executeCommand` (QF-01)
  4. An Iron programmer writes an `object Player { var health: Int }` with no `init`, sees `E03XA`, and invokes a quickfix that synthesizes a default `init(health: Int) { self.health = health }` as the first member of the object body — the resulting code compiles cleanly (QF-02)
  5. An Iron programmer writes a `readonly func` that attempts to mutate `self.field`, sees `E03F1`, and the LSP offers TWO distinct quickfix options: (a) remove the `readonly` modifier, (b) remove the offending write — the user picks; analogous two-option behaviour for `E03F2` (`readonly` calling mutating) (QF-04, QF-05)
**Plans**: 3 plans
- [x] 12-01-PLAN.md — Infrastructure widening + Wave 0 stubs (signature shift to multi-action; command_*; edit_text_edits[]; data_variant_idx; derive_body_indent lift; 3 stub test binaries) — D-11..D-15, D-23, D-27, D-31, D-37
- [x] 12-02-PLAN.md — KW-01/02 verify + KW-03 keyword_filter + QF-01 (command-style migrate) + QF-02 (synthesize default init) — KW-01, KW-02, KW-03, QF-01, QF-02
- [x] 12-03-PLAN.md — QF-03 (multi-edit inline default) + QF-04 (2-action readonly write) + QF-05 (1-or-2 action readonly calls mutating) + closeout — QF-03, QF-04, QF-05
**UI hint**: yes

### Phase 13: tree-sitter Grammar v3 Coverage
**Goal**: The tree-sitter grammar parses every v3 construct correctly, captures v3-specific tokens under canonical nvim-treesitter highlight names, and `locals.scm` resolves v3-specific binding forms (init parameters, `self` inside `patch` methods). Corpus fixtures cover all new constructs and the integration-corpus gate is green on migrated files. This phase runs on the editor-client side and can execute in parallel with Phases 9–12.
**Depends on**: Phase 8 (needs v3 baseline build so the updated `src/lexer/keywords.h` drives grammar keyword regeneration). **Independent of Phases 9–12** — can parallelize.
**Requirements**: GRM-01, GRM-02, GRM-03, GRM-04, GRM-05
**Success Criteria** (what must be TRUE):
  1. Running `tree-sitter parse` on every `.iron` file in `tests/integration/` (post-v3-migration) produces zero ERROR nodes — the generated parser accepts every v3 construct the compiler accepts; `test_tree_sitter_parses_integration_corpus` green (GRM-05)
  2. A Neovim user with iron-lsp + nvim-treesitter installed opens a v3 `.iron` file; `init` / `patch` / `pub` render under `@keyword`, `readonly` / `pure` render under `@keyword.modifier`, `func` renders under `@keyword.function` — matching the canonical nvim-treesitter capture taxonomy (GRM-02)
  3. A developer adds a v3 construct to `grammars/tree-sitter/iron/test/corpus/*.txt` and `tree-sitter test` validates the expected parse tree — corpus fixtures cover `init_declaration`, `patch_declaration`, `pub` visibility, `readonly`/`pure` modifiers (GRM-04)
  4. A Neovim user places the cursor on `self` inside a `patch object String { … }` method body and the `locals.scm` resolver identifies it as a binding to the patched type — not an unresolved identifier (GRM-03)
  5. `grammars/tree-sitter/iron/grammar.js` gains rules for `init_declaration`, `patch_declaration`, visibility modifier (`pub`), mutation-tier modifiers (`readonly`, `pure`); the generated parser is re-committed and the grammar-regeneration CI gate passes (GRM-01)
**Plans**: 3 plans
- [ ] 09-01-PLAN.md — Walker verify + symbol_name_path v3 routing + strict_v3 plumbing (AST-01, AST-02, AST-03; D-11 XXX_PHASE_9 marker resolved)
- [ ] 09-02-PLAN.md — iron_print_ast v3 parity: modifier prefixes + two-pass method-merge for OBJECT_DECL (AST-05; D-10 byte-identical fmt)
- [ ] 09-03-PLAN.md — Facade audit: hover modifier prefix + patch-skip guards + AST_CONTRACT.md v3 subsection + pytest-lsp v3 smoke (AST-01, AST-04, AST-06; D-12 case 4)
**UI hint**: yes

### Phase 14: Migration + Codemod Command + v3.0.0-alpha.1 Release
**Goal**: The integration fixture corpus is migrated to v3 syntax, parity goldens regenerated, editor extensions' version-range constants bumped to `">= 3.0.0, < 4.0.0"`, an optional `workspace/executeCommand: iron.migrate` LSP surface ships, and the 4 human-gated v1 release steps (branch protection, Apple secrets, release tag, Marketplace/Zed publish) are executed for the first time with v3.0.0-alpha.1 binaries — absorbing the deferred v1 release into this milestone.
**Depends on**: Phases 8–13 (needs v3-capable LSP + v3-capable grammars + v3-capable quickfixes before the release ships)
**Requirements**: MIG-01, MIG-02, MIG-03, MIG-04, MIG-05, MIG-06, MIG-07, CMD-01, CMD-02, CMD-03, REL-01, REL-02, REL-03, REL-04, REL-05, REL-06
**Success Criteria** (what must be TRUE):
  1. An Iron programmer runs `ironc migrate --from v2 --to v3` against the entire `tests/integration/` corpus, the codemod rewrites files in place (pre-migration snapshot archived as `tests/integration.v2.tar.gz`), and `test_parity_ironc_lsp` + `test_parity_ironc_lsp_fmt` are green against the v3-migrated corpus — the parity cutover from v2-under-v3-compiler to native-v3 happens atomically in this phase (MIG-01, MIG-02)
  2. An Iron programmer installs the v3.0.0-alpha.1 VSCode / Neovim / Zed extension and opens a `.iron` file; the extension's hard-refuse machinery (Phase 7 D-10) accepts the matching v3 ironls and rejects a v1 ironls with a clear diagnostic — version-range constants bumped from `">= 1.2.0, < 2.0.0"` to `">= 3.0.0, < 4.0.0"` in all three extensions (MIG-03, MIG-04, MIG-05, MIG-06)
  3. An Iron programmer in an editor invokes "Iron: Migrate v2 → v3" (VSCode command palette), `:IronLspMigrateV2ToV3` (Neovim), or the Zed slash command; `ironls` streams codemod progress via `$/progress`, returns a `WorkspaceEdit` containing the diff, and the editor previews the change before applying — the command is gated so it is unavailable in workspaces where `ironc --version` < 3.0.0 (CMD-01, CMD-02, CMD-03)
  4. An Iron user can install signed + notarized macOS `ironls` binaries from the `v3.0.0-alpha.1` GitHub Release (triggered by the `release.yml` workflow on tag push) without Gatekeeper warnings; the VSCode Marketplace and Zed extensions registry both serve the v3-compatible extension versions; branch protection on `main` is configured with the 8-check required list from `docs/dev/ci-gates.md` (REL-04, REL-05, REL-06)
  5. The `CHANGELOG.md` entry for v3.0.0-alpha.1 lists every v2.0 milestone requirement delivered, and `docs/site/migration-v2-to-v3.md` gains an "LSP notes" section documenting the extension version-range bump users need to apply if they pinned explicitly (REL-02, REL-03)
**Plans**: 3 plans
- [ ] 09-01-PLAN.md — Walker verify + symbol_name_path v3 routing + strict_v3 plumbing (AST-01, AST-02, AST-03; D-11 XXX_PHASE_9 marker resolved)
- [ ] 09-02-PLAN.md — iron_print_ast v3 parity: modifier prefixes + two-pass method-merge for OBJECT_DECL (AST-05; D-10 byte-identical fmt)
- [ ] 09-03-PLAN.md — Facade audit: hover modifier prefix + patch-skip guards + AST_CONTRACT.md v3 subsection + pytest-lsp v3 smoke (AST-01, AST-04, AST-06; D-12 case 4)
**Risk**: **MEDIUM** — first-time execution of the 4 v1 human-gated release steps combined with the v3 parity-golden cutover; either atomically ships or atomically rolls back.

## Progress

**Execution Order:**

Milestone v1.0 (Phases 1–7) executed in numeric order. Phases 4 and 5 could overlap in practice; Phase 6 grammar work could start as early as Phase 1 completed.

Milestone v2.0 (Phases 8–14) executes as follows:
- Phase 8 is the rebase gate — it blocks every other v2.0 phase.
- Phase 9 blocks Phases 10, 11, 12 (they all consume v3 AST + analyzer surfaces).
- Phase 13 (tree-sitter) is independent of Phases 9–12 and can parallelize.
- Phase 14 depends on Phases 8–13 all being green (release cut absorbs v3 fixture migration + parity regeneration + extension version-range bumps).

| Phase | Plans Complete | Status | Completed |
|-------|----------------|--------|-----------|
| 1. M0 Compiler Hardening | 5/5 | Complete    | 2026-04-18 |
| 2. M1 LSP Core | 6/6 | Complete    | 2026-04-18 |
| 3. M2 Navigation & Understanding | 6/6 | Complete    | 2026-04-18 |
| 4. M3 Editing Assistance | 7/7 | Complete    | 2026-04-18 |
| 5. M4 Formatting | 5/5 | Complete    | 2026-04-19 |
| 6. M5 Grammars & Editor Extensions | 6/6 | Engineering complete (publish deferred to Phase 14) | 2026-04-22 |
| 7. M6 Production Hardening | 8/8 | Engineering complete (4 human-gated release steps deferred to Phase 14) | 2026-04-23 |
| 8. Rebase + Baseline Rebuild | 0/? | Not started | - |
| 9. AST + Analyzer Surface Adaptation | 0/? | Not started | - |
| 10. Visibility + Mutation Tier Surfacing | 3/3 | Complete    | 2026-04-28 |
| 11. Patch Extension Support | 2/3 | In Progress|  |
| 12. Keyword Mirrors + New Diagnostic Quickfixes | 3/3 | Complete    | 2026-04-29 |
| 13. tree-sitter Grammar v3 Coverage | 0/? | Not started | - |
| 14. Migration + Codemod + v3.0.0-alpha.1 Release | 0/? | Not started | - |

## Coverage

### Milestone v1.0

**Total v1 requirements:** 93
**Mapped to phases:** 93 (100%)
**Unmapped:** 0

| Phase | Requirement Count | Requirements |
|-------|-------------------|--------------|
| 1. M0 Compiler Hardening | 12 | HARD-01…HARD-12 |
| 2. M1 LSP Core | 22 | CORE-01…CORE-22 |
| 3. M2 Navigation & Understanding | 16 | NAV-01…NAV-16 |
| 4. M3 Editing Assistance | 15 | EDIT-01…EDIT-15 |
| 5. M4 Formatting | 6 | FMT-01…FMT-06 |
| 6. M5 Grammars & Editor Extensions | 10 | EXT-01…EXT-10 |
| 7. M6 Production Hardening | 12 | HARD-13…HARD-24 |
| **Total v1.0** | **93** | |

### Milestone v2.0

**Total v2.0 requirements:** 54
**Mapped to phases:** 54 (100%)
**Unmapped:** 0

| Phase | Requirement Count | Requirements |
|-------|-------------------|--------------|
| 8. Rebase + Baseline Rebuild | 5 | REC-01…REC-05 |
| 9. AST + Analyzer Surface Adaptation | 6 | AST-01…AST-06 |
| 10. Visibility + Mutation Tier Surfacing | 9 | VIS-01…VIS-05, TIER-01…TIER-04 |
| 11. Patch Extension Support | 5 | PATCH-01…PATCH-05 |
| 12. Keyword Mirrors + New Diagnostic Quickfixes | 8 | KW-01…KW-03, QF-01…QF-05 |
| 13. tree-sitter Grammar v3 Coverage | 5 | GRM-01…GRM-05 |
| 14. Migration + Codemod + v3.0.0-alpha.1 Release | 16 | MIG-01…MIG-07, CMD-01…CMD-03, REL-01…REL-06 |
| **Total v2.0** | **54** | |

---
*Roadmap created: 2026-04-16*
*Granularity: standard (v1.0 = 7 phases; v2.0 = 7 additional phases; 14 phases total across both milestones)*
*Phase structure source: v1.0 from PROJECT.md M0–M6 milestones + research/SUMMARY.md; v2.0 from REQUIREMENTS.md v2.0 section + planner-supplied envelope (REC → AST → [VIS/TIER | PATCH | KW/QF | GRM in parallel] → MIG/CMD/REL)*
*Last updated: 2026-04-24 — milestone v2.0 phases 8–14 added*
