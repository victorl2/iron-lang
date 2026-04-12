# Roadmap: Iron Compiler — WebAssembly Target

## Overview

This milestone adds an Emscripten-driven WebAssembly build target to Iron so that Iron + raylib games compile and run in desktop browsers via `iron build --target=web`. The existing C emitter stays untouched; every new line of compiler logic lands in NEW files (`src/lir/web_main_loop_split.c`, `src/lir/emit_web.c`, `src/cli/build_web.c`, `src/stdlib/iron_time_web.c`, `src/cli/web_shell_template.h`) to avoid conflicts with the parallel PR #17 (networking foundation, draft) conflict zone — PR #13 landed on main 2026-04-10, so its file list is no longer a merge-risk concern. The journey goes: lock down guardrails (emsdk pin via `.emsdk-version`, Linux/macOS web CI with emcc smoke test) → wire `--target=web` through the CLI and a new `[web]` TOML section → harden the runtime for SharedArrayBuffer → introduce a WASM-safe `iron_time_web.c` → write the highest-risk piece (the LIR main-loop split pass that heap-promotes captured locals into a frame state struct) → emit the web `main()` wrapper → orchestrate a single `emcc` link against the raylib amalgamation → wire the COOP/COEP preflight shell + audio autoplay unlock → preload assets with a top-level loader guard → settle the `dist/web/` layout → prove the whole thing end-to-end with a Pong reference game → lock the path with integration tests → and finally (deferred, gated on the parallel networking milestone) replace `emrun` with an Iron-native `iron serve`.

