---
phase: 20-hir-lowering-pipeline-cutover
plan: "02"
subsystem: hir-to-lir
tags: [hir, lir, ssa, dominance-frontier, phi-placement, lowering]
dependency_graph:
  requires: [20-01]
  provides: [iron_hir_to_lir, HIR-06]
  affects: [src/hir, src/lir, tests/hir]
tech_stack:
  added: []
  patterns:
    - Three-pass HIR-to-LIR lowering (type-decls, flatten, SSA)
    - Dominance-frontier phi placement (classic algorithm)
    - Iterative dominator tree construction (Cooper-Harvey-Kennedy)
    - Two-phase param registration (all synthetic IDs first, then allocas)
key_files:
  created:
    - src/hir/hir_to_lir.h
    - src/hir/hir_to_lir.c
    - tests/hir/test_hir_to_lir.c
  modified:
    - CMakeLists.txt
    - tests/hir/CMakeLists.txt
decisions:
  - Two-phase param registration: allocate all synthetic param ValueIds contiguously (1..param_count) BEFORE creating any allocas, so the verifier's 1<=op<=param_count exemption applies correctly
  - alloca used for both mutable and immutable (val) variables for uniformity; SSA rename pass converts alloca/load/store to direct values and phis
  - compute_dominance_frontiers duplicated locally (not shared with lir_optimize.c which keeps them static) to keep lowering self-contained
  - Short-circuit AND/OR lowered to multi-block branch+phi pattern (same as lower_exprs.c)
  - Defer bodies emit via dedicated cleanup blocks; multiple returns at same depth share the chain
metrics:
  duration: ~15 minutes
  completed: "2026-03-30"
  tasks: 2
  files: 5
---

# Phase 20 Plan 02: HIR-to-LIR Three-Pass Lowering Summary

Three-pass HIR-to-LIR SSA construction with dominance-frontier phi placement using the classic iterative algorithm.

## What Was Built

### Pass 0 — Type Declaration Registration
Walk `Iron_Program->decls[]` to register interfaces, objects, enums, and extern functions in the LIR module. Matches the logic in `lower_types.c`'s `lower_module_decls()`.

### Pass 1 — Flatten HIR to Pre-SSA CFG
For each HIR function, walk the HIR body recursively, emitting LIR basic blocks:
- `STMT_IF` → `BRANCH` terminator, then/else blocks, merge block
- `STMT_WHILE` → header/body/exit blocks, BRANCH + back-edge JUMP
- `STMT_FOR` → pre_header/header/body/increment/exit blocks, GET_INDEX on iterable
- `STMT_MATCH` → `SWITCH` terminator, one block per arm, join block
- `STMT_RETURN` → defer cleanup chain, then `RETURN` terminator
- `STMT_DEFER` → push to defer stack (not inline)
- `EXPR_BINOP AND/OR` → short-circuit multi-block branch+phi
- All ALLOCAs emitted in entry block (`fn->blocks[0]`) per LIR invariant

### Pass 2 — SSA Construction via Dominance Frontiers
1. `rebuild_cfg_edges(fn)` — populate preds/succs by scanning terminators
2. `build_domtree(fn)` — iterative Cooper-Harvey-Kennedy algorithm
3. `compute_dominance_frontiers(fn, idom)` — classic DF computation
4. For each alloca: find defining blocks, compute DF+ closure, insert phi nodes
5. Rename pass — value_table patching for LOAD-after-STORE within domtree traversal
6. Fill phi incoming values from predecessor last-STORE analysis

