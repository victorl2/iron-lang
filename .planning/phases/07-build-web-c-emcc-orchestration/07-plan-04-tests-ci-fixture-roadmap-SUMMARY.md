---
phase: 07-build-web-c-emcc-orchestration
plan: 04
subsystem: testing
tags: [emscripten, web, ci, integration-test, cmake, ctest, workflow]

# Dependency graph
requires:
  - phase: 07-plan-01-iron-build-web-link-orchestration
    provides: iron_build_web_link function with emcc argv construction
  - phase: 07-plan-02-forbidden-flag-guard
    provides: forbidden flag self-audit in iron_build_web_link
  - phase: 07-plan-03-dispatch-and-emrun
    provides: build.c step 13 dispatch to iron_build_web_link
provides:
  - tests/integration/web/hello.iron (raylib-free minimal Iron fixture for web smoke)
  - tests/integration/web/test_cli_parse.c (integration test for Phase 2 CLI parse path)
  - test_cli_parse_web ctest target with IRONC_BINARY env wiring
  - test_emsdk_pin_discipline_phase7 ctest invariant
  - web.yml end-to-end smoke job (Configure + build ironc → iron build --target=web → artifact assertions)
  - ROADMAP Phase 7 SC1 rewritten (Windows deferred, fixture path updated)
  - Phase 7 marked 4/4 complete
