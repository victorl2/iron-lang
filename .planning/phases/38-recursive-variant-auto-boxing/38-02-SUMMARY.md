---
phase: 38-recursive-variant-auto-boxing
plan: 02
subsystem: compiler
tags: [adt, recursive-types, auto-boxing, c-codegen, enum, typecheck, generics, monomorphization]

# Dependency graph
requires:
  - phase: 38-recursive-variant-auto-boxing
    plan: 01
    provides: payload_is_boxed infrastructure, malloc at CONSTRUCT, dereference at GET_FIELD

provides:
  - Static _free helper functions emitted after each ADT struct definition for recursive enums
  - RETURN-site free injection for non-returned ADT locals with boxed fields
  - Per-function adt_boxed_allocas tracking map in EmitCtx
  - Generic recursive enum monomorphization cycle detection via mono_registry in TypeCtx
  - Concrete type arg binding in gen_scope (removes post-substitution, fixes recursive generic enums)
  - Branch(Tree[T], Tree[T]) type inference from Tree[Int] arguments
  - Integration test: Tree[Int] generic recursive enum compiles and runs correctly

affects: [future-phases, generic-recursive-enums, memory-management]

# Tech tracking
tech-stack:
  added: []
  patterns:
    - "TypeCtx.mono_registry keyed by mangled name provides cycle detection + caching for all mono paths"
    - "Concrete type args bound in gen_scope during payload resolution (T=Int not T=GENERIC_PARAM)"
    - "Per-function adt_boxed_allocas map reset in emit_func_body, freed at end"
    - "_free helper emitted immediately after ADT struct definition into struct_bodies buffer"
    - "RETURN-site injection: chase LOAD->alloca to identify returned value and skip its free"

key-files:
  created:
    - tests/integration/adt_recursive_generic.iron
    - tests/integration/adt_recursive_generic.expected
  modified:
    - src/lir/emit_c.c
    - src/analyzer/typecheck.c

key-decisions:
  - "mono_registry on TypeCtx enables both cycle detection (infinite recursion) and caching across both mono paths (signature + ENUM_CONSTRUCT inference)"
  - "Concrete type args bound in gen_scope: T=Int not T=GENERIC_PARAM, so resolve_type_annotation(Tree[T]) returns Tree[Int] and finds it in registry"
  - "substitute_generic_type removed: no longer needed since concrete binding handles all substitution at resolve time"
  - "Path 2 (ENUM_CONSTRUCT inference) checks mono_registry before creating new mono: ensures payload_is_boxed is populated from path 1 signature resolution"
  - "RETURN-site free: adt_boxed_allocas pre-scan runs after all other pre-scans; optimizer eliminates dead allocas so only surviving (multi-store) allocas get freed"

patterns-established:
  - "Generic recursive enum: register mono BEFORE payload loop to break cycle; all later recursive resolutions of same mangled name return the in-progress mono pointer"
  - "Branch payload type inference: when expected=Tree[T] and arg_t=Tree[Int], walk type_args arrays to extract T=Int for inferred_args map"

requirements-completed: []

# Metrics
duration: 36min
completed: 2026-04-04
---

# Phase 38 Plan 02: Recursive Free Helpers and Generic Enum Integration Test Summary

**Recursive ADT memory management via emitted `_free` helpers + correct monomorphization of recursive generic enums like Tree[Int] fixing infinite-recursion cycle in typecheck**

## Performance

- **Duration:** 36 min
- **Started:** 2026-04-04T23:19:40Z
- **Completed:** 2026-04-04T23:55:40Z
- **Tasks:** 2
- **Files modified:** 4

## Accomplishments
- Emitted static `TypeName_free` helper for each ADT enum with boxed payloads in the C struct body section
- Injected `TypeName_free(&local)` calls at RETURN sites for non-returned ADT enum locals, chasing LOAD→alloca to skip the returned value
- Fixed infinite recursion in `resolve_type_annotation` for recursive generic enums (Tree[T] with Branch(Tree[T], Tree[T])): added `mono_registry` for cycle detection + caching
- Fixed monomorphization to bind concrete type args in gen_scope (T=Int, not T=GENERIC_PARAM), eliminating the need for `substitute_generic_type` post-processing
- Enhanced type inference for Branch(Tree[T], Tree[T]) constructs: extract T=Int from Tree[Int] arguments
- Both recursive ADT integration tests (list, expr) and all 191 previously-passing tests continue to pass
- New Tree[Int] generic recursive enum test: construction, pattern matching, and recursive traversal all work correctly

## Task Commits

1. **Task 1: Recursive free helper emission and RETURN-site free injection** - `50d1a9c` (feat)
2. **Task 2: Generic recursive enum integration test** - `2d4cc9f` (feat)

**Plan metadata:** (docs commit follows)

## Files Created/Modified
- `src/lir/emit_c.c` - Added `adt_boxed_allocas` to EmitCtx, ALLOCA pre-scan, _free helper emission after ADT struct, RETURN-site free injection
- `src/analyzer/typecheck.c` - Added `mono_registry` to TypeCtx, concrete gen_scope binding, inference enhancement, removed substitute_generic_type
- `tests/integration/adt_recursive_generic.iron` - Tree[T] generic recursive enum test with sum_tree and match
- `tests/integration/adt_recursive_generic.expected` - Expected: sum:6, leaf:42

