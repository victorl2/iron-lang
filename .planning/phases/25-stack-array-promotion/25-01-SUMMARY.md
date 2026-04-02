---
phase: 25-stack-array-promotion
plan: 01
subsystem: compiler
tags: [lir, optimizer, codegen, stack-allocation, fill, arrays, constant-folding]

# Dependency graph
requires:
  - phase: 24-range-bound-hoisting
    provides: LIR optimizer passes and emit_func_body pre-scan patterns used as reference
provides:
  - Constant-count gate in optimize_array_repr() restricts sa_map to fills with CONST_INT count <= 1024
  - Declaration hoisting of fixed-size fill arrays to function entry (prevents VLA+goto bypass errors)
  - fill_hoisted tracking map in EmitCtx enables call-site to emit only initialization loop
  - Regression test bug_const_fill_early_return.iron covering MEM-02 constant-fill case
affects: [26-load-forwarding, 27-function-inlining, any phase touching fill() or stack array emission]

# Tech tracking
tech-stack:
  added: []
  patterns:
    - "Fill hoisting: constant-count fill() declarations hoisted to function entry, init-only at call site (mirrors phi_hoisted pattern)"
    - "Dual gate: MEM-01 constant-count gate applied in both optimizer (sa_map) and emitter pre-scan (sa_pre) for consistent behavior"
    - "fill_hoisted map in EmitCtx tracks which fills got entry declarations; call site checks before emitting declaration"

key-files:
  created:
    - tests/integration/bug_const_fill_early_return.iron
    - tests/integration/bug_const_fill_early_return.expected
    - .planning/phases/25-stack-array-promotion/25-01-SUMMARY.md
  modified:
    - src/lir/lir_optimize.c
    - src/lir/emit_c.c

key-decisions:
  - "Apply constant-count gate in both optimizer AND emitter pre-scan: gate in optimizer prevents sa_map pollution for dynamic fills; gate in emitter pre-scan prevents sa_pre from picking up dynamic fills that were never revoked (since they never entered sa_map, they also never got revoked by escape analysis)"
  - "Raise threshold from 256 to 1024 in both optimizer gate and emitter check to satisfy MEM-01 requirement (1024 * 8 = 8KB max stack cost per fill, within standard limits)"
  - "Use fill_hoisted map in EmitCtx (not reuse phi_hoisted): fill() hoisting is a different code path from phi hoisting; separate map keeps concerns clean"
  - "Hoisted _vN_len initialized to fill_count at function entry (not 0): matches actual constant value, avoids uninitialized read if early-return path never reaches init loop"

patterns-established:
  - "Declaration hoisting pattern: scan stack_array_ids for unique origin IDs that are CALL+CONST_INT, emit elem_t _vN[N] and _len at function entry, mark in fill_hoisted"
  - "Dual gate pattern: any optimization pass that restricts which instructions enter a promotion map must also restrict the emitter's pre-scan for that same instruction class"

requirements-completed: [MEM-01, MEM-02]

# Metrics
duration: 69min
completed: 2026-03-31
---

# Phase 25 Plan 01: Stack Array Promotion Summary

**Constant-count fill() stack promotion with declaration hoisting: fixed-size arrays at function entry, init loop at call site, 1024-element threshold**

## Performance

- **Duration:** 69 min
- **Started:** 2026-03-31T22:46:09Z
- **Completed:** 2026-03-31T23:55:09Z
- **Tasks:** 3
- **Files modified:** 4 (2 source, 2 test)

## Accomplishments

- Implemented MEM-01: only `fill(CONST, val)` with constant count <= 1024 enters the stack promotion path; dynamic-count fills fall through to heap/alloca correctly
- Implemented MEM-02: fixed-size array declarations hoisted to function entry via `fill_hoisted` map in EmitCtx, preventing VLA+goto bypass errors in functions with early returns
- Added regression test `bug_const_fill_early_return.iron` covering the constant-fill-after-early-return pattern; test passes with hoisted declarations
- 165 integration tests pass with 0 regressions introduced by these changes

## Task Commits

Each task was committed atomically:

1. **Task 1: Add constant-fill early-return regression test** - `554931b` (test)
2. **Task 2: Implement constant-count gate and declaration hoisting** - `aa5526f` (feat)
3. **Task 3: Full test suite validation** - (no new files; validation only)

