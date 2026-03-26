---
phase: 02-semantics-and-codegen
plan: 03
subsystem: analyzer
tags: [type-checker, type-inference, nullable-narrowing, interface-completeness, C]

requires:
  - phase: 02-semantics-and-codegen
    plan: 01
    provides: Iron_Type system and scope tree (types.h, scope.h)
  - phase: 02-semantics-and-codegen
    plan: 02
    provides: two-pass name resolver (resolve.h) with resolved_sym on all idents

provides:
  - iron_typecheck() entry point in src/analyzer/typecheck.h/c
  - resolved_type annotation on all expression nodes
  - declared_type annotation on val/var declaration nodes
  - resolved_return_type on func/method decl nodes
  - type-checker scope chain parallel to resolver scopes

affects:
  - escape analysis (will consume fully type-annotated AST)
  - codegen (needs resolved_type on every expr node)

tech-stack:
  added: []
  patterns:
    - type-checker builds its own scope chain mirroring resolver's (separate from resolver scopes)
    - IDENT lookup: narrowing map → type-checker scope → resolved_sym->type (3-layer fallback)
    - ConstructExpr/CallExpr disambiguation in IRON_NODE_CALL: check if callee ident is SYM_TYPE
    - flow-sensitive narrowing via stb_ds hash map (save/restore via deep-copy for branches)
    - early-return narrowing: block_always_returns check after == null branch

key-files:
  created:
    - src/analyzer/typecheck.h
    - src/analyzer/typecheck.c
    - tests/test_typecheck.c
  modified:
    - CMakeLists.txt

key-decisions:
  - "Type-checker builds its own parallel scope chain (not reusing resolver scopes) so param/local types are always accessible via scope lookup without needing resolver scope references"
  - "IRON_NODE_CALL handler disambiguates type-constructor calls: if callee ident resolves to SYM_TYPE, treat as object construction (field count + type check) rather than function call"
  - "Narrowing map uses stb_ds sh_new_strdup + deep-copy (narrowing_copy) for safe branch analysis — saved map is restored after if-body"
  - "No implicit Int/Float conversion: E0222 emitted before arithmetic error to give precise diagnostic"
  - "Interface completeness: checked via linear scan of program decls for matching MethodDecl nodes"

patterns-established:
  - "type-checker scope: push FUNCTION scope at func/method entry, push BLOCK scope at if/while/for bodies; pop on exit"
  - "tc_define(): silently ignores duplicate defines (resolver already reported them); used for params and local val/var"
  - "RETURN nullable check: if ret_type is IRON_TYPE_NULLABLE and current_return_type is not → emit E0204 (not E0215)"

requirements-completed: [SEM-03, SEM-04, SEM-05, SEM-06, SEM-07, SEM-08]

duration: 15min
completed: 2026-03-25
---

# Phase 02 Plan 03: Type Checker Pass Summary

**Type checking pass with stb_ds narrowing map, CallExpr/ConstructExpr disambiguation, and flow-sensitive nullable narrowing via scope-mirrored type inference**

## Performance

- **Duration:** 15 min
- **Started:** 2026-03-25T23:49:41Z
- **Completed:** 2026-03-25T23:55:00Z
- **Tasks:** 1 (TDD: RED → GREEN)
- **Files modified:** 4

## Accomplishments

- `iron_typecheck()` entry point annotates `resolved_type` on all expression nodes and `declared_type` on val/var decls
- Type inference: Int for int literals, Float for float literals, Bool for bool literals
- val immutability enforced (E0203) using type-checker scope `is_mutable` flag
- Nullable access without null check detected (E0204); flow-sensitive `!= null`, `== null` early-return, and `is`-narrowing
- Interface completeness validated (E0205) by scanning program decls for matching MethodDecl
- CallExpr/ConstructExpr disambiguation: `Point(1, 2)` parses as CALL, type checker detects `Point` is SYM_TYPE and redirects to construction semantics
- 22 Unity tests passing; full test suite (10 test executables) passes