## Decisions Made
- `mono_registry` on TypeCtx is the key cycle-breaking mechanism: registered before payload resolution, both path 1 (type annotation) and path 2 (ENUM_CONSTRUCT inference) check it first
- Concrete gen_scope binding (T=Int instead of T=GENERIC_PARAM) simplifies the entire pipeline: no substitution pass needed, recursive payload Tree[T] resolves directly to Tree[Int] and hits the registry
- `substitute_generic_type` was removed entirely (was deferred/broken for IRON_TYPE_ENUM anyway)
- Path 2 registry check uses `break` to return early from the outer `switch` case, reusing the fully-built mono from path 1

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 1 - Bug] Fixed infinite recursion in resolve_type_annotation for recursive generic enums**
- **Found during:** Task 2 (building adt_recursive_generic.iron)
- **Issue:** `Tree.Branch(Tree[T], Tree[T])` caused stack overflow in `resolve_type_annotation` because resolving `Tree[T]` → payload `Tree[T]` → same infinite call
- **Fix:** Added `mono_registry` (string hashmap) to TypeCtx; register mono BEFORE payload loop; recursive calls find the in-progress mono and return it immediately
- **Files modified:** `src/analyzer/typecheck.c`
- **Verification:** `adt_recursive_generic.iron` builds without stack overflow; all 192 tests pass
- **Committed in:** `2d4cc9f` (Task 2 commit)

**2. [Rule 1 - Bug] Fixed type mismatch: Tree[<null>] from stale GENERIC_PARAM gen_scope binding**
- **Found during:** Task 2 (after fixing stack overflow)
- **Issue:** Old approach bound T=GENERIC_PARAM("T") in gen_scope, then attempted post-substitution via `substitute_generic_type` which was defective for IRON_TYPE_ENUM (returned `t` unchanged). Result: vpt[j][k] = Tree[T] (not Tree[Int]), and inferred_args[0] = NULL → type mismatch `Tree[<null>]`
- **Fix:** Bind concrete type_args[i] directly in gen_scope (T=Int); resolve_type_annotation(Tree[T]) sees T=Int → resolves as resolve_type_annotation(Tree[Int]) → hits registry → returns mono; removed substitute_generic_type entirely
- **Files modified:** `src/analyzer/typecheck.c`
- **Verification:** No type mismatch errors; correct payload types in vpt; payload_is_boxed correctly set
- **Committed in:** `2d4cc9f` (Task 2 commit)

**3. [Rule 1 - Bug] Fixed C compilation error: Branch payload received value instead of pointer**
- **Found during:** Task 2 (after fixing type mismatch)
- **Issue:** Path 2 (ENUM_CONSTRUCT inference) created a fresh mono every time without checking registry; fresh mono had `payload_is_boxed = NULL`; optimizer marked CONSTRUCT as inline_eligible; inline emit used value compound literal where pointer was expected
- **Fix:** Added registry check to path 2 before creating new mono; if "Iron_Tree_Int" already in registry, return that mono (fully built with payload_is_boxed = true for Branch fields)
- **Files modified:** `src/analyzer/typecheck.c`
- **Verification:** C compiles cleanly; Tree[Int] branch fields are `Iron_Tree_Int *` with proper malloc allocation
- **Committed in:** `2d4cc9f` (Task 2 commit)

**4. [Rule 1 - Bug] Fixed type inference for Branch(Tree[T], Tree[T]) with Tree[Int] arguments**
- **Found during:** Task 2 (type mismatch Tree[<null>] investigation)
- **Issue:** Type inference only handled GENERIC_PARAM expected types, not generic enum expected types; `expected = Tree[T]`, `arg_t = Tree[Int]` → `inferred_args[0]` stayed NULL → mangled name "Iron_Tree_unknown"
- **Fix:** When expected is IRON_TYPE_ENUM with GENERIC_PARAM type args and arg_t is same enum with concrete args, walk the type_args arrays to extract inferences (T=Int)
- **Files modified:** `src/analyzer/typecheck.c`
- **Verification:** Tree.Branch(Tree.Leaf(1), Tree.Leaf(2)) correctly infers T=Int; mangled name = "Iron_Tree_Int"
- **Committed in:** `2d4cc9f` (Task 2 commit)

---

**Total deviations:** 4 auto-fixed (all Rule 1 — bugs discovered during Task 2 execution)
**Impact on plan:** All fixes essential for correctness of recursive generic enum support. Bugs were pre-existing limitations in the monomorphization pipeline exposed by the new Tree[T] test. No scope creep.

## Issues Encountered
- The stack overflow hit immediately on the first build of adt_recursive_generic.iron
- Each fix revealed the next issue in sequence: stack overflow → type mismatch → C compile error → all resolved by systematic cycle detection + concrete binding approach
- `substitute_generic_type` had a pre-existing defect (returned `t` unchanged for IRON_TYPE_ENUM even when `changed = true`); removing it was cleaner than fixing it

## Next Phase Readiness
- EDATA-04 completely done: recursive enums (generic and non-generic) compile, construct, match, traverse correctly
- Phase 38 complete: auto-boxing infrastructure + memory management + generic recursive enums all working
- All 192 integration tests pass

---
*Phase: 38-recursive-variant-auto-boxing*
*Completed: 2026-04-04*
