---
phase: 28-phi-elimination-improvement
plan: 01
subsystem: compiler-optimizer
tags: [lir, phi-elimination, dead-code-elimination, optimizer, C-codegen]

# Dependency graph
requires:
  - phase: 27-function-inlining
    provides: LIR function inlining producing result allocas with 0 loads; post-fixpoint pipeline placement
provides:
  - run_dead_alloca_elimination pass in lir_optimize.c
  - Post-fixpoint phi-origin alloca removal (zero-load allocas + their stores)
  - Escape + GET/SET_INDEX/FIELD safety guards for correct alloca removal
affects: [phi-elimination, lir-optimizer, C-codegen-temporaries]

# Tech tracking
tech-stack:
  added: []
  patterns:
    - "Dead alloca elimination: scan for loaded alloca IDs, check escape set, then compact-remove unloaded allocas and their stores (mirrors DCE pattern)"
    - "Placement after fixpoint convergence: post-fixpoint single-pass for dead alloca removal, not inside fixpoint loop"

key-files:
  created: []
  modified:
    - src/lir/lir_optimize.c

key-decisions:
  - "run_dead_alloca_elimination placed after compute_escape_set (not before) to avoid forward-declaration requirement — function ordering matters in C"
  - "Pass runs post-fixpoint (not inside fixpoint): single pass sufficient since copy-prop already eliminated single-store loads; simpler and correct"
  - "GET_INDEX/SET_INDEX and GET_FIELD/SET_FIELD references to an alloca mark it as live (not just LOAD) — prevents removing arrays mutated via index/field ops"
  - "Escape set integration: any alloca with escape_set[i].value==true is marked as loaded, preventing removal of address-escaping allocas"

patterns-established:
  - "Dead alloca elimination pattern: 3-scan algorithm (build loaded set -> identify dead allocas -> compact-remove dead alloca+store pairs)"
  - "Safety guard pattern: check both structural use (LOAD/GET_INDEX/SET_INDEX/GET_FIELD/SET_FIELD) and escape set before marking alloca as removable"

requirements-completed: [PHI-01, PHI-02]

# Metrics
duration: 25min
completed: 2026-04-01
---

# Phase 28 Plan 01: Dead Alloca Elimination Summary

**Post-fixpoint dead alloca elimination removes zero-load phi-origin allocas and their stores, reducing generated C temporaries in Iron_count_components from 108 (baseline) to 69**

## Performance

- **Duration:** ~25 min
- **Started:** 2026-04-01T13:20:00Z
- **Completed:** 2026-04-01T13:45:00Z
- **Tasks:** 2
- **Files modified:** 1

## Accomplishments

- Implemented `run_dead_alloca_elimination` pass in `src/lir/lir_optimize.c` (118 lines)
- Eliminated write-only phi-origin allocas (zero live loads) and all their stores from LIR
- Reduced declared temporaries in `Iron_count_components` from 108 to 69 (36% reduction, far exceeding the PHI-02 target of < 108)
- PHI-01 confirmed: simple loop test `hir_edge_var_loop_phi` no longer emits dead phi alloca declarations
- Zero regressions: 168/168 integration, 13/13 algorithm, 3/3 composite tests pass

## Task Commits

1. **Task 1: Implement run_dead_alloca_elimination pass** - `ea88280` (feat)
2. **Task 2: Validate temporary reduction and full test suite** - verification only, no additional code changes

**Plan metadata:** (docs commit to follow)

## Files Created/Modified

- `/Users/victor/code/iron-lang/src/lir/lir_optimize.c` - Added `run_dead_alloca_elimination` static function (after `compute_escape_set`), post-fixpoint call site, dump_passes block, and updated file header comment

## Decisions Made

- **Function ordering:** `run_dead_alloca_elimination` must be defined AFTER `compute_escape_set` in the file (C99 forward-declaration requirement). Initial placement before `compute_escape_set` caused a compile error; corrected by moving it after.
- **Pass placement:** Post-fixpoint (not inside fixpoint loop): a single pass after copy-prop has converged is sufficient. Inside-fixpoint placement adds marginal benefit and complexity.
- **Safety checks:** GET_INDEX/SET_INDEX/GET_FIELD/SET_FIELD references mark allocas as live (same as LOAD) — critical for correctness when arrays are accessed via index operations without explicit LOAD.

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 3 - Blocking] Function placement order caused compile error**
- **Found during:** Task 1 build verification
- **Issue:** `run_dead_alloca_elimination` was placed before `compute_escape_set` in the file; C99 does not allow calling an undeclared function
- **Fix:** Removed the function from its initial placement (before store/load-elim comment) and re-inserted it after `compute_escape_set` ends
- **Files modified:** src/lir/lir_optimize.c
- **Verification:** `cmake --build build` succeeded with zero errors after the move
- **Committed in:** ea88280 (Task 1 commit — single commit after corrected placement)

---

**Total deviations:** 1 auto-fixed (blocking — function ordering)
**Impact on plan:** Trivial ordering fix required by C99 one-pass compilation. No scope changes.

## Issues Encountered

- Initial function placement before `compute_escape_set` triggered 4 compile errors (undeclared function, type mismatch). Fixed by moving the function after `compute_escape_set`. The plan specified "place after run_dce" but `run_dce` is also before `compute_escape_set`, so placement after `compute_escape_set` (which also precedes store-load-elim) was the correct location.

## Next Phase Readiness

- PHI-01 and PHI-02 requirements satisfied
- Phase 28 Plan 01 complete; phase is single-plan, so Phase 28 is done
- Category 2 (multi-store phi-copy chains requiring copy coalescing) remains as a future optimization — out of scope per the research recommendation

## Self-Check: PASSED

- src/lir/lir_optimize.c: FOUND
- 28-01-SUMMARY.md: FOUND
- Commit ea88280: FOUND

---
*Phase: 28-phi-elimination-improvement*
*Completed: 2026-04-01*
