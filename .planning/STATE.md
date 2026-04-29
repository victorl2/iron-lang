---
gsd_state_version: 1.0
milestone: v1.0
milestone_name: — Full-scope LSP
status: executing
stopped_at: Phase 12 context gathered
last_updated: "2026-04-29T14:20:14.772Z"
last_activity: 2026-04-29
progress:
  total_phases: 12
  completed_phases: 12
  total_plans: 57
  completed_plans: 60
  percent: 100
---

# Project State

## Project Reference

See: .planning/PROJECT.md (updated 2026-04-24 — milestone v2.0 started)

**Core value:** An Iron programmer opens a `.iron` file in their editor and gets correct, fast, process-stable language intelligence that never diverges from what `ironc` compiles.
**Current focus:** Phase 12 — keyword-mirrors-quickfixes

## Current Position

Phase: 12
Plan: Not started
Last completed phase: 9 (AST + Analyzer Surface Adaptation — 3 plans, 10 commits, verifier 6/6 PASS)
Status: Executing Phase 12
Last activity: 2026-04-29

Progress: [██░░░░░░░░] 28% (2/7 v2.0 phases complete — Phase 8 + Phase 9)

## Performance Metrics

**Velocity:**

- Total plans completed: 23
- Average duration: — min
- Total execution time: 0.0 hours

**By Phase:**

| Phase | Plans | Total | Avg/Plan |
|-------|-------|-------|----------|
| 2 | 6 | - | - |
| 3 | 6 | - | - |
| 05 | 5 | - | - |
| 10 | 3 | - | - |
| 12 | 3 | - | - |

**Recent Trend:**

- Last 5 plans: —
- Trend: — (no data yet)

*Updated after each plan completion*
| Phase 02 P01 | 8min | 3 tasks | 16 files |
| Phase 02 P02 | 8min | 3 tasks | 14 files |
| Phase 02 P03 | 14min | 3 tasks tasks | 19 files files |
| Phase 02 P04 | 14min+rl-resume | 2 tasks | 16 files |
| Phase 02 P05 | 19min | 3 tasks tasks | 18 files files |
| Phase 02 P06 | 31min | 3 tasks tasks | 22 files files |
| Phase 03 P01 | 26min | 4 tasks | 50 files |
| Phase 03 P03 | 25min | 3 tasks | 24 files |
| Phase 03-m2-navigation-understanding P04 | 21min | 3 tasks | 20 files |
| Phase 03-m2-navigation-understanding P05 | 21min | 2 tasks | 16 files |
| Phase 03 P06 | 16min | 2 tasks | 19 files |
| Phase 04-m3-editing-assistance P01 | 75m | 2 tasks | 23 files |
| Phase 04-m3-editing-assistance P05 | 55min | 2 tasks tasks | 9 files files |
| Phase 04-m3-editing-assistance P07 | 20m | 3 tasks tasks | 10 files files |
| Phase 05-m4-formatting P02 | 35min | 3 tasks tasks | 21 files files |
| Phase 05-m4-formatting P03 | 25min | 2 tasks | 12 files |
| Phase 05-m4-formatting P04 | 9min | 2 tasks tasks | 9 files files |
| Phase 05-m4-formatting P05 | 2h | 3 tasks | 21 files |
| Phase 06 P02 | 1h45m | 3 tasks | 18 files |
| Phase Phase 06 PP03 | 7 min | 3 tasks tasks | 18 files files |
| Phase 06 P06 | 9 min | 2 tasks | 7 files |
| Phase 07 P02 | 12m | 2 tasks | 64 files |
| Phase 07 P03 | 6 min | 1 tasks | 11 files |
| Phase 7 P5 | 13min | 2 tasks | 24 files |
| Phase 07 P06 | 12min | 2 tasks tasks | 4 files files |
| Phase 07 P07 | 8 min | 2 tasks | 13 files |
| Phase 07 P08 | 25m | 2 tasks tasks | 10 files files |
| Phase 09 P02 | 1393 | 3 tasks | 113 files |
| Phase 11 P01 | 95min | 4 tasks tasks | 9 files files |
| Phase 11 P02 | 19min | 5 tasks | 13 files |