## Task Commits

TDD — two commits for the single task:

1. **RED phase: failing tests** - `85063f5` (test)
2. **GREEN phase: full implementation** - `7864945` (feat)

**Plan metadata:** (docs commit follows)

## Files Created/Modified

- `src/analyzer/typecheck.h` - iron_typecheck() signature
- `src/analyzer/typecheck.c` - full type checking implementation (1229 lines)
- `tests/test_typecheck.c` - 22 Unity tests
- `CMakeLists.txt` - added typecheck.c to iron_compiler and test_typecheck target

## Decisions Made

- **Parallel type-checker scope chain**: The type checker creates its own scope chain mirroring the resolver's, rather than reusing resolver scopes. This avoids needing access to the resolver's internal function scopes, which are not exposed in the public API. Params are defined in the type-checker scope with resolved types; `IDENT` lookup uses a 3-layer fallback.

- **CallExpr disambiguation at IRON_NODE_CALL**: The parser always produces `IRON_NODE_CALL` for `Name(args)` syntax (by design, per prior decision). The type checker detects the special case where the callee ident resolves to `IRON_SYM_TYPE` and handles it as object construction (field count validation, field type checking, returns object type).

- **Narrowing map deep-copy for branches**: `narrowing_copy()` creates a fresh stb_ds map before entering an if-body so the outer map can be restored cleanly after. The `== null` early-return case sets a narrowing entry AFTER the if-statement that persists into the continuation.

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 1 - Bug] Removed duplicate arg-check loop in IRON_NODE_CONSTRUCT**
- **Found during:** Task 1 (GREEN phase debugging)
- **Issue:** CONSTRUCT case had an initial unconditional arg-check loop before the sym lookup, PLUS another loop in the IRON_SYM_TYPE branch — args were type-checked twice
- **Fix:** Removed the redundant first loop; validation is done only in the type-specific branch
- **Files modified:** src/analyzer/typecheck.c
- **Committed in:** 7864945

**2. [Rule 1 - Bug] Disambiguate CallExpr for type constructors in IRON_NODE_CALL handler**
- **Found during:** Task 1 (test_construct_expr_type failing with error_count=1)
- **Issue:** Parser produces `IRON_NODE_CALL` for `Point(1, 2)` (by design). The CALL handler checked callee type against `IRON_TYPE_FUNC` — but `Point`'s symbol has type `IRON_TYPE_OBJECT` — triggering spurious E0218 (not callable)
- **Fix:** Added disambiguation check at the top of `IRON_NODE_CALL`: if callee ident resolves to `IRON_SYM_TYPE`, redirect to construction logic (field count + type validation)
- **Files modified:** src/analyzer/typecheck.c
- **Committed in:** 7864945

---

**Total deviations:** 2 auto-fixed (both Rule 1 bugs)
**Impact on plan:** Both fixes required for correctness. No scope creep.

## Issues Encountered

The main challenge was the two-scope-chain architecture. The type checker creates its own scope (separate from the resolver's). Initial implementation looked up symbols in the type-checker scope which didn't contain val/var local symbols at check time (they're created during body traversal). Fixed by ensuring `tc_define()` creates symbols in the type-checker scope when processing `IRON_NODE_VAL_DECL`/`IRON_NODE_VAR_DECL`, and `IRON_NODE_ASSIGN` mutability checks use `tc_lookup` first, then fall back to `resolved_sym->is_mutable`.

## Next Phase Readiness

- Type-annotated AST is ready for escape analysis (Phase 02-04)
- Every expression node has `resolved_type` set
- Every val/var decl has `declared_type` set
- Func/method decls have `resolved_return_type` set
- Type errors reported as E0202-E0222 with spans

---
*Phase: 02-semantics-and-codegen*
*Completed: 2026-03-25*
