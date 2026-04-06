---
phase: 34-bounds-checking
plan: 01
subsystem: analyzer
tags: [bounds-checking, array, type-checker, diagnostics, tdd]

# Dependency graph
requires:
  - phase: 33-type-validation-checks
    provides: type checker infrastructure, emit_error/emit_warning helpers, -Werror compliance patterns
provides:
  - IRON_ERR_INDEX_OUT_OF_BOUNDS (312) error code for constant out-of-bounds array indices
  - IRON_ERR_INVALID_SLICE_BOUNDS (313) error code (staged for Plan 02)
  - try_get_constant_int helper for extracting compile-time integer constants from AST nodes
  - Bounds validation in IRON_NODE_INDEX case of check_expr
  - Non-integer index type validation
affects: [34-02 slice bounds checking, 39-test-sweep]

# Tech tracking
tech-stack:
  added: []
  patterns: [constant-extraction from AST nodes for compile-time validation, bounds checking against known array size]

key-files:
  created: []
  modified:
    - src/diagnostics/diagnostics.h
    - src/analyzer/typecheck.c
    - tests/unit/test_typecheck.c

key-decisions:
  - "Used IRON_TOK_MINUS (not IRON_OP_NEG) for unary negation detection -- OpKind stores TokenKind values"
  - "Added __attribute__((unused)) to try_get_constant_int in Task 1, removed in Task 2 when actively called"

patterns-established:
  - "Constant extraction: try_get_constant_int handles INT_LIT and UNARY(-, INT_LIT) for compile-time index validation"
  - "Bounds checking: validate constant indices against array.size when size >= 0, silently skip dynamic arrays and non-constant indices"

requirements-completed: [BOUNDS-01, BOUNDS-02, BOUNDS-03]

# Metrics
duration: 5min
completed: 2026-04-03
---

# Phase 34 Plan 01: Array Index Bounds Checking Summary

**Compile-time constant array index bounds checking with try_get_constant_int helper and TDD tests for out-of-bounds, negative, non-integer, and valid index cases**

## Performance

- **Duration:** 5 min
- **Started:** 2026-04-03T02:12:10Z
- **Completed:** 2026-04-03T02:17:10Z
- **Tasks:** 2
- **Files modified:** 3

## Accomplishments
- Defined IRON_ERR_INDEX_OUT_OF_BOUNDS (312) and IRON_ERR_INVALID_SLICE_BOUNDS (313) error codes
- Implemented try_get_constant_int helper that extracts compile-time constants from INT_LIT and UNARY(-,INT_LIT) nodes
- Added bounds validation to IRON_NODE_INDEX case: constant out-of-bounds indices produce diagnostics, non-integer indices produce type errors
- 6 new TDD tests (54 total) all passing: out-of-bounds high, valid last, negative, non-integer type, valid zero, non-constant skip

## Task Commits

Each task was committed atomically:

1. **Task 1: Add error codes and constant-extraction helper** - `74c1855` (feat)
2. **Task 2 RED: Failing bounds-checking tests** - `cbb25fe` (test)
3. **Task 2 GREEN: Implement bounds checking** - `d779dee` (feat)

_Note: Task 2 followed TDD with separate RED and GREEN commits_

## Files Created/Modified
- `src/diagnostics/diagnostics.h` - Added IRON_ERR_INDEX_OUT_OF_BOUNDS (312) and IRON_ERR_INVALID_SLICE_BOUNDS (313)
- `src/analyzer/typecheck.c` - Added try_get_constant_int helper and bounds validation in IRON_NODE_INDEX case
- `tests/unit/test_typecheck.c` - Added 6 array index bounds tests (tests 49-54)

## Decisions Made
- Used IRON_TOK_MINUS for unary negation detection since Iron_OpKind stores Iron_TokenKind values (no separate IRON_OP_NEG exists)
- Temporarily marked try_get_constant_int with __attribute__((unused)) in Task 1 while it had no callers, removed in Task 2

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 1 - Bug] Fixed IRON_OP_NEG to IRON_TOK_MINUS**
- **Found during:** Task 1 (constant-extraction helper)
- **Issue:** Plan specified IRON_OP_NEG which does not exist; Iron uses IRON_TOK_MINUS for the negation operator
- **Fix:** Changed ue->op comparison from IRON_OP_NEG to IRON_TOK_MINUS
- **Files modified:** src/analyzer/typecheck.c
- **Verification:** Build succeeds with zero warnings
- **Committed in:** 74c1855 (Task 1 commit)

---

**Total deviations:** 1 auto-fixed (1 bug)
**Impact on plan:** Minor naming correction, no scope change.

## Issues Encountered
None beyond the IRON_OP_NEG naming fix documented above.

## User Setup Required
None - no external service configuration required.

## Next Phase Readiness
- Error codes and constant-extraction helper ready for Plan 02 (slice bounds checking)
- IRON_ERR_INVALID_SLICE_BOUNDS (313) defined and available for use
- try_get_constant_int can be reused for slice start/end validation
- All 54 typecheck tests green, no regressions

---
*Phase: 34-bounds-checking*
*Completed: 2026-04-03*
