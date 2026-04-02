---
phase: 32-lir-verifier-hardening
plan: 02
subsystem: lir-verifier
tags: [lir, call, type-checking, argument-validation, verifier, diagnostics]

# Dependency graph
requires:
  - phase: 32-01
    provides: "Invariant 7 PHI type check, IRON_ERR_LIR_PHI_TYPE_MISMATCH (306), verify_func with module parameter"
provides:
  - "Invariant 8: call argument count and type validation in LIR verifier"
  - "IRON_ERR_LIR_CALL_TYPE_MISMATCH error code (307)"
  - "find_func_by_name helper for callee lookup"
  - "Tests for call arg count mismatch, type mismatch, well-formed, and indirect skip"
affects: [emit-c, lowering]

# Tech tracking
tech-stack:
  added: []
  patterns: ["Call argument validation via callee lookup by name in module->funcs, then count/type comparison"]

key-files:
  created: []
  modified:
    - src/diagnostics/diagnostics.h
    - src/lir/verify.c
    - tests/lir/test_lir_verify.c

key-decisions:
  - "Linear scan of module->funcs by name for callee lookup -- sufficient for verification pass performance"
  - "Skip indirect calls (func_decl == NULL) silently since LIR lacks function type signatures for indirect calls (tracked as AGEN-01)"
  - "Count mismatch uses continue to skip per-arg type checks -- avoids out-of-bounds on mismatched arrays"

patterns-established:
  - "Callee lookup pattern: find_func_by_name(module, func_decl->name) for cross-function validation"

requirements-completed: [LIR-03, LIR-04, LIR-05]

# Metrics
duration: 2min
completed: 2026-04-02
---

# Phase 32 Plan 02: Call Argument Validation Summary

**LIR verifier Invariant 8 rejects direct calls with wrong argument count or types, emitting error code 307**

## Performance

- **Duration:** 2 min
- **Started:** 2026-04-02T23:12:48Z
- **Completed:** 2026-04-02T23:15:26Z
- **Tasks:** 2
- **Files modified:** 3

## Accomplishments
- Defined IRON_ERR_LIR_CALL_TYPE_MISMATCH (code 307) in diagnostics.h
- Added find_func_by_name helper to verify.c for callee lookup by name
- Implemented Invariant 8 in verify.c: validates call arg count and types for direct calls, skips indirect calls
- Added test_verify_call_arg_count_mismatch (callee expects 2 args, caller passes 1 -- triggers 307)
- Added test_verify_call_arg_type_mismatch (callee expects Int, caller passes Bool -- triggers 307)
- Added test_verify_call_well_formed (correct count and types pass with 0 errors)
- Added test_verify_call_indirect_skipped (func_decl=NULL does not trigger 307)
- All 14 verifier tests pass (10 pre-existing + 4 new)

## Task Commits

Each task was committed atomically:

1. **Task 1: Add error code and call validation tests (RED)** - `515c7c2` (test)
2. **Task 2: Implement Invariant 8 call argument validation (GREEN)** - `b0c9a74` (feat)

## Files Created/Modified
- `src/diagnostics/diagnostics.h` - Added IRON_ERR_LIR_CALL_TYPE_MISMATCH (307) error code
- `src/lir/verify.c` - Added find_func_by_name helper and Invariant 8 call argument validation
- `tests/lir/test_lir_verify.c` - Added 4 call validation tests (count mismatch, type mismatch, well-formed, indirect skip)

## Decisions Made
- Linear scan of module->funcs for callee lookup is sufficient for the verification pass (no need for hash map)
- Indirect calls skipped silently since LIR has no function type signature for them (tracked as AGEN-01)
- Count mismatch uses `continue` to skip per-arg type checks, avoiding out-of-bounds access on mismatched arrays

## Deviations from Plan

None - plan executed exactly as written.

## Issues Encountered
None

## User Setup Required
None - no external service configuration required.

## Next Phase Readiness
- Invariant 8 is complete and tested
- Phase 32 (LIR Verifier Hardening) is fully complete (both plans done)
- All 14 verifier tests pass with 0 failures

---
*Phase: 32-lir-verifier-hardening*
*Completed: 2026-04-02*
