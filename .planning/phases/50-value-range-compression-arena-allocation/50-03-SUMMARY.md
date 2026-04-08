---
phase: 50-value-range-compression-arena-allocation
plan: 03
subsystem: testing
tags: [integration-tests, value-range-compression, arena-allocation, split-collection, regression]

# Dependency graph
requires:
  - phase: 50-value-range-compression-arena-allocation
    provides: Value range analysis (50-01), arena-tracked allocation (50-02)
provides:
  - End-to-end integration tests for all Phase 50 requirements (VRC-01, VRC-02, ARENA-01, ARENA-02)
  - Regression guard for value range compression and arena allocation
affects: [future phases modifying split collection codegen or value range analysis]

# Tech tracking
tech-stack:
  added: []
  patterns:
    - "VRC integration test pattern: dead field triggers Stor struct, remaining fields compressed to uint8_t, arithmetic verified through widening reads"
    - "Arena integration test pattern: >8 elements triggers geometric growth, 3+ types exercises per-type sub-arrays, bulk free lifecycle"

key-files:
  created:
    - tests/integration/value_range_compress.iron
    - tests/integration/value_range_compress.expected
    - tests/integration/arena_split_collection.iron
    - tests/integration/arena_split_collection.expected
  modified: []

key-decisions:
  - "Added dead field (debug_id) to VRC test objects to trigger reduced storage (Stor struct), which is required for value range compression to apply"
  - "Used separate method definitions (func X.method()) matching existing test patterns, not inline method declarations"

patterns-established:
  - "Phase 50 integration test: VRC test requires dead field + small-range fields to exercise both dead field elimination and value range compression together"
  - "Arena test: 14 elements across 3 types ensures growth past initial capacity of 8; small 2-element collection verifies within-capacity path"

requirements-completed: [VRC-01, VRC-02, ARENA-01, ARENA-02]

# Metrics
duration: 8min
completed: 2026-04-08
---

# Phase 50 Plan 03: Integration Tests for Value Range Compression and Arena Allocation Summary

**End-to-end integration tests proving uint8_t field compression with correct widening/narrowing and arena-tracked split collection lifecycle with geometric growth**

## Performance

- **Duration:** 8 min
- **Started:** 2026-04-08T11:31:44Z
- **Completed:** 2026-04-08T11:40:15Z
- **Tasks:** 2
- **Files modified:** 4

## Accomplishments
- Created VRC integration test exercising field compression (kind/x/y/r/g all narrowed to uint8_t in Stor structs) with correct runtime arithmetic through widening reads
- Created arena allocation test with 14 elements across 3 types triggering geometric growth, plus small 2-element collection for within-capacity path
- Full integration test suite passes with no regressions (259s, all tests green)
- All 4 Phase 50 requirements (VRC-01, VRC-02, ARENA-01, ARENA-02) have integration test coverage

## Task Commits

Each task was committed atomically:

1. **Task 1: Create value range compression integration test** - `bc78dd1` (test)
2. **Task 2: Create arena allocation integration test and run full regression suite** - `2832677` (test)

## Files Created/Modified
- `tests/integration/value_range_compress.iron` - VRC test: Dot/Pixel with dead debug_id field, small-range kind/x/y/r/g fields compressed to uint8_t
- `tests/integration/value_range_compress.expected` - Expected: 25410, 6099
- `tests/integration/arena_split_collection.iron` - Arena test: Dog/Cat/Bird with 14 elements triggering growth, plus 2-element small collection
- `tests/integration/arena_split_collection.expected` - Expected: 1195, 14, 35

## Decisions Made
- Added dead field (debug_id) to VRC test objects: value range compression only applies to Stor structs which are emitted when dead field elimination creates reduced storage. Without a dead field, all fields are stored in full Iron_Dot/Iron_Pixel structs and compression has no effect.
- Used existing test syntax patterns (object X impl Interface, separate func X.method(), val bindings) rather than the plan's proposed syntax (inline methods, let bindings) to match the codebase conventions.

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 1 - Bug] Fixed VRC test to trigger actual compression**
- **Found during:** Task 1 (VRC integration test)
- **Issue:** Plan's proposed test had no dead fields, so Stor structs were never emitted and value range compression never applied. All fields stored in full structs at int64_t width.
- **Fix:** Added debug_id field to Dot and Pixel objects (not accessed through collection), triggering reduced storage (Stor struct) emission where VRC compression applies.
- **Files modified:** tests/integration/value_range_compress.iron
- **Verification:** --report-compression now shows all 6 fields compressed to uint8_t; runtime output matches expected values

---

**Total deviations:** 1 auto-fixed (Rule 1 bug in plan specification)
**Impact on plan:** Essential fix -- without the dead field, VRC-01 and VRC-02 were not actually being exercised. No scope creep.

## Issues Encountered
None beyond the deviation noted above.

## User Setup Required
None - no external service configuration required.

## Next Phase Readiness
- Phase 50 is complete: value range analysis, arena allocation, and integration tests all shipped
- All 4 requirements verified with passing integration tests
- Full regression suite confirmed green

## Self-Check: PASSED

- All 4 test files exist (value_range_compress.iron/.expected, arena_split_collection.iron/.expected)
- Both commits verified (bc78dd1, 2832677)
- VRC test: --report-compression shows 6 fields compressed to uint8_t
- Arena test: 7 _iron_sl_realloc_tracked calls and _iron_sl_free_all in generated C
- Full integration suite passes (259s, 0 failures)

---
*Phase: 50-value-range-compression-arena-allocation*
*Completed: 2026-04-08*
