---
phase: 53-analysis-improvements
plan: 01
subsystem: analysis
tags: [value-range, interprocedural, conditional-narrowing, field-compression]

requires:
  - phase: 50-vrc-arena
    provides: ValueRangeAnalysis infrastructure, field compression via type ladder
provides:
  - Interprocedural return range propagation at CALL sites
  - Conditional branch range narrowing (LT/LTE/GT/GTE/EQ/NEQ)
  - resolve_call_name helper for func_decl and func_ref resolution
  - detect_branch_narrowing shared analysis helper
affects: [54-test-hardening, emit-c, value-range]

tech-stack:
  added: []
  patterns: [block-entry-range-override, AND-chain-accumulation, two-pass-return-range]

key-files:
  created:
    - tests/integration/value_range_return_prop.iron
    - tests/integration/value_range_return_prop.expected
    - tests/integration/value_range_conditional.iron
    - tests/integration/value_range_conditional.expected
  modified:
    - src/lir/value_range.h
    - src/lir/value_range.c

key-decisions:
  - "func_ref resolution via value_table lookup enables CALL-site range propagation for indirect calls"
  - "Block entry range application uses replace semantics (not intersect with stale cross-path state)"
  - "detect_branch_narrowing extracted as shared helper for both collect_return_ranges and analyze_function_ranges"
  - "Conditional narrowing in collect_return_ranges enables clamp-style functions to report tight return ranges"

patterns-established:
  - "BlockRangeEntry pattern: per-block narrowed range overrides applied at block entry"
  - "AND-chain accumulation: record_block_entry_range intersects ranges for same variable at same target block"

requirements-completed: [ANAL-02, ANAL-03]

duration: 39min
completed: 2026-04-08
---

# Phase 53 Plan 01: Analysis Improvements Summary

**Interprocedural return range propagation and conditional branch narrowing for value range compression**

## Performance

- **Duration:** 39 min
- **Started:** 2026-04-08T21:02:13Z
- **Completed:** 2026-04-08T21:41:52Z
- **Tasks:** 2
- **Files modified:** 6

## Accomplishments
- CALL instruction results now use pre-computed function return ranges instead of conservative TOP, enabling field compression at call sites
- Conditional branch terminators (if x < C, if x > C, etc.) narrow variable ranges in target blocks, enabling compression in clamp-style patterns
- Both optimizations compose: a clamp function using conditionals produces tight return ranges that propagate to callers' field assignments
- Zero regressions across 272 integration tests

## Task Commits

Each task was committed atomically:

1. **Task 1: Return range collection and call-site propagation** - `8638464` (feat)
2. **Task 2: Conditional branch range narrowing** - `59771b9` (feat)

## Files Created/Modified
- `src/lir/value_range.h` - Added func_return_ranges field to ValueRangeAnalysis
- `src/lir/value_range.c` - Added collect_return_ranges pre-pass, BlockRangeEntry type, detect_branch_narrowing, narrow_from_comparison, resolve_call_name, range_intersect
- `tests/integration/value_range_return_prop.iron` - Integration test for return range propagation
- `tests/integration/value_range_return_prop.expected` - Expected output (25410)
- `tests/integration/value_range_conditional.iron` - Integration test for conditional narrowing
- `tests/integration/value_range_conditional.expected` - Expected output (489)

## Decisions Made
- Used resolve_call_name helper to handle both func_decl (direct) and func_ref (indirect via value_table) CALL instructions
- Block entry range application uses REPLACE semantics rather than intersect, because the shared value_ranges map carries state from previously-processed blocks on different control-flow paths
- Extracted detect_branch_narrowing as shared helper to avoid code duplication between collect_return_ranges and analyze_function_ranges
- Added conditional narrowing to collect_return_ranges (not just analyze_function_ranges) so clamp-style functions produce tight return ranges

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 1 - Bug] func_ref resolution missing for CALL-site range lookup**
- **Found during:** Task 1
- **Issue:** CALL instructions using func_ref + func_ptr (indirect calls to named functions) were not being resolved, causing return ranges to stay TOP even when the callee was known
- **Fix:** Added resolve_call_name helper that checks both func_decl and func_ref via value_table
- **Files modified:** src/lir/value_range.c
- **Committed in:** 8638464

**2. [Rule 1 - Bug] Block entry range apply used intersect instead of replace**
- **Found during:** Task 2
- **Issue:** apply_block_entry_ranges intersected narrowed ranges with stale cross-path state in value_ranges, causing narrowings to cancel out (e.g., [INT64_MIN,-1] intersect [0,INT64_MAX] = TOP)
- **Fix:** Changed to replace semantics -- AND-chain intersection is handled at record time in record_block_entry_range
- **Files modified:** src/lir/value_range.c
- **Committed in:** 59771b9

---

**Total deviations:** 2 auto-fixed (2 bugs)
**Impact on plan:** Both fixes essential for correct behavior. No scope creep.

## Issues Encountered
- Pre-existing emitter scoping bug: function inlining creates blocks where variable declarations appear after their use sites in linearized C output with gotos. This prevents tests from using function calls directly in constructor arguments within array literals. Worked around by using builder functions (not inlined because they contain calls) or pre-constructing objects as local vals. Logged as deferred item.

## User Setup Required

None - no external service configuration required.

## Next Phase Readiness
- Value range analysis now handles interprocedural return ranges and conditional narrowing
- Ready for Phase 53 Plan 02 (interprocedural monomorphic detection) or Phase 54 (test hardening)
- Pre-existing emitter inline scoping bug should be addressed in a future phase

---
*Phase: 53-analysis-improvements*
*Completed: 2026-04-08*
