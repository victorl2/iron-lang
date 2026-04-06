---
phase: 39-diagnostic-test-sweep
plan: 01
subsystem: testing
tags: [diagnostics, coverage, unity, typecheck, lir-verify]

# Dependency graph
requires:
  - phase: 32-lir-verifier-hardening
    provides: IRON_ERR_LIR_PHI_TYPE_MISMATCH, IRON_ERR_LIR_CALL_TYPE_MISMATCH diagnostics
  - phase: 33-type-validation-checks
    provides: IRON_ERR_NONEXHAUSTIVE_MATCH, IRON_ERR_DUPLICATE_MATCH_ARM, IRON_ERR_INVALID_CAST, IRON_ERR_CAST_OVERFLOW, IRON_WARN_NARROWING_CAST, IRON_WARN_NOT_STRINGABLE, IRON_WARN_POSSIBLE_OVERFLOW diagnostics
  - phase: 34-bounds-checking
    provides: IRON_ERR_INDEX_OUT_OF_BOUNDS, IRON_ERR_INVALID_SLICE_BOUNDS diagnostics
  - phase: 36-definite-assignment-analysis
    provides: IRON_ERR_POSSIBLY_UNINITIALIZED diagnostic
  - phase: 37-generic-constraint-checking
    provides: IRON_ERR_GENERIC_CONSTRAINT diagnostic
  - phase: 38-concurrency-safety
    provides: IRON_WARN_SPAWN_DATA_RACE diagnostic
provides:
  - Complete positive+negative test coverage for all 14 diagnostic codes from phases 32-38
affects: []

# Tech tracking
tech-stack:
  added: []
  patterns: [explicit-negative-assertion per diagnostic code]

key-files:
  created: []
  modified:
    - tests/unit/test_typecheck.c
    - tests/lir/test_lir_verify.c

key-decisions:
  - "Explicit TEST_ASSERT_FALSE assertions added for all gap codes rather than relying on error_count==0 as implicit negative coverage"

patterns-established:
  - "Every diagnostic code must have both TEST_ASSERT_TRUE (positive trigger) and TEST_ASSERT_FALSE (negative non-trigger) in the test suite"

requirements-completed: [TEST-01, TEST-02]

# Metrics
duration: 3min
completed: 2026-04-03
---

# Phase 39 Plan 01: Diagnostic Test Sweep Summary

**Audited all 14 diagnostic codes from phases 32-38 and filled 4 negative-test coverage gaps with explicit TEST_ASSERT_FALSE assertions**

## Performance

- **Duration:** 3 min
- **Started:** 2026-04-04T13:07:10Z
- **Completed:** 2026-04-04T13:10:51Z
- **Tasks:** 2
- **Files modified:** 2

## Accomplishments
- Audited all 14 diagnostic codes, found 4 missing explicit negative test assertions (not just the 1 gap anticipated in the plan)
- Added test_unique_match_arms_no_duplicate_error for IRON_ERR_DUPLICATE_MATCH_ARM (309)
- Added explicit TEST_ASSERT_FALSE for IRON_ERR_NONEXHAUSTIVE_MATCH, IRON_ERR_INVALID_CAST, and IRON_ERR_LIR_PHI_TYPE_MISMATCH in existing tests
- All 18 unit tests and 5 LIR tests pass with zero failures

## Task Commits

Each task was committed atomically:

1. **Task 1: Audit coverage and document gaps** - (read-only, no commit)
2. **Task 2: Add missing negative tests** - `8c67c00` (test)

**Plan metadata:** (pending)

## Files Created/Modified
- `tests/unit/test_typecheck.c` - Added test_unique_match_arms_no_duplicate_error function, explicit FALSE assertions for NONEXHAUSTIVE_MATCH and INVALID_CAST
- `tests/lir/test_lir_verify.c` - Added explicit FALSE assertion for LIR_PHI_TYPE_MISMATCH in test_verify_phi_well_formed

## Decisions Made
- Used explicit TEST_ASSERT_FALSE(has_error(CODE)) assertions rather than relying on implicit coverage from TEST_ASSERT_EQUAL_INT(0, error_count) -- ensures grep-based audit tools can verify coverage

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 2 - Missing Critical] Added negative assertions for 3 additional diagnostic codes**
- **Found during:** Task 1 (Audit)
- **Issue:** Plan anticipated only IRON_ERR_DUPLICATE_MATCH_ARM (309) missing negative coverage, but audit found 3 more: IRON_ERR_LIR_PHI_TYPE_MISMATCH (306), IRON_ERR_NONEXHAUSTIVE_MATCH (308), IRON_ERR_INVALID_CAST (310)
- **Fix:** Added explicit TEST_ASSERT_FALSE assertions in existing tests for all 3 codes, plus the new test function for code 309
- **Files modified:** tests/unit/test_typecheck.c, tests/lir/test_lir_verify.c
- **Verification:** All tests pass, grep confirms +1/-1 or better for all 14 codes
- **Committed in:** 8c67c00 (Task 2 commit)

---

**Total deviations:** 1 auto-fixed (1 missing critical coverage)
**Impact on plan:** Essential for meeting TEST-02 requirement. No scope creep -- all changes are single-line assertions in existing test functions plus one new 18-line test function.

## Issues Encountered
None

## User Setup Required
None - no external service configuration required.

## Next Phase Readiness
- All 14 diagnostic codes now have complete positive+negative test coverage
- Ready for Plan 02 (if additional test sweep tasks exist)

---
*Phase: 39-diagnostic-test-sweep*
*Completed: 2026-04-03*
