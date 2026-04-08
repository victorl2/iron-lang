---
phase: 50-value-range-compression-arena-allocation
plan: 02
subsystem: codegen
tags: [arena, allocation, split-collection, memory-management, pointer-registry]

# Dependency graph
requires:
  - phase: 48-layout-optimizations
    provides: Split collection struct emission, SoA/AoS push functions, free function
provides:
  - Iron_Arena pointer registry (iron_arena_track, iron_arena_realloc_tracked)
  - Arena-tracked split collection allocation with 1.5x geometric growth
  - Single bulk free operation for all split collection sub-arrays
affects: [50-03 integration tests, future collection runtime improvements]

# Tech tracking
tech-stack:
  added: []
  patterns:
    - "Inline static helper emission for generated C runtime code (_iron_sl_track, _iron_sl_realloc_tracked, _iron_sl_free_all)"
    - "Pointer registry pattern for bulk deallocation without full arena overhead"

key-files:
  created: []
  modified:
    - src/util/arena.h
    - src/util/arena.c
    - src/lir/emit_c.c

key-decisions:
  - "Emit tracking helpers as static inline in generated C rather than linking arena.c (generated code is standalone)"
  - "Inline pointer registry in SplitList struct (_tracked, _tracked_count, _tracked_cap) instead of embedding full Iron_Arena"
  - "1.5x geometric growth factor (per plan specification) instead of 2x"

patterns-established:
  - "Split collection tracking: _tracked/count/cap fields + _iron_sl_realloc_tracked for all sub-array allocations"
  - "Bulk free via _iron_sl_free_all replacing per-array free calls in generated free functions"

requirements-completed: [ARENA-01, ARENA-02]

# Metrics
duration: 11min
completed: 2026-04-08
---

# Phase 50 Plan 02: Arena Allocation with Pointer Registry Summary

**Iron_Arena extended with pointer registry; split collection sub-arrays use arena-tracked allocation with 1.5x growth and single bulk free**

## Performance

- **Duration:** 11 min
- **Started:** 2026-04-08T11:18:19Z
- **Completed:** 2026-04-08T11:29:34Z
- **Tasks:** 2
- **Files modified:** 3

## Accomplishments
- Extended Iron_Arena struct with tracked pointer registry (tracked_ptrs, tracked_count, tracked_cap)
- Implemented iron_arena_track() and iron_arena_realloc_tracked() for pointer lifecycle management
- Updated iron_arena_free() to release all tracked pointers before chunk walk
- Emitted static inline _iron_sl_track, _iron_sl_realloc_tracked, _iron_sl_free_all helpers in generated C
- Added _tracked/_tracked_count/_tracked_cap fields to SplitList struct
- Replaced all realloc calls in push functions (SoA, AoS, common field, order index) with _iron_sl_realloc_tracked
- Changed growth factor from 2x to 1.5x across all push functions
- Replaced per-array free calls with single _iron_sl_free_all in generated free function

## Task Commits

Each task was committed atomically:

1. **Task 1: Extend Iron_Arena with pointer registry** - `496e90a` (feat)
2. **Task 2: Integrate arena-tracked allocation into split collections** - `9ad513d` (feat, bundled with 50-01 integration)

## Files Created/Modified
- `src/util/arena.h` - Added tracked_ptrs/count/cap fields, iron_arena_track and iron_arena_realloc_tracked declarations
- `src/util/arena.c` - Implemented tracking functions, updated iron_arena_free and iron_arena_create
- `src/lir/emit_c.c` - Emitted tracking helpers, tracking struct fields, replaced realloc with tracked realloc, replaced per-array free with bulk free

## Decisions Made
- Emitted tracking helpers as static inline in generated C code rather than linking the compiler's arena.c into the runtime (generated C is standalone)
- Used inline _tracked fields in SplitList struct instead of embedding a full Iron_Arena (lighter weight, no header dependency in generated code)
- 1.5x geometric growth factor as specified in plan

## Deviations from Plan

None - plan executed exactly as written. Both tasks were already implemented as part of the prior session's arena + integration work.

## Issues Encountered
None

## User Setup Required
None - no external service configuration required.

## Next Phase Readiness
- Arena tracking and bulk free are in place for all split collection sub-arrays
- Ready for 50-03 integration tests to validate arena-tracked allocation behavior
- All existing tests (arena, LIR emit, integration) continue to pass

## Self-Check: PASSED

- All source files exist (arena.h, arena.c, emit_c.c)
- Both commits verified (496e90a, 9ad513d)
- arena.h contains 2 tracked allocation declarations
- emit_c.c contains 5 _iron_sl_realloc_tracked calls and 2 _iron_sl_free_all calls
- Build succeeds with no errors
- test_arena, test_lir_emit, test_integration all pass

---
*Phase: 50-value-range-compression-arena-allocation*
*Completed: 2026-04-08*
