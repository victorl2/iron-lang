---
phase: 17-strength-reduction-store-load-elimination
plan: "02"
subsystem: ir-optimize
tags: [optimization, strength-reduction, dominator-tree, loop-analysis, induction-variable]
dependency_graph:
  requires: [17-01]
  provides: [IROPT-05, build_domtree, build_loop_info, run_strength_reduction]
  affects: [ir-optimize, emit-c, benchmarks, v005-ir-optimization-spec]
tech-stack:
  added: []
  patterns:
    - "iterative dominator tree (Cooper-Harvey-Kennedy 2001)"
    - "natural loop detection via back-edge dominance"
    - "strength reduction: MUL(indvar, invariant) -> LOAD of new IV alloca"
    - "step value hoisting to preheader via step_alloca for C-scope safety"
key-files:
  created: []
  modified:
    - src/ir/ir_optimize.h
    - src/ir/ir_optimize.c
    - tests/ir/test_ir_optimize.c
decisions:
  - "rebuild_cfg_edges() must be called before domtree: IR constructors do not auto-populate preds/succs arrays"
  - "inv_val hoisting: loop-body-defined invariants must be stored to step_alloca in preheader to avoid C scope errors in latch block"
  - "MUL rewritten in-place (reuse value ID): load.ptr overlaps binop.left in union — set kind before load.ptr, never set binop.left after"
  - "CONST_INT constants in loop body hoisted by creating fresh CONST_INT in preheader block"
  - "Non-CONST_INT body-defined invariants: transformation skipped (conservatively safe)"
metrics:
  duration: "32 minutes"
  completed: "2026-03-29T23:13:39Z"
  tasks: 1
  files_modified: 3
requirements: [IROPT-05]
---

# Phase 17 Plan 02: Strength Reduction Pass Summary

Dominator tree computation, natural loop detection, and strength reduction pass that replaces `MUL(loop_indvar, loop_invariant)` patterns in loop bodies with induction variables using addition.

## What Was Built

**Dominator tree infrastructure (`build_domtree`):** Iterative Cooper-Harvey-Kennedy algorithm computing immediate dominators for all blocks. Pre-requisite: `rebuild_cfg_edges()` must be called first since the IR constructors never auto-populate `preds/succs` arrays.

**Natural loop detection (`build_loop_info`):** Identifies back edges (latch→header where header dominates latch), collects body blocks via backwards worklist from latch, detects preheader (unique non-loop predecessor of header with header as only successor), and identifies the induction variable (ALLOCA with `ADD(LOAD(alloca), CONST_INT)` store pattern in latch).

**Strength reduction pass (`run_strength_reduction`):** For each loop with a valid preheader and induction variable, scans body blocks for `MUL(LOAD(indvar_alloca), invariant)`. When found:
- Creates new IV alloca + step alloca in entry block
- Hoists step value to preheader via step_alloca store (required for C-scope safety)
- Inserts `init_mul = indvar_init * step` and `STORE iv_alloca, init_mul` in preheader
- Inserts `load_iv → load_step → stepped = ADD(load_iv, load_step) → STORE iv_alloca, stepped` in latch
- Rewrites the MUL instruction in-place to `LOAD iv_alloca` (preserves value ID so all uses remain valid)

**Three unit tests (19-21):** basic loop MUL elimination, no-loop safety, non-invariant operand rejection.

## TDD Execution

- **RED:** Tests 19-21 written and committed (test 19 failing, 20-21 trivially passing)
- **GREEN:** Implementation written; tests pass after fixing two bugs

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 1 - Bug] CFG edges not populated by IR constructors**
- **Found during:** Task 1, GREEN phase
- **Issue:** `preds` and `succs` arrays on `IronIR_Block` are always NULL — the IR constructors (`iron_ir_jump`, `iron_ir_branch`, etc.) never populate them. All existing passes work around this; the new domtree algorithm requires them.
- **Fix:** Added `rebuild_cfg_edges()` that scans all terminator instructions (JUMP/BRANCH/SWITCH) and populates preds/succs before each domtree build.
- **Files modified:** `src/ir/ir_optimize.c`
- **Commit:** 94f8043

**2. [Rule 1 - Bug] Union alias: binop.left overlaps load.ptr**
- **Found during:** Task 1, GREEN debugging
- **Issue:** `instr->binop.left` and `instr->load.ptr` map to the same memory (union). Original code set `instr->kind = IRON_IR_LOAD; instr->load.ptr = X; instr->binop.left = INVALID;` — the last line overwrote `load.ptr`.
- **Fix:** Removed the `binop.left = INVALID` line; only clear `binop.right` (bytes 4-7, safe).
- **Files modified:** `src/ir/ir_optimize.c`
- **Commit:** 94f8043

**3. [Rule 1 - Bug] Cross-block scope: inv_val from loop body used in latch**
- **Found during:** Task 1, integration test `heap_auto_free` compilation error `use of undeclared identifier '_v35'`
- **Issue:** The strength reduction inserted `ADD(load_iv, inv_val)` in the latch block where `inv_val` (a CONST_INT 2 defined in the body block) was out of C scope. The emitter emits the body block's constants as local declarations; the latch block cannot reference them.
- **Fix:** Added step_alloca pattern: for each IV transformation, create a dedicated step_alloca initialized in the preheader. For body-block-defined CONST_INTs, create a fresh CONST_INT in the preheader. The latch step ADD uses LOAD(step_alloca) instead of inv_val directly.
- **Files modified:** `src/ir/ir_optimize.c`
- **Commit:** 94f8043

## Test Results

```
./build/tests/ir/test_ir_optimize: 21 Tests 0 Failures 0 Ignored
ctest -L "unit|ir|integration": 100% tests passed, 0 tests failed out of 24
```

## Self-Check: PASSED

| Check | Result |
|-------|--------|
| src/ir/ir_optimize.c exists | FOUND |
| src/ir/ir_optimize.h exists | FOUND |
| tests/ir/test_ir_optimize.c exists | FOUND |
| 17-02-SUMMARY.md exists | FOUND |
| 15854d7 test commit exists | FOUND |
| 94f8043 feat commit exists | FOUND |
| run_strength_reduction count >= 2 | 2 |
| build_domtree count >= 2 | 2 |
| build_loop_info count >= 2 | 2 |
| IronIR_LoopInfo in .h >= 1 | 3 |
| IronIR_DomEntry in .h >= 1 | 1 |
| test_strength_reduction in tests >= 6 | 6 |
| 21 unit tests pass | PASS |
| Integration tests pass | PASS (24/24) |
