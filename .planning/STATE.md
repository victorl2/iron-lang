---
gsd_state_version: 1.0
milestone: v1.1
milestone_name: milestone
status: planning
stopped_at: Completed 05-lir-main-loop-split-pass-high-risk plan-01 lir-struct-and-diagnostic-codes
last_updated: "2026-04-11T19:11:55.775Z"
last_activity: "2026-04-10 — ROADMAP.md created from research synthesis (87/87 v1 requirements mapped across 14 phases, Phase 14 deferred/gated), then synced main from 4de97c8 → c517aef (v1.1.0-alpha, PR #13 merged), refocused WEB-BOOT-02 from PR #13 to PR #17 conflict zone, bumped WEB-TEST-11 baseline from 293 → 333 tests."
progress:
  total_phases: 14
  completed_phases: 4
  total_plans: 16
  completed_plans: 14
  percent: 0
---

# Project State

## Project Reference

See: `.planning/PROJECT.md` (updated 2026-04-10)

**Core value:** A user with an Iron + raylib game runs one command and gets a runnable HTML/JS/WASM bundle that plays in a modern browser with performance indistinguishable from a native build.

**Current focus:** Roadmap complete. Main synced to `c517aef` / **v1.1.0-alpha** (PR #13 merged). Ready to plan Phase 1 (Bootstrap & Guardrails). Parallel conflict zone is now PR #17 (networking foundation, draft open).

## Current Position

Phase: Not started (Phase 1 next — Bootstrap & Guardrails)
Plan: —
Status: Roadmap approved → ready to plan Phase 1
Last activity: 2026-04-10 — ROADMAP.md created from research synthesis (87/87 v1 requirements mapped across 14 phases, Phase 14 deferred/gated), then synced main from 4de97c8 → c517aef (v1.1.0-alpha, PR #13 merged), refocused WEB-BOOT-02 from PR #13 to PR #17 conflict zone, bumped WEB-TEST-11 baseline from 293 → 333 tests.

Progress: [░░░░░░░░░░] 0% (0 of TBD plans)

## Performance Metrics

**Velocity:**
- Total plans completed: 0
- Average duration: —
- Total execution time: —

**By Phase:**

| Phase | Plans | Total | Avg/Plan |
|-------|-------|-------|----------|
| — | — | — | — |

**Recent Trend:**
- Last 5 plans: —
- Trend: —

*Updated after each plan completion*
| Phase 01-bootstrap-guardrails P01 | 25 | 5 tasks | 5 files |
| Phase 02-cli-toml-scaffold P01 | 2 | 3 tasks | 3 files |
| Phase 02-cli-toml-scaffold P02 | 126 | 3 tasks | 1 files |
| Phase 02-cli-toml-scaffold P03 | 2 | 2 tasks | 1 files |
| Phase 02-cli-toml-scaffold P04 | 8m | 2 tasks | 2 files |
| Phase 02-cli-toml-scaffold P05 | 2m | 3 tasks | 2 files |
| Phase 02-cli-toml-scaffold P06 | 19m | 3 tasks | 4 files |
| Phase 03-runtime-audit-web-hardening P02 | 12m | 2 tasks | 1 files |
| Phase 03-runtime-audit-web-hardening P01 | 718 | 2 tasks | 3 files |
| Phase 03-runtime-audit-web-hardening P04 | 697s | 2 tasks | 2 files |
| Phase 03-runtime-audit-web-hardening P03 | 23m | 2 tasks | 11 files |
| Phase 04-wasm-safe-time-shim P01 | 712s | 1 tasks | 1 files |
| Phase 04-wasm-safe-time-shim P02 | 730s | 2 tasks | 2 files |
| Phase 05 P01 | 1039s | 2 tasks | 2 files |

## Accumulated Context

### Decisions

Decisions are logged in `.planning/PROJECT.md` Key Decisions table. Recent decisions affecting current work:

- **Locked:** C → Emscripten → WASM path (not direct LIR→WASM backend) — preserves 5.5k-line C emitter investment, raylib web for free
- **Locked:** Auto-transform `while(!WindowShouldClose())` at LIR emit time for `--target=web` (not Asyncify)
- **Locked:** Full pthread via Emscripten `-pthread` + SharedArrayBuffer (requires COOP/COEP hosting)
- **Locked:** Reference validation via Pong game with sound + specific tests
- **Locked:** `[web]` section in `iron.toml` for scoped target config
- **Locked:** Preload `assets/` via `--preload-file` identity mapping
- **Locked:** Iron-native dev server is Phase 14 — gated on networking milestone HTTP server landing; interim dev uses `emrun`
- **Locked:** emsdk 4.0.23 (last stable 4.x) via `.emsdk-version` file
- **Locked:** ALL new web logic lands in NEW files only — the "new-files-only" rule is now enforced against PR #17's open-diff zone (PR #13 merged 2026-04-10 so its file list is no longer a concern): avoid `src/lir/emit_c.c`, `src/lir/emit_helpers.c/h`, `src/runtime/iron_runtime.h`, `src/analyzer/resolve.c`, `src/analyzer/types.c/h`, `src/hir/hir_lower.c`, `src/parser/parser.c`, `src/parser/ast.h`, `src/stdlib/time.iron`, `src/stdlib/string.iron`. Files touched by BOTH milestones (`iron_string.c`, `iron_threads.c`, `typecheck.c`, `cli/build.c`) are WARN-only in Phase 1's forbidden-files hook.
- [Phase 01-bootstrap-guardrails]: mymindstorm/setup-emsdk@v14 has no version-file input; version read via shell step into GITHUB_OUTPUT; 4.0.23 does not appear in web.yml YAML
- [Phase 01-bootstrap-guardrails]: WEB-BOOT-02 (forbidden-files hook) dropped permanently — enforced by discipline + 333 integration tests, not by tooling
- [Phase 01-bootstrap-guardrails]: WEB-BOOT-03 retargeted from Windows CI to Linux/macOS CI only — Windows support gated on PR #17 landing
- [Phase 02-cli-toml-scaffold]: IronWebConfig embedded by value on IronProject; zero-init from calloc gives NULL/0 == not-set semantics
- [Phase 02-cli-toml-scaffold]: int used for numeric web fields matching existing plain-int style in IronProject.dep_count
- [Phase 02-cli-toml-scaffold]: Levenshtein cap at 64 chars returns INT_MAX; stack arrays v0[66]/v1[66] — no heap, no VLA
- [Phase 02-cli-toml-scaffold]: parse_toml_string_array kept static/unexported — single consumer in assets branch
- [Phase 02-cli-toml-scaffold]: No short form -t for --target — reserved for future use (CONTEXT.md decision)
- [Phase 02-cli-toml-scaffold]: iron run --target=web parses and stores flags only; emrun shell-out deferred to Phase 7
- [Phase 02-cli-toml-scaffold]: popen() used for emcc --version capture (simpler than posix_spawnp + pipe for one-shot read)
- [Phase 02-cli-toml-scaffold]: Interpretation A for -O2: append after -O3 (clang last-wins) rather than conditional — smaller diff, matches CONTEXT.md verbatim
- [Phase 02-cli-toml-scaffold]: Web dispatch placed as absolute first statement in iron_build() before base_dir resolution — web path fully owns its execution
- [Phase 02-cli-toml-scaffold]: Option B direct-compile toml.c into test_toml_web over static lib refactor — defers until second consumer exists
- [Phase 02-cli-toml-scaffold]: Dual-outcome web smoke test accepts either emcc banner or emcc-not-found — both prove dispatch reached build_web.c
- [Phase 03-runtime-audit-web-hardening]: IRON_WEB_MAX_WORKERS=4 kept private to iron_threads.c TU; elastic I/O pool uncapped on web; shutdown no-op via #ifndef __EMSCRIPTEN__ following existing #if defined(_WIN32) pattern
- [Phase 03-runtime-audit-web-hardening]: tsan-first: tsan stress test PASSED — no DCL upgrade needed for iron_string_intern; single-mutex is race-free
- [Phase 03-runtime-audit-web-hardening]: Thread-safety contract comment added to iron_string_intern documenting mutex-covers-full-critical-section and pthread_once memory-ordering
- [Phase 03-runtime-audit-web-hardening]: emcc probe uses /tmp output paths (-o /tmp/iron_rc.o) not /dev/null so test -s confirms non-empty object files
- [Phase 03-runtime-audit-web-hardening]: Grep invariant uses sh -c wrapper matching existing test_emsdk_pin_discipline style; labeled unit;web-portability for targeted ctest -L runs
- [Phase 03-runtime-audit-web-hardening]: Direct #include of cli/build.h in analyzer.h for IronBuildTarget enum — acceptable inversion for single typedef
- [Phase 03-runtime-audit-web-hardening]: test_web_await_check calls pass directly (not iron_analyze) to avoid handcrafted-AST resolve failures, matching test_concurrency.c pattern
- [Phase 04-wasm-safe-time-shim]: emscripten_sleep absent from iron_time_web.c even in comments — Asyncify-free policy enforced textually
- [Phase 04-wasm-safe-time-shim]: iron_time_now_ns uses ms-precision-in-ns-units (emscripten_get_now()*1e6); W3C performance.now() cap accepted per Phase 4 SC1
- [Phase 04-wasm-safe-time-shim]: Timer helpers duplicated verbatim from iron_time.c — iron_time_web.c is a self-contained translation unit
- [Phase 04-wasm-safe-time-shim]: Probe step renamed to reference WEB-RUNTIME-06 and WEB-RUNTIME-07; iron_time_web.c appended to single probe step (not new step) reusing Phase 3 emsdk setup
- [Phase 04-wasm-safe-time-shim]: test_iron_time_web_symbols uses sh -c for-loop (one ctest entry for all 9 symbols); test_emsdk_pin_discipline_iron_time_web labeled phase4-invariant
- [Phase 05]: web_frame_captures kept separate from capture_metadata to avoid corrupting lifted-lambda emission in emit_c.c
- [Phase 05]: 700 range chosen for Phase 5 web LIR errors; upward from 704 reserved for Phase 6+ web passes
- [Phase 05]: Single IRON_ERR_WEB_NON_CANONICAL_MAIN_LOOP covers all non-while shapes (for-loops, compound conditions) — avoids HIR-level loop-shape plumbing into LIR

### Pending Todos

None yet.

### Blockers/Concerns

- **Phase 14 (Iron-Native Dev Server)** — BLOCKED on parallel networking milestone's HTTP server landing on `main`. Phases 1–13 ship independently of this block.
- **Phase 5 (LIR main-loop split)** — HIGH risk; needs `/gsd:research-phase` spike during planning to verify closure-capture machinery and `emit_helpers.h` surface.
- **Phase 6 (emit_web.c)** — MEDIUM risk; depends on `emit_helpers.h` exposing enough surface without touching `emit_c.c`.
- **Phase 9 (shell template AudioContext tracking)** — LOW-MEDIUM risk; may require porting Proxy tracking from `shell.html:306-335` into the default shell.
- **Phase 10 (analyzer top-level loader error)** — MEDIUM risk; depends on Iron analyzer having "forbidden call at top-level" diagnostic infrastructure.

## Session Continuity

Last session: 2026-04-11T19:11:55.772Z
Stopped at: Completed 05-lir-main-loop-split-pass-high-risk plan-01 lir-struct-and-diagnostic-codes
Resume file: None
