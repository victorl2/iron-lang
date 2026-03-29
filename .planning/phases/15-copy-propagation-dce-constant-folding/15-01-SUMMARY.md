---
phase: 15-copy-propagation-dce-constant-folding
plan: 01
subsystem: ir
tags: [ir-optimizer, ir-emit, c-codegen, array-optimization, phi-elimination]

# Dependency graph
requires:
  - phase: 14-dependency-resolution-lockfile
    provides: stable build system and project infrastructure
provides:
  - iron_ir_optimize() public API callable on any IronIR_Module
  - IronIR_OptimizeInfo bridge struct carrying optimizer output to emitter
  - ArrayParamMode enum in ir_optimize.h
  - emit_c.c pure emission (no transformation logic)
  - --dump-ir-passes and --no-optimize CLI flags
affects: [15-copy-propagation-dce-constant-folding, any future IR optimization work]

# Tech tracking
tech-stack:
  added: [src/ir/ir_optimize.h, src/ir/ir_optimize.c]
  patterns:
    - Separation of optimization pipeline from emission: iron_ir_optimize() runs before iron_ir_emit_c()
    - IronIR_OptimizeInfo as bridge struct between optimizer and emitter
    - Per-task atomic commits with clear scope boundaries

key-files:
  created:
    - src/ir/ir_optimize.h
    - src/ir/ir_optimize.c
  modified:
    - src/ir/emit_c.h
    - src/ir/emit_c.c
    - src/cli/build.h
    - src/cli/build.c
    - src/cli/main.c
    - CMakeLists.txt
    - tests/ir/test_ir_emit.c

key-decisions:
  - "IronIR_OptimizeInfo carries both module-level (array_param_modes, revoked_fill_ids) and per-function tracking maps (stack_array_ids, heap_array_ids, escaped_heap_ids) — unified ownership simplifies API"
  - "iron_ir_get_array_param_mode() is non-static: stb_ds shgeti modifies map header internally, const would cause compiler error"
  - "iron_ir_optimize() accepts Iron_Arena* parameter because IronIR_Module has no module-level arena field (arena is per-function)"

patterns-established:
  - "Optimization passes run in iron_ir_optimize() before emission; emit_c.c does zero transformation"
  - "OptimizeInfo maps owned by caller (build.c), freed via iron_ir_optimize_info_free() after emission"

requirements-completed: [INFRA-02]

# Metrics
duration: 45min
completed: 2026-03-29
---

# Phase 15 Plan 01: IR Optimize Module Infrastructure Summary

**ir_optimize module extracts phi elimination, array param mode analysis, and array repr optimization from emit_c.c into a standalone pipeline; emit_c.c is now pure emission with --dump-ir-passes and --no-optimize CLI flags wired**

## Performance

- **Duration:** ~45 min
- **Started:** 2026-03-29T12:00:00Z
- **Completed:** 2026-03-29T12:45:00Z
- **Tasks:** 2
- **Files modified:** 9

## Accomplishments
- Created `src/ir/ir_optimize.h` and `src/ir/ir_optimize.c` with full extracted pass implementations (phi_eliminate, analyze_array_param_modes, optimize_array_repr)
- `emit_c.c` now contains zero transformation logic — phi elimination comment is the only trace (`/* Should never reach here — phi_eliminate() runs before emission */`)
- `iron_ir_emit_c()` signature updated to accept `IronIR_OptimizeInfo*`; build.c calls `iron_ir_optimize()` before emission
- `--dump-ir-passes` and `--no-optimize` CLI flags wired in main.c and build.c
- All 38 non-benchmark tests pass (100%)

## Task Commits

Each task was committed atomically:

1. **Task 1: Create ir_optimize.h/c with extracted passes and OptimizeInfo struct** - `b991ea8` (feat)
2. **Task 2: Wire ir_optimize into build.c and add CLI flags** - `2f716a0` (feat)

## Files Created/Modified
- `src/ir/ir_optimize.h` - Public API: iron_ir_optimize(), iron_ir_instr_is_pure(), IronIR_OptimizeInfo, ArrayParamMode, iron_ir_optimize_info_free(), iron_ir_get_array_param_mode()
- `src/ir/ir_optimize.c` - All IR transformation passes, ~470 lines
- `src/ir/emit_c.h` - Updated signature: iron_ir_emit_c() now takes IronIR_OptimizeInfo*
- `src/ir/emit_c.c` - Removed transformation functions; replaced 5 map fields with opt_info pointer; removed optimization calls from entry point
- `src/cli/build.h` - Added dump_ir_passes and no_optimize to IronBuildOpts
- `src/cli/build.c` - Added iron_ir_optimize() call, iron_ir_optimize_info_free() cleanup on all paths
- `src/cli/main.c` - Added --dump-ir-passes and --no-optimize flag parsing in both build and run commands
- `CMakeLists.txt` - Added src/ir/ir_optimize.c to iron_compiler STATIC sources
- `tests/ir/test_ir_emit.c` - Updated 6 test functions to call iron_ir_optimize() before iron_ir_emit_c()

## Decisions Made
- `IronIR_OptimizeInfo` carries all 5 maps (including per-function tracking maps) because it simplifies ownership: build.c allocates once, emitter mutates per-function maps in-place, build.c frees at the end.
- `iron_ir_get_array_param_mode()` cannot take `const IronIR_OptimizeInfo*` — stb_ds `shgeti` macro modifies the hash table header internally, so the parameter must be non-const.
- `iron_ir_optimize()` accepts `Iron_Arena *arena` because `IronIR_Module` has no module-level arena (`arena` is stored per-function). The arena is needed by `make_param_mode_key()` for string key allocation.

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 3 - Blocking] Updated tests/ir/test_ir_emit.c to use new iron_ir_emit_c() signature**
- **Found during:** Task 2 (build attempt)
- **Issue:** 6 test functions called `iron_ir_emit_c(mod, &out_arena, &g_diags)` with the old 3-argument signature
- **Fix:** Added `#include "ir/ir_optimize.h"`, added `iron_ir_optimize()` call before each `iron_ir_emit_c()` call, added `iron_ir_optimize_info_free()` in cleanup
- **Files modified:** tests/ir/test_ir_emit.c
- **Verification:** All 5 IR tests pass, 100% test suite pass rate
- **Committed in:** b991ea8 (Task 1 commit)

---

**Total deviations:** 1 auto-fixed (1 blocking)
**Impact on plan:** Required for compilation. No scope creep — tests were updated minimally to match the new API.

## Issues Encountered
- `const IronIR_OptimizeInfo *` caused a compiler error because stb_ds `shgeti` macro internally writes to the hash table header. Resolved by removing `const` from the parameter in `iron_ir_get_array_param_mode()`.

## Next Phase Readiness
- Optimization pipeline infrastructure is complete and wired
- Plan 02 can add copy propagation, constant folding, and DCE as new passes in `iron_ir_optimize()` — the stub fixpoint loop is already in place
- `iron_ir_instr_is_pure()` is available for DCE pass implementation

## Self-Check: PASSED

All created files confirmed on disk. All task commits confirmed in git history.

---
*Phase: 15-copy-propagation-dce-constant-folding*
*Completed: 2026-03-29*