## Accumulated Context

### Decisions

Decisions are logged in PROJECT.md Key Decisions table.
Recent decisions affecting current work:

- Init: LSP lives in-tree inside `iron-lang/`, linking `iron_compiler` as a PUBLIC static lib (C17)
- Init: Scope locked — type hierarchy + pull diagnostics IN; semantic tokens / call hierarchy / inlay hints OUT
- Init: 7-phase roadmap (M0–M6) reinforced by all 4 research dimensions; no new milestones needed
- Init: `test_parity_ironc_lsp` runs as a cross-phase thread — built P1, wired P2, blocking gate P7
- [Phase 02]: Vendored yyjson 0.12.0 as a two-file drop-in (Plan 02-01): reviewable diff, no network on configure
- [Phase 02]: IRON_BUILD_LSP defaults ON on Linux/macOS, hard-fails with FATAL_ERROR on Windows (Plan 02-01)
- [Phase 02]: phase-m1-invariant CTest label established on unit + smoke tests from day 1 (Plan 02-01)
- [Phase 02]: 10 MB Content-Length cap for T-02-02 DoS mitigation; rejected before body allocation (Plan 02-02)
- [Phase 02]: yyjson bound to Iron_Arena: free is no-op, realloc reuses pointer on shrink (bump-allocator invariant) (Plan 02-02)
- [Phase 02]: Writer queue capacity 256 + drop policy LOG -> NOTIFICATION -> FULL_DROPPED; responses never dropped while lower-priority slots exist (Plan 02-02)
- [Phase 02]: LF-only lenience in framer (LSP spec mandates CRLF) for hand-rolled-client compatibility (Plan 02-02)
- [Phase 02]: All transport threading via IRON_THREAD_* / IRON_MUTEX_* / IRON_COND_* macros; zero direct pthread_* in src/lsp/transport (Plan 02-02)
- [Phase 02]: Handler table is compile-time const + bsearch; capability matrix derived 1:1 from handler registry (PITFALLS.md #15 structurally unviolable) (Plan 02-03)
- [Phase 02]: Lifecycle gate runs BEFORE handler lookup in ilsp_dispatch_route (spec order: pre-init requests return -32002 regardless of method registration) (Plan 02-03)
- [Phase 02]: Cancel registry uses stb_ds sh_new_strdup map + iron_mutex_t + heap-owned _Atomic bool*; HARD-05 poll primitive is the consumer contract on worker threads (Plan 02-03)
- [Phase 02]: ilsp_exit_fn is non-static file-scope function-pointer seam so Unity tests extern-override without terminating; production default is _exit (Plan 02-03)
- [Phase 02]: malloc-backed document buffer is the deliberate CLAUDE.md arena exception -- long-lived buffers need realloc + individual-free semantics (Plan 02-04)
- [Phase 02]: Version monotonicity guard returns false WITHOUT mutation; multi-event didChange batches step intermediate versions by +1 (Plan 02-04)
- [Phase 02]: OOB range clamping at end-of-line is spec-correct LSP Range behavior; only inverted ranges (start > end) are rejected (Plan 02-04)
- [Phase 02]: contentHash drift detection is log-only (no rollback); client resolves drift via didClose+didOpen (Plan 02-04)
- [Phase 02]: Handler-table-driven capability auto-advertise: one-row dispatch.c edit flowed textDocumentSync into capabilities response + its test without modifying capabilities.c (Plan 02-04)
- [Phase 02]: Plan 05: coalescing mailbox with replace-in-place COMPILE slot; SHUTDOWN priority; 4-slot dequeue order
- [Phase 02]: Plan 05: 250ms debounce via iron_cond_timedwait_ms (Phase 1 primitive reused); worker re-dequeues newest coalesced version
- [Phase 02]: Plan 05: SIGABRT boundary uses _Thread_local doc ptr + sigaction(SA_SIGINFO | SA_NODEFER); siglongjmp into doc->abort_jmp
- [Phase 02]: Plan 05: second-strike quarantine via atomic_store(&quarantined, true); drains mailbox without compile
- [Phase 02]: Plan 05: facade is the SINGLE iron_analyze_buffer call site in src/lsp (enforced by CORE-22 parity)
- [Phase 02]: Plan 05: per-request 64KB Iron_Arena scoped to each facade call (HARD-06 contract)
- [Phase 02]: Plan 05: per-version cancel flag key <uri>#v<version> for race-safe compile cancellation
- [Phase 02]: Plan 05: window/showMessage Warning(2) on 1st strike, Error(1) on 2nd + quarantine
- [Phase 02]: Plan 05: resultId = v<version> on DocumentDiagnosticReport for client-side cache correlation
- [Phase 02]: Plan 05: parity harness 4th pass asserts facade vs direct-LSP (not CLI) -- keeps facade-correctness + LSP-vs-CLI-policy axes separate
- [Phase 02]: Plan 05: CORE-22 proof live -- facade_cli_mismatches=0 across all 381 fixtures
- [Phase 02]: Plan 06: XDG JSON-line log with pthread_once singleton + 4-level filter; log + trace are the documented CLAUDE.md static-mutable-state exception
- [Phase 02]: Plan 06: reader switched from fread() to read(2) via fileno(source) -- fread blocks waiting for CHUNK_SIZE bytes on a pipe; read(2) returns whatever is available
- [Phase 02]: Plan 06: pytest-lsp 1.0 is in beta on PyPI -- requirements + CI install pin '>=1.0.0b1' with --pre
- [Phase 02]: Plan 06: lsp_smoke CTest probes pytest-lsp at configure time; registers SKIP shim (exit 77) when missing so local dev stays green without the Python dep
- [Phase 02]: Plan 06: conftest stubs client/registerCapability as a no-op so pygls does not MethodNotFound on the server post-init watchers
- [Phase 02]: Plan 06: Phase 2 closes with all 22 CORE-XX requirements green; 47 RUN_TESTs across Unity + smoke + parity
- [Phase 03]: [Phase 03]: Plan 01: IRON_TOK_DOC_COMMENT appended before IRON_TOK_COUNT; iron_skip_newlines absorbs doc tokens; iron_collect_doc_run runs backwards-scan with two-step newline counting for blank-line association breaks
- [Phase 03]: [Phase 03]: Plan 01: bool sealed on Iron_Program + IRON_AST_ASSERT_UNSEALED macro (debug iron_ice / release (void)0); docs/dev/AST_CONTRACT.md locks the read-only contract
- [Phase 03]: [Phase 03]: Plan 01: Iron_AnalyzeResult gains .program field; set on successful parse and even on cancel bailouts so partial trees remain walkable
- [Phase 03]: [Phase 03]: Plan 01: IronLsp_SymbolId triple (canonical_path, name_path, kind) + FNV-1a 64-bit hash (pinned 0xcbf29ce484222325 / 0x100000001b3)
- [Phase 03]: [Phase 03]: Plan 01: ilsp_nav_node_at descends one level (object fields / iface method_sigs / enum variants); expression-body walking deferred to later NAV plans
- [Phase 03]: [Phase 03]: Plan 01: Wave 0 scaffold pattern established — TEST_IGNORE_MESSAGE stub + phase-m2-invariant label; flip to real by extending CMake source list
- [Phase 03]: [Phase 03]: Plan 01: lsp_smoke --ignore=nav/ so m1 pytest harness does not see m2 skipped stubs
- [Phase 03]: Plan 03: fzy constants pinned verbatim (-0.005/-0.01/1.0/0.9/0.8/0.7/0.6); MATCH_MAX_LEN=1024 DoS fallback; 5 nav facade TUs routed through ilsp_facade_compile_for_nav preserving CORE-22 single-call-site at 1 hit
- [Phase 03]: Plan 03: 5 dispatch rows alphabetically inserted (declaration, definition, documentSymbol, typeDefinition, workspace/symbol); capabilities auto-advertise via compile-time-const matrix; no capabilities.c edits needed this plan
- [Phase 03]: Plan 03: workspace/symbol ordering (score DESC, kind_rank ASC, canonical_path ASC, line ASC, col ASC); 256 result cap + 256-char query cap (T-03-07); cancel polled per entry
- [Phase 03-m2-navigation-understanding]: Plan 04: D-09 stdlib/dep filter is UNCONDITIONAL in ilsp_refs_query; no policy flag, no conditional path
- [Phase 03-m2-navigation-understanding]: Plan 04: analyze_lazy now routes through ilsp_facade_compile_for_nav to capture annotated Iron_Program; CORE-22 single-call-site preserved via shared facade_analyze static helper
- [Phase 03-m2-navigation-understanding]: Plan 04: Pitfall 6 drop-before-populate is an invariant -- ilsp_refs_populate_for_entry always calls drop first, and workspace_index invalidate_path_locked calls drop before free_entry
- [Phase 03-m2-navigation-understanding]: Plan 04: signatureHelpProvider shape override lives in capabilities.c caps_add (not handler-table auto-derive) -- the object shape with triggerCharacters cannot be produced by boolean default
- [Phase 03-m2-navigation-understanding]: Plan 05: Parallel contribution map in iface_workspace (not field on IndexEntry) to minimise cross-module coupling
- [Phase 03-m2-navigation-understanding]: Plan 05: typeHierarchy data round-trip via item.data {canonical_path,name_path,kind,hash} -- server never re-walks AST for follow-up super/subtypes calls
- [Phase 03-m2-navigation-understanding]: Plan 05: W-1 cancel-truncation -- subtypes object branch polls cancel_flag between workspace_index entries (D-16 iteration-boundary rule)
- [Phase 03]: Plan 06: workspace/diagnostic/refresh is a server->client REQUEST per LSP 3.17 (not a notification); pygls/lsprotocol rejects bodies without id -- emitter now allocates id from server->next_request_id
- [Phase 03]: Plan 06: IronLsp_WsDiagCache is eager-create at main.c startup (not lazy at initialize); per-slot 16KB arena clone_diaglist keeps cached DiagList independent of workspace_index entry invalidation
- [Phase 03]: Plan 06: refresh emits on MANIFEST and LOCKFILE invalidations (every file impacted by dep map cascade) + non-open SOURCE only; open-doc source events stay silent since publishDiagnostics push covers them
- [Phase 03]: Plan 06: 2s wall-clock budget = 1800ms analyze + 200ms serialize; files not analyzed at deadline return kind=unchanged with previousResultId; D-16 cancel polled at every per-file iteration boundary
- [Phase 03]: Plan 06: smoke conftest installs no-op workspace/diagnostic/refresh responder for all clients so inbound refresh requests do not raise MethodNotFound; mirrors existing client/registerCapability stub
- [Phase 03]: Plan 06: Phase 3 CLOSE -- all 16 NAV-* requirements Complete, CORE-22 single iron_analyze_buffer call site preserved at 1 hit through all 6 plans, test_parity_ironc_lsp byte-for-byte green, 17 Unity + 7 pytest-lsp smoke tests all live
- [Phase 04-m3-editing-assistance]: Helper-routing over inline for typo-candidate seeding — emit_undefined is the single call-site.
- [Phase 04-m3-editing-assistance]: Empty-string .suggestion as delete-line sentinel for IRON_WARN_UNUSED_IMPORT.
- [Phase 04-m3-editing-assistance]: Plan 05: sentinel code ILSP_ORGANIZE_IMPORTS_SENTINEL=-1 sits outside every diagnostic-code namespace (lex 1-99/parser 101-199/semantic 200-299/LIR 300-399/lowering 400-499/HIR 500-599/warn 600+), so ilsp_quickfix_lookup(-1)==NULL and resolve.c sentinel branch routes before registry lookup
- [Phase 04-m3-editing-assistance]: Plan 05: organizeImports is EXPLICIT-trigger only -- filter detector requires 'source.organizeImports' or broader 'source' prefix match; plain codeAction + only=['quickfix'] never surface it (matches VSCode Source Actions + editor.codeActionsOnSave paths)
- [Phase 04-m3-editing-assistance]: Plan 05: unused detection probes Iron_DiagList for IRON_WARN_UNUSED_IMPORT at matching line (reuses compiler's resolve.c:emit_unused_imports) rather than calling ilsp_refs_query -- avoids duplicated usage-tracking logic; bulk_analyze_done remains the user-visible Pitfall F policy gate
- [Phase 04-m3-editing-assistance]: Plan 05: window/showMessage Info fires from BOTH paths (initial codeAction pre-flight AND resolve) on cold workspace -- duplicate Info message is cheap; always-on feedback preferred over miss-at-application
- [Phase 04]: PITFALL B iface-fan-out uses conservative over-approximation: any stdlib/dep implementor aborts the entire rename (method-name exact match deferred to Phase 7)
- [Phase 04]: Scope-at-cursor collision detection for closed-file use-sites deferred to Phase 7; v1 runs collision check with same-name + empty-name + keyword guards only
- [Phase 04]: Rename keyword guard reuses Plan 04-02 configure_file keyword_mirror.h — byte-identical with lexer.c kw_table via existing CMake drift-guard test
- [Phase 04-m3-editing-assistance]: Plan 07: documentHighlight routes through ilsp_facade_compile_for_nav; CORE-22 single iron_analyze_buffer site preserved at 1 across ALL 7 Phase 4 plans
- [Phase 04-m3-editing-assistance]: Plan 07: foldingRange + selectionRange strictly parser-only (iron_lex_all + iron_parse); 0 analyzer-seam calls — endpoints survive on syntactically broken files
- [Phase 04-m3-editing-assistance]: Plan 07: selectionRange positions[] clamped to 256 (T-4-2 mitigation; matches workspace/symbol precedent)
- [Phase 04-m3-editing-assistance]: Plan 07: Iron AST has no IRON_NODE_COMPOUND_ASSIGN — compound = / += / -= all live in IRON_NODE_ASSIGN with Iron_OpKind op tag; highlight classifier merges both cases into role-based LHS/RHS rule
- [Phase 04-m3-editing-assistance]: Plan 07 CLOSES PHASE 4: all 15 EDIT-01..EDIT-15 requirements Complete; 20 phase-m3-invariant tests green; parity byte-for-byte preserved
- [Phase 05-m4-formatting]: Plan 05-02: single iron_format_source call site under src/lsp/ enforced by grep-invariant CTest; facade_format static helper routes all fmt entries
- [Phase 05-m4-formatting]: Plan 05-02: workspace_index.fmt_opts cached at initialize + reloaded on iron.toml didChangeWatchedFiles (D-13); _Atomic int_least64_t version stamp for T-05-02-05 mitigation
- [Phase 05-m4-formatting]: Plan 05-02: stub dispatch rows for range/on_type return empty TextEdit[] so capability advertisement is honest while Plans 05-03/04 fill bodies
- [Phase 05-m4-formatting]: Plan 05-02: window/logMessage sibling builder added to notifications.c; both show/log delegate to shared static send_window_message
- [Phase 05-m4-formatting]: Plan 05-02: workspace_root auto-resolved from initialize params (workspaceFolders[0].uri preferred, rootUri fallback) -- Phase 2/3 left this unwired; fmt caching requires it
- [Phase 05-m4-formatting]: Plan 05-03: format_internal.h private header distributes ilsp_facade_fmt_lex_parse to sibling TUs; impl lives in format.c so the single iron_format_source call site stays at 1 (D-10)
- [Phase 05-m4-formatting]: Plan 05-03: range walker uses stb_ds heap-header + arena-body swap pattern; per-decl cancel poll at top of for-loop bounds cancel-to-return latency to one atomic_load
- [Phase 05-m4-formatting]: Plan 05-03: RESEARCH Pitfall 4 (any-overlap + end.character==0 exclusive-line semantics) codified in decl_intersects_range; Unity test test_range_starts_on_decl_last_line_includes_decl locks the boundary case
- [Phase 05-m4-formatting]: Plan 05-03: Iron syntax locked to func + val across all fixtures (planner templates used fn + let which the Iron grammar rejects)
- [Phase 05-m4-formatting]: Plan 05-04: find_enclosing_block walks Iron_Program.decls[] and descends FUNC_DECL.body / METHOD_DECL.body / IRON_NODE_BLOCK.stmts[] only; OBJECT/INTERFACE/ENUM bodies deferred to v1.x
- [Phase 05-m4-formatting]: Plan 05-04: outer-depth via brace counting (v1 heuristic) -- documented cosmetic failure mode; v1.x upgrade path = AST-ancestor counting
- [Phase 05-m4-formatting]: Plan 05-04: documentOnTypeFormattingProvider shape override in capabilities.c caps_add mirrors signatureHelpProvider byte-for-byte; fmt + range providers remain auto-derived boolean
- [Phase 05-m4-formatting]: Plan 05-04: ilsp_byte_of_line (not ilsp_line_index_offset_for_line which doesn't exist) is the correct line_index.h helper
- [Phase 05-m4-formatting]: Plan 05-04: pytest-lsp on-type API = client.text_document_on_type_formatting_async(DocumentOnTypeFormattingParams); server_capabilities.document_on_type_formatting_provider.first_trigger_character on client side
- [Phase 05-m4-formatting]: Plan 05-05: test_fmt_quickfix_clean (D-07) + test_fmt_idempotent (D-09) phase-closeout gates green; 10 phase-m4-invariant CTests live; ilsp_span_to_lsp_range LSP-exclusive fix + iron_parse_call_args_ex RPAREN capture fix + 3 Phase 4 quickfix canonical-output fixes caught by the gate
- [Phase 05-m4-formatting]: Plan 05-05: Phase 5 CLOSE -- all 6 FMT-01..FMT-06 requirements Complete; 10 phase-m4-invariant tests green; cross-phase regression zero; ready for /gsd-verify-work
- [Phase 06]: Iron syntax corrections (comments --, interpolation {expr}, match arms ->, no top-level impl) applied as Rule 1 plan-bug auto-fixes; grammar now matches real Iron per src/lexer/lexer.c + src/parser/parser.c, not a hypothetical C-like model
- [Phase 06]: Replaced Plan 06-02's proposed impl_declaration with method_declaration (+ array_extension) matching iron_parse_func_or_method dispatch — no top-level impl exists in Iron AST
- [Phase 06]: Re-pinned tree-sitter-cli: package.json 0.25.10 (glibc 2.34 compat), CI 0.26.3 (latest 0.26.x). Plan 06-01's 0.26.1 does not exist on npm. Both CLIs produce identical ABI 15 output.
- [Phase Phase 06]: License on editors/vscode/ switched from plan-specified MIT to Apache-2.0 to match repo root (legal consistency).
- [Phase Phase 06]: tsconfig rootDir set to '.' (not 'src') so pretest:e2e tsc can emit both src/ and test/ under out/.
- [Phase Phase 06]: vscode-languageclient@9.0.1 bundled into dist/extension.js by esbuild so .vscodeignore can omit node_modules and keep .vsix under 5 MB.
- [Phase 06]: Phase 6 CLOSE: all 10 EXT-01..EXT-10 requirements complete; 6/6 plans shipped; zero new iron_analyze_buffer / iron_format_source call sites (Core Value parity preserved); CI consolidation audit green (no repairs needed); EXT-07 post-v1 PR tracked via docs/dev/nvim-lspconfig-upstream.md; real v1.2.0-alpha.6 release cut remains human-gated per D-11
- [Phase 07]: Bypass async writer queue for cap-exceed logMessage — write framed bytes directly to fd 1 before _exit(42)
- [Phase 07]: Strip interleaved idle-sleep events from committed workloads to keep event count exactly 28800/1800 per plan done-criterion
- [Phase 07]: IRON_SANITIZER default is 'address' to preserve pre-Phase-7 ASan+UBSan behaviour; 'thread' is strictly opt-in for the TSAN CI job
- [Phase 07]: Extended existing SANITIZERS+FUZZING mutex diagnostic to name 'thread' explicitly instead of adding a parallel TSAN-specific fire-site; keeps the mutex graph single-source-of-truth
- [Phase 07]: Line-anchored WARNING regex (re.MULTILINE '^') prevents log-payload substrings from false-firing the TSAN detector; locked in by driver.py --self-test case 4
- [Phase 07]: macOS TSAN job deferred to v2 OPS; driver.py refuses sys.platform=='darwin' with exit 2 so future accidental macos-latest addition to tsan.yml fails fast with a clear diagnostic
- [Phase 7]: Plan 07-05: fuzz_lsp_dispatch fuzzes pure paths (ilsp_handler_lookup + JSON parse/extract) rather than ilsp_dispatch_route to avoid spinning up a full server singleton per iteration; handler_stubs.c provides __builtin_trap stubs for all 38 ilsp_handle_* symbols so dispatch.c links
- [Phase 7]: Plan 07-05: fuzz.yml matrix restructured from 3-cell flat list to 7-cell strategy.matrix.include with per-cell {target, binary, corpus, dict, build_target} fields so compiler-side and LSP-side cells can share the same run/upload/triage pipeline while pointing at different binaries and corpus paths
- [Phase 7]: Plan 07-05: corpora live in-tree under tests/fuzz/lsp/*/corpus/ (hand-crafted seeds per D-07; CODEOWNERS-gated) rather than build/tests/fuzz/corpus/ (compiler-side convention where iron_seed_blobs auto-populates from tests/integration/)
- [Phase 07]: 07-06: macOS ironls Developer-ID signed + notarized + stapled via new sign-and-notarize-macos job in release.yml; scripts/ci/sign_and_notarize_macos.sh encapsulates 8-step flow with 1200s primary timeout + 30-min polling fallback
- [Phase 07]: 07-07: ilsp_server_version() runtime accessor routes CLI --version + initialize serverInfo.version through the same IRON_VERSION_STRING compile-time define; test_version_stamp_coherence pins byte-for-byte parity across iron/ironc/ironls
- [Phase 07]: 07-07: extension-side hard-refuse across VSCode + Zed + Neovim with consistent >= 1.2.0, < 2.0.0 same-major range; version_constraints section added to Zed extension.toml per D-10
- [Phase 07]: 07-07: pre-HARD-22 binaries (no --version support) hit the same refuse path as explicit version mismatch; structural guarantee that unstamped binaries cannot slip past the extension contract
- [Phase 07]: Dedicated single-job workflow parity.yml (not matrix) for stable branch-protection required-check name
- [Phase 07]: IRON_VERSION_FULL bumped to 1.2.0-alpha.7 (one-line CMakeLists change; propagates via 07-07 plumbing)
- [Phase 07]: Branch-protection apply is HUMAN-GATED; docs/dev/ci-gates.md is the canonical 8-check list
- [Milestone v2.0]: Phase envelope set by planner: 8 (Rebase/HIGH) → 9 (AST) → [10 (VIS+TIER) | 11 (PATCH) | 12 (KW+QF) | 13 (tree-sitter, parallelizable) ] → 14 (MIG+CMD+REL/MEDIUM)
- [Milestone v2.0]: Phase 14 absorbs the 4 deferred v1 human-gated release steps (branch protection, Apple secrets, release tag, Marketplace/Zed publish) — first execution happens with v3.0.0-alpha.1 binaries
- [Milestone v2.0]: IRON_VERSION_FULL bumped to 3.0.0-alpha.1 in Phase 8 (REC-04); binary version tracks Iron's language version even though the LSP milestone itself is v2.0 (documented in docs/dev/versioning.md via REL-01)
- [Milestone v2.0]: Parity cutover is atomic in Phase 14 MIG-02 — v3-migrated integration corpus + regenerated goldens flip test_parity_ironc_lsp + _fmt green under native v3 syntax in one change
- [Milestone v2.0]: Grammar drift-guard (KW-01/02) auto-regenerates for 6 new keywords via Phase 4 D-01 + Phase 6 D-01 configure_file pattern — zero manual grammar keyword edits required
- [Phase 09]: D-10 Approach 1 implemented: two-pass method-merge re-emits is_receiver_form methods inside owning Iron_ObjectDecl body during print
- [Phase 09]: is_pub field absent on Iron_FuncDecl + Iron_MethodDecl per parser drop semantics; printer emits readonly/pure on those nodes only, pub on Iron_Field only
- [Phase 09]: Wave 0 baseline re-stamped at Task 3 to reflect AST-05 output on the 108 non-v3-named fixtures already containing v3 syntax (Phase 8 corpus migration)
- [Phase 11]: Plan 11-01: ilsp_patch_for_each_method walks program->decls directly (resolve.c:1165-1195 flush-aware idiom) instead of using iron_type_patch_registry_build — registry requires global_scope + segfaults on NULL diags
- [Phase 11]: Plan 11-01: helper signatures take IronLsp_WorkspaceIndex* (NOT IronLsp_DepMap*) per RESEARCH Conflict 1 — IronLsp_DepEntry carries no parsed Iron_Program
- [Phase 11]: Plan 11-01: PATCH-01 same-file fallback path is the patch-aware site (option a accepted); iface_workspace conflation untouched (Pitfall 5)
- [Phase 11]: Plan 11-02: Atomic IronLsp_TypeHierarchyItem.detail struct extension (Conflict 2) lands in single commit + JSON serializer conditional emit
- [Phase 11]: Plan 11-02: PATCH-02 emission via ilsp_patch_for_each_method visitor in subtypes resolver; sort interleaved children by range.start.line ascending
- [Phase 11]: Plan 11-02: PATCH-03 emit_member_fields native walk also gains TIER-03 prefix (parity with Phase 10 D-10 emit_top_level idiom); patch walk routes through same maybe_push
- [Phase 11]: Plan 11-02: Pre-existing Rule 3 link-graph fix — visibility.c + patch_lookup.c registered in _LSP_PLAN03_SERVER_SRC unblocking 6 m1-invariant binaries broken since Phase 10/Plan 11-01
- [Phase 11]: Plan 11-02: Pitfall 2 (prepare-on-primitive) deferred to Plan 11-03 closeout — workaround documented (cursor on patch object decl token resolves to ObjectDecl with canonical_path)

### Pending Todos

None yet.

### Blockers/Concerns

- Phase 8 is flagged **RISK: HIGH** — rebase gate; merge conflicts across `src/lsp/main.c`, `src/lsp/server/dispatch.c`, `CMakeLists.txt`, `.github/workflows/ci.yml`; v3 compiler surface must build cleanly under `-Werror=switch-enum` before any other v2.0 phase can start. `/gsd-research-phase` recommended at planning time to pre-stage the conflict map.
- Phase 14 is flagged **RISK: MEDIUM** — absorbs the deferred v1 human-gated release steps (branch protection, Apple secrets, release tag, Marketplace/Zed publish) combined with v3 parity-golden cutover + extension version-range bumps. First-time execution of all four human-gated steps happens here.
- 4 v1 release items still deferred from Phase 7: branch protection apply, Apple Developer secrets rotation, v1.2.0-alpha.7 release tag cut, Marketplace/Zed publish. These are re-absorbed into Phase 14 against the v3.0.0-alpha.1 binary (REL-04/05/06).

## Deferred Items

Items carried forward from previous milestone close (v1.0 → v2.0):

| Category | Item | Status | Deferred At |
|----------|------|--------|-------------|
| Release | Branch protection apply on `main` (8-check required list from docs/dev/ci-gates.md) | Re-absorbed into Phase 14 (REL-04) | Phase 7 close 2026-04-23 |
| Release | Apple Developer ID secret rotation + signing certificate install | Re-absorbed into Phase 14 (REL-04) | Phase 7 close 2026-04-23 |
| Release | Tag cut for v1 — superseded by v3.0.0-alpha.1 tag in Phase 14 | Re-absorbed into Phase 14 (REL-05) | Phase 7 close 2026-04-23 |
| Release | VSCode Marketplace + Zed registry publish (was v1.2.0-alpha.6; now v3.0.0-alpha.1) | Re-absorbed into Phase 14 (REL-06) | Phase 6 close 2026-04-22 |

## Session Continuity

Last session: 2026-04-28T21:23:36.030Z
Stopped at: Phase 12 context gathered
Resume file: .planning/phases/12-keyword-mirrors-quickfixes/12-CONTEXT.md
