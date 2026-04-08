---
phase: 52-emitter-refactoring
plan: 03
subsystem: compiler
tags: [c-emitter, refactoring, code-extraction, emit-split, emit-fusion, split-collections, loop-fusion]

requires:
  - phase: 52-emitter-refactoring
    provides: emit_helpers.h with EmitCtx struct and shared utility declarations (Plan 01), emit_structs.c with type declaration emission (Plan 02)

provides:
  - emit_split.h/c with split collection prescan, arena helpers, and per-interface struct/push/free emission
  - emit_fusion.h/c with fused loop emission for chained collection operations
  - Complete 4-module emitter decomposition (helpers, structs, split, fusion)
  - emit_c.c reduced to core concerns (5114 lines from original 7120)

affects: [53-analysis-improvements, 54-test-hardening]

tech-stack:
  added: []
  patterns: [split collection emission isolated in emit_split module, fusion emission isolated in emit_fusion module, emit_expr_to_buf made non-static for cross-module access]

key-files:
  created:
    - src/lir/emit_split.h
    - src/lir/emit_split.c
    - src/lir/emit_fusion.h
    - src/lir/emit_fusion.c
  modified:
    - src/lir/emit_structs.c
    - src/lir/emit_c.c
    - CMakeLists.txt

key-decisions:
  - "emit_split_arena_helpers emitted once before per-interface loop; emit_split_collection_for_iface called per interface"
  - "emit_expr_to_buf made non-static in emit_c.c so emit_fusion.c can call it (declared in emit_fusion.h)"
  - "emit_fused_chain kept its original name (already has emit_ prefix)"

patterns-established:
  - "emit_split.h as the single include for split collection prescan and emission"
  - "emit_fusion.h as the single include for fused loop emission"
  - "Cross-module function visibility via header declarations (emit_expr_to_buf)"

requirements-completed: [EMIT-01, EMIT-02]

duration: 37min
completed: 2026-04-08
---

# Phase 52 Plan 03: Extract emit_split and emit_fusion Summary

**Extracted split collection emission (~620 lines) and fused loop emission (~400 lines) into dedicated modules, completing the 4-module emitter decomposition**

## Performance

- **Duration:** 37 min
- **Started:** 2026-04-08T18:46:39Z
- **Completed:** 2026-04-08T19:23:39Z
- **Tasks:** 3
- **Files modified:** 7

## Accomplishments
- Created emit_split.h/c with emit_prescan_split_collections (module-level ARRAY_LIT scan), emit_split_arena_helpers (tracked allocation helpers), and emit_split_collection_for_iface (per-interface struct/push/free generation)
- Created emit_fusion.h/c with emit_fused_chain (flat and split collection fused loop emission, ~400 lines)
- Updated emit_structs.c to delegate split collection code to emit_split module (reduced from 1073 to 527 lines)
- Updated emit_c.c: removed prescan_split_collections and emit_fused_chain definitions, made emit_expr_to_buf non-static (reduced from 5550 to 5114 lines)
- All 45 tests pass with zero regressions (integration tests 100% pass rate)
- Final emitter distribution: emit_c.c (5114), emit_helpers.c (400), emit_structs.c (527), emit_split.c (622), emit_fusion.c (398) = 7061 total

## Task Commits

Each task was committed atomically:

1. **Task 1: Create emit_split.h/c -- extract split collection emission** - `158a58f` (feat)
2. **Task 2: Create emit_fusion.h/c -- extract fused loop emission** - `f97e177` (feat)
3. **Task 3: Update CMakeLists.txt, full build, and test verification** - `d42f0cf` (chore)

## Files Created/Modified
- `src/lir/emit_split.h` - Public API: emit_prescan_split_collections, emit_split_arena_helpers, emit_split_collection_for_iface
- `src/lir/emit_split.c` - Split collection prescan, arena helpers, per-interface struct/push/free (622 lines)
- `src/lir/emit_fusion.h` - Public API: emit_fused_chain, emit_expr_to_buf cross-module declaration
- `src/lir/emit_fusion.c` - Fused loop emission for flat and split collections (398 lines)
- `src/lir/emit_structs.c` - Removed split collection code, now delegates to emit_split (527 lines, was 1073)
- `src/lir/emit_c.c` - Removed prescan and fused chain, emit_expr_to_buf non-static (5114 lines, was 5550)
- `CMakeLists.txt` - Added emit_split.c and emit_fusion.c to iron_compiler library

## Decisions Made
- Split the arena-tracked allocation helpers into a separate `emit_split_arena_helpers` function called once before the interface loop, rather than embedding them in the per-interface function
- Made `emit_expr_to_buf` non-static (was internal to emit_c.c) and declared it in emit_fusion.h so emit_fusion.c can call it for expression inlining within fused loops
- Kept `emit_fused_chain` name unchanged since it already followed the emit_ prefix convention

## Deviations from Plan

None - plan executed exactly as written.

## Issues Encountered
None

## User Setup Required
None - no external service configuration required.

## Next Phase Readiness
- Phase 52 emitter refactoring is now complete: all 4 sub-modules (helpers, structs, split, fusion) extracted
- emit_c.c is focused on core concerns: main entry point, instruction emission, function bodies, expression inlining
- Each emission concern is independently readable in its own file
- Ready for Phase 53 (analysis improvements) and Phase 54 (test hardening)

---
*Phase: 52-emitter-refactoring*
*Completed: 2026-04-08*
