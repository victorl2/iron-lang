---
phase: 26-load-expression-inlining
plan: 01
subsystem: optimizer
tags: [lir, optimizer, expression-inlining, load, c-codegen]

# Dependency graph
requires:
  - phase: 25-stack-array-promotion
    provides: stack array promotion with constant-count gate; fill_hoisted pattern
provides:
  - LOAD expression inlining for same-block LOADs via blanket exclusion removal
  - Regression test coverage for LOAD inlining (expr_inline_load)
affects: [27-function-inlining, 28-phi-coalescing]

# Tech tracking
tech-stack:
  added: []
  patterns:
    - "LOAD inlining: same-block LOADs now pass through to the existing cross-block guard rather than being excluded upfront"
    - "Cross-block guard at lir_optimize.c:1779-1785 is the single chokepoint preventing undeclared-variable errors from LOADs that span blocks"

key-files:
  created:
    - tests/integration/expr_inline_load.iron
    - tests/integration/expr_inline_load.expected
  modified:
    - src/lir/lir_optimize.c

key-decisions:
  - "LOAD blanket exclusion removed: cross-block guard already handles the dangerous case, so the early return in instr_is_inline_expressible was conservative overhead"
  - "bug_vla_goto_bypass pre-existing failure: confirmed failing before phase 26 — unrelated to LOAD inlining; deferred to separate fix"

patterns-established:
  - "When removing a safety guard, confirm via git stash that the target test failure was NOT caused by the change before declaring it a regression"

requirements-completed: [EXPR-01, EXPR-02]

# Metrics
duration: 18min
completed: 2026-04-01
---

# Phase 26 Plan 01: Load Expression Inlining Summary

**Removed 4-line LOAD blanket exclusion from `instr_is_inline_expressible`, enabling same-block mutable variable loads to inline as sub-expressions in generated C**

## Performance

- **Duration:** 18 min
- **Started:** 2026-04-01T10:53:08Z
- **Completed:** 2026-04-01T11:11:18Z
- **Tasks:** 2 (1 code change + validation)
- **Files modified:** 3

## Accomplishments

- Removed 4-line LOAD guard block (`comment + if (kind == IRON_LIR_LOAD) return false;`) from `instr_is_inline_expressible` in `src/lir/lir_optimize.c`
- Same-block LOADs now pass through to the existing cross-block guard, which correctly excludes only cross-block LOADs
- Added `expr_inline_load.iron` regression test (4 scenarios: sum of vars, doubled var, cross-block conditional, triple load) — outputs 30/10/42/21
- Full suite: 166/167 integration, 12/13 algorithms, 3/3 composite — both failures are pre-existing and unrelated to this change

## Task Commits

Each task was committed atomically:

1. **Task 1: Add LOAD inlining regression test and remove blanket exclusion** - `687bcba` (feat)
2. **Task 2: Full test suite validation** - (validation only, no code changes)

**Plan metadata:** (docs commit follows)

## Files Created/Modified

- `src/lir/lir_optimize.c` — Removed LOAD blanket exclusion from `instr_is_inline_expressible` (lines 1463-1466 deleted)
- `tests/integration/expr_inline_load.iron` — New regression test: LOAD inlining with same-block and cross-block scenarios
- `tests/integration/expr_inline_load.expected` — Expected output: 30, 10, 42, 21

## Decisions Made

- **LOAD blanket exclusion removed**: The original comment cited cross-block undeclared-variable errors as the reason. The cross-block guard at lir_optimize.c:1779-1785 already handles this — it checks `use_site_block[ub_idx].value != blk->id` and skips cross-block LOADs. The blanket exclusion was redundant conservative overhead.
- **bug_vla_goto_bypass treated as pre-existing**: Confirmed via `git stash` that this test fails on commit `c333493` (the last commit before phase 26 began). The failure is a separate VLA declaration hoisting issue unrelated to LOAD inlining.

## Deviations from Plan

None - plan executed exactly as written. The `bug_vla_goto_bypass` failure was pre-existing and documented in `deferred-items.md`.

## Issues Encountered

- `bug_vla_goto_bypass` integration test fails at runtime (exit code 133). Investigation confirmed this is a pre-existing failure: the generated C frees an `Iron_List` value in the early-return block before it was initialized (goto bypasses initialization). This is a separate issue requiring VLA declaration hoisting to function entry, not caused by LOAD inlining. Documented in `deferred-items.md`.
- `concurrent_hash_map` algorithm test fails at build. Also pre-existing.

## User Setup Required

None - no external service configuration required.

## Next Phase Readiness

- EXPR-01 and EXPR-02 requirements satisfied
- Phase 27 (function inlining) can proceed; LOAD inlining reduces verbosity of generated C for mutable variable patterns, which may affect how inlined function bodies look
- The `bug_vla_goto_bypass` failure should be addressed before or during Phase 28 (phi coalescing), as it covers the same VLA/goto interaction

## Self-Check: PASSED

- `src/lir/lir_optimize.c`: FOUND
- `tests/integration/expr_inline_load.iron`: FOUND
- `tests/integration/expr_inline_load.expected`: FOUND
- `26-01-SUMMARY.md`: FOUND
- Commit `687bcba`: FOUND
- LOAD guard removed: VERIFIED (grep returns 0 matches for `IRON_LIR_LOAD.*return false` in `instr_is_inline_expressible`)

---
*Phase: 26-load-expression-inlining*
*Completed: 2026-04-01*