affects:
  - phase-08-raylib-web-integration (Phase 8 builds on Phase 7's CI smoke foundation)
  - phase-13-integration-tests (this plan establishes the test_cli_parse.c pattern)

# Tech tracking
tech-stack:
  added: []
  patterns:
    - Integration test C file with IRONC_BINARY env var for ctest portability
    - if(NOT WIN32) CMake guard for Linux/macOS-only tests compiled as executables
    - Phase pin-discipline invariant pattern (grep inverted to catch hardcoded version strings)
    - web.yml two-step CI pattern: cmake build ironc first, then invoke ironc for smoke

key-files:
  created:
    - tests/integration/web/hello.iron
    - tests/integration/web/test_cli_parse.c
    - .planning/phases/07-build-web-c-emcc-orchestration/07-plan-04-tests-ci-fixture-roadmap-SUMMARY.md
  modified:
    - CMakeLists.txt
    - .github/workflows/web.yml
    - .planning/ROADMAP.md

key-decisions:
  - "Phase 7 end-to-end CI smoke uses Debug build type (faster than Release; size budget deferred to Phase 13)"
  - "Windows support for build_web.c deferred — find_emcc() does not probe emcc.bat/emcc.cmd; mirrors Phase 1 Linux/macOS-only CI decision"
  - "hello.iron is raylib-free (println only) so Phase 7 CI smoke succeeds without Phase 8's raylib amalgamation"
  - "test_cli_parse.c uses raw exit codes (not Unity) to match integration-test style vs unit-test style"

patterns-established:
  - "Phase 7 integration test pattern: C executable + IRONC_BINARY env var + LABELS integration;phase7"
  - "Pin-discipline invariant pattern: inverted grep over new fixture/test files per phase"

requirements-completed:
  - WEB-BUILD-02

# Metrics
duration: 10min
completed: 2026-04-11
---

# Phase 7 Plan 04: Tests, CI Fixture & Roadmap Summary

**Raylib-free hello.iron fixture + test_cli_parse.c integration harness + web.yml end-to-end emcc link smoke, closing Phase 7 (4/4 plans)**

## Performance

- **Duration:** ~10 min
- **Started:** 2026-04-11T00:00:00Z
- **Completed:** 2026-04-11T00:10:00Z
- **Tasks:** 2
- **Files modified:** 5

## Accomplishments

- Created `tests/integration/web/hello.iron` — 3-line raylib-free Iron fixture (`println("hello from iron web")`) that Phase 7 CI invokes via `iron build --target=web`
- Created `tests/integration/web/test_cli_parse.c` — 4-test integration harness validating `--target=web`, `--target=native`, unknown target error, and no-target default; uses `IRONC_BINARY` env var for ctest portability; registered as `test_cli_parse_web` ctest (passes green locally)
- Extended `.github/workflows/web.yml` with two new steps: "Configure + build ironc" then "End-to-end smoke — iron build --target=web tests/integration/web/hello.iron" asserting `dist/web/index.{html,js,wasm}` all exist and are non-empty
- Updated ROADMAP.md Phase 7 SC1 to remove Windows parity language (deferred with rest of Iron's Windows story); fixture path updated from `examples/hello/hello.iron` to `tests/integration/web/hello.iron`; Phase 7 marked `[x]` complete (4/4 plans) in both Phases list and progress table

## Task Commits

1. **Task 1: hello.iron fixture + test_cli_parse.c + CMakeLists.txt** - `49b0e76` (feat)
2. **Task 2: web.yml smoke job + ROADMAP updates** - `6ae4990` (feat)

**Plan metadata:** `(pending docs commit)`

## Files Created/Modified

- `tests/integration/web/hello.iron` — Raylib-free 3-line Iron program; sole input to Phase 7 CI smoke
- `tests/integration/web/test_cli_parse.c` — 4-subtest integration harness for Phase 2 CLI parse path; #ifdef _WIN32 #error guard; IRONC_BINARY env wiring
- `CMakeLists.txt` — Added test_cli_parse_web executable + ctest registration + test_emsdk_pin_discipline_phase7 invariant, both guarded by if(NOT WIN32)
- `.github/workflows/web.yml` — Added tests/integration/web/ entries to both paths filters; added cmake + ironc build step; added end-to-end smoke step with artifact assertions
- `.planning/ROADMAP.md` — Phase 7 SC1 rewritten (Windows deferred), Phase 7 [x] complete 4/4, progress row updated

## Decisions Made

1. **Debug build in CI smoke** — `cmake -DCMAKE_BUILD_TYPE=Debug` for faster CI compile; Release size-budget checks deferred to Phase 13. This is consistent with other phases' CI steps.
2. **raylib-free fixture** — `hello.iron` uses only `println` (no raylib calls), so Phase 7 CI smoke links only Iron runtime + stdlib shims without Phase 8's raylib amalgamation. Phase 8 will add a raylib fixture.
3. **No Unity for integration test** — `test_cli_parse.c` uses raw exit codes (not Unity) to match the `tests/integration/` style; Unity is reserved for unit tests linked against iron_compiler.
4. **Windows deferred in SC1** — The old "works identically on Linux, macOS, and Windows" claim was incorrect; `build_web.c` already has `#ifdef _WIN32 #error`. SC1 now mirrors Phase 1's Linux/macOS-only wording.

## Deviations from Plan

None — plan executed exactly as written. Both tasks matched the PLAN.md specification, including the exact test function names, CMakeLists.txt block structure, web.yml step names, and ROADMAP.md replacement text.

## Issues Encountered

- `.planning/` directory is in `.gitignore` — staged ROADMAP.md with `git add -f .planning/ROADMAP.md` (same workaround used by all prior Phase 7 plans).
- `cmake --build build --target test_cli_parse_web` required a fresh `cmake -S . -B build` reconfigure before the new target was recognized (normal CMake behavior when CMakeLists.txt changes).

## User Setup Required

None — no external service configuration required.

## Next Phase Readiness

Phase 8 (Raylib Web Integration) can now begin:
- `iron build --target=web` is wired end-to-end and proven by CI
- `tests/integration/web/hello.iron` fixture is in place; Phase 8 will add a raylib fixture alongside it
- `build_web.c` argv layout has 48 slots with margin for Phase 8 raylib additions (noted in Section I comment)
- No blockers from Phase 7

---
*Phase: 07-build-web-c-emcc-orchestration*
*Completed: 2026-04-11*
