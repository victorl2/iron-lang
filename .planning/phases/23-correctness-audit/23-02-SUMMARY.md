---
phase: 23-correctness-audit
plan: 02
subsystem: testing
tags: [integration-tests, pfor, struct, defer, for-loop, correctness, regression]

# Dependency graph
requires:
  - phase: 23-01
    provides: "audit checklist identifying 8 coverage gap candidates; two bug fixes for heap var alloca and heap object method calls"

provides:
  - "6 new integration test pairs covering pfor variable range, pfor boundary range, struct chain passing, struct method mutation, for-in on struct array, defer in for body"
  - "Bug fix: CONST_NULL with IRON_TYPE_OBJECT emits zero-initialized struct instead of void* in emit_c.c"
  - "Bug fix: range-for desugaring wraps user body in BLOCK stmt so defer fires at iteration scope exit (before increment)"

affects: [future-refactors, hir_lower, emit_c, ssa-correctness]

# Tech tracking
tech-stack:
  added: []
  patterns:
    - "Correctness regression test naming: audit_<feature>.iron in tests/integration/"
    - "CONST_NULL object type: emit {0} zero-init for struct PHI predecessors"
    - "Range-for defer scope: user body in inner BLOCK stmt, increment in outer body (fires after defers)"

key-files:
  created:
    - tests/integration/audit_pfor_variable_range.iron
    - tests/integration/audit_pfor_variable_range.expected
    - tests/integration/audit_struct_chain_passing.iron
    - tests/integration/audit_struct_chain_passing.expected
    - tests/integration/audit_struct_method_mutation.iron
    - tests/integration/audit_struct_method_mutation.expected
    - tests/integration/audit_for_array_of_structs.iron
    - tests/integration/audit_for_array_of_structs.expected
    - tests/integration/audit_defer_in_for_body.iron
    - tests/integration/audit_defer_in_for_body.expected
    - tests/integration/audit_pfor_boundary_range.iron
    - tests/integration/audit_pfor_boundary_range.expected
  modified:
    - src/lir/emit_c.c
    - src/hir/hir_lower.c

key-decisions:
  - "CONST_NULL with object type: zero-init struct ({0}) not void* NULL — fixes PHI predecessor type mismatch for struct for-vars after SSA conversion"
  - "Range-for defer ordering: wrap user body in IRON_HIR_STMT_BLOCK so its defer scope exits before the compiler-inserted increment stmt runs"

patterns-established:
  - "For-in array-of-structs tests: GET_INDEX on Iron_List returns struct value after CONST_NULL fix"
  - "Defer in for-range body: defer sees pre-increment loop variable value (correct semantics)"

requirements-completed: [AUDIT-03]

# Metrics
duration: 11min
completed: 2026-03-31
---

# Phase 23 Plan 02: Correctness Regression Tests Summary

**6 integration test pairs targeting coverage gaps in pfor variable range, struct value chains, struct methods, for-in on struct arrays, defer-in-for-body, and pfor boundary ranges — plus two compiler bug fixes discovered during test creation**

## Performance

- **Duration:** 11 min
- **Started:** 2026-03-31T17:13:27Z
- **Completed:** 2026-03-31T17:24:00Z
- **Tasks:** 1
- **Files modified:** 14 (12 new test files, 2 compiler source files)

## Accomplishments

- Created 6 new correctness-focused integration test pairs covering the highest-value coverage gaps identified in Phase 23 research
- Discovered and fixed: `CONST_NULL` with `IRON_TYPE_OBJECT` type was emitting `void* = NULL` instead of `TypeName = {0}`, causing a C type error when for-in loops over struct arrays use SSA PHI predecessors
- Discovered and fixed: range-for desugaring appended the increment stmt to the user body block, so deferred expressions saw the already-incremented loop variable; fixed by wrapping user body in `IRON_HIR_STMT_BLOCK` so its defer scope exits before the increment runs
- Full test suite passes: 165 integration + 13 algorithms + 3 composite = 181 total, 0 failures

## Task Commits

1. **Task 1: Create 6 correctness regression tests** - `e3a4afc` (feat)

## Files Created/Modified

- `tests/integration/audit_pfor_variable_range.iron` — pfor with variable (not literal) range expression
- `tests/integration/audit_pfor_variable_range.expected` — expected output: "pfor variable range done / 20"
- `tests/integration/audit_struct_chain_passing.iron` — struct passed by value through 4-function chain (make_vec -> scale -> add -> dot)
- `tests/integration/audit_struct_chain_passing.expected` — expected output including "chain: 675"
- `tests/integration/audit_struct_method_mutation.iron` — Counter struct with increment/display methods, value-type self
- `tests/integration/audit_struct_method_mutation.expected` — expected output including "alpha: 8"
- `tests/integration/audit_for_array_of_structs.iron` — for-in loop over [Item] array with field access and accumulation
- `tests/integration/audit_for_array_of_structs.expected` — expected output including "total: 10"
- `tests/integration/audit_defer_in_for_body.iron` — defer inside for-range body, verifies per-iteration scope
- `tests/integration/audit_defer_in_for_body.expected` — expected output: start/defer pairs in order 0,1,2
- `tests/integration/audit_pfor_boundary_range.iron` — pfor with ranges 1, 2, 3 to test boundary emission
- `tests/integration/audit_pfor_boundary_range.expected` — expected output: "range N done" for N=1,2,3
- `src/lir/emit_c.c` — Fix CONST_NULL emission for IRON_TYPE_OBJECT: emit `TypeName _vN = {0};` not `void* _vN = NULL;`
- `src/hir/hir_lower.c` — Fix range-for desugaring: wrap user body in BLOCK stmt so defer scope exits before increment

