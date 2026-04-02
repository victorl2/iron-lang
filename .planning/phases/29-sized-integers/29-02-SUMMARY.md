---
phase: 29-sized-integers
plan: 02
subsystem: benchmarks
tags: [int32, benchmarks, codegen, phi-elimination, stack-arrays]

# Dependency graph
requires:
  - phase: 29-01
    provides: Int32 type coercion rules, Iron_List_int32_t runtime support, fill(N, Int32(0)) stack promotion

provides:
  - connected_components benchmark rewritten with Int32 parent/rank arrays (int32_t stack bandwidth)
  - int32_array_sum micro-benchmark for isolated 32-bit array throughput measurement
  - Two codegen bug fixes: GET/SET_INDEX param-value type lookup and phi zero-initialization for sized integers

affects: [30-sized-integers-validation, benchmarks, codegen]

# Tech tracking
tech-stack:
  added: []
  patterns:
    - "get_value_type(fn, vid) helper pattern: parameter value IDs (1..param_count) have NULL value_table entries; use fn->params[vid-1].type fallback"
    - "Phi zero-init for sized integers: use iron_lir_const_int(0) not const_null for INT8/INT16/INT32/INT64/UINT* types"

key-files:
  created:
    - tests/benchmarks/problems/int32_array_sum/main.iron
    - tests/benchmarks/problems/int32_array_sum/expected_output.txt
    - tests/benchmarks/problems/int32_array_sum/config.json
    - tests/benchmarks/problems/int32_array_sum/solution.c
  modified:
    - tests/benchmarks/problems/connected_components/main.iron
    - tests/benchmarks/problems/connected_components/config.json
    - src/lir/emit_c.c
    - src/hir/hir_to_lir.c

key-decisions:
  - "GET_INDEX/SET_INDEX type lookup: add get_value_type() helper that handles parameter values (NULL in value_table) by consulting fn->params[vid-1]; this is the correct fix rather than hardcoding a type"
  - "Phi zero-initialization: all sized integer types must use const_int(0) not const_null; the original code only handled IRON_TYPE_INT and IRON_TYPE_BOOL, missing INT8/16/32/64 and UINT variants"
  - "CONST_NULL emitter defensive fix: scalar integer types now emit typed zero even if const_null slips through; belt-and-suspenders alongside hir_to_lir fix"
  - "connected_components max_ratio lowered from 1500 to 500: Int32 stack arrays reduce memory bandwidth; tighter threshold reflects the improvement"

requirements-completed: [INT-01, INT-02]

# Metrics
duration: 25min
completed: 2026-04-01
---

# Phase 29 Plan 02: Sized Integers Benchmarks Summary

**connected_components rewritten with int32_t parent/rank stack arrays plus dedicated Int32 array sum micro-benchmark; two codegen bugs fixed (param-value GET_INDEX type lookup and phi zero-init for sized int types)**

## Performance

- **Duration:** ~25 min
- **Started:** 2026-04-01T15:17:51Z
- **Completed:** 2026-04-01T15:43:00Z
- **Tasks:** 2
- **Files modified:** 8 (4 created, 4 modified)

## Accomplishments
- Rewrote `connected_components/main.iron` to use `[Int32]` for all parent/rank arrays and node indices; all 4 correctness test cases pass with unchanged expected output
- Created complete `int32_array_sum` micro-benchmark (main.iron, expected_output.txt, config.json, solution.c) measuring isolated 32-bit array throughput; all 4 test cases pass
- Fixed GET_INDEX/SET_INDEX codegen: parameter values have NULL entries in `value_table` by design; new `get_value_type()` helper consults `fn->params[vid-1]` so `.items[index]` path is taken instead of wrong `Iron_List_int64_t_get()` fallback
- Fixed phi zero-initialization: sized integer types (INT8/16/32/64, UINT variants) now use `iron_lir_const_int(0)` instead of `const_null`; prevents `void* NULL` being assigned to `int32_t` phi variables in loops with uninitialized paths
- All 43 non-benchmark-smoke tests pass (integration, algorithms, composite, unit, hir, lir)

## Task Commits

Each task was committed atomically:

1. **Task 1: Rewrite connected_components benchmark to use Int32 arrays** - `2f72547` (feat)
2. **Task 2: Add dedicated Int32 micro-benchmark** - `cdd7af8` (feat)

**Plan metadata:** `(pending final commit)` (docs: complete plan)

