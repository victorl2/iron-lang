---
phase: 15-copy-propagation-dce-constant-folding
plan: 02
subsystem: compiler-ir
tags: [ir, optimization, copy-propagation, dce, constant-folding, fixpoint]

# Dependency graph
requires:
  - phase: 15-copy-propagation-dce-constant-folding plan 01
    provides: ir_optimize module infrastructure with phi elimination, array optimization, and fixpoint stub

provides:
  - run_copy_propagation(): replaces LOAD of single-store allocas with stored value
  - run_constant_folding(): evaluates CONST_INT arithmetic and comparisons at compile time
  - run_dce(): removes pure instructions whose results are never referenced
  - Fixpoint loop (max 32 iterations) with iron_ir_verify between each pass

affects:
  - Phase 16 (expression inlining) — new passes reduce IR noise before inlining
  - All future IR passes — establishes verified-pass pattern

# Tech tracking
tech-stack:
  added: []
  patterns:
    - Named typedef pattern for anonymous stb_ds hashmap structs to avoid type mismatch in hmput
    - apply_replacements() mirror of verify.c collect_operands for mutation-side operand rewriting
    - opt_collect_operands() exact copy of verify.c collect_operands for read-side operand collection
    - Fixpoint loop with per-pass iron_ir_verify verification gates

key-files:
  created: []
  modified:
    - src/ir/ir_optimize.c
    - tests/ir/test_ir_emit.c

key-decisions:
  - "Named typedefs (ValueReplEntry, StoreInfoVal, StoreInfoEntry) required for stb_ds hmput with anonymous struct map types — C does not allow assigning across distinct anonymous struct types even when structurally identical"
  - "test_ir_emit emission-only tests use skip_new_passes=true to prevent optimizer from eliminating unused constants before assertion checks"
  - "DCE seeds liveness from side-effecting instructions and instructions with INVALID id (STORE, terminators) then propagates transitively through operands"

patterns-established:
  - "IR passes mirror verify.c operand collection exactly to avoid silent correctness bugs from missed instruction kinds"
  - "Inter-pass ir_verify calls: every optimization pass followed by iron_ir_verify before the next pass runs"

requirements-completed: [IROPT-01, IROPT-03, IROPT-04]

# Metrics
duration: 45min
completed: 2026-03-29
---

# Phase 15 Plan 02: Copy Propagation, DCE & Constant Folding Summary

**Three optimization passes (copy propagation, constant folding, DCE) in a verified fixpoint loop that eliminates redundant loads, folds compile-time constants, and removes dead pure instructions**

## Performance

- **Duration:** ~45 min
- **Started:** 2026-03-29T13:00:00Z
- **Completed:** 2026-03-29T13:45:00Z
- **Tasks:** 1
- **Files modified:** 2

## Accomplishments

- Implemented `run_copy_propagation()` — finds single-store allocas, maps their LOADs to the stored value, and rewrites all operand references via `apply_replacements()`
- Implemented `run_constant_folding()` — evaluates CONST_INT arithmetic (ADD/SUB/MUL/DIV/MOD) and comparisons (EQ/NEQ/LT/LTE/GT/GTE) in-place with div/mod by zero guard
- Implemented `run_dce()` — worklist-based liveness analysis seeds from side-effecting instructions, propagates through operand chains, removes non-live pure instructions
- Wired all three passes into fixpoint loop (max 32 iterations) with `iron_ir_verify` called between each pass

## Task Commits

1. **Task 1: Copy propagation, constant folding, and DCE passes** - `c51246f` (feat)

**Plan metadata:** (in progress)

## Files Created/Modified

- `/Users/victor/code/iron-lang/src/ir/ir_optimize.c` - Added ~660 lines: helper typedefs, apply_replacements(), opt_collect_operands(), run_copy_propagation(), run_constant_folding(), run_dce(), updated iron_ir_optimize() fixpoint loop
- `/Users/victor/code/iron-lang/tests/ir/test_ir_emit.c` - Updated test_emit_hello_world and test_emit_arithmetic to use skip_new_passes=true since they test unoptimized IR emission

## Decisions Made

