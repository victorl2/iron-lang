---
phase: 20-hir-lowering-pipeline-cutover
plan: "05"
subsystem: testing
tags: [hir, ast-to-hir, unit-tests, lowering, iron_hir_lower]

# Dependency graph
requires:
  - phase: 20-hir-lowering-pipeline-cutover
    provides: iron_hir_lower() AST-to-HIR lowering pass (20-01 through 20-04)

provides:
  - 57 unit tests covering AST-to-HIR lowering for the full Iron feature matrix

affects:
  - Future HIR lowering changes (regression protection)
  - HIR-to-LIR pipeline (tests verify HIR structure consumed by next stage)

# Tech tracking
tech-stack:
  added: []
  patterns:
    - "Each test: build minimal AST programmatically → iron_hir_lower() → assert HIR structure"
    - "Shared setUp/tearDown with iron_arena_create/iron_types_init/iron_diaglist_create"
    - "ARENA_ALLOC + memset pattern for AST node construction in test helpers"

key-files:
  created: []
  modified:
    - tests/hir/test_hir_lower.c

key-decisions:
  - "iron_type_make_array() requires a non-null arena — use &g_ast_arena not NULL in tests"
  - "57 tests total (> 50 target): 15 smoke + 28 feature-matrix + 14 edge-case"
  - "test_hir_lower_binary_all_ops iterates over all 13 binary ops in a table-driven loop"
  - "Global constant lazy injection verified by asserting STMT_LET appears before return in function body"

patterns-established:
  - "Table-driven test pattern for exhaustive operator coverage (see test_hir_lower_binary_all_ops)"
  - "Edge-case tests use make_* helpers and build nested ASTs inline without helper functions"

requirements-completed: [HIR-05]

# Metrics
duration: 20min
completed: 2026-03-29
---

# Phase 20 Plan 05: AST-to-HIR Lowering Test Suite Summary

**57-test unit suite covering every Iron feature lowered by iron_hir_lower(): literals, operators, desugarings, memory, closures, concurrency, and edge cases**

## Performance

- **Duration:** ~20 min
- **Started:** 2026-03-29T00:00:00Z
- **Completed:** 2026-03-29T00:20:00Z
- **Tasks:** 2 (combined in single implementation pass)
- **Files modified:** 1

## Accomplishments

- Expanded test_hir_lower.c from 15 to 57 tests, exceeding the 50-test target
- Feature-matrix tests (28 new): float/bool/null literals, interpolated strings, unary NEG/NOT, all 13 binary operators, for-range-int and for-array-iter, compound-assign +=/−=, match with 3 arms, heap/rc/free/leak, call with args, method call, field access, index, Float() cast, lambda closure, spawn, parallel-for, await, defer, nested scope, func-ref
- Edge-case tests (14 new): 5-level nested if, while-inside-if, match-inside-while, 3 defers in sequence, defer+early-return, shadowed variable VarIds, global constant lazy injection, nested call f(g(x),h(y)), chained field access a.b.c, index-of-call result, empty array literal, error node no-crash (null_lit poison), nested defer scopes, large-program verify
- All 57 tests pass with 0 failures; full test suite unaffected

## Task Commits

1. **Tasks 1+2: Feature-matrix and edge-case tests** - `6995459` (feat)

**Plan metadata:** (docs commit below)

## Files Created/Modified

- `/Users/victor/code/iron-lang/tests/hir/test_hir_lower.c` - Expanded from 15 to 57 unit tests

## Decisions Made

- `iron_type_make_array()` requires a non-null arena; the test used `NULL` causing SEGV — fixed to use `&g_ast_arena`
- All new tests use `iron_hir_module_destroy(mod)` in teardown to avoid leaks under ASAN
- Task 1 and Task 2 implemented in a single implementation pass since all tests compile and run together

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 1 - Bug] iron_type_make_array called with NULL arena**
- **Found during:** Task 1 (`test_hir_lower_array_literal_empty`)
- **Issue:** `iron_type_make_array(NULL, int_ty, -1)` caused SEGV under ASAN — arena pointer is dereferenced unconditionally
- **Fix:** Changed to `iron_type_make_array(&g_ast_arena, int_ty, -1)`
- **Files modified:** tests/hir/test_hir_lower.c
- **Verification:** All 57 tests pass with 0 failures
- **Committed in:** 6995459

---

**Total deviations:** 1 auto-fixed (1 bug)
**Impact on plan:** Single-line fix required for ASAN compliance. No scope creep.

## Issues Encountered

None beyond the NULL arena bug caught by ASAN.

## Next Phase Readiness

- 57 AST-to-HIR unit tests complete; HIR lowering is well-covered
- HIR-to-LIR unit tests (planned separately) can now be added with same confidence
- Phase 20 HIR lowering pipeline is fully tested end-to-end

---
*Phase: 20-hir-lowering-pipeline-cutover*
*Completed: 2026-03-29*
