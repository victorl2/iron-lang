---
phase: 22-struct-codegen-fix
plan: 02
subsystem: HIR/LIR codegen, testing
tags: [struct, codegen, hir-to-lir, regression-test, constructor-detection]

requires:
  - phase: 22-01
    provides: Removed spurious CONSTRUCT special case and added field_count clamp in emit_c.c

provides:
  - STRUCT-03: game_loop_headless passes with correct struct value passing end-to-end
  - ALG-01: quicksort produces correctly sorted output
  - ALG-02: hash_map runs without crash and produces correct output
  - ALG-03: all 13 algorithm tests pass
  - Regression test for function-returning-struct pattern (bug_struct_return_func)

affects: [future-struct-tests, algorithm-tests, composite-tests]

tech-stack:
  added: []
  patterns: [check callee type not call result type for constructor detection]

key-files:
  created:
    - tests/integration/bug_struct_return_func.iron
    - tests/integration/bug_struct_return_func.expected
  modified:
    - src/hir/hir_to_lir.c

key-decisions:
  - "Constructor detection must check callee FUNC_REF's own type (IRON_TYPE_OBJECT means it IS a type name) — not the call result type which matches both constructors and regular functions returning structs"
  - "Plan 01 removal of the constructor detection block was too aggressive — the block was conceptually correct but checked the wrong condition; the fix is refinement not removal"

patterns-established:
  - "In hir_to_lir.c IRON_HIR_EXPR_CALL: FUNC_REF callee->type == IRON_TYPE_OBJECT means type constructor; callee->type == IRON_TYPE_FUNC means regular function"

requirements-completed: [STRUCT-03, ALG-01, ALG-02, ALG-03]

duration: 12min
completed: 2026-03-31
---

# Phase 22 Plan 02: Struct Codegen Verification Summary

**Corrected constructor detection in hir_to_lir.c by checking callee type (not call result type), enabling all struct-dependent tests to pass: 13/13 algorithms, 157/157 integration, 3/3 composite.**

## Performance

- **Duration:** ~12 min
- **Started:** 2026-03-31T15:44:58Z
- **Completed:** 2026-03-31T15:57:00Z
- **Tasks:** 2
- **Files modified:** 3 (src/hir/hir_to_lir.c, tests/integration/bug_struct_return_func.iron, tests/integration/bug_struct_return_func.expected)

## Accomplishments

- Discovered and fixed the over-broad Plan 01 removal: constructor detection must check the callee's own type, not the call result type
- Created targeted regression test for function-returning-struct pattern (4 cases: simple return, struct-to-struct, field extraction, nested call)
- All 13 algorithm tests pass including quicksort, hash_map, concurrent_hash_map, graph_bfs_dfs
- All 157 integration tests pass including the new bug_struct_return_func
- All 3 composite tests pass including game_loop_headless with correct struct value passing

## Task Commits

1. **Deviation fix: corrected constructor detection** - `f75a024` (fix)
2. **Task 1: regression test files** - `00f9c76` (feat)

**Plan metadata:** (docs commit follows)

## Files Created/Modified

- `src/hir/hir_to_lir.c` - Refined constructor detection: check callee FUNC_REF's own type for IRON_TYPE_OBJECT rather than the call result type
- `tests/integration/bug_struct_return_func.iron` - Regression test exercising 4 function-returning-struct cases
- `tests/integration/bug_struct_return_func.expected` - Expected output: p1 (10,20), p2 (15,17), rect (15,17,100,50), p3 (4,6)

## Decisions Made

- The Plan 01 fix (complete removal of the constructor detection block) was incorrect — the block was needed but checked the wrong condition. The `type` variable in the condition referred to the call's *return* type (IRON_TYPE_OBJECT for any struct-returning call), when it should check the callee's own type (IRON_TYPE_OBJECT means the identifier IS a type name, not a function).
- Refinement: `expr->call.callee->type->kind == IRON_TYPE_OBJECT` correctly identifies type constructors (`Point`, `GameState`, etc.) while `IRON_TYPE_FUNC` identifies regular user functions (`make_point`, `offset_point`, etc.).

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 1 - Bug] Plan 01 constructor detection removal was too aggressive**

- **Found during:** Task 1 (creating regression test and trying to compile it)
- **Issue:** After Plan 01 removed the block at hir_to_lir.c:714-730, constructors like `Point(x, y)` inside function bodies were no longer emitted as LIR CONSTRUCT instructions. Instead, they fell through to CALL lowering and generated invalid C code: `Iron_Point(_v1, _v2)` which uses the type name as a function (not valid C syntax). The root cause was that the original block checked the CALL's result type (`type->kind == IRON_TYPE_OBJECT`) which matched both constructors AND regular functions returning structs — that's why Plan 01 removed it. But the correct fix is to check the callee's own type instead.
- **Fix:** Added back the constructor detection block in `IRON_HIR_EXPR_CALL`, but changed the condition from checking `type->kind == IRON_TYPE_OBJECT` (call result type) to `expr->call.callee->type->kind == IRON_TYPE_OBJECT` (callee's own type). When an identifier like `Point` is lowered as FUNC_REF, its type is `IRON_TYPE_OBJECT` (because `Point` IS an object type). When `make_point` is lowered as FUNC_REF, its type is `IRON_TYPE_FUNC`. This distinction is correct and precise.
- **Files modified:** `src/hir/hir_to_lir.c`
- **Verification:** `./build/ironc build tests/integration/bug_struct_return_func.iron -o /tmp/test_struct_return && /tmp/test_struct_return` produces correct output; all 13 algorithm tests pass; all composite tests pass.
- **Committed in:** f75a024 (separate fix commit before Task 1 test commit)

---

**Total deviations:** 1 auto-fixed (Rule 1 — bug in Plan 01 fix logic)
**Impact on plan:** Critical fix — without it no struct constructors would work. The refinement is surgical (1-line condition change) with zero scope creep.

## Issues Encountered

The Plan 01 "fix" introduced a regression by removing the only path that lowers type-constructor calls (`Point(x, y)`, `GameState(...)`) to LIR CONSTRUCT instructions. The diagnostic was clear from `--verbose` output showing `Iron_Point(_v1, _v2)` in generated C which is invalid syntax. The root cause (checking call result type vs callee type) was identifiable by examining how FUNC_REF nodes are typed in hir_lower.c.

## Next Phase Readiness

- Phase 22 is complete: both struct bugs fixed, all affected tests passing
- Struct codegen is correct for the Iron feature set tested
- Requirements STRUCT-01, STRUCT-02, STRUCT-03, ALG-01, ALG-02, ALG-03 all satisfied
- Ready for Phase 23 (next phase in roadmap)

---
*Phase: 22-struct-codegen-fix*
*Completed: 2026-03-31*
