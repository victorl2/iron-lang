---
phase: 47-collection-methods
plan: 01
subsystem: compiler
tags: [parser, type-checker, resolver, generics, array-methods, extension-methods]

# Dependency graph
requires: []
provides:
  - "Array extension method syntax: func [T].method_name[U](...) -> RetType { body }"
  - "is_array_extension and elem_type_name fields on Iron_MethodDecl"
  - "Declaration-based array method return type resolution in type checker"
  - "Method chaining support for array methods on non-ident receivers"
  - "Type error for sum() on non-numeric arrays"
affects: [47-02, 47-03, hir-lowering, emit-c, stdlib-collections]

# Tech tracking
tech-stack:
  added: []
  patterns:
    - "Array extension method sentinel type_name __Array"
    - "resolve_array_ext_method helper for decl-based type inference"
    - "Heuristic fallback for built-in array methods (push/pop/len/etc)"

key-files:
  created: []
  modified:
    - src/parser/ast.h
    - src/parser/parser.c
    - src/analyzer/resolve.c
    - src/analyzer/typecheck.c

key-decisions:
  - "Used __Array sentinel for type_name to distinguish array extension methods from regular methods"
  - "Extracted resolve_array_ext_method and resolve_array_builtin_method as helpers for reuse in chaining path"
  - "Kept heuristic fallback for existing built-in methods (push, pop, len, etc) to avoid regressions"

patterns-established:
  - "Array extension method syntax: func [T].method_name[U](params) -> RetType { body }"
  - "Extension method type resolution: search program decls for is_array_extension methods, substitute T/U generics"

requirements-completed: [COLL-05, COLL-06, COLL-07, COLL-08, COLL-09]

# Metrics
duration: 7min
completed: 2026-04-07
---

# Phase 47 Plan 01: Array Extension Method Syntax Summary

**Generic array extension method syntax (func [T].method[U](...)) in parser, resolver, and type checker with declaration-based return type resolution**

## Performance

- **Duration:** 7 min
- **Started:** 2026-04-07T17:08:09Z
- **Completed:** 2026-04-07T17:15:36Z
- **Tasks:** 2
- **Files modified:** 4

## Accomplishments
- Parser recognizes `func [T].method_name[U](...) -> RetType { body }` and produces Iron_MethodDecl with is_array_extension=true and elem_type_name set
- Resolver skips owner_sym lookup for array extension methods (no owning type in scope)
- Type checker resolves array method return types from extension method declarations: map returns [U] (inferred from lambda), filter returns [T], reduce returns U (inferred from init), sum returns T, forEach returns Void
- Method chaining on arrays works via non-ident receiver path (e.g. arr.map(...).filter(...))
- Type error emitted for sum() on non-numeric arrays
- All 238 existing integration tests pass with zero regressions

## Task Commits

Each task was committed atomically:

1. **Task 1: Add array extension method syntax to parser and AST** - `75787a8` (feat)
2. **Task 2: Update resolver and type checker for array extension methods** - `ebec130` (feat)

## Files Created/Modified
- `src/parser/ast.h` - Added is_array_extension and elem_type_name fields to Iron_MethodDecl
- `src/parser/parser.c` - Added parsing of func [T].method_name[U](...) syntax before existing name check
- `src/analyzer/resolve.c` - Early return in attach_method for array extension methods
- `src/analyzer/typecheck.c` - Added resolve_array_ext_method and resolve_array_builtin_method helpers, replaced heuristic block

## Decisions Made
- Used `__Array` sentinel as type_name for array extension methods rather than introducing a new node kind, minimizing changes to downstream passes
- Extracted type resolution into helper functions (resolve_array_ext_method, resolve_array_builtin_method) for reuse in both the IDENT and non-IDENT receiver paths (method chaining)
- Kept heuristic fallback for existing built-in methods (push, pop, len, get, set, free, sort, reverse, any, all, find) to preserve backward compatibility

## Deviations from Plan

None - plan executed exactly as written.

## Issues Encountered
- Iron_Node base struct does not have resolved_type field; args[0]->resolved_type compilation error. Fixed by using check_expr(ctx, mc->args[0]) to retrieve arg types (idempotent re-check).
- resolve_array_builtin_method had unused ctx parameter causing -Werror failure. Removed parameter from signature.

## User Setup Required
None - no external service configuration required.

## Next Phase Readiness
- Parser and type checker ready for Plan 02 (stdlib collection method declarations)
- Extension method declarations like `func [T].map[U](...)` will be recognized and type-checked
- HIR/LIR/emit_c lowering for array extension methods needed in Plan 03

---
*Phase: 47-collection-methods*
*Completed: 2026-04-07*
