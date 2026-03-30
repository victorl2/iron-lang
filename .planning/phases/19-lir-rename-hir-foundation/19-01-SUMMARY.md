---
phase: 19-lir-rename-hir-foundation
plan: 01
subsystem: infra
tags: [rename, lir, ir, cmake, c]

# Dependency graph
requires: []
provides:
  - "src/lir/ directory with all LIR source files (lir.h, lir.c, lir_optimize.h, lir_optimize.c, lower.*, emit_c.*, print.*, verify.*)"
  - "IronLIR_ type prefix, IRON_LIR_ constant prefix, iron_lir_ function prefix throughout codebase"
  - "IRON_ERR_LIR_* error codes in diagnostics.h"
  - "tests/lir/ directory with all LIR test files using test_lir_* naming"
affects: [HIR, hir, lir, phase-20, phase-21]

# Tech tracking
tech-stack:
  added: []
  patterns:
    - "LIR (Lower IR) naming convention: IronLIR_/IRON_LIR_/iron_lir_ for the SSA+CFG IR layer"
    - "HIR namespace (IronHIR_/IRON_HIR_/iron_hir_) now clear for future high-level IR"

key-files:
  created: []
  modified:
    - "src/lir/lir.h"
    - "src/lir/lir.c"
    - "src/lir/lir_optimize.h"
    - "src/lir/lir_optimize.c"
    - "src/lir/lower.h"
    - "src/lir/lower.c"
    - "src/lir/lower_internal.h"
    - "src/lir/lower_exprs.c"
    - "src/lir/lower_stmts.c"
    - "src/lir/lower_types.c"
    - "src/lir/emit_c.h"
    - "src/lir/emit_c.c"
    - "src/lir/print.h"
    - "src/lir/print.c"
    - "src/lir/verify.h"
    - "src/lir/verify.c"
    - "src/cli/build.c"
    - "src/diagnostics/diagnostics.h"
    - "CMakeLists.txt"
    - "tests/lir/CMakeLists.txt"
    - "tests/lir/test_lir_data.c"
    - "tests/lir/test_lir_emit.c"
    - "tests/lir/test_lir_lower.c"
    - "tests/lir/test_lir_optimize.c"
    - "tests/lir/test_lir_print.c"
    - "tests/lir/test_lir_verify.c"

key-decisions:
  - "IRON_ERR_IR_* error codes renamed IRON_ERR_LIR_* (not caught by IRON_IR_ pattern — required explicit separate sed pass)"
  - "Snapshot paths in test_lir_print.c and test_lir_lower.c hardcoded 'tests/ir/snapshots/' — updated to 'tests/lir/snapshots/' as auto-fix"

patterns-established:
  - "LIR layer uses IronLIR_/IRON_LIR_/iron_lir_ prefix convention"
  - "HIR namespace reserved: IronHIR_/IRON_HIR_/iron_hir_ available for new high-level IR"

requirements-completed: [INFRA-01]

# Metrics
duration: 15min
completed: 2026-03-29
---

# Phase 19 Plan 01: LIR Rename Summary

**Renamed src/ir/ -> src/lir/ and all IronIR_/IRON_IR_/iron_ir_ symbols to IronLIR_/IRON_LIR_/iron_lir_ across the entire codebase, clearing the HIR namespace for future high-level IR**

## Performance

- **Duration:** ~15 min
- **Started:** 2026-03-29T11:42:55Z
- **Completed:** 2026-03-29T11:57:00Z
- **Tasks:** 2
- **Files modified:** 28

## Accomplishments
- Moved entire src/ir/ directory to src/lir/ with git mv (full rename tracking)
- Renamed ir.h/ir.c/ir_optimize.h/ir_optimize.c to lir.h/lir.c/lir_optimize.h/lir_optimize.c
- Renamed all 6 test files from test_ir_*.c to test_lir_*.c in tests/lir/
- Applied three global symbol renames (IronIR_, IRON_IR_, iron_ir_) across all 26 source/test files
- Updated IRON_ERR_IR_* error codes in diagnostics.h and verify.c to IRON_ERR_LIR_*
- Updated all include paths from "ir/*" to "lir/*"
- Updated CMakeLists.txt source list, test subdirectory, and test targets
- All 39 non-benchmark tests pass; zero compiler warnings

