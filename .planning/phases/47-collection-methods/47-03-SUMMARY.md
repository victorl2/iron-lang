---
phase: 47-collection-methods
plan: 03
subsystem: compiler
tags: [collection-methods, method-chaining, split-collection, dispatch, map, filter, reduce, interface, closures]

# Dependency graph
requires:
  - phase: 47-01
    provides: "Array extension method syntax and type resolution"
  - phase: 47-02
    provides: "C runtime collection methods (map/filter/reduce/forEach/sum) on typed arrays"
provides:
  - "Method chaining on arrays: arr.map(...).filter(...).sum()"
  - "Split collection dispatch for collection methods on interface-typed arrays"
  - "Cross-type map/reduce generics ([T].map[U] and [T].reduce[U])"
  - "Interface type resolution in HIR lower for lambda parameters"
  - "7 new integration tests (chain, split dispatch, empty array, float sum)"
affects: [loop-fusion, stdlib-collections, generic-monomorphization]

# Tech tracking
tech-stack:
  added: []
  patterns:
    - "Inline split collection dispatch in emit_c.c using _order array for ordered iteration"
    - "Cross-type generic stubs ([T].map[U]) with U inferred from lambda return type"
    - "Interface type resolution in HIR lower resolve_type_ann"

key-files:
  created:
    - tests/integration/coll_chain_map_filter_sum.iron
    - tests/integration/coll_chain_advanced.iron
    - tests/integration/coll_split_map.iron
    - tests/integration/coll_split_filter.iron
    - tests/integration/coll_split_reduce.iron
    - tests/integration/coll_empty_array.iron
    - tests/integration/coll_float_sum.iron
  modified:
    - src/stdlib/list.iron
    - src/hir/hir_lower.c
    - src/hir/hir_to_lir.c
    - src/lir/emit_c.c
    - src/analyzer/typecheck.c

key-decisions:
  - "Inline split collection dispatch in C emitter rather than generating LIR loops (simpler, avoids HIR control flow changes)"
  - "Use _order array for ordered iteration of split collections (preserves insertion order)"
  - "Cross-type map/reduce generics via [U] method-level generic parameter"
  - "Filter on split collections returns new split collection (tracked in split_collection_ids)"

patterns-established:
  - "Split collection method dispatch: inline C expansion using _order array, switch on tag, wrap items in tagged union"
  - "Cross-type collection stubs: func [T].map[U](f: func(T) -> U) -> [U] with U inferred from lambda return type"

requirements-completed: [COLL-10, COLL-11]

# Metrics
duration: 49min
completed: 2026-04-07
---

# Phase 47 Plan 03: Method Chaining and Split Collection Dispatch Summary

**Method chaining on arrays (map/filter/sum chains) and inline split collection dispatch for collection methods on interface-typed arrays with 7 new integration tests**

## Performance

- **Duration:** 49 min
- **Started:** 2026-04-07T17:50:50Z
- **Completed:** 2026-04-07T18:40:00Z
- **Tasks:** 1
- **Files modified:** 19 (5 source + 14 test files)

## Accomplishments
- Method chaining works end-to-end: `arr.map(f).filter(g).sum()` compiles and produces correct results
- Split collection dispatch for map, filter, reduce, and forEach on interface-typed arrays
- Cross-type map ([T] -> [U]) and cross-type reduce properly infer return types from lambda
- Interface type resolution added to HIR lower so lambda parameters typed as interfaces emit correct C types
- 250 total integration tests pass with zero regressions (7 new tests added)
- Empty array edge cases work (map returns [], sum returns 0, reduce returns init value)
- Float array sum works

## Task Commits

Each task was committed atomically:

1. **Task 1: Enable method chaining and fix split collection method dispatch** - `df774b4` (feat)

## Files Created/Modified
- `src/stdlib/list.iron` - Updated map/reduce stubs to use cross-type generics [U]
- `src/hir/hir_lower.c` - Added IRON_NODE_INTERFACE_DECL handling in resolve_type_ann
- `src/hir/hir_to_lir.c` - Added IRON_TYPE_INTERFACE case in collection method elem_suffix switch
- `src/lir/emit_c.c` - Added inline split collection dispatch for map/filter/reduce/forEach (346 lines)
- `src/analyzer/typecheck.c` - Cleanup only (debug output removed)
- `tests/integration/coll_chain_map_filter_sum.iron` - [1..10].map(*2).filter(>10).sum() = 80
- `tests/integration/coll_chain_advanced.iron` - map+filter+reduce chain and map+for iteration
- `tests/integration/coll_split_map.iron` - shapes.map(area) on split collection
- `tests/integration/coll_split_filter.iron` - shapes.filter(area>20) on split collection
- `tests/integration/coll_split_reduce.iron` - shapes.reduce(0, acc+area) on split collection
- `tests/integration/coll_empty_array.iron` - empty array map/filter/sum/reduce edge cases
- `tests/integration/coll_float_sum.iron` - [1.5, 2.5, 3.0].sum() = 7

