---
phase: 27-function-inlining
plan: 01
subsystem: lir-optimizer
tags: [inlining, lir, optimization, c-emission, value-remapping]
dependency_graph:
  requires: [phi_eliminate, optimize_array_repr, apply_replacements]
  provides: [run_function_inlining]
  affects: [copy_prop, dce, store_load_elim, emit_c]
tech_stack:
  added: []
  patterns: [value-id-remapping, block-id-remapping, block-splitting, result-alloca, stb-ds-snapshot]
key_files:
  created:
    - tests/integration/inline_basic.iron
    - tests/integration/inline_basic.expected
  modified:
    - src/lir/lir_optimize.c
    - src/lir/emit_c.c
decisions:
  - "Threshold set to 30 instructions (not 20) because phi_eliminate adds ~9 extra param alloca/store pairs per parameter"
  - "Result alloca inserted into call_block at call_idx BEFORE block split to guarantee C declaration before all uses"
  - "Step 9 applies result_remap to ALL original caller blocks (not just cont) to fix CALL results referenced in branch target blocks"
  - "emit_c.c backward-reference hoisting extended to handle ALLOCA and any value-producing instruction (CALL, binop, etc.)"
  - "Separate minimal result_remap (not id_remap) applied to caller blocks to avoid callee-ID collisions with caller IDs"
metrics:
  duration_minutes: 120
  tasks_completed: 2
  files_modified: 4
  completed_date: "2026-03-31"
---

# Phase 27 Plan 01: Function Inlining Summary

LIR-level function inlining for small pure non-recursive functions, enabling copy-prop/DCE to optimize merged callee+caller code; verified by inline_basic regression test and connected_components benchmark.

## Objective

Implement `run_function_inlining()` in `src/lir/lir_optimize.c` that clones small pure callee bodies into call sites, remaps all value and block IDs, and wires correctly between `optimize_array_repr` and the copy-prop/DCE fixpoint loop.

## Tasks Completed

| Task | Name | Commit | Files |
|------|------|--------|-------|
| 1 | Add inline_basic regression test | 9cfbb32 | tests/integration/inline_basic.iron, tests/integration/inline_basic.expected |
| 2 | Implement run_function_inlining | 63aa7d9 | src/lir/lir_optimize.c, src/lir/emit_c.c |

## Implementation Details

### Algorithm Overview (inline_call_site)

1. Pre-populate `id_remap` with callee param synthetic IDs -> call arg IDs (from STORE patterns in callee entry block)
2. Insert result ALLOCA into `call_block` at `call_idx` BEFORE splitting (ensures C declaration precedes all uses)
3. Clone callee blocks; assign fresh IDs via `fn->next_block_id++`; deep-copy stb_ds arrays in instructions
4. Apply `id_remap` to all cloned instructions via `apply_replacements`
5. Apply `block_remap` to terminators in cloned blocks (JUMP, BRANCH, SWITCH targets)
6. Create merge block; insert LOAD from result alloca (load_result_id replaces call_instr->id)
7. Replace RETURN in cloned blocks with STORE to result alloca + JUMP to merge block
8. Split call_block; wire JUMP edges (call_block -> cloned_entry, merge -> cont)
9. Apply `{call_instr->id -> load_result_id}` to ALL original caller blocks (0..cloned_block_start-1) and cont using a fresh minimal remap (not id_remap, which contains callee IDs that collide with caller IDs)

### emit_c.c Extensions

The C emitter's backward-reference hoisting was extended to handle instructions that are defined in later blocks but referenced in earlier blocks (a common pattern after inlining moves continuation blocks to the end of the block array):

- **use_block_min tracking**: Added comprehensive switch statement covering all instruction kinds (STORE, LOAD, binops, CALL args, GET_INDEX, SET_INDEX, BRANCH, RETURN, GET/SET_FIELD, INTERP_STRING, CAST, IS_NULL, etc.) to track the earliest block index where each value is used
- **Hoisting loop**: Extended from LOAD-only to handle any value-producing instruction (ALLOCA skips declaration at definition site; all others emit assignment without type prefix)
- **CALL result hoisting**: Added `is_hoisted` check in CALL emission to suppress type prefix when the result was pre-declared at function entry

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 1 - Bug] Threshold raised from 20 to 30 instructions**
- **Found during:** Task 2 (connected_components `find_root` not being inlined)
- **Issue:** `find_root` has 23 instructions after phi_eliminate (plan estimated ~14), because phi_eliminate adds alloca+store pairs for each parameter (2 instructions per param x 1 param = 2 extra, plus the alloca/store/load pattern in entry block totals ~9 extra vs source-level count)
- **Fix:** Raised INLINE_THRESHOLD from 20 to 30
- **Files modified:** src/lir/lir_optimize.c
- **Commit:** 63aa7d9

**2. [Rule 1 - Bug] Result alloca placement to fix undeclared variable**
- **Found during:** Task 2 (C compilation error: `_v73` undeclared in interp_string)
- **Issue:** Result alloca was placed in merge block (appended at end), but STORE in cloned blocks references it before its declaration in C output
- **Fix:** Insert result alloca into call_block at call_idx BEFORE the split, not into merge block
- **Files modified:** src/lir/lir_optimize.c
- **Commit:** 63aa7d9

**3. [Rule 1 - Bug] id_remap collision with caller IDs in continuation block**
- **Found during:** Task 2 (wrong println argument: `_v73` instead of `_v74`)
- **Issue:** Applying id_remap (which maps callee IDs -> new caller IDs) to the continuation block caused wrong substitutions because callee ID 4 collided with caller's `%4` (interp_string result)
- **Fix:** Step 9 uses a separate fresh minimal remap `{call_instr->id -> load_result_id}` instead of id_remap for applying to caller blocks
- **Files modified:** src/lir/lir_optimize.c
- **Commit:** 63aa7d9

**4. [Rule 1 - Bug] CALL result hoisting for multi-block references**
- **Found during:** Task 2 (`_v18` undeclared in `Iron_unite` after inlining first find_root call)
- **Issue:** After inlining, second `find_root` call moved to cont block (appended at end). But `if_then_8` (earlier block) has `store %55, %18` referencing `%18`. C emitter placed `_v18` declaration in cont (later block).
- **Fix:** Extended emit_c.c hoisting to handle CALL results; extended step 9 to apply result_remap to ALL original caller blocks (not just cont)
- **Files modified:** src/lir/emit_c.c, src/lir/lir_optimize.c
- **Commit:** 63aa7d9

**5. [Rule 3 - Blocking] Replaced opt_collect_operands with inline switch**
- **Found during:** Task 2 (build error: `opt_collect_operands` not declared in emit_c.c)
- **Issue:** `opt_collect_operands` and `MAX_OPERANDS` are static symbols in lir_optimize.c and cannot be called from emit_c.c
- **Fix:** Replaced the call with a comprehensive inline switch statement covering all instruction operand types
- **Files modified:** src/lir/emit_c.c
- **Commit:** 63aa7d9

## Verification Results

- `inline_basic` test: PASS (output: 6, 32, 5, 49, 8, 10, 15)
- `connected_components` benchmark: PASS (Test 1: 50, Test 2: 1, Test 3: 10, Test 4: 25)
- Full integration suite: 167 passed, 1 failed (bug_audit_heap_method_call is a pre-existing failure unrelated to this work)

## Self-Check: PASSED

All created/modified files confirmed present. All task commits confirmed in git history.
