---
phase: 52-emitter-refactoring
plan: 02
subsystem: compiler
tags: [c-emitter, refactoring, code-extraction, emit-structs, type-declarations]

requires:
  - phase: 52-emitter-refactoring
    provides: emit_helpers.h with EmitCtx struct and shared utility declarations (Plan 01)

provides:
  - emit_structs.h with emit_type_decls and emit_estimate_type_size API
  - emit_structs.c with all type declaration emission (topo sort, struct bodies, tagged unions, split collections, enums)
  - emit_c.c reduced by ~1050 lines, no longer contains struct/type declaration generation

affects: [52-03, emit_split, emit_fusion]

tech-stack:
  added: []
  patterns: [type declaration emission isolated in emit_structs module, emit_estimate_type_size public API for variant size analysis]

key-files:
  created:
    - src/lir/emit_structs.h
    - src/lir/emit_structs.c
  modified:
    - src/lir/emit_c.c
    - CMakeLists.txt

key-decisions:
  - "Move emit_type_decls as a single unit including split collection generation (Plan 03 will extract split portions)"
  - "Rename estimate_type_size to emit_estimate_type_size for public API consistency"
  - "Internal helpers (IrTopoState, ir_topo_visit, find_ir_type_decl_idx, ir_has_subtype, emit_object_struct_body) remain static in emit_structs.c"

patterns-established:
  - "emit_structs.h as the single include for type declaration emission"
  - "emit_estimate_type_size as public API for variant size estimation (used by emit_structs internally, available for future modules)"

requirements-completed: [EMIT-03]

duration: 36min
completed: 2026-04-08
---

# Phase 52 Plan 02: Extract emit_structs Summary

**Extracted type declaration emission (~1050 lines) from emit_c.c into emit_structs.c/h with topo sort, tagged unions, split collection structs, and enum layouts**

## Performance

- **Duration:** 36 min
- **Started:** 2026-04-08T18:07:28Z
- **Completed:** 2026-04-08T18:44:26Z
- **Tasks:** 2
- **Files modified:** 4

## Accomplishments
- Created emit_structs.h with emit_type_decls and emit_estimate_type_size public API
- Created emit_structs.c (1073 lines) with IrTopoState, topo sort, object struct body emission, interface tagged union generation (tag enums, data unions, wrapping constructors), split collection struct generation (per-type sub-arrays, push/free functions), and enum/ADT layouts
- Updated emit_c.c: removed extracted code, added emit_structs.h include; reduced from 6600 to 5550 lines
- All 44 tests pass including integration tests with zero regressions

## Task Commits

Each task was committed atomically:

1. **Task 1: Create emit_structs.h/c with type declaration emission** - `bf7e6a9` (feat)
2. **Task 2: Update emit_c.c and CMakeLists.txt, verify all tests** - `4dcf506` (feat)

## Files Created/Modified
- `src/lir/emit_structs.h` - Public API: emit_type_decls, emit_estimate_type_size
- `src/lir/emit_structs.c` - All type declaration emission code (1073 lines)
- `src/lir/emit_c.c` - Removed extracted code, added emit_structs.h include
- `CMakeLists.txt` - Added emit_structs.c to iron_compiler library

## Decisions Made
- Moved emit_type_decls as a single unit (including split collection generation) to emit_structs.c; Plan 03 will extract the split-specific portions into emit_split.c
- Renamed estimate_type_size to emit_estimate_type_size for consistent public API naming
- Internal helpers remain static -- only emit_type_decls and emit_estimate_type_size are exposed via header

## Deviations from Plan

None - plan executed exactly as written.

## Issues Encountered
None

## User Setup Required
None - no external service configuration required.

## Next Phase Readiness
- emit_structs.c now contains split collection generation code (~500 lines) ready for extraction to emit_split.c in Plan 03
- emit_c.c is 5550 lines and still contains interface dispatch functions (Phase 3b in iron_lir_emit_c) and all instruction emission
- No blockers for Plan 03

---
*Phase: 52-emitter-refactoring*
*Completed: 2026-04-08*
