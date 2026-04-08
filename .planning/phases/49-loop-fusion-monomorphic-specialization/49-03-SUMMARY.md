---
phase: 49-loop-fusion-monomorphic-specialization
plan: 03
subsystem: compiler
tags: [monomorphic-specialization, split-collections, emit_c, optimization, lir]

# Dependency graph
requires:
  - phase: 49-loop-fusion-monomorphic-specialization
    provides: "Plans 01-02: @fusible annotation, chain detection, fused loop emission"
  - phase: 47-collection-methods
    provides: "Collection method dispatch (map/filter/reduce/forEach/sum)"
  - phase: 48-layout-optimizations
    provides: "Split collection tracking, SoA/AoS layout, field access analysis"
provides:
  - "Monomorphic collection detection pass (whole-program, conservative)"
  - "Monomorphic collapse: removes single-type collections from split_collection_ids"
  - "Specialization registry preventing duplicate dispatch function emission"
  - "3 monomorphic integration tests (single-type, multi-type, registry)"
affects: []

# Tech tracking
tech-stack:
  added: []
  patterns:
    - "Whole-program monomorphic analysis: scan ARRAY_LIT elements, track concrete types, mark single-type"
    - "Conservative scope: only local collections (no parameters, no escaping)"
    - "Collapse via split_collection_ids removal: monomorphic collections fall through to standard Iron_List path"

key-files:
  created:
    - tests/integration/mono_single_type_collapse.iron
    - tests/integration/mono_single_type_collapse.expected
    - tests/integration/mono_multi_type_no_collapse.iron
    - tests/integration/mono_multi_type_no_collapse.expected
    - tests/integration/mono_specialization_registry.iron
    - tests/integration/mono_specialization_registry.expected
  modified:
    - src/lir/emit_c.c

key-decisions:
  - "Monomorphic collapse to standard Iron_List path (not plain typed array) preserves type compatibility with downstream interface dispatch"
  - "Conservative detection: only ARRAY_LIT-created collections in same function, no parameter tracking, escape analysis excludes collections passed to other functions"
  - "Specialization registry tracks interface dispatch functions to prevent duplicate emission"
  - "Iron type inference already handles obvious single-type arrays (inferred as [Circle] not [Shape]) -- monomorphic detection covers edge cases where type checker assigns interface type"

patterns-established:
  - "monomorphic_collections map: ValueId -> concrete_type for single-type collection tracking"
  - "specialization_registry: string hash map keyed on func:type for deduplication"
  - "Escape analysis pattern: CALL args, RETURN, SET_FIELD, MAKE_CLOSURE all mark collection as escaped"

requirements-completed: [MONO-01, MONO-02]

# Metrics
duration: 26min
completed: 2026-04-08
---

# Phase 49 Plan 03: Monomorphic Collection Collapse and Specialization Registry Summary

**Monomorphic collection detection with whole-program type analysis, collapse to non-split path, specialization registry, and 3 integration tests**

## Performance

- **Duration:** 26 min
- **Started:** 2026-04-08T03:18:26Z
- **Completed:** 2026-04-08T03:44:26Z
- **Tasks:** 2
- **Files modified:** 7

## Accomplishments
- Whole-program monomorphic detection pass scans all ARRAY_LIT elements across all functions, tracks concrete types per collection, marks single-type collections
- Monomorphic collapse removes identified collections from split_collection_ids, eliminating split sub-arrays, _order arrays, and tag dispatch overhead
- Specialization registry prevents duplicate interface dispatch function emission via stb_ds string hash map
- 3 integration tests verify single-type collapse (249), multi-type no-collapse (118), and specialization registry (87, 183)
- All 268 integration tests pass (265 existing + 3 new)

## Task Commits

Each task was committed atomically:

1. **Task 1: Monomorphic collection detection, collapse emission, and specialization registry** - `d4cf900` (feat)
2. **Task 2: Monomorphic collapse integration tests** - `39f914d` (feat)

