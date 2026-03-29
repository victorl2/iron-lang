---
phase: 16-expression-inlining
plan: 01
subsystem: compiler
tags: [ir, optimizer, expression-inlining, emit-c, use-count, purity-analysis, stb-ds]

# Dependency graph
requires:
  - phase: 15-copy-propagation-dce-constant-folding
    provides: IronIR_OptimizeInfo bridge struct, copy-prop/DCE/const-fold passes

provides:
  - Expression inlining: single-use pure IR values emitted as inline C expressions
  - Use-count analysis: counts every ValueId reference across all operands per function
  - Value-block map: maps each ValueId to its defining BasicBlock
  - Inline eligibility analysis: multi-exclusion logic with ordering hazard detection
  - Function purity analysis: 2-phase fixpoint identifies transitively pure user functions
  - emit_expr_to_buf: recursive C expression builder for all inlineable IR instruction kinds
  - 7 new unit tests: 5 analysis (ir_optimize), 2 emission (ir_emit)

affects: [17-*, future-emit-phases, perf-benchmarking]

# Tech tracking
tech-stack:
  added: []
  patterns:
    - "instr_mutates_memory() helper: determines ordering hazard for inlining safety"
    - "Use-site position tracking (use_site_pos map) enables between-def-and-use scan for side effects"
    - "emit_expr_to_buf recursive pattern: check inline_eligible -> look up value_table -> build C expr"
    - "MAKE_CLOSURE excluded from instr_is_inline_expressible (multi-statement env struct emission)"

key-files:
  created:
    - .planning/phases/16-expression-inlining/16-01-SUMMARY.md
  modified:
    - src/ir/ir_optimize.h
    - src/ir/ir_optimize.c
    - src/ir/emit_c.c
    - tests/ir/test_ir_optimize.c
    - tests/ir/test_ir_emit.c

key-decisions:
  - "instr_mutates_memory() check between def-pos and use-pos: conservative but correct ordering hazard detection prevents inlining past STORE/SET_INDEX/SET_FIELD/CALL"
  - "MAKE_CLOSURE excluded from instr_is_inline_expressible: emits multi-statement env struct alloc pattern, result void* must remain as named variable"
  - "ARRAY_LIT non-stack path uses emit_expr_to_buf for elements (not emit_val): previously emit_val could reference inlined (undeclared) values"
  - "PARALLEL_FOR range_val and MAKE_CLOSURE captures explicitly excluded from inlining: emit_instr uses emit_val (template pattern) not emit_expr_to_buf for these"
  - "use_site_pos map tracks first-use instruction index within each block: enables O(n) scan for ordering hazards per potentially eligible value"

requirements-completed: [IROPT-02]

# Metrics
duration: ~2h
completed: 2026-03-29
---

# Phase 16 Plan 01: Expression Inlining Summary

**Single-use pure IR values inlined as compound C expressions via emit_expr_to_buf, with use-count/purity/eligibility analysis and 7 new unit tests covering all exclusion cases**

## Performance

- **Duration:** ~2h
- **Started:** 2026-03-29
- **Completed:** 2026-03-29
- **Tasks:** 3
- **Files modified:** 5

## Accomplishments
- Recursive emit_expr_to_buf helper builds inline C expressions for all 15+ inlineable IR instruction kinds
- Inline eligibility analysis with 6 exclusion categories: multi-use, stack-array, INTERP_STRING parts, MAKE_CLOSURE captures/result, PARALLEL_FOR range/captures, SPAWN pool_val, ordering hazards, cross-block values, ARRAY_LIT-typed values
- Function purity 2-phase fixpoint identifies transitively pure functions; pure CALLs are inline-eligible
- All 38 integration tests pass, all 39 tests pass (excluding benchmark_smoke timeout)

## Task Commits

1. **Task 1: Analysis infrastructure** - `954a8a1` (feat)
2. **Task 2: emit_expr_to_buf + integration** - `9ef67a4` (feat)
3. **Task 3: Unit tests** - `b144096` (test)

## Files Created/Modified
- `src/ir/ir_optimize.h` - 4 named typedefs, 4 new IronIR_OptimizeInfo fields, 6 new public function declarations
- `src/ir/ir_optimize.c` - instr_is_inline_expressible, instr_mutates_memory, compute_func_purity, iron_ir_compute_use_counts, iron_ir_compute_value_block, iron_ir_compute_inline_eligible, iron_ir_compute_inline_info
- `src/ir/emit_c.c` - emit_expr_to_buf recursive helper, EmitCtx inline_eligible/value_block/current_block_id fields, emit_instr skip-on-eligible, per-function pre-scan in emit_func_body, ARRAY_LIT heap-path uses emit_expr_to_buf
- `tests/ir/test_ir_optimize.c` - 5 new tests (use-count, multi-use exclusion, impure CALL exclusion, cross-block exclusion, func purity)
- `tests/ir/test_ir_emit.c` - 2 new tests (basic inlining, CONSTRUCT inlining)