## Files Created/Modified

- `src/lir/lir_optimize.c` - Added CONST_INT gate in optimize_array_repr() Pass 2: only fills with count > 0 && <= 1024 enter sa_map for stack promotion
- `src/lir/emit_c.c` - Added fill_hoisted field to EmitCtx; added hoisting block after phi_hoisted section; updated use_stack call-site to emit init-only when hoisted; added const-count gate in Phase A pre-scan; raised threshold 256 -> 1024
- `tests/integration/bug_const_fill_early_return.iron` - Regression test: fill(5,0) after early return (constant-count variant of bug_vla_goto_bypass)
- `tests/integration/bug_const_fill_early_return.expected` - Expected: empty=-1, found=2, none=0

## Decisions Made

- **Dual gate in optimizer AND emitter pre-scan:** Without the gate in the emitter's Phase A pre-scan, dynamic-count fills that return from functions could not be revoked (they were never in sa_map, so escape analysis never touched them), causing them to appear in sa_pre and eventually take the VLA/alloca path, producing `int64_t*` where `Iron_List_T` was expected. The gate must be applied in both places.

- **Threshold 1024:** Requirements specify <= 1024. Raised from the legacy 256 value in both optimizer gate and emitter threshold check. Stack cost is at most 8KB per fill (1024 int64_t values), within standard stack limits.

- **_vN_len initialized at function entry to fill_count (not 0):** The companion length variable is declared and initialized with the actual count at function entry so it's always valid; the call-site assigns it again as a no-op for clarity.

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 1 - Bug] Dynamic-count fill escape path broken by optimizer gate**
- **Found during:** Task 2 (implement constant-count gate)
- **Issue:** After adding the constant-count gate in the optimizer, dynamic-count fills no longer entered sa_map and were therefore never added to revoked_fill_ids by escape analysis. The emitter's Phase A pre-scan checked `hmgeti(revoked_fill_ids, ...) < 0` — which returned true for dynamic fills (not in revoked = not revoked). Dynamic fills entered sa_pre → stack_array_ids. At call site, use_stack=false (count > 1024) but get_stack_array_origin() != INVALID, so VLA/alloca path fired, producing `int64_t*`. Functions returning dynamic fills caused type mismatch: `int64_t*` vs `Iron_List_T`.
- **Fix:** Added constant-count gate in emitter Phase A pre-scan (emit_c.c) so only CONST_INT count > 0 && <= 1024 fills enter sa_pre. Dynamic fills are excluded from sa_pre entirely and take the heap path.
- **Files modified:** src/lir/emit_c.c
- **Verification:** `bug_array_reassign_from_call` test (which uses `fill(n, 0)` returning from function) passes after fix.
- **Committed in:** aa5526f (Task 2 commit)

---

**Total deviations:** 1 auto-fixed (Rule 1 - Bug)
**Impact on plan:** Necessary correctness fix. The plan's description of the change was correct but incomplete — the pre-scan gate must mirror the optimizer gate to maintain invariants. No scope creep.

## Issues Encountered

- `bug_vla_goto_bypass` test was already failing before this plan (pre-existing: dynamic-count fill in `double_array()` returning heap list, unrelated to this plan's changes). Confirmed pre-existing by reverting changes and testing. Out of scope — logged to deferred-items.

## Next Phase Readiness

- MEM-01 and MEM-02 fully satisfied: constant-count fills use fixed-size stack arrays with hoisted declarations
- fill_hoisted pattern established for future emitter hoisting needs
- Dynamic-count fills correctly excluded from stack promotion path (verified via bug_array_reassign_from_call)
- Ready for Phase 26 (LOAD forwarding) which may interact with stack_array_ids propagation

---
*Phase: 25-stack-array-promotion*
*Completed: 2026-03-31*

## Self-Check: PASSED

- FOUND: tests/integration/bug_const_fill_early_return.iron
- FOUND: tests/integration/bug_const_fill_early_return.expected
- FOUND: src/lir/lir_optimize.c (modified)
- FOUND: src/lir/emit_c.c (modified)
- FOUND: .planning/phases/25-stack-array-promotion/25-01-SUMMARY.md
- FOUND commit: 554931b (test: add constant-fill early-return regression test)
- FOUND commit: aa5526f (feat: implement constant-count gate and declaration hoisting)
