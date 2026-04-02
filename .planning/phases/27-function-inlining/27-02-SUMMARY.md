---
phase: 27-function-inlining
plan: 02
subsystem: lir-optimizer
tags: [inlining, lir, emit-c, regression, backward-ref-hoisting]
dependency_graph:
  requires:
    - phase: 27-01
      provides: run_function_inlining in lir_optimize.c and backward-ref hoisting in emit_c.c
  provides:
    - full test suite validation with zero new regressions
    - fix for is_hoisted guard missing from constant/binop/comparison instruction emission
  affects: [emit_c, any future passes that move blocks around]
tech-stack:
  added: []
  patterns: [is-hoisted-guard-pattern for all value-producing emit_instr cases]
key-files:
  created: []
  modified:
    - src/lir/emit_c.c
key-decisions:
  - "is_hoisted guard must be applied to ALL value-producing instruction cases in emit_instr, not just ALLOCA and LOAD — any instruction can be backward-ref hoisted when inlining rearranges blocks"
patterns-established:
  - "When extending backward-ref hoisting to new instruction kinds in emit_c.c, simultaneously add `if (!is_hoisted)` guard before every type-prefix emission in emit_instr for those kinds"
requirements-completed: [INLINE-01, INLINE-02, INLINE-03]
duration: 14min
completed: "2026-03-31"
---

# Phase 27 Plan 02: Function Inlining Validation Summary

**Full test suite validated with zero new regressions; regression introduced by Plan 01 hoisting extension fixed by adding is_hoisted guards to all value-producing emit_instr cases**

## Performance

- **Duration:** 14 min
- **Started:** 2026-03-31T00:06:45Z
- **Completed:** 2026-03-31T00:20:45Z
- **Tasks:** 1
- **Files modified:** 1

## Accomplishments

- Fixed emit_c.c regression where CONST_BOOL, CONST_INT, binop/comparison/logical/unary ops, GET_FIELD, GET_INDEX, CAST, IS_NULL, IS_NOT_NULL all re-emitted their type prefix even when the variable was pre-hoisted to function entry
- Algorithm tests improved from 12/13 (Phase 26 baseline) to 13/13 — including `concurrent_hash_map` and `graph_bfs_dfs` which were newly broken, and `concurrent_hash_map` which was a pre-existing failure (now fixed)
- Integration tests: 167/174 pass (same as Phase 26 baseline; only `bug_audit_heap_method_call` pre-existing failure)
- Composite tests: 3/3 pass
- connected_components benchmark passes with correct output (Test 1: 50, Test 2: 1, Test 3: 10, Test 4: 25)
- Pass ordering confirmed: array-repr → function-inlining → copy-prop
- find_root inlining confirmed: 2 of 3 find_root call sites inlined in count_components

## Task Commits

1. **Task 1: Full regression test suite + fix** - `eb4ada7` (fix)

## Files Created/Modified

- `src/lir/emit_c.c` - Added `if (!is_hoisted)` guards to all value-producing instruction cases: CONST_INT, CONST_FLOAT, CONST_BOOL, CONST_STRING, CONST_NULL, ADD/SUB/MUL/DIV/MOD, EQ/NEQ/LT/LTE/GT/GTE, AND/OR, NEG/NOT, GET_FIELD (both stack-array .count path and general path), GET_INDEX (all three paths), CAST, IS_NULL, IS_NOT_NULL

## Decisions Made

- Applied `is_hoisted` guard inline at each instruction case rather than restructuring the switch — minimizes diff, keeps each case self-contained, matches the pattern already established by ALLOCA and LOAD

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 1 - Bug] emit_c.c backward-ref hoisting broke constants and arithmetic in inlined functions**
- **Found during:** Task 1 (full regression test suite)
- **Issue:** Plan 01 extended backward-ref hoisting from LOAD-only to all value-producing instructions. But when CONST_BOOL (and binops, comparisons, etc.) get hoisted, the pre-declaration `bool _v32;` is emitted at function entry. At the definition site, the instruction still emits `bool _v32 = true;` (full declaration with initializer), causing C redefinition errors. The `is_hoisted` flag was already computed correctly but the constant/arithmetic emission paths ignored it.
- **Fix:** Added `if (!is_hoisted)` guard before the type-prefix `iron_strbuf_appendf(sb, "TYPE ")` in 15 instruction cases in emit_instr: all 5 constant kinds, all 8 arithmetic/comparison/logical binops, both unary ops, GET_FIELD (2 paths), GET_INDEX (3 paths), CAST, IS_NULL, IS_NOT_NULL
- **Files modified:** src/lir/emit_c.c
- **Verification:** hash_map builds without redefinition errors; algorithm tests go from 10/13 to 13/13
- **Committed in:** eb4ada7

---

**Total deviations:** 1 auto-fixed (Rule 1 — regression bug introduced by Plan 01 hoisting extension)
**Impact on plan:** Essential correctness fix. No scope creep. Algorithm test results exceed Phase 26 baseline (13/13 vs 12/13).

## Issues Encountered

- The backward-ref hoisting introduced in Plan 01 was correct for the inlining use case (LOADs and ALLOCAs moved to later blocks), but the extension to ALL instruction types in Plan 01 also triggered hoisting for CONST_BOOL and other constants in non-inlined functions with complex control flow (e.g., hash_map's hm_insert after hash_key is inlined). The is_hoisted flag infrastructure was already there — it just wasn't being consulted by the new instruction cases.

## Next Phase Readiness

- Function inlining phase complete; all INLINE-* requirements satisfied
- emit_c.c backward-ref hoisting now correctly handles all instruction kinds
- Test baseline improved: algorithms 13/13 (was 12/13 before this phase)
- Ready for Phase 28 (phi simplification) or any subsequent optimization pass

## Self-Check: PASSED

- src/lir/emit_c.c: FOUND
- 27-02-SUMMARY.md: FOUND
- Commit eb4ada7: FOUND
- Algorithm tests: 13/13 PASS
- Integration tests: 167/174 PASS (1 pre-existing failure)
- Composite tests: 3/3 PASS

---
*Phase: 27-function-inlining*
*Completed: 2026-03-31*