## Decisions Made
- **Ordering hazard detection**: Used `use_site_pos` map (ValueId -> instruction index) and scanned between def_pos and use_pos for memory-mutating instructions. Conservative but correct: any STORE/SET_INDEX/SET_FIELD/CALL between def and use excludes the value. This fixed `bug_transitive_param_mode` where `GET_INDEX` (temp=arr[i]) was inlined past a `SET_INDEX` (arr[i]=arr[j]).
- **MAKE_CLOSURE exclusion**: Both the result value and captures excluded. The result (`void* _vN`) is excluded via `instr_is_inline_expressible` returning false; captures are explicitly excluded in Pass 1 of compute_inline_eligible.
- **ARRAY_LIT element emission**: Non-stack ARRAY_LIT path changed from `emit_val` to `emit_expr_to_buf` for each element, enabling inlining of CONST_INT elements passed to Iron_List_T_push.

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 1 - Bug] ARRAY_LIT non-stack heap path used emit_val for elements**
- **Found during:** Task 2 verification
- **Issue:** Non-stack ARRAY_LIT (Iron_List) emitted element references via `emit_val`, so inlined CONST_INT elements became undeclared identifiers
- **Fix:** Changed to `emit_expr_to_buf` for elements in heap ARRAY_LIT path
- **Files modified:** src/ir/emit_c.c
- **Verification:** quicksort/merge_sort/hash_map algorithm tests pass
- **Committed in:** 9ef67a4

**2. [Rule 1 - Bug] MAKE_CLOSURE result marked inline-eligible via instr_is_inline_expressible**
- **Found during:** Task 2 verification (lambda_capture integration test)
- **Issue:** MAKE_CLOSURE is listed as "pure" in iron_ir_instr_is_pure, so instr_is_inline_expressible returned true for it. emit_expr_to_buf falls back to emit_val for MAKE_CLOSURE, causing undeclared identifier when declaration was skipped
- **Fix:** Added explicit `if (kind == IRON_IR_MAKE_CLOSURE) return false;` in instr_is_inline_expressible
- **Files modified:** src/ir/ir_optimize.c
- **Verification:** lambda_capture integration test passes
- **Committed in:** 9ef67a4

**3. [Rule 1 - Bug] Ordering hazard: GET_INDEX inlined past SET_INDEX in swap()**
- **Found during:** Task 2 verification (bug_transitive_param_mode wrong output)
- **Issue:** Classic swap pattern - temp=arr[i] was inlined as arr[i] AFTER arr[i]=arr[j] mutated it, reading wrong value
- **Fix:** Added instr_mutates_memory() helper + use_site_pos map + scan between def and use for memory mutations; any mutation excludes the value from inlining
- **Files modified:** src/ir/ir_optimize.c
- **Verification:** bug_transitive_param_mode produces correct sorted output 0-9
- **Committed in:** 9ef67a4

**4. [Rule 1 - Bug] PARALLEL_FOR/MAKE_CLOSURE/SPAWN operands used via emit_val templates**
- **Found during:** Task 2 verification (test_parallel, parallel_for_capture, test_combined)
- **Issue:** emit_instr for PARALLEL_FOR (range_val), MAKE_CLOSURE (captures), SPAWN (pool_val) uses emit_val in template patterns, not emit_expr_to_buf — inlined values created undeclared identifiers
- **Fix:** Added explicit exclusions in compute_inline_eligible Pass 1 for these operands
- **Files modified:** src/ir/ir_optimize.c
- **Verification:** All parallel/closure integration tests pass
- **Committed in:** 9ef67a4

---

**Total deviations:** 4 auto-fixed (4 Rule 1 bugs — all correctness issues)
**Impact on plan:** All fixes necessary for correctness. The ordering hazard check was the most significant addition — not in the plan but critical for safe inlining.

## Issues Encountered
- Benchmark_smoke test times out (>300s) — pre-existing behavior unrelated to this plan; the benchmark compiles ~130 Iron programs. All other tests pass.

## Next Phase Readiness
- Expression inlining is active and correct across all test cases
- IronIR_OptimizeInfo now carries full per-module purity analysis
- emit_expr_to_buf handles all inlineable instruction kinds with recursive fallback
- Ready for Phase 17 (loop optimizations or further IR passes)

---
*Phase: 16-expression-inlining*
*Completed: 2026-03-29*
