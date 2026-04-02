---
phase: 24-range-bound-hoisting
plan: 01
subsystem: compiler
tags: [hir-to-lir, loop-optimization, code-generation, tdd]

# Dependency graph
requires:
  - phase: 23-hir-correctness-audit
    provides: correct HIR lowering pipeline that this optimization builds on
provides:
  - GET_FIELD .count hoisted from for-loop header to pre-header block
  - unit test test_h2l_for_loop_count_hoisted verifying structural placement
  - all 137 benchmarks passing with hoisted loop bound
affects:
  - 25-stack-array-promotion
  - 27-function-inlining
  - any phase that relies on for-loop LIR structure

# Tech tracking
tech-stack:
  added: []
  patterns:
    - "Loop bound hoisting: emit_alloca_in_entry for loop invariant, GET_FIELD+STORE in pre_header, LOAD in header"

key-files:
  created: []
  modified:
    - src/hir/hir_to_lir.c
    - tests/hir/test_hir_to_lir.c

key-decisions:
  - "count_alloca placed immediately after var_alloca while pre_header is the active block, so count_alloca is in scope for the LOAD in header"
  - "LOAD of count_alloca replaces GET_FIELD in header — one LOAD vs one GET_FIELD per iteration, bound computed once in pre_header"

patterns-established:
  - "Loop invariant hoisting: emit_alloca_in_entry + store in pre_header + load in header (count_alloca pattern)"

requirements-completed:
  - LOOP-01
  - LOOP-02

# Metrics
duration: 25min
completed: 2026-03-31
---

# Phase 24 Plan 01: Range Bound Hoisting Summary

**GET_FIELD .count hoisted from for-loop header to pre-header in hir_to_lir.c, eliminating redundant struct field access on every iteration; verified by TDD unit test and 137/137 benchmarks passing**

## Performance

- **Duration:** ~25 min
- **Started:** 2026-03-31T00:00:00Z
- **Completed:** 2026-03-31T00:25:00Z
- **Tasks:** 2 (1 TDD implementation, 1 validation)
- **Files modified:** 2

## Accomplishments

- Hoisted `GET_FIELD .count` from the header block to the pre-header block in the `IRON_HIR_STMT_FOR` case of `hir_to_lir.c`; the bound is now evaluated once per loop entry rather than on every back-edge iteration
- Added `test_h2l_for_loop_count_hoisted` TDD test that verifies: entry has `for_count` alloca, pre_header has GET_FIELD, header has zero GET_FIELD and at least one LOAD of the hoisted bound
- All 137 benchmarks pass with no correctness regressions; 165 integration, 13 algorithm, and 3 composite tests also pass

## Task Commits

1. **Task 1: Add unit test and implement GET_FIELD .count hoisting** - `f622473` (feat)

**Plan metadata:** (docs commit follows)

_Note: Task 2 was validation-only (no file changes); results recorded in summary._

## Files Created/Modified

- `src/hir/hir_to_lir.c` - Added count_alloca, GET_FIELD+STORE in pre_header, replaced header GET_FIELD with LOAD
- `tests/hir/test_hir_to_lir.c` - Added test_h2l_for_loop_count_hoisted and RUN_TEST registration

## Benchmark Results (Task 2 Validation)

- **Total:** 137/137 passed (0 failed, 0 errors, 0 skipped)
- **Integration tests:** 165 passed, 0 failed
- **Algorithm tests:** 13 passed, 0 failed
- **Composite tests:** 3 passed, 0 failed
- **Unit tests (hir_to_lir):** 59 passed, 0 failed
- **For-range benchmarks passing:** quicksort, binary_search, kth_smallest_matrix, connected_components all pass
- **LOOP-01 (structural):** Verified by test_h2l_for_loop_count_hoisted — GET_FIELD in pre_header, zero in header
- **LOOP-02 (no regression):** All 137 benchmarks pass

## Decisions Made

- `count_alloca` declared and initialized while `pre_header` is the active block (before `switch_block(ctx, header)`), ensuring it is in scope as a C variable when the LOAD is emitted in the header section
- LOAD replaces GET_FIELD in header: one struct field access per loop entry rather than per iteration

## Deviations from Plan

None - plan executed exactly as written.

## Issues Encountered

None. The TDD cycle was clean: test failed on the first run confirming RED, implementation fixed it on the first attempt, all existing tests remained green.

## Next Phase Readiness

- Phase 24 range bound hoisting complete; LIR for-loop structure now has hoisted bound pattern
- Phase 25 (stack array promotion) can proceed; for-loop LIR is stable
- Phase 27 (function inlining) blocker check for `find_root` array param mode still applies

---
*Phase: 24-range-bound-hoisting*
*Completed: 2026-03-31*