## Decisions Made

- `CONST_NULL` with object type emits zero-initialized struct: The SSA conversion creates `const_null` instructions as PHI predecessor initializers. When the phi type is `IRON_TYPE_OBJECT`, the old `void*` C type caused a type mismatch at the assignment site. Zero-initializing the struct (`{0}`) is correct since it is never actually read before the first real assignment.
- Range-for defer ordering fixed via BLOCK wrapper: The simplest correct fix was wrapping the user-visible for-loop body in `IRON_HIR_STMT_BLOCK` (which has its own defer scope in hir_to_lir.c), while keeping the compiler-inserted increment in the outer body. This ensures defer fires at the iteration scope boundary (after user stmts, before the counter increment), matching expected language semantics.

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 1 - Bug] CONST_NULL object type emits void* causing C type mismatch**
- **Found during:** Task 1 (audit_for_array_of_structs compilation)
- **Issue:** `for item in items` where items is `[Item]` (array of structs) failed to compile: `assigning to 'Iron_Item' from incompatible type 'void *'`. The SSA conversion creates a `const_null : <object>` instruction as the initial PHI predecessor for the loop variable; `emit_c.c` always emitted `void*` for `CONST_NULL` regardless of the instruction's type.
- **Fix:** In `emit_c.c` `IRON_LIR_CONST_NULL` `emit_instr` case: when `instr->type->kind == IRON_TYPE_OBJECT`, emit `TypeName _vN = {0};` instead of `void* _vN = NULL;`
- **Files modified:** `src/lir/emit_c.c`
- **Verification:** `audit_for_array_of_structs` compiles and produces correct output; all 165 integration tests pass
- **Committed in:** `e3a4afc` (Task 1 commit)

**2. [Rule 1 - Bug] Defer in range-for body saw post-increment loop variable**
- **Found during:** Task 1 (audit_defer_in_for_body output verification)
- **Issue:** `defer println("defer {i}")` inside `for i in 3 { }` printed `defer 1`, `defer 2`, `defer 3` instead of `defer 0`, `defer 1`, `defer 2`. The range-for desugaring appended the increment stmt (`i = i + 1`) directly to the HIR body block after the user stmts. The WHILE lowering in hir_to_lir.c emits all body stmts in order, so the increment runs (and is stored to the alloca) before `emit_scope_defers` emits the deferred expression, which reads the incremented value.
- **Fix:** In `hir_lower.c` range-for desugaring: wrap the user body stmts in `IRON_HIR_STMT_BLOCK` (creates its own defer scope in hir_to_lir.c); append the increment to the outer `body_blk` after the block stmt. The block's defer scope now exits before the increment runs.
- **Files modified:** `src/hir/hir_lower.c`
- **Verification:** `audit_defer_in_for_body` produces `start 0 / defer 0 / start 1 / defer 1 / start 2 / defer 2 / done`; all 165 integration tests pass (including existing while/for/defer tests)
- **Committed in:** `e3a4afc` (Task 1 commit)

---

**Total deviations:** 2 auto-fixed (2x Rule 1 - Bug)
**Impact on plan:** Both fixes were required to make the new tests pass. Neither introduces scope creep — both fix real compiler correctness bugs in previously-untested codegen paths.

## Issues Encountered

- `defer { block }` syntax is not valid Iron — only `defer expr` is supported. The test was updated to use `defer println("defer {i}")` directly. The plan's example used block syntax which is not in the language grammar.

## Next Phase Readiness

- Phase 23 correctness audit complete: all HIR instruction paths checked, bugs fixed, regression tests in place
- 181 tests (165 integration + 13 algorithm + 3 composite) pass with zero failures
- The two newly discovered bugs (CONST_NULL object type, range-for defer ordering) are fixed and guarded by tests

---
*Phase: 23-correctness-audit*
*Completed: 2026-03-31*

## Self-Check: PASSED

- All 12 test files (6 .iron + 6 .expected) exist in tests/integration/
- src/lir/emit_c.c and src/hir/hir_lower.c exist with fixes applied
- Task commit e3a4afc exists in git log
- `find tests/integration -name "audit_*.iron" | wc -l` returns 6
- Full test suite: 165 integration + 13 algorithms + 3 composite = 0 failures
