---
phase: 48-layout-optimizations
plan: 01
subsystem: compiler-backend
tags: [lir, emit-c, dead-field-elimination, layout-optimization, split-collections, reduced-storage]

# Dependency graph
requires:
  - phase: 41-collection-splitting
    provides: "Split collection infrastructure (SplitList structs, per-type sub-arrays)"
provides:
  - "LayoutAnalysis module for field usage tracking across split collections"
  - "Dead field elimination in split collection storage structs"
  - "Iron_SplitCollectionId typedef for type-safe split collection maps"
  - "Interprocedural method-based field analysis"
  - "Reduced storage typedefs (Iron_<Type>_Stor) excluding dead fields"
affects: [48-02-soa-layout, 48-03-common-field-factoring, 48-04-alignment-padding]

# Tech tracking
tech-stack:
  added: []
  patterns:
    - "Reduced storage struct pattern: Iron_<Type>_Stor for dead field elimination"
    - "Interprocedural field analysis: trace self.field through method implementations"
    - "Object reconstruction from reduced storage: zero-init + field copy for dispatch"

key-files:
  created:
    - src/lir/layout_analysis.h
    - src/lir/layout_analysis.c
    - tests/integration/layout_dead_field.iron
    - tests/integration/layout_dead_field.expected
  modified:
    - src/lir/emit_c.c
    - CMakeLists.txt

key-decisions:
  - "Interprocedural method analysis: scan interface method implementations for self.field accesses to determine which fields are used through collections"
  - "Union semantics for multi-collection: if ANY collection of an interface uses a field, include it in reduced storage"
  - "Object reconstruction approach: zero-init full concrete type + copy used fields from reduced storage, then wrap in tagged union for dispatch compatibility"
  - "Iron_SplitCollectionId typedef: named struct to avoid anonymous struct incompatibility across translation units"

patterns-established:
  - "LayoutAnalysis: analyzable field usage context available from prescan_split_collections before struct generation"
  - "Reduced storage: Iron_<Type>_Stor typedef with only used fields, push copies only used fields, loop reconstructs full object"

requirements-completed: [LAYOUT-02]

# Metrics
duration: 24min
completed: 2026-04-07
---

# Phase 48 Plan 01: Dead Field Elimination Summary

**Field access analysis with interprocedural method scanning eliminates unused fields from split collection storage structs, reducing memory footprint for interface-typed collections**

## Performance

- **Duration:** 24 min
- **Started:** 2026-04-07T20:24:22Z
- **Completed:** 2026-04-07T20:48:22Z
- **Tasks:** 2
- **Files modified:** 6

## Accomplishments
- Created layout_analysis.h/c with field usage tracking API supporting both direct field access and interprocedural method analysis
- Wired dead field elimination into emit_c.c: prescan identifies split collections module-wide, analysis determines used fields, struct generation emits reduced storage types
- Integration test proves Circle(radius, debug_name) + Square(side, debug_tag) eliminate debug_name/debug_tag from collection storage while maintaining correct program output

## Task Commits

Each task was committed atomically:

1. **Task 1: Create layout analysis module with field usage tracking** - `a2ffc06` (feat)
2. **Task 2: Wire dead field elimination into emit_c.c and add integration test** - `67ce183` (feat)

## Files Created/Modified
- `src/lir/layout_analysis.h` - LayoutAnalysis type, Iron_SplitCollectionId typedef, field usage query API
- `src/lir/layout_analysis.c` - Direct field access analysis + interprocedural method-based analysis
- `src/lir/emit_c.c` - prescan_split_collections(), reduced storage struct emission, field-selective push, loop reconstruction
- `CMakeLists.txt` - Added layout_analysis.c to iron_compiler build
- `tests/integration/layout_dead_field.iron` - Test: 2-field objects through interface, only 1 field used per type
- `tests/integration/layout_dead_field.expected` - Expected output: 118, 42, done

## Decisions Made
- **Interprocedural analysis**: The plan's direct GET_FIELD scanning only works when fields are accessed directly in the loop body. Since Iron accesses fields through methods (self.radius in Circle.area()), added a second analysis pass that scans method implementations for self field accesses and marks those as used for all collections of that interface.
- **Object reconstruction**: Instead of bypassing tagged union dispatch (which would require CALL instruction rewriting), reconstruct full concrete objects from reduced storage by zero-initializing dead fields, maintaining dispatch compatibility. This is simpler and correct.
- **Iron_SplitCollectionId typedef**: C anonymous structs are incompatible between translation units even if structurally identical. Introduced a named typedef to share the split collection ID map type between layout_analysis.h and emit_c.c.

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 1 - Bug] Anonymous struct type incompatibility between translation units**
- **Found during:** Task 1 (layout analysis module compilation)
- **Issue:** `struct { IronLIR_ValueId key; const char *value; }` used in header parameter list creates incompatible types between .h and .c files
- **Fix:** Introduced `Iron_SplitCollectionId` named typedef in layout_analysis.h
- **Files modified:** src/lir/layout_analysis.h, src/lir/layout_analysis.c
- **Verification:** Build succeeds with zero errors
- **Committed in:** a2ffc06 (Task 1 commit)

**2. [Rule 2 - Missing Critical] Interprocedural method analysis for field usage**
- **Found during:** Task 2 (integration test showed no reduced storage)
- **Issue:** Plan's intraprocedural analysis found no field accesses in loop body because fields are accessed through method calls (self.radius inside Circle.area()), not directly in the loop
- **Fix:** Added analyze_method_fields() pass: for each interface, find method implementations, scan for self.field accesses, mark as used for all collections of that interface
- **Files modified:** src/lir/layout_analysis.c
- **Verification:** Generated C shows Iron_Circle_Stor(radius only), test passes
- **Committed in:** 67ce183 (Task 2 commit)

**3. [Rule 1 - Bug] LIR function naming convention mismatch**
- **Found during:** Task 2 (method analysis not matching functions)
- **Issue:** Expected function names like `Iron_Circle_area` but LIR uses lowercase: `circle_area`
- **Fix:** Build lowercase type prefix for matching (`circle_` for Circle methods)
- **Files modified:** src/lir/layout_analysis.c
- **Verification:** Method analysis correctly finds circle_area accessing radius field
- **Committed in:** 67ce183 (Task 2 commit)

---

**Total deviations:** 3 auto-fixed (2 bugs, 1 missing critical)
**Impact on plan:** All auto-fixes necessary for correctness. The interprocedural analysis was essential for the feature to work with real Iron code patterns. No scope creep.

## Issues Encountered
None beyond the deviations documented above.

## User Setup Required
None - no external service configuration required.

## Next Phase Readiness
- Layout analysis infrastructure (LayoutAnalysis, iron_layout_is_field_used) ready for Plan 02 (SoA layout) and Plan 03 (common field factoring)
- reduced_storage_types map in EmitCtx available for Plan 02 to extend with SoA transformations
- All 251 integration tests passing, clean foundation for further layout optimizations

## Self-Check: PASSED

All files verified present, all commits verified in git log.

---
*Phase: 48-layout-optimizations*
*Completed: 2026-04-07*
