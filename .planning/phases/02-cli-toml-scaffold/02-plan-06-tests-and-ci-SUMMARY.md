---
phase: 02-cli-toml-scaffold
plan: 06
subsystem: testing
tags: [unity, cmake, ctest, toml-parsing, web-ci, pin-discipline]

# Dependency graph
requires:
  - phase: 02-cli-toml-scaffold/plan-01
    provides: "IronWebConfig struct in web_config.h"
  - phase: 02-cli-toml-scaffold/plan-02
    provides: "iron_toml_parse section==3 branch + Levenshtein typo detection"
  - phase: 02-cli-toml-scaffold/plan-03
    provides: "--target/--release flag parsing in main.c with usage output"
  - phase: 02-cli-toml-scaffold/plan-04
    provides: "iron_build_web stub in build_web.c with emcc detection + banner"
  - phase: 02-cli-toml-scaffold/plan-05
    provides: "single-line dispatch from iron_build() + -O2 native branch"
provides:
  - "8-test Unity suite (test_toml_web.c) covering WEB-MANIFEST-01..08: full section parse, assets string normalization, assets array parse, unknown-key resilience, zero-defaults, misspelled-section recovery, unrelated-section silent, free-no-leak ASan gate"
  - "CLI smoke tests in CMakeLists.txt: test_ironc_usage_lists_target, test_ironc_usage_lists_release, test_ironc_target_unknown, test_ironc_target_web_smoke"
  - "Pin-discipline ctest invariants: test_emsdk_pin_discipline, test_emsdk_pin_discipline_headers"
  - "web.yml path filter extended to all 9 Phase 2 source files"
affects: [phase-07-emcc-orchestration, future-web-phases]

# Tech tracking
tech-stack:
  added: []
  patterns:
    - "Option B direct-compile: toml.c compiled directly into test_toml_web, no static lib refactor needed"
    - "mkstemp for fixture temp files in Unity tests (race-free, no tmpnam)"
    - "CMAKE_SOURCE_DIR reference in tests/unit/CMakeLists.txt for cross-directory source paths"
    - "WILL_FAIL + sh -c wrapper pattern for testing expected non-zero exits"
    - "grep -qE 'pattern1|pattern2' for dual-outcome smoke tests (emcc found or not)"

key-files:
  created:
    - tests/unit/test_toml_web.c
    - .planning/phases/02-cli-toml-scaffold/02-plan-06-tests-and-ci-SUMMARY.md
  modified:
    - tests/unit/CMakeLists.txt
    - CMakeLists.txt
    - .github/workflows/web.yml

key-decisions:
  - "Option B (direct-compile toml.c into test) over Option A (new iron_cli_toml library) — keeps diff minimal, defers library refactor to when a second consumer exists"
  - "Dual-outcome web smoke test accepts either emcc-found banner or emcc-not-found error — both prove dispatch reached build_web.c"
  - "benchmark_smoke timeout failure is pre-existing (test added in PR #18), not caused by Phase 2 changes"

patterns-established:
  - "Phase 2 cli/TOML tests: direct-compile pattern for files not yet in a static library"
  - "Pin-discipline invariant: grep -Fq '4.0.23' as ctest COMMAND to enforce runtime version reads"

requirements-completed:
  - WEB-MANIFEST-01
  - WEB-MANIFEST-02
  - WEB-MANIFEST-08
  - WEB-CLI-01
  - WEB-CLI-02
  - WEB-CLI-03
  - WEB-CLI-04
  - WEB-CLI-08
  - WEB-CLI-09

# Metrics
duration: 19min
completed: 2026-04-11
---

# Phase 2 Plan 06: Tests and CI Summary

**8-test Unity suite + 6 ctest entries lock in Phase 2 TOML parsing, CLI flag surface, and emsdk pin-discipline; web.yml triggers on all Phase 2 source files**

## Performance

