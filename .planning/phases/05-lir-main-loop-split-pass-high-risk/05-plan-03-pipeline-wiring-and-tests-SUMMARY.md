---
phase: 05-lir-main-loop-split-pass-high-risk
plan: 03
subsystem: compiler
tags: [lir, web, emscripten, raylib, unity-test, ci]

requires:
  - phase: 05-plan-01
    provides: "IronLIR_Func.web_frame_captures field + IRON_ERR_WEB_* error codes 700-703"
  - phase: 05-plan-02
    provides: "iron_lir_web_main_loop_split() implementation in src/lir/web_main_loop_split.c"

provides:
  - "iron_lir_web_main_loop_split wired into build.c between iron_lir_optimize and iron_lir_emit_c (step 7b)"
  - "7-case Unity test suite in tests/unit/test_web_main_loop_split.c covering canonical detection, capture population, error emission, and target-gating"
  - "CMake registration for test_web_main_loop_split in tests/unit/CMakeLists.txt"
  - "web.yml paths filter extended for Phase 5 source files (both push + pull_request blocks)"

affects:
  - phase-06-emit-web
  - phase-07-iron-build-web-pipeline

tech-stack:
  added: []
  patterns:
    - "Step 7b error-bail: iron_lir_web_main_loop_split + if (diags.error_count > 0) block that frees all pipeline resources"
    - "Hand-built LIR fixture pattern: iron_lir_module_create + iron_lir_func_create + manual CFG edge wiring (arrput preds/succs)"

key-files:
  created:
    - tests/unit/test_web_main_loop_split.c
  modified:
    - src/cli/build.c
    - tests/unit/CMakeLists.txt
    - .github/workflows/web.yml

key-decisions:
  - "build.c call site runs only on the native path today — web builds short-circuit to iron_build_web() before reaching the LIR pipeline; Phase 6 will refactor iron_build_web to reuse this pipeline"
  - "Test suite uses 7 cases instead of the planned 6 — added test_canonical_read_only_capture_not_mutable to assert is_mutable=false for read-only captures, improving WEB-EMIT-03 coverage"
  - "CFG edges (preds/succs) wired manually via arrput() in fixture builders — required because lir_block_create does not auto-wire edges; this matches the existing test_lir_optimize.c pattern"

patterns-established:
  - "Phase 5 LIR unit tests: allocate a dedicated ir_arena per test, call iron_types_init on both g_arena (in setUp) and ir_arena (per test), destroy module + free ir_arena in each test body"
  - "Iron_FuncDecl stubs for extern functions: ARENA_ALLOC + memset to zero + set name + is_extern=true; pass only reads func_decl->name"

requirements-completed:
  - WEB-EMIT-01
  - WEB-EMIT-02
  - WEB-EMIT-03
  - WEB-EMIT-04

duration: ~20min
completed: 2026-04-11
---

# Phase 5 Plan 03: Pipeline Wiring and Tests Summary

**LIR web main-loop split pass wired into build.c as step 7b with 7-case Unity test coverage for canonical detection, capture population, error emission (700/701/702), and target-gating**

## Performance

- **Duration:** ~20 min
- **Started:** 2026-04-11
- **Completed:** 2026-04-11
- **Tasks:** 4 (wiring, unit test, CMake registration, web.yml extension)
- **Files modified:** 4 (build.c, test_web_main_loop_split.c, CMakeLists.txt, web.yml)

## Accomplishments

- Wired `iron_lir_web_main_loop_split` into `src/cli/build.c` between `iron_lir_optimize` and `iron_lir_emit_c` as step 7b with a full error-bail block matching the existing pipeline error-handling pattern
- Created `tests/unit/test_web_main_loop_split.c` (7 cases, ~620 lines) exercising all WEB-EMIT-01..04 contracts via hand-built LIR fixtures
- Registered the test in `tests/unit/CMakeLists.txt` linking `iron_compiler + unity`
- Extended `web.yml` paths filter in both `push` and `pull_request` blocks with 5 new Phase 5 paths

## Task Commits

1. **Task 1: Wire iron_lir_web_main_loop_split into build.c** - `c7f8d2c` (feat)
2. **Task 2+3: Add Unity test suite + CMake registration** - `22a394e` (test)
3. **Task 4: Extend web.yml paths filter** - `55d659a` (chore)

## Files Created/Modified

- `src/cli/build.c` — Added `#include "lir/web_main_loop_split.h"` and step 7b call with error-bail
- `tests/unit/test_web_main_loop_split.c` — 7 Unity test cases covering all WEB-EMIT-01..04 contracts
- `tests/unit/CMakeLists.txt` — Phase 5 test registration block
- `.github/workflows/web.yml` — Phase 5 source files added to both push + pull_request paths filters

## Decisions Made

- The `build.c` call site runs only on the native path today. Web builds short-circuit to `iron_build_web()` before reaching the LIR pipeline. The call is placed here now to be forward-compatible; Phase 6 refactors `iron_build_web` to reuse this pipeline.
- Unit test suite has 7 cases (plan said minimum 6). An extra test (`test_canonical_read_only_capture_not_mutable`) was added to explicitly assert `is_mutable=false` for read-only captures, strengthening WEB-EMIT-03 coverage.
- Fixtures wire CFG edges manually via `arrput(block->preds, ...)` / `arrput(block->succs, ...)`. The LIR constructors do not auto-wire edges — this is the established pattern from `test_lir_optimize.c`.

## Deviations from Plan

None — plan executed exactly as written (7 cases instead of minimum 6 is additive, not a deviation).

## Issues Encountered

None. Build, test, and full suite all passed on first attempt.

## Phase 5 Completion Note

This is the last plan of Phase 5. All three plans are complete:
- 05-01: IronLIR_Func.web_frame_captures field + IRON_ERR_WEB_* diagnostic codes
- 05-02: iron_lir_web_main_loop_split() pass implementation
- 05-03: Pipeline wiring + 7-case Unity test coverage (this plan)

**WEB-EMIT-01..04 are fully implemented and tested. Phase 6 (emit_web.c) can proceed.**

Phase 6 dependency: `iron_build_web()` in `src/cli/build_web.c` must be refactored to run the analyzer → HIR → LIR pipeline before the Step 7b call site in `build.c` becomes reachable on `--target=web`.

## Self-Check: PASSED

- `tests/unit/test_web_main_loop_split.c` — EXISTS
- `src/cli/build.c` contains `iron_lir_web_main_loop_split` — VERIFIED
- `src/cli/build.c` contains `lir/web_main_loop_split.h` include — VERIFIED
- Commits c7f8d2c, 22a394e, 55d659a — VERIFIED
- `ctest -R test_web_main_loop_split` — 7/7 PASS
- `ctest -E benchmark_smoke` — 66/66 PASS (100%)
- `cmake --build build` — CLEAN
- `git diff src/lir/emit_c.c` — EMPTY (no touches)
- No "4.0.23" in test_web_main_loop_split.c — VERIFIED

---
*Phase: 05-lir-main-loop-split-pass-high-risk*
*Completed: 2026-04-11*