## Files Created/Modified
- `tests/benchmarks/problems/connected_components/main.iron` - Rewrites all Int arrays to [Int32], all element operations use Int32() casts
- `tests/benchmarks/problems/connected_components/config.json` - max_ratio lowered from 1500.0 to 500.0
- `tests/benchmarks/problems/int32_array_sum/main.iron` - New benchmark: Int32 array sum with 200-element stack array, 10M iterations
- `tests/benchmarks/problems/int32_array_sum/expected_output.txt` - 4 correctness test lines
- `tests/benchmarks/problems/int32_array_sum/config.json` - max_ratio 20.0, 60s timeout
- `tests/benchmarks/problems/int32_array_sum/solution.c` - C reference with int32_t stack arrays
- `src/lir/emit_c.c` - get_value_type() helper; updated GET_INDEX/SET_INDEX (both statement and expression paths); CONST_NULL emitter handles scalar integer types
- `src/hir/hir_to_lir.c` - Phi zero-init extended to all sized integer type kinds

## Decisions Made
- Added `get_value_type()` helper to `emit_c.c` rather than inline the parameter lookup at each GET/SET_INDEX site — cleaner, reusable pattern
- Extended phi zero-init to cover all `IRON_TYPE_INT*` / `IRON_TYPE_UINT*` variants at the source (hir_to_lir.c) and defensively in the emitter — belt-and-suspenders prevents future type additions from hitting the same bug
- `connected_components` max_ratio set to 500.0 — conservative enough to pass on slow machines while reflecting real Int32 improvement (was 1500.0 before any optimization)

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 1 - Bug] Fixed GET_INDEX/SET_INDEX using wrong list accessor for parameter arrays**
- **Found during:** Task 1 (Rewrite connected_components benchmark)
- **Issue:** When `instr->index.array` is a parameter value ID (1..N), `fn->value_table[vid]` is NULL (synthetic parameter slots). The fallback used hardcoded `Iron_List_int64_t` default, causing clang `-Wint-conversion` errors for `[Int32]` params.
- **Fix:** Added `get_value_type(fn, vid)` helper in `emit_c.c` that falls back to `fn->params[vid-1].type` for parameter value IDs. Updated all three GET_INDEX/SET_INDEX code paths (statement emitter x2, expression emitter x1) to use this helper.
- **Files modified:** `src/lir/emit_c.c`
- **Verification:** `connected_components/main.iron` compiles without type errors; `.items[index]` path used correctly
- **Committed in:** `2f72547` (Task 1 commit)

**2. [Rule 1 - Bug] Fixed phi zero-initialization for sized integer types using CONST_NULL**
- **Found during:** Task 1 (after fixing GET_INDEX, second compilation error)
- **Issue:** In `hir_to_lir.c`, phi variables with "no visible definition" (variables only assigned in conditional branches) were initialized with `const_null` for all types except `IRON_TYPE_INT` and `IRON_TYPE_BOOL`. For `IRON_TYPE_INT32`, this emitted `void* _vN = NULL`, causing `int32_t = void*` type error.
- **Fix:** Extended the type check in `hir_to_lir.c` to use `iron_lir_const_int(0)` for all INT8/16/32/64 and UINT8/16/32/64 type kinds. Added defensive `CONST_NULL` emitter fix in `emit_c.c` as belt-and-suspenders.
- **Files modified:** `src/hir/hir_to_lir.c`, `src/lir/emit_c.c`
- **Verification:** No more `void* = int32_t` errors; `real_rx`/`real_ry` phi variables in `count_components` now get proper `int32_t _vN = 0` initialization
- **Committed in:** `2f72547` (Task 1 commit)

---

**Total deviations:** 2 auto-fixed (2 Rule 1 bugs)
**Impact on plan:** Both fixes required for Int32 benchmark correctness. They expose pre-existing codegen gaps that only surfaced when `[Int32]` arrays were used with by-value passing and uninitialized phi variables. No scope creep.

## Issues Encountered
- The two codegen bugs were not anticipated in the plan but were directly caused by Int32 usage patterns in the rewrite (by-value array params + phi variables in conditional branches). Both were isolated to `emit_c.c` and `hir_to_lir.c`.

## Next Phase Readiness
- Phase 29 (Sized Integers) complete: INT-01 and INT-02 requirements fulfilled
- `connected_components` now uses `int32_t` stack arrays matching the C reference; performance gap should be measurable
- `int32_array_sum` provides a clean baseline for isolated Int32 array throughput comparison
- Codegen bug fixes (get_value_type, phi zero-init) benefit any future work using sized integer types

---
*Phase: 29-sized-integers*
*Completed: 2026-04-01*
