---
phase: 33-value-mutable-captures-optimizer-guards
plan: "02"
subsystem: analyzer
tags: [c, typecheck, runtime, collections, lir, dce, closures, func-type]

# Dependency graph
requires:
  - phase: 33-01
    provides: Iron_TypeAnnotation extended with is_func/func_params/func_param_count/func_return
  - phase: 32-closure-wiring
    provides: Iron_Closure struct and closure codegen foundation
provides:
  - resolve_type_annotation handles func-type annotations (IRON_TYPE_FUNC) and array-of-func
  - Iron_List_Iron_Closure typedef + IRON_LIST_DECL + IRON_LIST_IMPL in runtime
  - DCE preserves capturing MAKE_CLOSURE instructions (capture_count > 0 check in run_dce)
affects:
  - 33-03: codegen can now rely on IRON_TYPE_FUNC being resolved for func-type params
  - future: array-of-closures (capture_04) can proceed with both type resolution and list support

# Tech tracking
tech-stack:
  added: []
  patterns:
    - "func-type annotation resolution: early-return branch in resolve_type_annotation before primitive name checks"
    - "DCE override pattern: inline capture_count check in run_dce instead of modifying iron_lir_instr_is_pure"
    - "Iron_List_Iron_Closure: follows same IRON_LIST_DECL/IMPL macro pair as all other collection types"

key-files:
  created: []
  modified:
    - src/analyzer/typecheck.c
    - src/runtime/iron_runtime.h
    - src/runtime/iron_collections.c
    - src/lir/lir_optimize.c

key-decisions:
  - "iron_arena_alloc requires 3 args (arena, size, align) — used _Alignof(Iron_Type *) for param_types array"
  - "DCE fix uses Option B: inline capture_count check in run_dce only, not touching iron_lir_instr_is_pure — avoids modifying 8+ call sites"
  - "Iron_List_Iron_Closure typedef placed inside #ifndef IRON_CODEGEN_PROVIDES_STRUCTS guard — consistent with all other list typedefs"

patterns-established:
  - "resolve_type_annotation is_func branch: returns early with IRON_TYPE_FUNC before any named-type logic"
  - "DCE capture guard: capturing MAKE_CLOSURE treated as side-effecting in both Step 1 (seed) and Step 3 (keep/remove)"

requirements-completed: [OPT-01, OPT-02, OPT-03]

# Metrics
duration: 10min
completed: 2026-04-03
---

# Phase 33 Plan 02: Typecheck Func-Type Resolution, Iron_List_Iron_Closure, and DCE Purity Fix Summary

**Func-type annotations now resolve to IRON_TYPE_FUNC in typecheck; Iron_List_Iron_Closure added to runtime collections; DCE preserves capturing closures via inline capture_count guards in run_dce**

## Performance

- **Duration:** ~10 min
- **Started:** 2026-04-03T01:44:00Z
- **Completed:** 2026-04-03T01:47:36Z
- **Tasks:** 2
- **Files modified:** 4

## Accomplishments
- Extended `resolve_type_annotation` with `is_func` early-return branch that builds `IRON_TYPE_FUNC` from func-type annotations, including array-of-func wrapping and nullable support
- Added `Iron_List_Iron_Closure` typedef, `IRON_LIST_DECL`, and `IRON_LIST_IMPL` to the runtime collection infrastructure — enables array-of-closures storage
- Fixed DCE `run_dce` to treat capturing `MAKE_CLOSURE` instructions as non-pure at both Step 1 (seeding) and Step 3 (keep/remove), preventing elimination of closure side-effects
- Project builds with zero errors

## Task Commits

Each task was committed atomically:

1. **Task 1: Add func-type resolution to typecheck.c resolve_type_annotation** - `7e0e9b7` (feat)
2. **Task 2: Add Iron_List_Iron_Closure to runtime + fix DCE purity** - `674df99` (feat)

**Plan metadata:** committed in final docs commit

## Files Created/Modified
- `src/analyzer/typecheck.c` - Added `if (ann->is_func)` branch before primitive name checks; resolves param types and return type recursively; wraps in IRON_TYPE_ARRAY and/or nullable as needed
- `src/runtime/iron_runtime.h` - Added `Iron_List_Iron_Closure` typedef inside `#ifndef IRON_CODEGEN_PROVIDES_STRUCTS` block; added `IRON_LIST_DECL(Iron_Closure, Iron_Closure)`
- `src/runtime/iron_collections.c` - Added `IRON_LIST_IMPL(Iron_Closure, Iron_Closure)` after other list implementations
- `src/lir/lir_optimize.c` - Added `capture_count > 0` inline guards in `run_dce` Step 1 and Step 3; `iron_lir_instr_is_pure` left unchanged

## Decisions Made
- `iron_arena_alloc` requires three arguments (arena, size, align); used `_Alignof(Iron_Type *)` for the param_types array allocation — plan omitted the alignment argument, auto-fixed as Rule 1 (build error)
- DCE fix uses "Option B" from RESEARCH.md: inline check in `run_dce` only, avoiding modification of `iron_lir_instr_is_pure` and its 8+ call sites

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 1 - Bug] Missing third argument to iron_arena_alloc**
- **Found during:** Task 1 (func-type resolution)
- **Issue:** Plan snippet used `iron_arena_alloc(ctx->arena, sizeof(Iron_Type *) * param_count)` with only 2 args; actual signature requires `(Iron_Arena *a, size_t size, size_t align)`
- **Fix:** Added `_Alignof(Iron_Type *)` as third argument
- **Files modified:** src/analyzer/typecheck.c
- **Verification:** Build succeeded after fix
- **Committed in:** 7e0e9b7 (Task 1 commit)

---

**Total deviations:** 1 auto-fixed (1 bug — wrong arity in plan snippet)
**Impact on plan:** Trivial alignment-argument fix; no scope creep.

## Issues Encountered
- Plan snippet for `iron_arena_alloc` omitted the required `align` argument — discovered at first compile. Fixed immediately by consulting arena.h.

## User Setup Required
None - no external service configuration required.

## Next Phase Readiness
- Type resolver now handles func-type annotations — capture_07 (callback arg) and capture_04 (array-of-closures) can proceed through typecheck
- Runtime has Iron_List_Iron_Closure — array-of-closures storage is ready for codegen
- DCE fix means capturing closures won't be silently eliminated — closure semantics preserved through optimizer
- Plan 03 (codegen for func params) can reference IRON_TYPE_FUNC from resolved annotations

## Self-Check: PASSED
- All 4 modified files exist on disk
- Both task commits (7e0e9b7, 674df99) confirmed in git log

---
*Phase: 33-value-mutable-captures-optimizer-guards*
*Completed: 2026-04-03*