## Decisions Made
- **Inline expansion over function call for split dispatch**: The C runtime collection methods (Iron_List_T_map etc.) expect flat arrays, not split collections. Rather than creating a new set of split-aware C runtime functions, we inline the iteration logic in the C emitter using the split collection's _order array for ordered traversal.
- **Filter returns new split collection**: When filter is called on a split collection, the result is also a split collection (tracked in split_collection_ids) so subsequent for-loop iteration uses per-type dispatch.
- **Cross-type generics for map/reduce**: Changed list.iron stubs from `func [T].map(f: func(T)->T)->[T]` to `func [T].map[U](f: func(T)->U)->[U]` so the type checker infers the correct return type when the lambda returns a different type than the input element.

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 1 - Bug] list.iron map/reduce stubs used same-type generics**
- **Found during:** Task 1 (investigating why shapes.map returns [Shape] instead of [Int])
- **Issue:** `func [T].map(f: func(T) -> T) -> [T]` returns [T] (same as input), but map should support cross-type transformation T -> U
- **Fix:** Changed to `func [T].map[U](f: func(T) -> U) -> [U]` and `func [T].reduce[U](init: U, f: func(U, T) -> U) -> U`
- **Files modified:** src/stdlib/list.iron
- **Committed in:** df774b4

**2. [Rule 1 - Bug] HIR lower resolve_type_ann missing interface type resolution**
- **Found during:** Task 1 (lambda param `s: Shape` emitted as `void*` instead of `Iron_Shape`)
- **Issue:** resolve_type_ann in hir_lower.c handled object and enum types but not interface types, causing lambda parameters with interface type annotations to resolve to NULL (emitted as void*)
- **Fix:** Added IRON_NODE_INTERFACE_DECL case to resolve_type_ann
- **Files modified:** src/hir/hir_lower.c
- **Committed in:** df774b4

**3. [Rule 1 - Bug] HIR-to-LIR elem_suffix missing IRON_TYPE_INTERFACE case**
- **Found during:** Task 1 (collection method call on [Shape] generated wrong C function name)
- **Issue:** switch(elem->kind) in collection method handler had no case for IRON_TYPE_INTERFACE, defaulting to "int64_t" suffix
- **Fix:** Added IRON_TYPE_INTERFACE case that uses "Iron_<InterfaceName>" as suffix
- **Files modified:** src/hir/hir_to_lir.c
- **Committed in:** df774b4

---

**Total deviations:** 3 auto-fixed (3 Rule 1 bugs)
**Impact on plan:** All fixes were necessary for correct compilation. The list.iron generic fix was the root cause of incorrect type resolution for cross-type map. The HIR lower fix was essential for correct lambda parameter types. No scope creep.

## Issues Encountered
- Method chaining on regular arrays already worked from Plan 01/02 infrastructure (no code changes needed)
- String interpolation inside lambda bodies is a pre-existing bug (Int printed as empty string). Tests avoid this by using for-loop iteration instead of forEach+println.
- Empty array literal `val empty: [Int] = []` syntax not supported. Tests use `var arr = [1]; arr.pop()` workaround.
- Float printing uses %g format: 7.0 prints as "7" not "7.0". Expected output adjusted.

## User Setup Required
None - no external service configuration required.

## Next Phase Readiness
- Phase 47 (Collection Methods) is now complete: all 3 plans executed
- COLL-05 through COLL-11 requirements satisfied
- 12 collection method integration tests pass covering map, filter, reduce, forEach, sum, chaining, split dispatch, empty arrays, float arrays
- Future optimizations: loop fusion (Phase 49) can optimize chained collection method calls into single-pass loops
- Known limitations: forEach with println inside lambda has string interpolation bug (pre-existing, not introduced by collection methods)

---
*Phase: 47-collection-methods*
*Completed: 2026-04-07*