- Named typedefs (`ValueReplEntry`, `StoreInfoVal`, `StoreInfoEntry`) required because stb_ds `hmput` macro uses assignment, and C treats two anonymous structs with identical fields as incompatible types — the naive inline struct approach from the plan pseudocode fails with `-Werror`.
- Emission-only unit tests in `test_ir_emit.c` use `skip_new_passes=true` to isolate emitter behavior from optimizer effects, since DCE correctly removes unused `const_string` and constant folding correctly eliminates `10 + 20` before emission.
- `opt_collect_operands` mirrors `verify.c:collect_operands` verbatim (including `HEAP_ALLOC`, `RC_ALLOC`, `FREE`, `INTERP_STRING`, `SPAWN`, `PARALLEL_FOR`, `AWAIT` kinds) to guarantee DCE never misclassifies an instruction's operands as dead.

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 1 - Bug] Fixed anonymous struct type incompatibility in stb_ds hmput calls**
- **Found during:** Task 1 (first build attempt)
- **Issue:** Plan pseudocode used `struct { IronIR_ValueId key; struct { int count; IronIR_ValueId val; } value; } *store_info` then tried to hmput a locally-declared `struct { int count; IronIR_ValueId val; } sv` — C compiler treats these as incompatible types even though structurally identical
- **Fix:** Declared named typedefs `StoreInfoVal` and `StoreInfoEntry` at file scope; `apply_replacements` parameter uses `ValueReplEntry *` typedef
- **Files modified:** src/ir/ir_optimize.c
- **Verification:** Build succeeded with zero errors after typedef introduction
- **Committed in:** c51246f (Task 1 commit)

**2. [Rule 1 - Bug] Fixed plan pseudocode field name mismatches vs actual ir.h structs**
- **Found during:** Task 1 (pre-implementation code review of ir.h and verify.c)
- **Issue:** Plan pseudocode used field names from a conceptual struct layout that differed from actual ir.h: `instr->unary.operand` should be `instr->unop.operand`; `instr->get_field.base` should be `instr->field.object`; `instr->set_field.base/value` should be `instr->field.object/value`; `instr->get_index.base` should be `instr->index.array`; `instr->cast.operand` should be `instr->cast.value`; `instr->construct.fields` should be `instr->construct.field_vals`; `instr->array_lit.elems/elem_count` should be `instr->array_lit.elements/element_count`; `instr->is_null.operand` should be `instr->null_check.value`; `instr->switch_.cond` should be `instr->sw.subject`; `instr->call.receiver` doesn't exist (use `instr->call.func_ptr` for indirect)
- **Fix:** Implemented all operand handling using actual ir.h field names discovered by reading verify.c's collect_operands (the ground truth)
- **Files modified:** src/ir/ir_optimize.c
- **Verification:** All IR tests pass
- **Committed in:** c51246f (Task 1 commit)

**3. [Rule 1 - Bug] Updated test_ir_emit tests that failed due to optimizer correctly eliminating code**
- **Found during:** Task 1 (test run after implementation)
- **Issue:** `test_emit_hello_world` expected `iron_string_from_literal` but DCE correctly removed unused `const_string`; `test_emit_arithmetic` expected `+` but constant folding correctly folded `10 + 20` to `30`
- **Fix:** Changed both tests to use `skip_new_passes=true` — these tests are emitter unit tests, not optimizer tests; using skip_new_passes preserves their intent of testing IR emission of specific instruction kinds
- **Files modified:** tests/ir/test_ir_emit.c
- **Verification:** All IR emit tests pass (6/6)
- **Committed in:** c51246f (Task 1 commit)

---

**Total deviations:** 3 auto-fixed (all Rule 1 bugs)
**Impact on plan:** All fixes were correctness corrections to plan pseudocode. No scope creep. The plan's algorithm descriptions were correct; only the C implementation details (field names, type compatibility) required adjustment.

## Issues Encountered

- `benchmark_smoke` test suite has pre-existing performance failures (spawn_pipeline_stages, target_sum, topological_sort_kahn, validate_bst) that are not related to this plan's changes — they fail due to execution speed thresholds on timing-sensitive algorithms.

## Self-Check

- [x] `src/ir/ir_optimize.c` exists and contains all three pass functions
- [x] `c51246f` commit exists
- [x] 100% of non-benchmark tests pass (38/38)
- [x] Acceptance criteria verified: all 10 criteria from plan met

## Self-Check: PASSED

## Next Phase Readiness

- Three optimization passes are live and running on all compiled Iron programs
- Phase 16 (expression inlining) can now work on cleaner IR with fewer redundant loads
- The verified fixpoint loop pattern is established and ready for additional passes

---
*Phase: 15-copy-propagation-dce-constant-folding*
*Completed: 2026-03-29*
