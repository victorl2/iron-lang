---
phase: 49-loop-fusion-monomorphic-specialization
plan: 02
subsystem: compiler
tags: [loop-fusion, fused-loops, emit_c, split-collections, integration-tests]

# Dependency graph
requires:
  - phase: 49-loop-fusion-monomorphic-specialization
    provides: "Plan 01: @fusible annotation pipeline, chain detection pre-scan, FusionChain data structures"
  - phase: 47-collection-methods
    provides: "Collection method dispatch infrastructure (map/filter/reduce/forEach/sum)"
  - phase: 48-layout-optimizations
    provides: "SoA layout queries, split collection tracking"
provides:
  - "emit_fused_chain() function for flat arrays and split collections"
  - "Chain-interior skip and hoisting suppression enabled"
  - "8 fusion integration tests covering all method combinations"
  - "Split collection pre-population ordering fix for chain detection"
affects: [49-03 monomorphic-specialization]

# Tech tracking
tech-stack:
  added: []
  patterns:
    - "Fused loop emission: typedef+memcpy per lambda, single for-loop composing all chain operations"
    - "Split collection fused loops: per-type iteration with sequential accumulation"
    - "Pre-populate split_collection_ids with STORE/LOAD propagation before chain detection"

key-files:
  created:
    - tests/integration/fusion_flat_map_filter_sum.iron
    - tests/integration/fusion_flat_map_reduce.iron
    - tests/integration/fusion_flat_filter_sum.iron
    - tests/integration/fusion_flat_map_filter_reduce.iron
    - tests/integration/fusion_split_map_filter_reduce.iron
    - tests/integration/fusion_split_map_filter_sum.iron
    - tests/integration/fusion_chain_break.iron
    - tests/integration/fusion_intermediate_escape.iron
  modified:
    - src/lir/emit_c.c

key-decisions:
  - "Fused loop emission placed before emit_instr as static helper function"
  - "Per-node type tracking with node_in_type/node_out_type arrays for correct lambda casting"
  - "Split collection pre-population with STORE/LOAD propagation moved before chain detection"
  - "Chain-interior hoisting suppression enabled to prevent unused variable declarations"

patterns-established:
  - "emit_fused_chain(): flat path emits single for-loop; split path emits per-type for-loops with shared accumulator"
  - "Lambda function casting: typedef _FuseFn_N + memcpy from .fn + env extraction from .env"
  - "Node output type resolution: map uses CALL return type, filter passes through, reduce/sum use scalar type"

requirements-completed: [FUSE-01, FUSE-02]

# Metrics
duration: 25min
completed: 2026-04-08
---

# Phase 49 Plan 02: Fused Loop Emission and Integration Tests Summary

**Fused loop emission composing map/filter/reduce/sum into single-pass loops for flat arrays and split collections, verified with 8 integration tests**

## Performance

- **Duration:** 25 min
- **Started:** 2026-04-08T02:50:19Z
- **Completed:** 2026-04-08T03:15:19Z
- **Tasks:** 2
- **Files modified:** 17

## Accomplishments
- Implemented emit_fused_chain() handling both flat arrays (single fused loop) and split collections (per-type fused loops with sequential accumulation)
- Enabled chain-interior skip and hoisting suppression from Plan 01
- Created 8 integration tests verifying all fusion method combinations, chain breaks, and escape detection
- All 265 integration tests pass (257 existing + 8 new)

## Task Commits

Each task was committed atomically:

1. **Task 1: Fused loop emission for flat arrays and split collections** - `660b0da` (feat)
2. **Task 2: Fusion integration tests covering all method combinations** - `7b86ce7` (feat)

## Files Created/Modified
- `src/lir/emit_c.c` - Added emit_fused_chain() function (~300 lines), enabled chain-interior skip and hoisting suppression, added split collection pre-population before chain detection
- `tests/integration/fusion_flat_map_filter_sum.iron` + `.expected` - map+filter+sum on flat [Int] array (expected: 120)
- `tests/integration/fusion_flat_map_reduce.iron` + `.expected` - map+reduce on flat array (expected: 55)
- `tests/integration/fusion_flat_filter_sum.iron` + `.expected` - filter+sum on flat array (expected: 120)
- `tests/integration/fusion_flat_map_filter_reduce.iron` + `.expected` - map+filter+reduce full chain (expected: 45)
- `tests/integration/fusion_split_map_filter_reduce.iron` + `.expected` - split collection map+filter+reduce (expected: 102)
- `tests/integration/fusion_split_map_filter_sum.iron` + `.expected` - split collection map+filter+sum (expected: 85)
- `tests/integration/fusion_chain_break.iron` + `.expected` - chain break at non-fusible boundary (expected: 30, 24)
- `tests/integration/fusion_intermediate_escape.iron` + `.expected` - intermediate escape prevents fusion (expected: 300, 240)

## Decisions Made
- **emit_fused_chain placement:** Added as a static function before emit_instr, called at chain terminal nodes. Keeps fused emission logic separate from the main instruction dispatch.
- **Per-node type tracking:** Built node_in_type and node_out_type arrays to correctly determine lambda typedef argument/return types at each chain position. Map changes element type; filter passes through; reduce/sum produce scalar.
- **Split collection pre-population ordering:** Moved ARRAY_LIT split collection ID detection before fusion chain detection (with STORE/LOAD propagation) so chains on split collections are correctly identified as split.
- **Chain-interior hoisting suppression:** Uncommented the Plan 01 hoisting guard so chain-interior values that produce no code are not hoisted as unused declarations.

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 3 - Blocking] Split collection pre-population ordering**
- **Found during:** Task 2 (split fusion integration tests)
- **Issue:** Chain detection pre-scan ran before Phase 41's split_collection_ids population from ARRAY_LIT instructions. Chains on split collections were incorrectly identified as flat arrays, causing C compilation errors (accessing .count/.items on Iron_SplitList).
- **Fix:** Added duplicate ARRAY_LIT pre-scan with STORE/LOAD propagation before the fusion chain detection. The Phase 41 scan below is idempotent (hmput overwrites same key).
- **Files modified:** src/lir/emit_c.c
- **Verification:** fusion_split_map_filter_reduce and fusion_split_map_filter_sum both compile and produce correct output
- **Committed in:** 7b86ce7 (Task 2 commit)

---

**Total deviations:** 1 auto-fixed (1 blocking)
**Impact on plan:** Necessary ordering fix for split collection chain detection. No scope creep.

## Issues Encountered
None beyond the deviation noted above.

## User Setup Required
None - no external service configuration required.

## Next Phase Readiness
- Fusion chain detection and fused loop emission are complete
- Plan 03 (monomorphic specialization) can now build on the fusion infrastructure
- All fusion tests provide regression coverage for future changes

---
*Phase: 49-loop-fusion-monomorphic-specialization*
*Completed: 2026-04-08*