## Files Created/Modified
- `src/lir/emit_c.c` - Added monomorphic_collections and specialization_registry EmitCtx fields, whole-program detection pass, per-function collapse pass, GET_INDEX comment, specialization registry in dispatch emission, cleanup
- `tests/integration/mono_single_type_collapse.iron` + `.expected` - Single-type collection (only Circles) produces 249
- `tests/integration/mono_multi_type_no_collapse.iron` + `.expected` - Multi-type collection (Circle + Square) produces 118 via split dispatch
- `tests/integration/mono_specialization_registry.iron` + `.expected` - Two single-type collections compile without duplicate symbols (87, 183)

## Decisions Made
- **Collapse to standard Iron_List path:** Monomorphic collections are removed from split_collection_ids and fall through to the standard non-split Iron_List emission. This preserves interface-typed tagged union compatibility with downstream method dispatch while eliminating split overhead (no per-type sub-arrays, no _order array, no tag switch). A direct-to-concrete-type approach would require modifying all downstream method dispatch.
- **Conservative detection scope:** Only ARRAY_LIT instructions in the same function. Collections that escape via CALL args, RETURN, SET_FIELD, or MAKE_CLOSURE are excluded. Parameter-based collections are not tracked (may receive different types from different callers).
- **Iron type inference already handles single-type arrays:** When all array literal elements are the same concrete type, Iron's type checker infers the concrete type (e.g., [Circle]) rather than the interface type ([Shape]). The monomorphic detection infrastructure covers edge cases where type inference assigns an interface type despite only one concrete type being present.
- **Specialization registry on interface dispatch:** Registry tracks emitted dispatch functions via `__dispatch:InterfaceName` keys to prevent duplicate emission of the same interface dispatch function.

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 1 - Bug] GET_INDEX type mismatch with direct concrete type emission**
- **Found during:** Task 1 (GET_INDEX monomorphic path)
- **Issue:** The plan specified emitting GET_INDEX results typed as `Iron_<ConcreteType>` for monomorphic collections, but downstream code expects `Iron_<Interface>` (tagged union) for method dispatch. Emitting `Iron_Circle` instead of `Iron_Shape` causes type mismatches in interface dispatch calls like `Iron_shape_area(s)`.
- **Fix:** Removed the GET_INDEX type override. Monomorphic collections use the standard non-split GET_INDEX path which preserves the interface type via `array.items[i]` returning tagged unions. Added explanatory comment documenting the design choice.
- **Files modified:** src/lir/emit_c.c
- **Verification:** All 268 integration tests pass
- **Committed in:** d4cf900 (Task 1 commit)

**2. [Rule 1 - Bug] ARRAY_LIT emission for monomorphic collections**
- **Found during:** Task 1 (ARRAY_LIT monomorphic path)
- **Issue:** The plan specified emitting monomorphic collections as `Iron_List_Iron_<ConcreteType>` with direct element pushes. But the runtime doesn't have IRON_LIST_IMPL instantiations for struct types, and downstream code expects interface-typed values.
- **Fix:** Removed the ARRAY_LIT type override. Monomorphic collections fall through to the standard non-split Iron_List emission which creates `Iron_List_Iron_Shape` with proper tagged union wrapping. The optimization benefit comes from removing split collection overhead.
- **Files modified:** src/lir/emit_c.c
- **Verification:** All 268 integration tests pass
- **Committed in:** d4cf900 (Task 1 commit)

---

**Total deviations:** 2 auto-fixed (2 bugs in plan specification)
**Impact on plan:** Both fixes ensure type safety. The optimization still eliminates split collection overhead (no per-type sub-arrays, no _order array, no tag dispatch in for-loops). The interface type preservation is necessary for correctness with the existing dispatch architecture.

## Issues Encountered
None beyond the deviations noted above.

## User Setup Required
None - no external service configuration required.

## Next Phase Readiness
- Phase 49 (Loop Fusion and Monomorphic Specialization) is complete
- All 3 plans executed: fusible annotation pipeline, fused loop emission, monomorphic collapse
- FUSE-01, FUSE-02, MONO-01, MONO-02 requirements verified via 11 new integration tests
- Ready for next phase

## Self-Check: PASSED

All files exist, all commits verified.

---
*Phase: 49-loop-fusion-monomorphic-specialization*
*Completed: 2026-04-08*
