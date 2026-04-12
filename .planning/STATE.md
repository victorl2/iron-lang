---
gsd_state_version: 1.0
milestone: v1.1
milestone_name: milestone
status: planning
stopped_at: Completed 09-shell-template-audio-autoplay-unlock plan-01 web-shell-template-header
last_updated: "2026-04-12T02:31:00Z"
last_activity: "2026-04-10 — ROADMAP.md created from research synthesis (87/87 v1 requirements mapped across 14 phases, Phase 14 deferred/gated), then synced main from 4de97c8 → c517aef (v1.1.0-alpha, PR #13 merged), refocused WEB-BOOT-02 from PR #13 to PR #17 conflict zone, bumped WEB-TEST-11 baseline from 293 → 333 tests."
progress:
  total_phases: 14
  completed_phases: 8
  total_plans: 25
  completed_plans: 25
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
| Phase 05-lir-main-loop-split-pass-high-risk P02 | 722s | 3 tasks | 3 files |
| Phase 05 P03 | 1200 | 4 tasks | 4 files |
| Phase 06-emit-web-c-wrapper-medium-risk P01 | 681s | 2 tasks | 2 files |
| Phase 06-emit-web-c-wrapper-medium-risk P02 | 606 | 2 tasks | 3 files |
| Phase 06-emit-web-c-wrapper-medium-risk PP03 | 1018s | 3 tasks | 5 files |
| Phase 07-build-web-c-emcc-orchestration P01 | 509s | 1 tasks | 2 files |
| Phase 07-build-web-c-emcc-orchestration P02 | 12m | 1 tasks | 1 files |
| Phase 07-build-web-c-emcc-orchestration P03 | 668 | 1 tasks | 1 files |
| Phase 07-build-web-c-emcc-orchestration P04 | 600 | 2 tasks | 5 files |
| Phase 08-raylib-web-integration-amalgamation P01 | 31540024s | 1 tasks | 1 files |
| Phase 08-raylib-web-integration-amalgamation P02 | 15 | 3 tasks | 3 files |
| Phase 09-shell-template-audio-autoplay-unlock P01 | 2m | 2 tasks | 1 files |

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
- [Phase 05-lir-main-loop-split-pass-high-risk]: ws_ helpers copied from lir_optimize.c (not exposed via header) to avoid mutating a heavily-included interface
- [Phase 05-lir-main-loop-split-pass-high-risk]: Metadata-only pass in Phase 5; LOAD/STORE rewrites deferred to Phase 6 emit_web.c per RESEARCH.md recommendation
- [Phase 05]: build.c call site runs only on native path today — web builds short-circuit to iron_build_web() before LIR pipeline; Phase 6 refactors iron_build_web to reuse this pipeline
- [Phase 05]: 7-case unit test suite (1 extra: read-only capture is_mutable=false) strengthens WEB-EMIT-03 coverage beyond minimum-6 requirement
- [Phase 06-emit-web-c-wrapper-medium-risk]: emit_func_signature/body/instr promoted from static to extern in emit_c.c; declared in emit_helpers.h under Phase 6 section — pure visibility change, zero behavioral impact
- [Phase 06-emit-web-c-wrapper-medium-risk]: ROADMAP.md Phase 6 SC3/SC4 already had correct retired-invariant language pre-populated by planner; Task 2 was a no-op verify
- [Phase 06-emit-web-c-wrapper-medium-risk]: emit_web.c uses _e=state local alias for emit_instr lambda-capture path compatibility; header BRANCH terminator suppressed and replaced by synthetic shutdown branch; no emit_c.c touches
- [Phase 06-emit-web-c-wrapper-medium-risk]: Option (a) chosen for build_web.c restructuring: iron_build_web kept as preflight (not renamed); iron_build() calls it before pipeline, falls through on 0 return
- [Phase 06-emit-web-c-wrapper-medium-risk]: IRON_SOURCE_DIR compile definition added to test_emit_web CMake target to enable emit_web.c boundary invariant test
- [Phase 07-build-web-c-emcc-orchestration]: CWD-relative 'src' for iron_src_dir mirrors invoke_clang pattern; -Isrc/stdlib added alongside -Isrc; IRON_WEB_SRC_COUNT=13 macro; (void)cfg deferred to Phase 11
- [Phase 07-build-web-c-emcc-orchestration]: strstr substring match used in is_forbidden_flag() — handles both -s<NAME> and -f<flag> forms; ASYNCIFY=1 entry preserves -sASYNCIFY=0 canonical flag without false positives
- [Phase 07-build-web-c-emcc-orchestration]: cfg=NULL passed to iron_build_web_link: Phase 7 hardcodes canonical flags; [web] config overrides deferred to Phase 11
- [Phase 07-build-web-c-emcc-orchestration]: posix_spawnp for emrun (PATH-resolved) vs posix_spawn for native binary (explicit path) — deliberate distinction
- [Phase 07-build-web-c-emcc-orchestration]: return 0 on emrun ENOENT: build artifacts exist; iron run is best-effort affordance, not a build requirement
- [Phase 07-build-web-c-emcc-orchestration]: Phase 7 end-to-end smoke uses Debug build type in CI (faster than Release; size budget deferred to Phase 13)
- [Phase 07-build-web-c-emcc-orchestration]: Windows support for build_web.c deferred — find_emcc() does not probe emcc.bat/emcc.cmd; mirrors Phase 1 Linux/macOS-only CI decision
- [Phase 08-raylib-web-integration-amalgamation]: Single-token -Isrc/vendor/raylib form and string literals for all 4 new argv entries — no malloc/free, simpler cleanup paths
- [Phase 08-raylib-web-integration-amalgamation]: No -I src/vendor/raylib/external/glfw/include added — -sUSE_GLFW=3 already in IRON_WEB_CANONICAL_FLAGS provides GLFW/glfw3.h via emcc sysroot
- [Phase 08-raylib-web-integration-amalgamation]: WHITE color constant used (not RAYWHITE): WHITE defined in raylib.iron as Color(255,255,255,255); RAYWHITE is C-level macro not exposed in Iron bindings
- [Phase 09-shell-template-audio-autoplay-unlock]: Dropped FileSaver.js CDN + saveFileFromMEMFSToDisk from minshell.html — reduces external dependencies; Iron does not use them
- [Phase 09-shell-template-audio-autoplay-unlock]: tabindex=-1 retained (minshell default) — keydown listener attaches to document not canvas so tabindex irrelevant for audio unlock
- [Phase 09-shell-template-audio-autoplay-unlock]: Adjacent-string-literal concatenation (one C string per HTML line) — keeps diffs line-local, C89/C99 compatible

### Pending Todos

None yet.

### Blockers/Concerns

- **Phase 14 (Iron-Native Dev Server)** — BLOCKED on parallel networking milestone's HTTP server landing on `main`. Phases 1–13 ship independently of this block.
- **Phase 5 (LIR main-loop split)** — HIGH risk; needs `/gsd:research-phase` spike during planning to verify closure-capture machinery and `emit_helpers.h` surface.
- **Phase 6 (emit_web.c)** — MEDIUM risk; depends on `emit_helpers.h` exposing enough surface without touching `emit_c.c`.
- **Phase 9 (shell template AudioContext tracking)** — LOW-MEDIUM risk; may require porting Proxy tracking from `shell.html:306-335` into the default shell.
- **Phase 10 (analyzer top-level loader error)** — MEDIUM risk; depends on Iron analyzer having "forbidden call at top-level" diagnostic infrastructure.

## Session Continuity

Last session: 2026-04-12T02:31:00Z
Stopped at: Completed 09-shell-template-audio-autoplay-unlock plan-01 web-shell-template-header
Resume file: None
