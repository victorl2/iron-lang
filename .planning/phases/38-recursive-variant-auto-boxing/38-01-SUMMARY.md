---
phase: 38-recursive-variant-auto-boxing
plan: 01
subsystem: compiler
tags: [adt, recursive-types, auto-boxing, c-codegen, enum, typecheck]

# Dependency graph
requires:
  - phase: 37-generic-enums
    provides: Monomorphization infrastructure, variant_payload_types populated in 2 mono paths
  - phase: 34-hir-extensions-and-match-lowering
    provides: ADT pattern matching, GET_FIELD field path format (data.Variant._N), CONSTRUCT LIR instructions
provides:
  - Auto-boxing detection for recursive enum payload fields in typecheck (3 sites)
  - Pointer-indirected C struct fields for recursive enum variants
  - Heap allocation (malloc) emission for boxed fields at CONSTRUCT time
  - Dereference (*) emission for GET_FIELD accesses on boxed recursive slots
  - Inline exclusion for CONSTRUCTs with boxed payloads
  - Integration tests: recursive linked list and expression tree
affects: [future-phases, generic-recursive-enums]

# Tech tracking
tech-stack:
  added: []
  patterns:
    - "payload_is_boxed parallel bool** array on Iron_Type.enu mirrors variant_payload_types layout"
    - "Non-generic pre-pass writes pib to both AST node (ev->payload_is_boxed) and type (ty->enu.payload_is_boxed)"
    - "Monomorphization paths write pib only on the mono type to avoid shared AST mutation"
    - "Boxed CONSTRUCTs: pre-emit malloc+assign before struct literal, reference __box_N_K pointer"
    - "Enum defs (plain C enums) emitted before struct bodies for correct cross-enum reference ordering"

key-files:
  created:
    - tests/integration/adt_recursive_list.iron
    - tests/integration/adt_recursive_list.expected
    - tests/integration/adt_recursive_expr.iron
    - tests/integration/adt_recursive_expr.expected
  modified:
    - src/parser/ast.h
    - src/analyzer/types.h
    - src/analyzer/typecheck.c
    - src/lir/emit_c.c
    - src/lir/lir_optimize.c

key-decisions:
  - "payload_is_boxed stored on Iron_Type.enu (not just AST node) so monomorphized and non-generic enums share same read path in emit_c.c"
  - "enum_defs buffer emitted before struct_bodies in final C output: plain C enums (typedef enum) must be defined before ADT structs that reference them as non-boxed fields"
  - "GET_FIELD dereference resolves object enum type by traversing LOAD->ALLOCA chain; direct type check first, then alloca.alloc_type fallback"

patterns-established:
  - "Recursion detection: iron_type_equals(row[k], ty) where ty is the owning enum type"
  - "Boxed field C emission: T *_N vs T _N in variant payload struct; __box_ID_K heap pointer names"

requirements-completed: [EDATA-04]

# Metrics
duration: 24min
completed: 2026-04-04
---

# Phase 38 Plan 01: Recursive Variant Auto-Boxing Summary

**Recursive enum auto-boxing via pointer-indirected fields: malloc at CONSTRUCT, dereference at GET_FIELD, detected via iron_type_equals in 3 typecheck paths**

## Performance

- **Duration:** 24 min
- **Started:** 2026-04-04T22:49:06Z
- **Completed:** 2026-04-04T23:13:41Z
- **Tasks:** 2
- **Files modified:** 9

## Accomplishments
- Added `payload_is_boxed` bool** to both `Iron_EnumVariant` (ast.h) and `Iron_Type.enu` (types.h)
- Implemented recursion detection in all 3 typecheck paths: non-generic pre-pass + 2 monomorphization paths
- C emitter now produces `T *_N` struct fields for boxed slots and pre-emits `malloc+assign` before CONSTRUCT struct literals
- GET_FIELD emitter now wraps boxed slot accesses with `*(obj.data.V._N)` dereference
- Inline optimizer excludes CONSTRUCTs with any boxed payload from inlining
- Fixed enum output ordering (plain C enums before ADT structs) needed for mixed-enum programs like Expr+Op
- Both recursive ADT integration tests pass with correct output

## Task Commits

1. **Task 1: Auto-boxing detection and C emission** - `ffee25f` (feat)
2. **Task 2: Integration tests for recursive enum construction** - `9f2bb7d` (feat)

**Plan metadata:** (docs commit follows)

## Files Created/Modified
- `src/parser/ast.h` - Added `bool *payload_is_boxed` to Iron_EnumVariant
- `src/analyzer/types.h` - Added `bool **payload_is_boxed` to Iron_Type.enu
- `src/analyzer/typecheck.c` - Recursion detection in pre-pass (non-generic) and 2 mono paths
- `src/lir/emit_c.c` - Boxed struct fields, malloc CONSTRUCT, dereference GET_FIELD, enum output ordering fix
- `src/lir/lir_optimize.c` - Inline exclusion for boxed CONSTRUCTs
- `tests/integration/adt_recursive_list.iron` - Linked list Cons(Int, List) sum test
- `tests/integration/adt_recursive_list.expected` - Expected: sum:6, head:1
- `tests/integration/adt_recursive_expr.iron` - BinOp(Expr, Op, Expr) eval tree test
- `tests/integration/adt_recursive_expr.expected` - Expected: result:14, simple:42

## Decisions Made
- `payload_is_boxed` stored on `Iron_Type.enu` (not just AST node) so both generic and non-generic enums use the same emit_c.c read path
- Plain C enum `typedef enum` blocks are now emitted before ADT struct bodies so non-recursive enum fields (like `Op` in `BinOp(Expr, Op, Expr)`) compile correctly
- GET_FIELD dereference resolves the object enum type by traversing LOAD -> ALLOCA to handle the typical pattern of loading from an alloca holding the enum

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 1 - Bug] Fixed C output ordering: plain enums before struct bodies**
- **Found during:** Task 2 (adt_recursive_expr test — adt_recursive_list passed immediately)
- **Issue:** `Iron_Op _1` in `Iron_Expr_BinOp_data` struct referenced `Iron_Op` before it was defined; `enum_defs` buffer was emitted after `struct_bodies`
- **Fix:** Swapped emit order in `iron_lir_emit_c` — `enum_defs` now emitted before `struct_bodies`
- **Files modified:** `src/lir/emit_c.c`
- **Verification:** adt_recursive_expr compiles and all 170+ integration tests pass
- **Committed in:** `9f2bb7d` (Task 2 commit)

---

**Total deviations:** 1 auto-fixed (Rule 1 — bug)
**Impact on plan:** Essential correctness fix for programs using non-recursive plain enum as field of ADT variant. No scope creep.

## Issues Encountered
- The expr tree test required a secondary fix for C output ordering — plain enum typedefs must precede ADT struct bodies containing them. The list test (single recursive enum) succeeded immediately.

## Next Phase Readiness
- EDATA-04 core infrastructure complete: recursive enums compile and run correctly for both non-generic and generic enums
- Pattern matching on recursive variants works via the existing GET_FIELD field path mechanism + new dereference logic
- All 170+ existing integration tests continue passing

---
*Phase: 38-recursive-variant-auto-boxing*
*Completed: 2026-04-04*
