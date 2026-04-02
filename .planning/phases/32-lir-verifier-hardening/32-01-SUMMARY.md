---
phase: 32-lir-verifier-hardening
plan: 01
subsystem: lir-verifier
tags: [lir, phi, type-checking, ssa, verifier, diagnostics]

# Dependency graph
requires: []
provides:
  - "Invariant 7: PHI type consistency check in LIR verifier"
  - "IRON_ERR_LIR_PHI_TYPE_MISMATCH error code (306)"
  - "Tests for PHI type mismatch detection and well-formed PHI acceptance"
affects: [32-02, emit-c, lowering]

# Tech tracking
tech-stack:
  added: []
  patterns: ["PHI incoming value type validation via iron_type_equals against PHI result type"]

key-files:
  created: []
  modified:
    - src/diagnostics/diagnostics.h
    - src/lir/verify.c
    - tests/lir/test_lir_verify.c

key-decisions:
  - "Used function name and block label in PHI mismatch diagnostic messages for debuggability"
  - "Skipped iron_type_to_string in error messages to keep stack-buffer snprintf simple and avoid arena allocation in error path"

patterns-established:
  - "PHI invariant check pattern: iterate phi.values, skip INVALID and synthetic params, lookup value_table, compare types with iron_type_equals"

requirements-completed: [LIR-01, LIR-02]

# Metrics
duration: 2min
completed: 2026-04-02
---

# Phase 32 Plan 01: PHI Type Consistency Summary

**LIR verifier Invariant 7 rejects PHI nodes whose incoming value types differ from the PHI result type, emitting error code 306**

## Performance

- **Duration:** 2 min
- **Started:** 2026-04-02T23:08:22Z
- **Completed:** 2026-04-02T23:10:38Z
- **Tasks:** 2
- **Files modified:** 3

## Accomplishments
- Defined IRON_ERR_LIR_PHI_TYPE_MISMATCH (code 306) in diagnostics.h
- Implemented Invariant 7 in verify.c: iterates PHI incoming values, compares types via iron_type_equals, emits diagnostic on mismatch
- Added test_verify_phi_type_mismatch (PHI with Bool result but Int incoming triggers 306)
- Added test_verify_phi_well_formed (matching types pass with 0 errors)
- All 10 verifier tests pass (8 pre-existing + 2 new)

## Task Commits

Each task was committed atomically:

1. **Task 1: Add error code and PHI type mismatch tests (RED)** - `ee92fd9` (test)
2. **Task 2: Implement Invariant 7 PHI type consistency (GREEN)** - `47854d5` (feat)

## Files Created/Modified
- `src/diagnostics/diagnostics.h` - Added IRON_ERR_LIR_PHI_TYPE_MISMATCH (306) error code
- `src/lir/verify.c` - Added Invariant 7 PHI type consistency check loop after Invariant 6
- `tests/lir/test_lir_verify.c` - Added test_verify_phi_type_mismatch and test_verify_phi_well_formed

## Decisions Made
- Used function name and block label in diagnostic message for PHI mismatch rather than type names, keeping the snprintf simple and avoiding arena allocation in error paths
- Followed existing param-skip pattern (1..param_count) to avoid false positives on synthetic parameter ValueIds

## Deviations from Plan

None - plan executed exactly as written.

## Issues Encountered
None

## User Setup Required
None - no external service configuration required.

## Next Phase Readiness
- Invariant 7 is complete and tested
- Ready for plan 32-02 (next LIR verifier hardening work)
- All pre-existing tests continue to pass

---
*Phase: 32-lir-verifier-hardening*
*Completed: 2026-04-02*