## Task Commits

Each task was committed atomically:

1. **Task 1: Move directories and rename all files** - `9dc36ec` (chore)
2. **Task 2: Rename all symbols, includes, and CMake references** - `8e74927` (feat)

## Files Created/Modified
- `src/lir/lir.h` - Renamed from ir.h; IronLIR_ types and IRON_LIR_ constants
- `src/lir/lir.c` - Renamed from ir.c; iron_lir_ constructor functions
- `src/lir/lir_optimize.h` - Renamed from ir_optimize.h; IronLIR_OptimizeInfo
- `src/lir/lir_optimize.c` - Renamed from ir_optimize.c; iron_lir_optimize()
- `src/lir/verify.c` - IRON_ERR_LIR_* error codes
- `src/diagnostics/diagnostics.h` - IRON_ERR_LIR_* error code definitions
- `CMakeLists.txt` - src/lir/lir.c, src/lir/lir_optimize.c, add_subdirectory(tests/lir)
- `tests/lir/CMakeLists.txt` - test_lir_* targets, LABELS "lir"
- `tests/lir/test_lir_print.c` - Snapshot paths updated to tests/lir/snapshots/
- `tests/lir/test_lir_lower.c` - Snapshot paths updated to tests/lir/snapshots/
- All other src/lir/*.c, src/lir/*.h, tests/lir/test_lir_*.c - Symbol renames applied

## Decisions Made
- IRON_ERR_IR_* codes required a separate sed pass from IRON_IR_* because `IRON_ERR_IR_` does not contain the substring `IRON_IR_` — the pattern is `IRON_` + `ERR_IR_`, not `IRON_IR_` prefixed.

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 3 - Blocking] Hardcoded snapshot paths in test files**
- **Found during:** Task 2 (symbol renames)
- **Issue:** test_lir_print.c and test_lir_lower.c had hardcoded `"tests/ir/snapshots/"` paths in snapshot test calls; after directory move these paths no longer existed, causing 9 snapshot tests to fail
- **Fix:** Applied `sed -i '' 's|tests/ir/snapshots/|tests/lir/snapshots/|g'` to both test files
- **Files modified:** tests/lir/test_lir_print.c, tests/lir/test_lir_lower.c
- **Verification:** All 6 lir tests pass (was 4/6 before fix)
- **Committed in:** 8e74927 (Task 2 commit)

**2. [Rule 3 - Blocking] IRON_ERR_IR_* error codes missed by initial sed pass**
- **Found during:** Task 2 verification
- **Issue:** `IRON_ERR_IR_MISSING_TERMINATOR` and 5 similar codes in diagnostics.h, verify.c, and test_lir_verify.c were not renamed because `IRON_ERR_IR_` does not contain the substring `IRON_IR_`
- **Fix:** Explicit sed pass replacing all 6 IRON_ERR_IR_* -> IRON_ERR_LIR_* patterns in all three files
- **Files modified:** src/diagnostics/diagnostics.h, src/lir/verify.c, tests/lir/test_lir_verify.c
- **Verification:** grep confirms zero IRON_ERR_IR_ remaining; build and tests pass
- **Committed in:** 8e74927 (Task 2 commit)

---

**Total deviations:** 2 auto-fixed (2 blocking)
**Impact on plan:** Both auto-fixes essential for correctness. No scope creep — both were direct consequences of the directory rename operation.

## Issues Encountered
- The `IRON_ERR_IR_` pattern is structurally different from `IRON_IR_` (the `ERR_` is between `IRON_` and `IR_`), so the general sed pass for `IRON_IR_` -> `IRON_LIR_` correctly did not match it. The plan correctly listed these as separate items 18-23.

## User Setup Required
None - no external service configuration required.

## Next Phase Readiness
- LIR naming convention fully established throughout codebase
- HIR namespace (IronHIR_/IRON_HIR_/iron_hir_) is now clear and unambiguous
- All existing tests pass; build is clean
- Ready for Phase 19 Plan 02: HIR type definitions

---
*Phase: 19-lir-rename-hir-foundation*
*Completed: 2026-03-29*
