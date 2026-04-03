---
phase: 35-escape-analysis-extension
plan: 01
subsystem: analyzer
tags: [escape-analysis, field-access, index-access, call-args, ast-recursion]

# Dependency graph
requires:
  - phase: 34-bounds-checking
    provides: Bounds checking infrastructure in type checker
provides:
  - Recursive expr_ident_name for FIELD_ACCESS and INDEX node traversal
  - Call/method-call argument escape tracking in collect_stmt
  - 7 new escape analysis tests (18 total)
affects: [36-definite-assignment, 38-concurrency-analysis]

# Tech tracking
tech-stack:
  added: []
  patterns: [recursive-ast-walker-for-name-extraction, conservative-argument-escape]

key-files:
  created: []
  modified:
    - src/analyzer/escape.c
    - tests/unit/test_escape.c

key-decisions:
  - "Conservative argument escape: any heap binding passed to a function or method call is marked escaped (callee may store the pointer)"
  - "Recursive expr_ident_name: unified approach through FIELD_ACCESS and INDEX nodes to extract root identifier name"

patterns-established:
  - "Recursive expr_ident_name: switch on node->kind to recurse through FIELD_ACCESS.object and INDEX.object"
  - "Conservative call argument scanning: iterate args array, check each against heap bindings"

requirements-completed: [ESC-01, ESC-02, ESC-03, ESC-04]

# Metrics
duration: 15min
completed: 2026-04-03
---

# Phase 35 Plan 01: Escape Analysis Extension Summary

**Extended escape analysis with field/index assignment recursion and call/method-call argument tracking for 4 new escape paths**

## Performance

- **Duration:** 15 min
- **Started:** 2026-04-03T03:06:38Z
- **Completed:** 2026-04-03T03:21:38Z
- **Tasks:** 2
- **Files modified:** 2

## Accomplishments
- Extended expr_ident_name to recursively extract root identifiers from FIELD_ACCESS and INDEX AST nodes
- Added IRON_NODE_CALL and IRON_NODE_METHOD_CALL handlers to collect_stmt for argument escape tracking
- 7 new tests covering field assign, index assign, chained field access, call args, method call args, and false-positive guards
- All 18 escape tests pass, full unit test suite green (17/17 suites, 0 failures)

## Task Commits

Each task was committed atomically:

1. **Task 1: Extend expr_ident_name and field/index escape tests** - `edaa458` (test) + `5f56ae6` (feat)
2. **Task 2: Add call/method-call argument escape tracking** - `5fc856f` (test) + `66f1fe3` (feat)

_Note: TDD tasks have separate test and implementation commits_

## Files Created/Modified
- `src/analyzer/escape.c` - Extended expr_ident_name with FIELD_ACCESS/INDEX recursion; added CALL/METHOD_CALL argument scanning in collect_stmt
- `tests/unit/test_escape.c` - 7 new tests (Tests 12-18): field assign, index assign, chained field, non-heap field false positive, call arg, method call arg, non-heap call false positive

## Decisions Made
- Conservative argument escape: any heap binding passed as a function or method call argument is marked as escaped, since the callee may store the pointer
- Recursive expr_ident_name: unified approach handles FIELD_ACCESS.object and INDEX.object recursion to find root identifiers
- Field/index assignment tests pass without implementation change because existing code already checks bare-ident RHS; the expr_ident_name extension adds robustness for complex RHS patterns (e.g., obj.d where d is heap)

## Deviations from Plan

None - plan executed exactly as written.

## Issues Encountered
- Tests 12-15 (field/index assign) passed immediately without the expr_ident_name extension because the existing assignment handler already catches bare-ident RHS values. The implementation was still applied for correctness when RHS contains field/index expressions. This is expected TDD behavior when existing code covers the test scenarios.

## User Setup Required

None - no external service configuration required.

## Next Phase Readiness
- Escape analysis now handles all 4 escape paths: return, assignment, field/index store, and function/method call arguments
- Ready for Phase 36 (definite assignment) which may depend on escape tracking
- Ready for Phase 38 (concurrency analysis) which requires escape analysis as a prerequisite

## Self-Check: PASSED

All files exist, all 4 commits verified.

---
*Phase: 35-escape-analysis-extension*
*Completed: 2026-04-03*
