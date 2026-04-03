---
phase: 38-concurrency-safety
plan: 01
subsystem: analyzer
tags: [concurrency, parallel-for, mutation-detection, field-access, index-expression, data-race]

# Dependency graph
requires:
  - phase: 35-escape-analysis-extension
    provides: expr_ident_name pattern for recursive field/index name extraction
provides:
  - Extended parallel-for mutation detection covering field access and array index assignment targets
  - expr_ident_name helper in concurrency.c for root variable extraction
affects: [38-02-spawn-capture, 39-diagnostic-test-sweep]

# Tech tracking
tech-stack:
  added: []
  patterns: [recursive AST traversal for root identifier extraction, shared with escape.c]

key-files:
  created: []
  modified: [src/analyzer/concurrency.c, tests/unit/test_concurrency.c]

key-decisions:
  - "Reused expr_ident_name pattern from escape.c as static helper in concurrency.c (same logic, independent copy for encapsulation)"
  - "Conservative skip for non-identifier-rooted targets (expr_ident_name returns NULL) -- don't flag what we can't analyze"

patterns-established:
  - "expr_ident_name: canonical pattern for extracting root identifier from field/index chains, now used in both escape.c and concurrency.c"

requirements-completed: [CONC-01, CONC-02]

# Metrics
duration: 2min
completed: 2026-04-03
---

# Phase 38 Plan 01: Field and Index Mutation Detection Summary

**Extended parallel-for mutation detection to cover obj.field and arr[i] assignment targets via recursive root-identifier extraction**

## Performance

- **Duration:** 2 min
- **Started:** 2026-04-03T11:19:47Z
- **Completed:** 2026-04-03T11:21:54Z
- **Tasks:** 1 (TDD: RED + GREEN)
- **Files modified:** 2

## Accomplishments
- E0208 now fires for `obj.field = val` and `arr[i] = val` in parallel-for bodies when obj/arr are outer-scope variables
- Local variable field/index mutations correctly not flagged (zero false positives)
- All 9 concurrency tests pass (5 existing + 4 new), zero regressions across full 18-test unit suite

## Task Commits

Each task was committed atomically:

1. **Task 1 RED: Failing tests for field/index mutation** - `eb9f9c7` (test)
2. **Task 1 GREEN: Implement field/index mutation detection** - `4218f9d` (feat)

## Files Created/Modified
- `src/analyzer/concurrency.c` - Added expr_ident_name helper, extended check_stmt_for_mutation to handle IRON_NODE_FIELD_ACCESS and IRON_NODE_INDEX targets
- `tests/unit/test_concurrency.c` - Added make_field_access/make_index_expr builders and 4 new test functions

## Decisions Made
- Reused expr_ident_name pattern from escape.c as an independent static helper in concurrency.c -- same recursive logic for IDENT/FIELD_ACCESS/INDEX traversal, keeps each analyzer self-contained
- Conservative approach: if expr_ident_name returns NULL (non-identifier-rooted target), the assignment is silently skipped rather than flagged

## Deviations from Plan

None - plan executed exactly as written.

## Issues Encountered
None

## User Setup Required
None - no external service configuration required.

## Next Phase Readiness
- Concurrency checker now handles field and index mutation patterns
- Ready for 38-02: spawn block capture analysis and read-write race detection
- The expr_ident_name helper can be reused for spawn capture analysis

---
*Phase: 38-concurrency-safety*
*Completed: 2026-04-03*
