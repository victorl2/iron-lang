---
phase: 53-analysis-improvements
plan: 03
subsystem: compiler
tags: [monomorphic, interprocedural, split-collection, lir, emit, gap-closure]

requires:
  - phase: 53-analysis-improvements
    provides: "Phase 0 func_return_types pre-pass and Phase E parameter tracking from 53-02"

provides:
  - "Phase A.1 wiring of CALL results to coll_types via func_return_types"
  - "Chained-return integration test exercising CALL-to-coll_types path"
  - "Specialization heuristic test exercising Phase E call_site_counts and instruction threshold rejection"

affects: [54-test-hardening]

tech-stack:
  added: []
  patterns:
    - "Phase A.1 CALL-result-to-coll_types wiring pattern: lookup CALL in split_collection_ids, resolve callee name, copy func_return_types entry into coll_types"

key-files:
  created: []
  modified:
    - src/lir/emit_c.c
    - tests/integration/mono_interprocedural.iron
    - tests/integration/mono_specialization_heuristic.iron
    - tests/integration/mono_specialization_heuristic.expected

key-decisions:
  - "Phase A.1 placed after ARRAY_LIT scan and before Phase B STORE/LOAD propagation, so CALL results participate in vid_origin chains"
  - "Chained return test pattern (buildMixed -> wrapShapes -> main) validates CALL-only functions with no ARRAY_LIT"
  - "bigProcess() uses 25 arithmetic statements to exceed 50 LIR instruction threshold for heuristic rejection testing"

patterns-established:
  - "Gap closure plans can wire existing infrastructure without new data structures"

requirements-completed: [ANAL-01]

duration: 11min
completed: 2026-04-09
---

# Phase 53 Plan 03: Gap Closure -- CALL-to-coll_types Wiring and Test Coverage Summary

**Phase A.1 wiring connects func_return_types to coll_types for CALL results, closing the missing link between Phase 0 return type collection and Phase D monomorphic evaluation**

## Performance

- **Duration:** 11 min
- **Started:** 2026-04-09T00:02:05Z
- **Completed:** 2026-04-09T00:13:47Z
- **Tasks:** 2
- **Files modified:** 4

## Accomplishments
- Phase A now adds CALL results found in split_collection_ids to coll_types by looking up the callee in func_return_types and copying the return type set -- the critical missing link identified by verification
- mono_interprocedural.iron rewritten with chained returns (buildMixed -> wrapShapes -> main) where wrapShapes has no ARRAY_LIT, only a CALL result, directly exercising the new Phase A.1 code path
- mono_specialization_heuristic.iron rewritten with sumMixedAreas() calling buildMixed() (exercises Phase E call_site_counts) and bigProcess() exceeding 50 instructions (tests heuristic rejection)
- All 274 integration tests pass with zero regressions

## Task Commits

Each task was committed atomically:

1. **Task 1: Wire func_return_types into Phase A coll_types for CALL results** - `acacab0` (feat)
2. **Task 2: Rewrite mono_specialization_heuristic to exercise Phase E parameter tracking** - `a97ceb0` (feat)

## Files Created/Modified
- `src/lir/emit_c.c` - Added Phase A.1 block: CALL result lookup in split_collection_ids, callee name resolution, func_return_types type set copy into coll_types
- `tests/integration/mono_interprocedural.iron` - Chained return test: buildMixed -> wrapShapes -> main (output 118)
- `tests/integration/mono_specialization_heuristic.iron` - Three-function test: sumCircleAreas (local monomorphic), sumMixedAreas (CALL result iteration), bigProcess (large function heuristic rejection)
- `tests/integration/mono_specialization_heuristic.expected` - Updated to 249/118/2080

## Decisions Made
- Phase A.1 block positioned after ARRAY_LIT scanning and before Phase B STORE/LOAD propagation so that CALL results are added to vid_origin and can be propagated through subsequent STORE/LOAD chains in the same function
- Chained return pattern chosen because wrapShapes() contains no ARRAY_LIT -- its only collection comes from a CALL result, isolating the new code path
- bigProcess() uses 25 arithmetic statements which produce well over 50 LIR instructions, ensuring the heuristic threshold is exceeded

## Deviations from Plan

None -- plan executed exactly as written.

## Issues Encountered
None -- both tasks compiled and produced expected output on first attempt.

## User Setup Required
None -- no external service configuration required.

## Next Phase Readiness
- All three verification gaps from 53-VERIFICATION.md are now addressed:
  - Gap 1 (CALL results not in coll_types): Fixed by Phase A.1 wiring
  - Gap 2 (no test exercises parameter tracking): sumMixedAreas calls buildMixed and iterates
  - Gap 3 (no test exercises heuristic approval/rejection): bigProcess exceeds threshold
- Phase 53 ANAL-01 requirement is now fully wired and tested
- Ready for Phase 54 test hardening

---
*Phase: 53-analysis-improvements*
*Completed: 2026-04-09*
