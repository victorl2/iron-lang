---
phase: 06-milestone-gap-closure
plan: 01
subsystem: runtime-stdlib
tags: [runtime, stdlib, builtins, range, timer, integration-tests]
dependency_graph:
  requires: []
  provides: [range-builtin, timer-stdlib]
  affects: [src/runtime, src/stdlib, src/analyzer, src/codegen, tests/integration]
tech_stack:
  added: []
  patterns: [auto-static-dispatch, value-semantics, struct-guard-macro]
key_files:
  created:
    - tests/integration/test_range.iron
    - tests/integration/test_range.expected
  modified:
    - src/runtime/iron_runtime.h
    - src/runtime/iron_builtins.c
    - src/analyzer/resolve.c
    - src/codegen/gen_exprs.c
    - src/codegen/codegen.c
    - src/stdlib/iron_time.h
    - src/stdlib/iron_time.c
    - src/stdlib/time.iron
    - tests/integration/test_time.iron
decisions:
  - "Timer C functions changed from pointer-based to value-based (Iron_Timer t not *t) to match auto-static dispatch which passes args by value"
  - "IRON_TIMER_STRUCT_DEFINED guard emitted by codegen before iron_time.h include to prevent struct redefinition conflict between codegen-emitted struct Iron_Timer and header-defined struct"
  - "object Timer { val start_ms: Int } declared in time.iron so codegen emits struct Iron_Timer with correct field matching iron_time.c ABI"
  - "Iron_range implemented as identity function: for i in range(N) compiles to for (i = 0; i < Iron_range(N); i++) where the for-loop codegen already treats iterable as integer upper bound"
metrics:
  duration_minutes: 7
  completed_date: "2026-03-26"
  tasks_completed: 2
  files_modified: 9
---

# Phase 06 Plan 01: Range Builtin and Timer Wrappers Summary

Added `range()` builtin function and `Timer` stdlib wrappers to close RT-07 and STD-03 requirements. `range(N)` is now callable from Iron source in for-loops; `Timer.create()`, `Timer.since(t)`, and `Timer.reset(t)` are callable from Iron source via the time stdlib.

## Tasks Completed

| Task | Name | Commit | Files |
|------|------|--------|-------|
| 1 | Add range builtin to resolver, codegen, runtime, and integration test | 329589c | iron_runtime.h, iron_builtins.c, resolve.c, gen_exprs.c, test_range.iron, test_range.expected |
| 2 | Add Timer wrappers to time.iron with value-based C functions | 5955ed9 | iron_time.h, iron_time.c, time.iron, codegen.c, test_time.iron |

## Decisions Made

1. **Iron_range as identity function**: `range(N)` is implemented as `int64_t Iron_range(int64_t n) { return n; }`. The for-loop codegen already produces `for (int64_t i = 0; i < <iterable>; i++)` so the identity function is correct — it just passes N through as the upper bound.

2. **Timer value semantics**: Changed `Iron_timer_since(const Iron_Timer *t)` and `Iron_timer_reset(Iron_Timer *t)` to `Iron_timer_since(Iron_Timer t)` and `Iron_timer_reset(Iron_Timer t) -> Iron_Timer`. The auto-static dispatch path in gen_exprs.c passes arguments by value (no `&`), so pointer-based functions would receive garbage. `Iron_timer_reset` now returns a new Timer by value.

3. **IRON_TIMER_STRUCT_DEFINED guard**: The codegen emits `object Timer { val start_ms: Int }` which causes the codegen to emit `struct Iron_Timer { int64_t start_ms; }`. The C header also defines `typedef struct Iron_Timer { int64_t start_ms; } Iron_Timer;`. To prevent a struct redefinition error in the generated C, the codegen.c now emits `#define IRON_TIMER_STRUCT_DEFINED` before `#include "stdlib/iron_time.h"`. The header conditionally emits only `typedef struct Iron_Timer Iron_Timer;` (forward declaration, C11 compatible duplicate) when the guard is defined, allowing codegen's struct body to be the authoritative definition.

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 1 - Bug] Fixed struct redefinition conflict for Iron_Timer**

- **Found during:** Task 2
- **Issue:** The plan did not account for a C struct redefinition conflict. The Iron `object Timer { val start_ms: Int }` declaration causes codegen to emit `struct Iron_Timer { int64_t start_ms; }`. But `iron_time.h` (included unconditionally in all generated C) also defined `typedef struct { int64_t start_ms; } Iron_Timer;` (anonymous struct), causing clang to error with "redefinition of Iron_Timer" and "typedef redefinition with different types".
- **Fix:** (a) Changed the header's struct to use a named struct `typedef struct Iron_Timer { int64_t start_ms; } Iron_Timer;` instead of anonymous. (b) Added `IRON_TIMER_STRUCT_DEFINED` guard mechanism: codegen emits `#define IRON_TIMER_STRUCT_DEFINED` before the time.h include; the header conditionally skips the struct body when the guard is set, emitting only a forward typedef. The codegen's struct body becomes the authoritative definition, matching the exact field layout that `iron_time.c` expects.
- **Files modified:** src/stdlib/iron_time.h, src/codegen/codegen.c
- **Commit:** 5955ed9

## Verification Results

- Build: PASS (no errors, only linker duplicate-library warning which is pre-existing)
- Range test: PASS — `./build/iron build tests/integration/test_range.iron && ./test_range` prints "range works"
- Time test: PASS — `./build/iron build tests/integration/test_time.iron && ./test_time` prints "time works"
- Full integration suite: 12 passed, 0 failed (test_range and test_time both PASS)
- ctest: 21/23 passed; 2 failures (test_interp_codegen, test_parallel_codegen) are pre-existing "Not Run" failures — executables not built — unrelated to this plan

## Self-Check: PASSED