**Baseline:** `main` @ c517aef (**v1.1.0-alpha** — includes PR #13: push-on-interface, mono chain, SoA+fusion, bench stabilization, `Time.now_ns()`, `Hint.black_box()`, bitwise operators). 333 native integration tests passing.
**Worker scope:** This worker is independent of the v0.1.4, v0.2.0, and networking worktrees (PR #13 merged into main on 2026-04-10 — PR #17 / networking foundation is the remaining parallel open PR). Phase numbering is local (Phase 1..14), not continuous with other milestones.
**Granularity:** Fine (14 phases) — the natural boundaries in SUMMARY.md's 14-phase plan are preserved because each phase delivers a verifiable capability and several have distinct risk profiles that warrant separate planning passes.

## Phases

**Phase Numbering:**
- Integer phases (1..14): Planned milestone work, local to this worker.
- Decimal phases (e.g. 5.1): Reserved for urgent insertions post-planning.

- [x] **Phase 1: Bootstrap & Guardrails** - Pin emsdk to 4.0.23 via `.emsdk-version`, scaffold Linux/macOS web CI with `emcc hello.c` smoke test (no hook, no Windows CI) (completed 2026-04-11)
- [x] **Phase 2: CLI + TOML Scaffold** - `--target=web|native` flag and `[web]` section parsed into `IronWebConfig` (completed 2026-04-11)
- [ ] **Phase 3: Runtime Audit (Web Hardening)** - Intern race fix, `iron_threads_shutdown` no-op, pool cap, `await`-on-main error
- [x] **Phase 4: WASM-Safe Time Shim** - New `iron_time_web.c` providing `now_ns` via `emscripten_get_now()` (completed 2026-04-11)
- [x] **Phase 5: LIR Main-Loop Split Pass (HIGH risk)** - Detect `while(!WindowShouldClose())`, lift captured locals into a frame state struct (completed 2026-04-11)
- [x] **Phase 6: emit_web.c Wrapper (MEDIUM risk)** - Emit web `main()` wiring `emscripten_set_main_loop_arg`, dispatch from build.c step 12 (completed 2026-04-12)
- [x] **Phase 7: build_web.c emcc Orchestration** - Construct argv, find emcc, mkdir_p, forbidden-flag guard, Windows deferred (completed 2026-04-11)
- [x] **Phase 8: Raylib Web Integration (Amalgamation)** - Compile `src/vendor/raylib/raylib.c` with `-DPLATFORM_WEB` through emcc (completed 2026-04-11)
- [x] **Phase 9: Shell Template + Audio Autoplay Unlock (LOW-MEDIUM risk)** - COOP/COEP preflight, audio resume listener, webglcontextlost handler (completed 2026-04-12)
- [x] **Phase 10: Asset Preload + Top-Level Loader Guard (MEDIUM risk)** - `--preload-file` mapping + analyzer error for top-level `LoadTexture` (completed 2026-04-11)
- [ ] **Phase 11: dist/web/ Output Layout** - Predictable `index.{html,js,wasm,data}` drop-to-itch.io output folder
- [ ] **Phase 12: Pong Reference Game + Validation** - End-to-end Pong proof exercising render, input, audio, captures
- [ ] **Phase 13: Integration Tests + Full Regression** - CLI/TOML/loop-split/shell/asset/analyzer tests, size budget, Windows CI, 333 native tests green
- [ ] **Phase 14: Iron-Native Dev Server (DEFERRED, GATED)** - `iron serve --target=web` with COOP/COEP headers — BLOCKED on networking milestone's HTTP server landing on main

## Phase Details

### Phase 1: Bootstrap & Guardrails
**Goal**: Lock the Emscripten toolchain version to `4.0.23` via `.emsdk-version` and scaffold a Linux/macOS CI workflow that installs the pinned toolchain and validates emcc reproducibility with a trivial smoke test. No pre-commit hook; the "new-files-only" discipline against the PR #17 conflict zone is enforced by careful development and the existing 333 integration tests.
**Depends on**: Nothing (first phase)
**Requirements**: WEB-BOOT-01, WEB-BOOT-03 (WEB-BOOT-02 dropped 2026-04-10 — hook not built in this milestone)
**Success Criteria** (what must be TRUE):
  1. A file `.emsdk-version` exists at the repo root containing exactly `4.0.23`.
  2. The CI workflow `.github/workflows/web.yml` runs to completion on both `ubuntu-latest` and `macos-latest` runners: emsdk 4.0.23 is installed from the pinned `.emsdk-version` file, `emcc scripts/ci-smoke/hello.c -o /tmp/hello.js` produces non-empty `hello.js` and `hello.wasm`, and `emcc --version` output contains `4.0.23`.
**Plans**: 1 plan
- [ ] 01-01-PLAN.md — Lock emsdk to 4.0.23 via .emsdk-version, scaffold Linux/macOS web CI workflow (web.yml) with emcc hello.c smoke test, drop WEB-BOOT-02 and retarget WEB-BOOT-03 in REQUIREMENTS.md and ROADMAP.md

### Phase 2: CLI + TOML Scaffold
**Goal**: Users can invoke `iron build --target=web` and configure the web build declaratively via an `iron.toml` `[web]` section, with clear errors on bad input.
**Depends on**: Phase 1
**Requirements**: WEB-CLI-01, WEB-CLI-02, WEB-CLI-03, WEB-CLI-04, WEB-CLI-05, WEB-CLI-06, WEB-CLI-07, WEB-CLI-08, WEB-CLI-09, WEB-MANIFEST-01, WEB-MANIFEST-02, WEB-MANIFEST-03, WEB-MANIFEST-04, WEB-MANIFEST-05, WEB-MANIFEST-06, WEB-MANIFEST-07, WEB-MANIFEST-08
**Success Criteria** (what must be TRUE):
  1. User can run `iron build --target=web main.iron` and have the CLI dispatch to the web build path (stub "not yet implemented" exit is acceptable at end of this phase; real link comes in Phase 7).
  2. User can run `iron build --target=native main.iron` and get the current native behavior; `iron build main.iron` with no `--target` flag behaves identically (default is native).
  3. User who runs `iron build --target=unknown` sees a clear error listing the valid target values and the build exits non-zero.
  4. User whose `iron.toml` contains `[web] assets = ["a.png", "b.png"]`, `title = "Pong"`, `initial_memory = 268435456`, `stack_size = 2097152`, and `pthread_pool_size = 8` gets a populated `IronWebConfig` struct with all five fields set; a subsequent misspelled `[wbe]` section or unknown `[web].foo` key produces a warning but does not fail the build.
  5. Build banner prints `using emcc <version> from <path>` before compilation starts and, when `emcc` is missing from PATH, prints a friendly install one-liner referencing the pinned emsdk version from `.emsdk-version`.
  6. `iron run --target=web` builds then shells out to `emrun` on the produced HTML file; `iron build --target=web --release` uses the optimized flag set (`-Oz -flto -sASSERTIONS=0`) and `iron build --target=web` without `--release` uses the debug set (`-O0 -g -sASSERTIONS=1`).
**Plans**: 6 plans
- [ ] 02-plan-01-type-foundations-PLAN.md — Type foundations: new web_config.h (IronWebConfig + default #defines) + extend build.h (target enum + release bool) + extend toml.h (embed IronWebConfig on IronProject)
- [ ] 02-plan-02-toml-web-parsing-PLAN.md — toml.c: section==3 [web] parser branch (6 fields + assets normalization), Levenshtein typo detection for misspelled sections, unknown-key warning, iron_toml_free extension
- [ ] 02-plan-03-cli-flag-parsing-PLAN.md — main.c: --target=web|native (= and space forms), --release, unknown-target error, print_usage extension
- [ ] 02-plan-04-build-web-stub-PLAN.md — New build_web.h + build_web.c: find_emcc, runtime .emsdk-version read, using-emcc banner, drift warning, install one-liner, config validation, iron_build_web stub (exit 0 with Phase 2 complete message)
- [ ] 02-plan-05-dispatch-and-wiring-PLAN.md — build.c: single-line iron_build_web dispatch at top of iron_build() + native --release -O2 branch in build_src_list; CMakeLists.txt: add src/cli/build_web.c to ironc target
- [ ] 02-plan-06-tests-and-ci-PLAN.md — New tests/unit/test_toml_web.c (Unity suite for [web] parsing); CLI smoke tests + pin-discipline invariants in root CMakeLists.txt; extend .github/workflows/web.yml path filter to cover new Phase 2 source files

### Phase 3: Runtime Audit (Web Hardening)
**Goal**: Iron's existing runtime (intern table, thread pool, analyzer) survives cross-compilation under Emscripten with SharedArrayBuffer without crashes, races, or main-thread deadlocks.
**Depends on**: Phase 2
**Requirements**: WEB-RUNTIME-01, WEB-RUNTIME-02, WEB-RUNTIME-03, WEB-RUNTIME-04, WEB-RUNTIME-07
**Success Criteria** (what must be TRUE):
  1. A stress test that spawns multiple workers each interning different string literals concurrently under `-pthread` does not crash or corrupt the intern table (double-checked lock in `iron_string_intern` is in place in `src/runtime/iron_string.c`).
  2. A web build whose program exits normally does not hang on shutdown: `iron_threads_shutdown()` is a `#ifdef __EMSCRIPTEN__` no-op and the web thread pool is capped at 4 workers regardless of `navigator.hardwareConcurrency`.
  3. User who writes `await` reachable from `Iron_main()` and runs `iron build --target=web` gets a clear analyzer error at build time naming the restriction; same program builds cleanly for `--target=native`.
  4. `src/runtime/iron_rc.c`, `src/runtime/iron_builtins.c`, and `src/runtime/iron_collections.c` compile under emcc with zero modifications to those files and zero warnings.
  5. The 333 existing native integration tests still pass unchanged; neither `src/runtime/iron_string.c` nor `src/runtime/iron_threads.c` appears in the PR #17 forbidden-paths list (pre-commit hook stays silent).
**Plans**: 4 plans
- [ ] 03-plan-01-intern-table-hardening-PLAN.md — tsan stress test + thread-safety contract comment for iron_string_intern (WEB-RUNTIME-01)
- [ ] 03-plan-02-thread-pool-web-hardening-PLAN.md — IRON_WEB_MAX_WORKERS cap + iron_threads_shutdown no-op under __EMSCRIPTEN__ (WEB-RUNTIME-02, WEB-RUNTIME-03)
- [ ] 03-plan-03-analyzer-target-and-web-await-check-PLAN.md — IronBuildTarget plumbed through iron_analyze + new web_await_check reachability pass + unit test + web.yml paths filter (WEB-RUNTIME-04)
- [ ] 03-plan-04-portability-probe-ci-PLAN.md — emcc try-compile probe in web.yml + local ctest grep invariant against sysconf/pthread_attr/sigaction (WEB-RUNTIME-07)

### Phase 4: WASM-Safe Time Shim
**Goal**: `time.now_ns()` resolves to a monotonic millisecond-precision clock under `--target=web` without any edit to the PR #17 conflict zone (`iron_time.c` itself is safe — PR #17 does not touch it).
**Depends on**: Phase 3
**Requirements**: WEB-RUNTIME-05, WEB-RUNTIME-06
**Success Criteria** (what must be TRUE):
  1. A new file `src/stdlib/iron_time_web.c` exists providing `Iron_time_now()`, `Iron_time_now_ms()`, and `Iron_time_now_ns()` via `emscripten_get_now()`.
  2. A minimal Iron program that calls `time.now_ns()` builds with `--target=web` and links with no undefined symbols; `iron_time.c` is never touched and the pre-commit hook stays silent.
  3. The Pong validation game (Phase 12) observes strictly monotonic `now_ns()` values frame-over-frame for at least 60 consecutive frames.
**Plans**: 2 plans
- [ ] 04-plan-01-iron-time-web-shim-PLAN.md — Create src/stdlib/iron_time_web.c mirroring iron_time.c body under #ifdef __EMSCRIPTEN__ guard with emscripten_get_now / emscripten_date_now / spin-loop sleep (WEB-RUNTIME-05)
- [ ] 04-plan-02-ci-probe-and-symbol-gate-PLAN.md — Extend web.yml portability probe with iron_time_web.c emcc try-compile + add test_iron_time_web_symbols and test_emsdk_pin_discipline_iron_time_web ctest gates (WEB-RUNTIME-06)

### Phase 5: LIR Main-Loop Split Pass (HIGH risk)
**Goal**: A LIR pass detects the canonical `while(!WindowShouldClose()) { body }` shape in Iron programs and rewrites it into a frame-callback form where captured locals are heap-promoted into a state struct, so an emit-time wrapper can register the callback with `emscripten_set_main_loop_arg`.
**Depends on**: Phase 4
**Requirements**: WEB-EMIT-01, WEB-EMIT-02, WEB-EMIT-03, WEB-EMIT-04
**Risk flag**: HIGH — requires reusing Iron's v1.0.0-alpha closure-capture machinery; needs a `/gsd:research-phase` spike during planning to verify `emit_helpers.h` surface and closure-capture API shape before implementation begins.
**Success Criteria** (what must be TRUE):
  1. A canonical Pong-style Iron source (stack locals for ball position, ball velocity, paddle positions, and score, all referenced inside a single top-level `while(!WindowShouldClose())` loop in a function that also calls `InitWindow`) compiles through the pass without errors, and writes to those locals inside the loop body persist correctly frame-over-frame (capture-by-reference semantics).
  2. An Iron program with two sequential top-level `while(!WindowShouldClose())` loops in the same function produces a clear compile error naming the canonical shape and suggesting a state machine.
  3. An Iron program using `for !WindowShouldClose()`, a `do-while`, or a nested `while` produces a precise error identifying the unsupported shape and pointing at the canonical alternative.
  4. The same Iron source, when built with `--target=native`, bypasses the split pass entirely and runs unchanged on native (pass is web-target-gated).
**Plans**: 3 plans
- [ ] 05-plan-01-lir-struct-and-diagnostic-codes-PLAN.md — Extend IronLIR_Func with web_frame_captures metadata fields + define four IRON_ERR_WEB_* error codes in the 700 range (Wave 1, depends on nothing)
- [ ] 05-plan-02-web-main-loop-split-pass-PLAN.md — New src/lir/web_main_loop_split.{h,c} with canonical while(!WindowShouldClose()) detector, capture-set computation, four error paths, and CMake registration (Wave 2, depends on plan 01)
- [ ] 05-plan-03-pipeline-wiring-and-tests-PLAN.md — Wire iron_lir_web_main_loop_split into build.c between optimize and emit + Unity test suite with 6 hand-built LIR fixtures + web.yml paths filter extension (Wave 3, depends on plans 01 and 02)

### Phase 6: emit_web.c Wrapper (MEDIUM risk)
**Goal**: The LIR-level rewrites from Phase 5 are consumed by a brand-new emitter file `src/lir/emit_web.c` that produces web-specific C (including `<emscripten/emscripten.h>`, runtime init, frame state alloc, `emscripten_set_main_loop_arg(fn, state, 0, 0)`, and cleanup inside the frame-callback shutdown branch), reusing three `emit_c.c` helpers (`emit_func_signature`, `emit_func_body`, `emit_instr`) via `emit_helpers.h` after a zero-behavior visibility promotion. PR #17 merged 2026-04-10 retired the "zero touches to emit_c.c" conflict-zone rule; the surviving invariant is behavior-identity of non-web emission.
**Depends on**: Phase 5
**Requirements**: WEB-EMIT-05, WEB-EMIT-06, WEB-EMIT-07, WEB-EMIT-08, WEB-EMIT-09
**Risk flag**: MEDIUM — depends on `emit_helpers.h` exposing enough surface; 06-RESEARCH.md confirmed that promoting `emit_func_signature`, `emit_func_body`, and `emit_instr` from `static` to extern is sufficient (≤10 lines of new declarations, zero behavioral change).
**Success Criteria** (what must be TRUE):
  1. A no-op Iron program built with `--target=web` produces emitted C that contains `#include <emscripten/emscripten.h>` at the top and a `main()` wrapper that allocates a state struct, calls `emscripten_set_main_loop_arg(frame_cb, state, 0, 0)` with `simulate_infinite_loop = 0`, and returns 0.
  2. A web build whose `WindowShouldClose()` becomes true runs `CloseWindow()`, `iron_runtime_shutdown()`, `free(state)`, and `emscripten_cancel_main_loop()` in that order from inside the frame callback's shutdown branch — verified by a memory snapshot showing no leak growth across page reloads.
  3. `src/cli/build.c` step 12 dispatches to `emit_web_module(...)` when `target == WEB` and to `iron_lir_emit_c(...)` otherwise. `src/lir/emit_web.c` exists as a distinct emitter file (no inlining of web logic into `emit_c.c`) and non-web emission behavior is unchanged: the full native `ctest` suite remains 100% green with no regressions attributable to Phase 6. The original PR #17 conflict-zone rule that forbade any edit to `emit_c.c` is retired — PR #17 merged on 2026-04-10, so the rationale for that invariant no longer applies; Phase 6 plan 01 makes a zero-behavior three-function visibility change (`static` -> extern for `emit_func_signature`, `emit_func_body`, `emit_instr`) to enable clean reuse from `emit_web.c`.
  4. `src/lir/emit_web.c` consumes `emit_c.c` helpers ONLY through `src/lir/emit_helpers.h` (the shared helper header). It does NOT `#include "lir/emit_c.h"` and does NOT reference any `emit_c.c` symbol whose declaration does not appear in `emit_helpers.h`. The clean-boundary guarantee is enforced at the header level, not at the line-diff level.
**Plans**: 3 plans
- [ ] 06-plan-01-emit-c-visibility-and-roadmap-update-PLAN.md — Promote emit_func_signature / emit_func_body / emit_instr from static to extern in src/lir/emit_c.c, declare them in src/lir/emit_helpers.h, and rewrite ROADMAP.md Phase 6 SC3/SC4 to retire the zero-touch invariant (Wave 1, no dependencies)
- [ ] 06-plan-02-emit-web-module-implementation-PLAN.md — Create src/lir/emit_web.h + src/lir/emit_web.c implementing emit_web_module(): emscripten include, FrameState struct synthesis from fn->web_frame_captures, frame callback with CloseWindow/iron_runtime_shutdown/free/emscripten_cancel_main_loop shutdown branch, main() wrapper calling emscripten_set_main_loop_arg(cb, state, 0, 0); register new .c file in CMakeLists.txt (Wave 2, depends on plan 01)
- [ ] 06-plan-03-dispatch-build-web-restructure-and-tests-PLAN.md — Wire src/cli/build.c step 12 dispatch (target==WEB -> emit_web_module, else iron_lir_emit_c), remove the Phase 2 stub short-circuit at iron_build() entry while preserving the find_emcc/banner preflight via iron_build_web, add tests/unit/test_emit_web.c with snapshot assertions on the emitted C, extend .github/workflows/web.yml paths filter for the Phase 6 files (Wave 3, depends on plans 01 and 02)

### Phase 7: build_web.c emcc Orchestration
**Goal**: A single `emcc` link command is constructed and executed with the full canonical flag set (pthread, memory, GLFW, filesystem, exports, asyncify off) against the emitted C + Iron runtime + vendored raylib amalgamation + stdlib shims, producing output in `dist/web/` across Linux, macOS, and Windows.
**Depends on**: Phase 6
**Requirements**: WEB-BUILD-01, WEB-BUILD-02, WEB-BUILD-03, WEB-BUILD-04, WEB-BUILD-06, WEB-BUILD-07, WEB-BUILD-08
**Success Criteria** (what must be TRUE):
  1. User runs `iron build --target=web tests/integration/web/hello.iron` on a hello-world program and sees `dist/web/index.html`, `dist/web/index.js`, and `dist/web/index.wasm` on disk; the same invocation works identically on Linux and macOS. Windows support is deferred with the rest of Iron's Windows story — `find_emcc()` does not probe `emcc.bat`/`emcc.cmd`, and `build_web.c` keeps the `#ifdef _WIN32 #error` guard. This mirrors Phase 1's Linux/macOS-only CI decision.
  2. Every translation unit in the link (emitted C, Iron runtime, stdlib shims including `iron_time_web.c` instead of `iron_time.c`, raylib amalgamation) is compiled with `-pthread`; no `wasm-ld: --shared-memory is disallowed` error occurs.
  3. User whose shell has no `emcc` on PATH runs `iron build --target=web` and sees a friendly error printing the install one-liner and the pinned emsdk version (`4.0.23`).
  4. User whose `iron.toml` `[web]` config attempts to set any forbidden flag (`ASYNCIFY=1`, `MINIMAL_RUNTIME`, `PROXY_TO_PTHREAD`, `SAFE_HEAP`, `ALLOW_BLOCKING_ON_MAIN_THREAD`, `ERROR_ON_UNDEFINED_SYMBOLS=0`, `MODULARIZE`, `EXPORT_ES6`, `-fwasm-exceptions`) gets a clear diagnostic naming the flag and refusing to build.
  5. Building from a fresh clone with no prior `dist/` directory succeeds without manual `mkdir`; `mkdir_p("dist/web")` handles the cross-platform path creation.
  6. `tests/integration/web/test_cli_parse.c` passes (CLI parse coverage from Phase 2 is verified by the integration test harness spawned from this phase).
**Plans**: 4 plans
- [ ] 07-plan-01-iron-build-web-link-orchestration-PLAN.md — iron_build_web_link function + mkdir_p + argv with canonical flag set + release/debug flag sets + posix_spawnp emcc (WEB-BUILD-01, 03, 04, 06)
- [ ] 07-plan-02-forbidden-flag-guard-PLAN.md — IRON_WEB_FORBIDDEN_FLAGS array + is_forbidden_flag helper + self-audit walk in iron_build_web_link (WEB-BUILD-07, 08)
- [ ] 07-plan-03-dispatch-and-emrun-PLAN.md — build.c step 13 target branch to iron_build_web_link + iron run --target=web → emrun posix_spawnp flow + fallback message
- [ ] 07-plan-04-tests-ci-fixture-roadmap-PLAN.md — hello.iron fixture + test_cli_parse.c integration test + web.yml end-to-end smoke job + ROADMAP SC1 Windows-parity update + Phase 7 pin discipline (WEB-BUILD-02)

### Phase 8: Raylib Web Integration (Amalgamation)
**Goal**: The vendored raylib 5.5 compiles through emcc via its amalgamation driver with `-DPLATFORM_WEB -DGRAPHICS_API_OPENGL_ES2 -sUSE_GLFW=3` and links cleanly with the Iron-emitted C, so a minimal raylib program opens a canvas in a desktop browser.
**Depends on**: Phase 7
**Requirements**: WEB-BUILD-05
**Success Criteria** (what must be TRUE):
  1. A minimal Iron program that calls `InitWindow`, `BeginDrawing`, `ClearBackground(RAYWHITE)`, `EndDrawing`, and `CloseWindow` builds with `iron build --target=web` and produces `dist/web/index.html` with no undefined-symbol errors from the linker (no missing `glfwGetError`, `glfwGetGamepadState`, or GLES2 entry points).
  2. Running `emrun --no_browser --port 8080 dist/web/index.html` and opening the URL in Chrome, Firefox, or Safari shows a blank canvas with the expected clear color — the raylib runtime has reached `EndDrawing` without crashing.
  3. The build invokes emcc against `src/vendor/raylib/raylib.c` (the amalgamation driver that `#include`s `platforms/rcore_web.c` under `PLATFORM_WEB`) — not raylib's Makefile, not a separate `.a` artifact.
**Plans**: 2 plans
- [x] 08-plan-01-build-web-raylib-amalgamation-link-PLAN.md — Extend iron_build_web_link in src/cli/build_web.c with a gated block appending src/vendor/raylib/raylib.c + -DPLATFORM_WEB -DGRAPHICS_API_OPENGL_ES2 -Isrc/vendor/raylib when opts.use_raylib is true (WEB-BUILD-05)
- [x] 08-plan-02-raylib-fixture-and-ci-smoke-PLAN.md — Add tests/integration/web/hello_raylib.iron + .github/workflows/web.yml Phase 8 end-to-end smoke step + paths filter additions + ROADMAP/REQUIREMENTS marking complete

### Phase 9: Shell Template + Audio Autoplay Unlock (LOW-MEDIUM risk)
**Goal**: The default HTML shell protects users from the #1 deployment pitfall (COOP/COEP misconfiguration silently disabling SharedArrayBuffer) and from the #2 game-audio pitfall (suspended `AudioContext` on first frame), while remaining fully overrideable via `[web].shell`.
**Depends on**: Phase 8
**Requirements**: WEB-SHELL-01, WEB-SHELL-02, WEB-SHELL-03, WEB-SHELL-04, WEB-SHELL-05, WEB-SHELL-06, WEB-SHELL-07, WEB-AUDIO-01, WEB-AUDIO-02, WEB-AUDIO-03, WEB-AUDIO-04
**Risk flag**: LOW-MEDIUM — needs a `/gsd:research-phase` spike during planning to decide whether miniaudio auto-tracks `AudioContext`s via Emscripten, or whether the Proxy tracking pattern from `shell.html:306-335` must be ported into the default.
**Success Criteria** (what must be TRUE):
  1. A file `src/cli/web_shell_template.h` exists embedding the default shell as a C string literal derived from `src/vendor/raylib/minshell.html` with COOP/COEP preflight and audio-autoplay patches, a `<canvas id="canvas">` element, the `{{{ SCRIPT }}}` substitution slot preserved, and a `webglcontextlost` handler that shows a reload prompt.
  2. A Pong build served without the COOP/COEP headers displays a visible red error banner explaining the requirement and naming the exact header pair, instead of hanging or silently failing on `pthread_create`.
  3. A user loads Pong in Chrome, Firefox, or Safari and on the very first paddle hit (first user gesture is a keydown or pointerdown) hears the paddle sound effect; subsequent hits continue to play audio normally. The one-shot listener removes itself after first activation.
  4. A user who points `[web].shell` at a custom HTML file missing `{{{ SCRIPT }}}` gets a clear build error at validation time naming the missing token.
  5. The build does NOT compile miniaudio with `MA_ENABLE_AUDIO_WORKLETS`.
**Plans**: 2 plans
- [x] 09-plan-01-web-shell-template-header-PLAN.md — New src/cli/web_shell_template.h header embedding the default shell HTML as a C string literal derived from minshell.html with COOP/COEP preflight + audio unlock + webglcontextlost + canvas + {{{ SCRIPT }}} patches (WEB-SHELL-01/02/03/04/05/07)
- [x] 09-plan-02-build-web-shell-file-wiring-and-ci-smoke-PLAN.md — Wire iron_build_web_link to materialize the default shell via mkstemp + validate custom [web].shell contains {{{ SCRIPT }}} + emit --shell-file argv + extend web.yml CI grep assertions + mark Phase 9 complete (WEB-SHELL-06)

### Phase 10: Asset Preload + Top-Level Loader Guard (MEDIUM risk)
**Goal**: Iron games can declare their `assets/` directory once in `iron.toml` and use the same `LoadTexture("assets/foo.png")` code on native and web, while a compile-time analyzer error prevents the race between `--preload-file` MEMFS mounting and static initializers.
**Depends on**: Phase 9
**Requirements**: WEB-ASSET-01, WEB-ASSET-02, WEB-ASSET-03, WEB-ASSET-04, WEB-ASSET-05
**Risk flag**: MEDIUM — depends on Iron analyzer having infrastructure to emit "forbidden call at top-level" diagnostics; needs a `/gsd:research-phase` spike during planning to verify the analyzer surface before implementation (may require a small analyzer extension).
**Success Criteria** (what must be TRUE):
  1. An Iron project with `[web] assets = "assets/"` in its `iron.toml` and `LoadTexture("assets/ball.png")` in its source builds with `--target=web` and at runtime successfully loads the texture from MEMFS; the same code with the same path runs unchanged on `--target=native`.
  2. An Iron program calling `LoadTexture`, `LoadSound`, `LoadFont`, or `LoadModel` at module-level (outside any function body) produces a clear analyzer error naming the offending call and the required fix (move inside a function) when built with `--target=web`; the same program builds cleanly for `--target=native`.
  3. Asset directory paths in `iron.toml` resolve relative to the `iron.toml` file's directory, not the shell's current working directory; a user invoking `iron build` from a subdirectory still gets correct asset mounting.
  4. A project with no `assets` directory produces a warning (not an error) and still builds successfully (asset-free games are supported).
**Plans**: 3 plans
- [x] 10-plan-01-toml-dir-field-PLAN.md — Add toml_dir field to IronProject populated by iron_toml_parse (WEB-ASSET-04 groundwork)
- [x] 10-plan-02-asset-preload-argv-PLAN.md — Wire cfg->assets into iron_build_web_link via --preload-file with stat check + missing-dir warning + hello_raylib_assets CI fixture (WEB-ASSET-01/02/04/05)
- [x] 10-plan-03-top-level-loader-check-PLAN.md — New analyzer pass banning top-level LoadTexture/LoadSound/LoadFont/LoadModel on web + unit test + pipeline registration (WEB-ASSET-03)

### Phase 11: dist/web/ Output Layout
**Goal**: Every web build lands in a predictable `dist/web/` folder with stable filenames (`index.html`, `index.js`, `index.wasm`, optional `index.data`, optional `index.wasm.map`) so users can drag-and-drop the folder to itch.io, Netlify, or Cloudflare Pages.
**Depends on**: Phase 10
**Requirements**: WEB-OUT-01, WEB-OUT-02, WEB-OUT-03, WEB-OUT-04, WEB-OUT-05
**Success Criteria** (what must be TRUE):
  1. User runs `iron build --target=web` on any Iron program (regardless of source filename) and the primary HTML is always named `dist/web/index.html` with sibling `index.js` and `index.wasm`; when assets are declared, `index.data` also appears.
  2. User runs `iron build --target=web` (debug) and additionally sees `dist/web/index.wasm.map` for DevTools sourcemap support; release builds omit the map.
  3. User zips `dist/web/` and uploads it to itch.io as an HTML5 game; itch.io finds `index.html` at the archive root and serves a playable embed.
  4. Building from a fresh clone with no prior `dist/` directory succeeds without manual `mkdir`.
**Plans**: TBD

### Phase 12: Pong Reference Game + Validation
**Goal**: A real Iron + raylib Pong game (with ball, two paddles, score, win condition, state machine, and paddle-hit audio) compiles and plays in a desktop browser with performance indistinguishable from the native build of the same source — proving the end-to-end pipeline.
**Depends on**: Phase 11
**Requirements**: WEB-VALIDATE-01, WEB-VALIDATE-02, WEB-VALIDATE-03, WEB-VALIDATE-04, WEB-VALIDATE-05, WEB-VALIDATE-06, WEB-VALIDATE-07, WEB-VALIDATE-08
**Success Criteria** (what must be TRUE):
  1. A file `examples/pong/pong.iron` exists implementing a two-paddle Pong game with ball, per-player score, win condition, and a `title → play → game over → restart` state machine driven from a single top-level `while(!WindowShouldClose())` loop with at least four captured locals (ball position, ball velocity, paddle positions, score).
  2. User runs `iron run --target=web examples/pong` and sees Pong open in Chrome via `emrun`: paddles render at both screen edges, arrow keys (or W/S and Up/Down) move the paddles, the ball reflects off paddles and walls, the first paddle hit after page load plays the `paddle.wav` sound effect, and the score increments correctly.
  3. User runs `iron run --target=native examples/pong` on the exact same source (no code changes) and sees the game play identically on native — same LIR main-loop split source, different target.
  4. `dist/web/index.wasm` from `iron build --target=web --release examples/pong` is under 2 MB gzipped.
  5. Reloading the Pong page in the browser 10 times does not leak heap allocations: a browser memory snapshot before and after shows no net growth in the WASM heap.
**Plans**: TBD

### Phase 13: Integration Tests + Full Regression
**Goal**: The full web path is guarded in CI against regressions — every public behavior from the earlier phases has a test, and the 333 existing native integration tests continue to pass.
**Depends on**: Phase 12
**Requirements**: WEB-TEST-01, WEB-TEST-02, WEB-TEST-03, WEB-TEST-04, WEB-TEST-05, WEB-TEST-06, WEB-TEST-07, WEB-TEST-08, WEB-TEST-09, WEB-TEST-10, WEB-TEST-11
**Success Criteria** (what must be TRUE):
  1. A new test suite under `tests/integration/web/` contains at minimum: `hello.iron` (minimal print-and-exit program builds to web), `test_cli_parse.c` (all four `--target` cases parse correctly), `test_toml_parse.c` (every `[web]` field parses into `IronWebConfig`), `test_loop_split.c` (canonical shape accepted plus 4 non-canonical shapes each producing the expected precise error), `test_asset_path.c` (round-trip from `iron.toml` directory → emcc flag → MEMFS mount → successful `fopen`), `test_shell_subst.c` (substitution fills `{{{ SCRIPT }}}`, `{{IRON_TITLE}}`, canvas tokens), and `test_analyzer_loaders.c` (top-level `LoadTexture` errors on web, succeeds on native).
  2. A headless browser smoke test runs against a Pong test build and asserts `self.crossOriginIsolated === true` and `typeof SharedArrayBuffer === 'function'` before any Iron code executes.
  3. A size-budget CI script runs on the release Pong build and fails the job if `dist/web/index.wasm` exceeds 2 MB gzipped.
  4. Windows CI runs the full hello-world web build end-to-end on a fresh runner (not just the smoke test from Phase 1) and Linux/macOS CI runs the complete web integration suite.
  5. All 333 existing native integration tests continue to pass unchanged; no test in the native suite references any of the new web files.
**Plans**: TBD

### Phase 14: Iron-Native Dev Server (DEFERRED, GATED)
**Goal**: Replace the interim `emrun` dev loop with an Iron-native `iron serve --target=web` that sends the COOP/COEP headers Iron's web builds require, using the HTTP server from the parallel networking milestone.
**Depends on**: Phase 13 AND the networking milestone's HTTP server landing on `main`
**Requirements**: WEB-SERVER-01, WEB-SERVER-02, WEB-SERVER-03
**Status**: BLOCKED — waiting on HTTP server landing on main from the parallel networking milestone. Not built in this worker. Tracked so the dependency is visible at merge time.
**Success Criteria** (what must be TRUE):
  1. User runs `iron serve --target=web` from any Iron project with a `[web]` section and the command starts an Iron-native HTTP server (not `emrun`) serving the `dist/web/` directory.
  2. Every response from the dev server includes `Cross-Origin-Opener-Policy: same-origin` and `Cross-Origin-Embedder-Policy: require-corp` headers so that `self.crossOriginIsolated` is true and `SharedArrayBuffer` is available to Iron's pthread runtime.
  3. Documentation (README, docs site) replaces all `emrun` references in web-target sections with `iron serve` once this phase lands.
**Plans**: TBD

## Progress

**Execution Order:**
Phases execute in numeric order: 1 → 2 → 3 → 4 → 5 → 6 → 7 → 8 → 9 → 10 → 11 → 12 → 13 → 14.

Phase 14 is blocked on the parallel networking milestone and does NOT gate any of Phases 1–13. Phases 1–13 ship independently.

| Phase | Plans Complete | Status | Completed |
|-------|----------------|--------|-----------|
| 1. Bootstrap & Guardrails | 1/1 | Complete    | 2026-04-11 |
| 2. CLI + TOML Scaffold | 6/6 | Complete   | 2026-04-11 |
| 3. Runtime Audit | 3/4 | In Progress|  |
| 4. WASM-Safe Time Shim | 2/2 | Complete   | 2026-04-11 |
| 5. LIR Main-Loop Split Pass | 3/3 | Complete   | 2026-04-11 |
| 6. emit_web.c Wrapper | 3/3 | Complete   | 2026-04-12 |
| 7. build_web.c emcc Orchestration | 3/4 | In Progress|  |
| 8. Raylib Web Integration | 2/2 | Complete   | 2026-04-12 |
| 9. Shell + Audio Autoplay | 2/2 | Complete   | 2026-04-12 |
| 10. Asset Preload + Guard | 3/3 | Complete   | 2026-04-11 |
| 11. dist/web/ Output Layout | 0/TBD | Not started | - |
| 12. Pong Reference Game | 0/TBD | Not started | - |
| 13. Integration Tests + Regression | 0/TBD | Not started | - |
| 14. Iron-Native Dev Server | 0/TBD | Deferred (gated) | - |

## Risk Summary

| Phase | Risk | Mitigation |
|-------|------|------------|
| 5 | HIGH — LIR main-loop split must reuse v1.0.0-alpha closure-capture machinery to heap-promote captured locals; naive textual rewrite demonstrably fails | Dedicated `/gsd:research-phase` spike during Phase 5 planning: read closure-lowering sources, verify HIR/LIR node shapes at pass entry, prototype on canonical Pong shape before implementing error shapes |
| 6 | MEDIUM — `emit_web.c` depends on `emit_helpers.h` exposing enough surface without touching `emit_c.c` | `/gsd:research-phase` spike during Phase 6 planning: audit `emit_helpers.h` exports; if insufficient, promote internals via header-only edit (still zero `emit_c.c` .c touches) |
| 9 | LOW-MEDIUM — miniaudio may not auto-track `AudioContext`s; may need to port Proxy tracking from `shell.html:306-335` | `/gsd:research-phase` spike during Phase 9 planning: inspect miniaudio Emscripten backend, decide between Proxy port vs miniaudio patch (prefer Proxy port as less invasive) |
| 10 | MEDIUM — analyzer may lack "forbidden call at top-level" diagnostic infrastructure | `/gsd:research-phase` spike during Phase 10 planning: verify analyzer emission API; if missing, small analyzer extension is a prerequisite to the main pass |

## Dependency Order Rationale

- **Phase 1 first** — pre-commit hook and emsdk pin must exist before any edit, or the first edit can land in PR #17 zone and trigger a painful rebase.
- **Phase 2 (CLI + TOML) before runtime work** — dispatching to the web path is a prerequisite for every downstream phase to know which code path it's modifying.
- **Phase 3 (runtime hardening) before Phase 5 (LIR pass)** — the LIR pass produces C that links the runtime; runtime must not deadlock or race under SharedArrayBuffer before downstream code exercises it.
- **Phase 4 (`iron_time_web.c`) before Phase 7 (`build_web.c`)** — `build_web.c` must know which stdlib shim to link; swapping the time shim at link time requires the new file to exist first.
- **Phase 5 (LIR loop split) before Phase 6 (emit_web)** — `emit_web.c` consumes the rewritten LIR; without the split pass the emitter has nothing to wrap.
- **Phase 6 before Phase 7** — `build_web.c` must dispatch to the correct emitter; the dispatch branch in `build.c` step 12 references the `emit_web_module` symbol from Phase 6.
- **Phase 7 before Phase 8** — Phase 8 adds raylib-specific compile flags to the argv that Phase 7's `build_web.c` constructs.
- **Phase 8 before Phase 9** — the shell template is only meaningful once the link actually produces a runnable `index.html` with a loaded canvas.
- **Phase 9 before Phase 10** — the shell's COOP/COEP preflight and audio resume must be in place before Phase 12's Pong audio validation can pass.
- **Phase 10 before Phase 11** — `--preload-file` mapping drives where `.data` lands relative to `dist/web/`.
- **Phase 11 before Phase 12** — Pong validation needs a predictable output folder layout to assert against.
- **Phase 12 before Phase 13** — integration tests cover behaviors exercised by Pong and reference the validated pipeline.
- **Phase 14 LAST, gated, deferred** — blocked on the parallel networking milestone; not built in this worker.

---
*Roadmap created: 2026-04-10 after research synthesis (SUMMARY.md canonical 14-phase plan)*
*Worker scope: WASM target on `main` @ c517aef (v1.1.0-alpha baseline, PR #13 merged 2026-04-10). Independent of v0.1.4 / v0.2.0 / PR #17 (draft).*