## Smoke Tests (13 passing)
1. `test_hir_to_lir_empty_func` — entry block + implicit RETURN
2. `test_hir_to_lir_val_binding` — immutable val → ALLOCA + STORE
3. `test_hir_to_lir_var_binding` — mutable var → ALLOCA + STORE
4. `test_hir_to_lir_if_else_cfg` — if/else → ≥3 blocks + BRANCH
5. `test_hir_to_lir_while_loop_cfg` — while → ≥3 blocks + BRANCH + JUMP
6. `test_hir_to_lir_match_switch` — match → SWITCH terminator
7. `test_hir_to_lir_return_value` — return expr → non-void RETURN
8. `test_hir_to_lir_binop` — `a + b` → IRON_LIR_ADD
9. `test_hir_to_lir_call` — function call → IRON_LIR_CALL
10. `test_hir_to_lir_alloca_in_entry` — loop-declared var → alloca in blocks[0]
11. `test_hir_to_lir_phi_at_merge` — if/else modifying var → PHI at merge
12. `test_hir_to_lir_verify_passes` — `add(a,b)` passes `iron_lir_verify()`
13. `test_hir_to_lir_short_circuit_and` — `a && b` → multi-block + PHI

## Decisions Made

1. **Two-phase param registration**: All synthetic param ValueIds must be allocated contiguously (1..param_count) before any ALLOCA instructions, because the LIR verifier's use-before-def exemption is `1 <= op <= fn->param_count`. Interleaving allocas breaks this.

2. **alloca-for-all strategy**: Both mutable (`var`) and immutable (`val`) variables use alloca/store/load uniformly. The SSA Pass 2 rename pass converts these to direct values and phi nodes, optimizing out the alloca overhead.

3. **Self-contained domtree functions**: `rebuild_cfg_edges`, `build_rpo`, `build_domtree`, and `compute_dominance_frontiers` are duplicated locally in `hir_to_lir.c` rather than shared with `lir_optimize.c` (which declares them `static`). This keeps the HIR-to-LIR pass independent of the optimizer internals.

4. **Short-circuit lowering pattern**: AND/OR use a multi-block branch+phi pattern identical to `lower_exprs.c`. The false/true constant for the short-circuit path is emitted into the entry block.

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 1 - Bug] Two-phase param registration for verifier compliance**
- **Found during:** Task 1 (discovered when test 12 failed)
- **Issue:** Allocating synthetic param ValueIds interleaved with alloca IDs caused param_val_id for param N > 0 to exceed param_count, breaking the LIR verifier's exemption
- **Fix:** Split param registration into two loops: all synthetic IDs first (sequential, 1..N), then all allocas
- **Files modified:** `src/hir/hir_to_lir.c`
- **Commit:** Part of 532eaf9 → fixed in 86b21b7

**2. [Rule 3 - Blocking] Iron_Param uses `type_ann` not `resolved_type`**
- **Found during:** Task 1 compile
- **Issue:** `Iron_Param` struct has `type_ann: Iron_Node*` not `resolved_type: Iron_Type*`
- **Fix:** Used `iron_type_make_primitive(IRON_TYPE_VOID)` as fallback for extern param types in Pass 0
- **Files modified:** `src/hir/hir_to_lir.c`
- **Commit:** 532eaf9

**3. [Rule 3 - Blocking] Iron_Type::array uses `.elem` not `.element_type`**
- **Found during:** Task 1 compile
- **Issue:** Field name in FOR loop iterable type extraction was wrong
- **Fix:** Changed `array.element_type` → `array.elem`
- **Files modified:** `src/hir/hir_to_lir.c`
- **Commit:** 532eaf9

## Self-Check

Files created:
- /Users/victor/code/iron-lang/src/hir/hir_to_lir.h ✓
- /Users/victor/code/iron-lang/src/hir/hir_to_lir.c ✓ (1800+ lines)
- /Users/victor/code/iron-lang/tests/hir/test_hir_to_lir.c ✓ (629 lines, 13 tests)

Commits:
- 532eaf9 feat(20-02): implement HIR-to-LIR three-pass lowering ✓
- 86b21b7 feat(20-02): add smoke tests for HIR-to-LIR lowering ✓

Tests: 13/13 pass, all 5 HIR tests green.

## Self-Check: PASSED