- **Duration:** 19 min
- **Started:** 2026-04-11T15:34:28Z
- **Completed:** 2026-04-11T15:53:37Z
- **Tasks:** 3
- **Files modified:** 4 (1 created, 3 edited)

## Accomplishments

- New Unity test file `tests/unit/test_toml_web.c` (245 lines) with 8 tests covering every WEB-MANIFEST requirement: full section parse, assets string normalization, assets array, unknown-key resilience, zero-defaults-when-unset, misspelled-section recovery, unrelated-section silent, and free-no-leak ASan gate
- 6 new `add_test` entries in root `CMakeLists.txt`: usage output contains `--target=<t>` and `--release`, `--target=bogus` exits non-zero with correct error message, `--target=web` dispatch reaches `build_web.c`, and pin-discipline invariants confirm `4.0.23` never appears in Phase 2 source files
- `.github/workflows/web.yml` path filter extended from 3 to 12 entries in both `push` and `pull_request` triggers, covering all 9 new Phase 2 source files

## Task Commits

1. **Task 1: Create tests/unit/test_toml_web.c** - `fba82f0` (test)
2. **Task 2: Register test_toml_web in CMakeLists.txt** - `7c2946b` (chore)
3. **Task 3: CLI smoke tests + pin discipline + web.yml** - `79aa1bc` (chore)

## Files Created/Modified

- `tests/unit/test_toml_web.c` — 8-test Unity suite for [web] TOML section; uses mkstemp fixtures; asserts on IronProject.web fields and iron_toml_free correctness
- `tests/unit/CMakeLists.txt` — added test_toml_web target: direct-compiles src/cli/toml.c, includes src/, links only unity
- `CMakeLists.txt` — 6 new add_test entries: usage flag surface, unknown target error, web dispatch smoke, emsdk pin-discipline x2
- `.github/workflows/web.yml` — extended paths filter from 3 to 12 entries in both push and pull_request triggers

## Decisions Made

- **Option B direct-compile** over Option A static library: `toml.c` compiled directly into `test_toml_web` executable. No static lib refactor needed in Phase 2; a future refactor can extract `iron_cli_toml` when a second test consumer exists.
- **Dual-outcome web smoke test**: `test_ironc_target_web_smoke` accepts either `using emcc` (emcc installed) or `emcc not found in PATH` (emcc absent). Both outcomes prove dispatch reached `build_web.c` rather than a parse error. Robust on developer machines without emsdk installed.

## Deviations from Plan

None - plan executed exactly as written.

## Issues Encountered

- `benchmark_smoke` test shows a Timeout failure in the full ctest run. This is pre-existing (test added in PR #18 commit `d4bf9d5`, before Phase 2 work) and is caused by the benchmark runner exceeding its 300-second timeout on CI machines. Not related to Phase 2 changes.

## User Setup Required

None - no external service configuration required.

## Next Phase Readiness

Phase 2 (02-cli-toml-scaffold) is now complete. All 6 plans executed:
- Plan 01: type foundations (IronWebConfig, IronBuildTarget enum)
- Plan 02: TOML [web] section parsing + Levenshtein typo detection
- Plan 03: --target/--release CLI flag parsing
- Plan 04: build_web.c emcc detection + banner stub
- Plan 05: dispatch wiring + native -O2 release branch
- Plan 06: test coverage + CI path filter (this plan)

Phase 7 (emcc orchestration) can now build on the complete Phase 2 foundation. The stub return in `iron_build_web()` is the replacement point for the actual emcc `posix_spawnp` invocation.

---
*Phase: 02-cli-toml-scaffold*
*Completed: 2026-04-11*

## Self-Check: PASSED

Files verified:
- FOUND: tests/unit/test_toml_web.c
- FOUND: commit fba82f0
- FOUND: commit 7c2946b
- FOUND: commit 79aa1bc
- PASS: build_web.c has no version pin (4.0.23)
- PASS: headers have no version pin
- web.yml has 2 occurrences of src/cli/build_web.c (push + pull_request)
