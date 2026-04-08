---
phase: 53-analysis-improvements
plan: 02
subsystem: compiler
tags: [monomorphic, interprocedural, split-collection, specialization, lir, emit]

requires:
  - phase: 49-loop-fusion
    provides: "Monomorphic collection detection, specialization registry"
  - phase: 52-emitter-refactoring
    provides: "Modular emitter with emit_helpers, emit_split, emit_fusion"

provides:
  - "Interprocedural return-type tracking across function boundaries"
  - "emit_type_to_c returns Iron_SplitList_ for interface-typed arrays"
  - "CALL result split collection propagation in per-function prescan"
  - "Split loop iterable_vid hoisting for inlined function returns"
  - "Phase E parameter type collection with specialization heuristic"

affects: [54-test-hardening, future-interprocedural-optimization]

tech-stack:
  added: []
  patterns:
    - "func_return_types string hashmap for cross-function type propagation"
    - "Compound literal zero-init for hoisted SplitList declarations"
    - "Per-function CALL result tracking for interface-array returns"

key-files:
  created:
    - tests/integration/mono_interprocedural.iron
    - tests/integration/mono_interprocedural.expected
    - tests/integration/mono_specialization_heuristic.iron
    - tests/integration/mono_specialization_heuristic.expected
  modified:
    - src/lir/emit_c.c
    - src/lir/emit_helpers.c

key-decisions:
  - "emit_type_to_c uses Iron_SplitList_ for interface arrays (fixes pre-existing return type mismatch)"
  - "CALL results for interface arrays added to split_collection_ids per-function (not module-level)"
  - "Split loop iterable_vid hoisting handles inlined function returns"
  - "Specialization heuristic: <=50 instrs, 1-2 call sites, dispatch branches required"

patterns-established:
  - "Phase 53 pre-pass pattern: collect per-function return types before monomorphic scan"
  - "Hoisted SplitList uses compound literal: _vN = (Iron_SplitList_X){0}"

requirements-completed: [ANAL-01]

duration: 52min
completed: 2026-04-08
---

# Phase 53 Plan 02: Interprocedural Monomorphic Detection Summary

**Cross-function return-type tracking with parameter-based specialization heuristic and split collection return support**

## Performance

- **Duration:** 52 min
- **Started:** 2026-04-08T22:29:00Z
- **Completed:** 2026-04-08T23:21:00Z
- **Tasks:** 2
- **Files modified:** 6

## Accomplishments
- Functions returning interface-typed arrays now use Iron_SplitList_ in signatures (fixing pre-existing codegen bug)
- CALL results for interface arrays properly tracked as split collections, enabling cross-function iteration
- Phase 0 pre-pass collects concrete return types per function for interprocedural type propagation
- Phase E parameter type collection with heuristic-gated specialization (size, call-sites, dispatch overhead)
- Split loop iterable_vid hoisting handles inlined function returns that define collections in later blocks

## Task Commits

Each task was committed atomically:

1. **Task 1: Interprocedural return-type tracking** - `7edc61b` (feat)
2. **Task 2: Parameter-based specialization heuristic** - `eb5dfc5` (feat)

## Files Created/Modified
- `src/lir/emit_c.c` - Phase 0 return type collection, Phase E parameter tracking, split loop hoisting, CALL result propagation
- `src/lir/emit_helpers.c` - emit_type_to_c returns Iron_SplitList_ for interface-typed arrays
- `tests/integration/mono_interprocedural.iron` - Cross-function split collection return test
- `tests/integration/mono_interprocedural.expected` - Expected output (118)
- `tests/integration/mono_specialization_heuristic.iron` - Heuristic-gated specialization test
- `tests/integration/mono_specialization_heuristic.expected` - Expected output (249, 69)

## Decisions Made
- emit_type_to_c changed to use Iron_SplitList_ for interface-typed arrays; this fixes a pre-existing bug where functions returning [Interface] had Iron_List_ in their signature but created Iron_SplitList_ in their body
- CALL result split collection tracking done per-function (in emit_func_body prescan) rather than module-level, because the per-function STORE/LOAD propagation needs to run in the same scope
- Split loop iterable_vid hoisting added as a separate pass after split loop detection, to handle cases where inlined function returns define collections in later blocks that the standard hoisting mechanism misses
- Specialization heuristic uses conservative thresholds (50 instrs, 2 callers, 1 dispatch branch) to prevent code bloat

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 1 - Bug] Fixed emit_type_to_c for interface-typed arrays**
- **Found during:** Task 1
- **Issue:** emit_type_to_c returned Iron_List_ for [Interface] arrays, but the emitter creates Iron_SplitList_ for interface arrays, causing C compilation errors for functions returning [Interface]
- **Fix:** Changed IRON_TYPE_ARRAY case in emit_type_to_c to use Iron_SplitList_ when elem is IRON_TYPE_INTERFACE
- **Files modified:** src/lir/emit_helpers.c
- **Committed in:** 7edc61b

**2. [Rule 1 - Bug] Fixed SplitList declaration hoisting for inlined returns**
- **Found during:** Task 1
- **Issue:** When a function returning [Interface] was inlined, the ARRAY_LIT creating the SplitList was in a later block than the for-loop using it, but the standard hoisting mechanism didn't detect this because the reference came through split_collection_ids propagation rather than standard LIR instruction operands
- **Fix:** Added dedicated iterable_vid hoisting pass in emit_func_body after split loop detection
- **Files modified:** src/lir/emit_c.c
- **Committed in:** 7edc61b

**3. [Rule 1 - Bug] Fixed hoisted ARRAY_LIT emission for SplitList**
- **Found during:** Task 1
- **Issue:** Hoisted ARRAY_LIT instructions emitted duplicate type prefix (Iron_SplitList_X Iron_SplitList_X _vN = {0})
- **Fix:** Added phi_hoisted check in ARRAY_LIT emission to use compound literal form when hoisted
- **Files modified:** src/lir/emit_c.c
- **Committed in:** 7edc61b

---

**Total deviations:** 3 auto-fixed (3 bugs)
**Impact on plan:** All auto-fixes were required to make the interprocedural return tracking work correctly. The emit_type_to_c fix also resolves a latent bug for any future code that returns [Interface] from functions.

## Issues Encountered
- Iron type system does not support array covariance ([Circle] cannot be assigned to [Shape]), which limits the scenarios where interprocedural monomorphic collapse can trigger. The return type tracking infrastructure is in place for when the type system is extended.
- Passing [Interface] arrays as function parameters uses pointer mode, which is incompatible with SplitList representation. The parameter-based specialization test was adjusted to avoid this pattern.

## User Setup Required
None - no external service configuration required.

## Next Phase Readiness
- Interprocedural return type tracking infrastructure in place
- Phase E parameter tracking collects cross-function type information
- Specialization heuristic gates code duplication based on function size and dispatch overhead
- Ready for Phase 54 test hardening

---
*Phase: 53-analysis-improvements*
*Completed: 2026-04-08*
