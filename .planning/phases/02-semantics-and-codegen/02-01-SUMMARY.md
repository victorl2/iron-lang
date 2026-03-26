---
phase: 02-semantics-and-codegen
plan: 01
subsystem: analyzer
tags: [c, type-system, scope, diagnostics, unity, tdd, stb_ds]

# Dependency graph
requires:
  - phase: 01-frontend
    provides: Iron_Arena, Iron_Node/AST types, Iron_DiagList, iron_diag_emit, stb_ds_impl.c

provides:
  - Iron_Type discriminated union with 26 type kinds and structural equality
  - Interned primitive type singletons (pointer equality valid for all primitives)
  - Iron_Scope tree with stb_ds hash-map symbols and parent-chain lookup
  - iron_scope_define duplicate rejection and child-scope shadowing
  - Semantic E-codes 200-223 in diagnostics.h

affects:
  - 02-02 (name resolution uses Iron_Scope and Iron_Symbol)
  - 02-03 (type checker uses Iron_Type and iron_type_equals)
  - 02-04 and beyond (all semantic passes depend on these foundations)

# Tech tracking
tech-stack:
  added: []
  patterns:
    - Primitive types interned as static singletons; pointer equality sufficient for comparison
    - Compound types (nullable, rc, func, array) heap-allocated via ARENA_ALLOC
    - Scope chains use stb_ds sh_new_strdup for string-keyed symbol hash maps
    - iron_scope_define uses shgeti for O(1) duplicate detection before shput

key-files:
  created:
    - src/analyzer/types.h
    - src/analyzer/types.c
    - src/analyzer/scope.h
    - src/analyzer/scope.c
    - tests/test_types.c
  modified:
    - src/diagnostics/diagnostics.h (added semantic E-codes 200-223)
    - CMakeLists.txt (added analyzer sources and test_types target)

key-decisions:
  - "Primitive singletons stored in static array s_primitives[IRON_TYPE_ERROR+1] indexed by kind — zero allocation, pointer equality valid"
  - "IRON_TYPE_VOID, IRON_TYPE_NULL, IRON_TYPE_ERROR interned alongside primitives for uniform handling"
  - "iron_scope_define takes Iron_Symbol* (arena-allocated by caller) not by value — avoids extra copy"
  - "scope.c uses sh_new_strdup so stb_ds owns key copies; symbol name pointer in Iron_Symbol is caller-owned"

patterns-established:
  - "Type constructor pattern: ARENA_ALLOC + memset to zero + fill fields — consistent across all compound type constructors"
  - "Scope lookup: walk parent chain in iron_scope_lookup; iron_scope_lookup_local checks only current scope"
  - "Diagnostic E-codes: lexer 1-99, parser 100-199, semantic 200-299 — clear numeric namespace by layer"

requirements-completed: [SEM-01, SEM-02]

# Metrics
duration: 4min
completed: 2026-03-25
---

# Phase 2 Plan 01: Type System and Scope Tree Summary

**Iron_Type discriminated union with 26 kinds and interned primitive singletons, Iron_Scope parent-chain tree using stb_ds hash maps, and semantic E-codes 200-223 — all verified by 23 Unity tests**

## Performance

- **Duration:** ~4 min
- **Started:** 2026-03-25T14:28:44Z
- **Completed:** 2026-03-25T14:32:26Z
- **Tasks:** 1
- **Files modified:** 7

## Accomplishments
- Full Iron_Type system with all 26 type kinds including complex compound types (func, array, nullable, rc, generic_param)
- Primitive type interning via static singleton table — `iron_type_make_primitive(IRON_TYPE_INT)` always returns same pointer
- Structural equality (`iron_type_equals`) handles all type kinds recursively; pointer short-circuit covers interned primitives
- Iron_Scope tree with `sh_new_strdup` stb_ds hash maps, parent-chain lookup, and duplicate-rejection on `iron_scope_define`
- 24 semantic error codes (200-223) added to diagnostics.h for all subsequent semantic analysis passes
- 23 Unity tests covering all behaviors; all pass alongside all 7 existing Phase 1 test suites

## Task Commits

Each task was committed atomically:

1. **Task 1: Create Iron_Type system and scope tree infrastructure** - `2a6a5ce` (feat)

**Plan metadata:** (docs commit — see below)

## Files Created/Modified
- `src/analyzer/types.h` - Iron_TypeKind enum (26 kinds), Iron_Type discriminated union, all type constructor and predicate declarations
- `src/analyzer/types.c` - Primitive interning via static s_primitives[], all constructors, iron_type_equals, iron_type_to_string
- `src/analyzer/scope.h` - Iron_ScopeKind, Iron_SymbolKind, Iron_Symbol, Iron_SymbolEntry, Iron_Scope, CRUD function declarations
- `src/analyzer/scope.c` - Scope tree implementation using sh_new_strdup/shget/shput/shgeti from stb_ds
- `src/diagnostics/diagnostics.h` - Added semantic E-codes 200-223 (IRON_ERR_UNDEFINED_VAR through IRON_ERR_CIRCULAR_TYPE)
- `CMakeLists.txt` - Added src/analyzer/types.c and src/analyzer/scope.c to iron_compiler; added test_types target
- `tests/test_types.c` - 23 Unity tests for type interning, structural equality, nullable/func constructors, scope CRUD, parent-chain, shadow, duplicate-rejection

## Decisions Made
- Primitive singletons in static array indexed by Iron_TypeKind value — zero allocation, pointer equality valid for all primitive comparisons
- IRON_TYPE_VOID, IRON_TYPE_NULL, IRON_TYPE_ERROR are also interned (same table) for uniform treatment in equality checks
- `iron_scope_define` takes a caller-allocated Iron_Symbol pointer; arena parameter reserved for future use
- `sh_new_strdup` mode means stb_ds owns key memory; `sym->name` is the caller-interned string (arena-copied by the caller)

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 1 - Bug] Fixed stack address returned from iron_type_to_string array case**
- **Found during:** Task 1 (build step after initial implementation)
- **Issue:** IRON_TYPE_ARRAY branch in iron_type_to_string used a local `char buf[256]` and returned a pointer to it as a fallback — `-Werror,-Wreturn-stack-address` caught it
- **Fix:** Allocate the output buffer directly from the arena before snprintf; use `"[...]"` string literal as fallback
- **Files modified:** src/analyzer/types.c
- **Verification:** Build passes with zero errors; all 23 tests pass
- **Committed in:** 2a6a5ce (part of task 1 commit)

---

**Total deviations:** 1 auto-fixed (1 bug)
**Impact on plan:** Required for correctness; the fix allocates the array string from the arena as originally intended. No scope creep.

## Issues Encountered
None beyond the single auto-fixed stack-address issue caught by -Werror.

## User Setup Required
None - no external service configuration required.

## Next Phase Readiness
- Iron_Type and Iron_Scope are ready for Plan 02-02 (name resolution pass)
- All 8 test suites (Phase 1 + new test_types) pass green
- No blockers for proceeding to semantic analysis

---
*Phase: 02-semantics-and-codegen*
*Completed: 2026-03-25*
