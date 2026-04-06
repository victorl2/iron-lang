---
phase: 37-generic-enums
plan: 01
subsystem: type-system
tags: [generics, enum, monomorphization, ast, type-checker, parser, c-compiler]

# Dependency graph
requires:
  - phase: 36-methods-syntax-migration
    provides: enum methods and plain enum match lowering
  - phase: 34-hir-extensions-and-match-lowering
    provides: ADT match lowering, HIR extension, IRON_TYPE_GENERIC_PARAM
provides:
  - Iron_EnumDecl with generic_params/generic_param_count fields
  - Iron_Type.enu extended with type_args, type_arg_count, mangled_name
  - Parser accepting enum Option[T] { Some(T), None } syntax
  - Recursive type annotation parsing for nested generics (Result[Option[Int], String])
  - resolve_type_annotation producing monomorphized Iron_Type for generic enum instantiation
  - substitute_generic_type helper for GENERIC_PARAM -> concrete type substitution
  - IRON_NODE_ENUM_CONSTRUCT infers type args from argument types for generic enums
affects: [38-recursive-enums, 37-02-generic-enums-emit]

# Tech tracking
tech-stack:
  added: []
  patterns:
    - "Generic enum monomorphization at use site: type_args + mangled_name + substituted variant_payload_types stored on Iron_Type.enu"
    - "Temporary scope injection for generic param resolution: save/swap ctx->global_scope with a gen_scope containing GENERIC_PARAM symbols, restore after"
    - "Recursive type annotation parsing for use sites vs. iron_parse_generic_params for declaration sites"

key-files:
  created: []
  modified:
    - src/parser/ast.h
    - src/analyzer/types.h
    - src/analyzer/types.c
    - src/parser/parser.c
    - src/analyzer/typecheck.c

key-decisions:
  - "types.c includes parser/ast.h to access Iron_EnumDecl->name for iron_type_to_string — avoids forward-declaration limitation"
  - "Generic enum variant_payload_types pre-pass skips generic enums (generic_param_count > 0); monomorphization populates payload types at use site only"
  - "IRON_NODE_ENUM_CONSTRUCT for generic enums infers type args by matching GENERIC_PARAM payload types against actual arg types; unit variants return base uninstantiated type"
  - "Nested generic type annotations parse correctly via recursive iron_parse_type_annotation calls at use sites (not iron_parse_generic_params which only parses identifiers)"

patterns-established:
  - "substitute_generic_type: walk Iron_Type tree replacing IRON_TYPE_GENERIC_PARAM nodes by name lookup against ed->generic_params[i]"
  - "Temporary global scope swap for generic param resolution: iron_scope_create(ctx->arena, ctx->global_scope, IRON_SCOPE_BLOCK) + define GENERIC_PARAM symbols + swap/restore"

requirements-completed: [GENER-01, GENER-02]

# Metrics
duration: 35min
completed: 2026-04-04
---

# Phase 37 Plan 01: Generic Enums — AST, Types, Parser, and Type Checker Summary

**Generic enum monomorphization: Iron_EnumDecl and Iron_Type.enu extended with type_args/mangled_name; parser accepts `enum Option[T]`; type checker produces substituted monomorphized types at instantiation sites**

## Performance

- **Duration:** ~35 min
- **Started:** 2026-04-04T20:14:00Z
- **Completed:** 2026-04-04T20:49:24Z
- **Tasks:** 2
- **Files modified:** 5

## Accomplishments

- Extended `Iron_EnumDecl` with `generic_params`/`generic_param_count` following the `Iron_ObjectDecl` pattern
- Extended `Iron_Type.enu` with `type_args`, `type_arg_count`, and `mangled_name` fields; updated `iron_type_equals` and `iron_type_to_string` accordingly
- Parser now parses `[T, E]` after enum name; type annotation parser uses recursive calls for nested generics like `Result[Option[Int], String]`
- `resolve_type_annotation` produces fully monomorphized `Iron_Type` with substituted `variant_payload_types` when given a generic enum instantiation
- `IRON_NODE_ENUM_CONSTRUCT` handler infers type args from argument types for generic enum constructs
- Zero test regressions: 184/184 integration tests pass

## Task Commits

Each task was committed atomically:

1. **Task 1: Extend AST and type data structures for generic enums** - `3d6d59c` (feat)
2. **Task 2: Parser and type checker — parse generic enums + monomorphized type instantiation** - `2d64ecc` (feat)

## Files Created/Modified

- `src/parser/ast.h` - Added `generic_params`/`generic_param_count` fields to `Iron_EnumDecl`
- `src/analyzer/types.h` - Added `type_args`, `type_arg_count`, `mangled_name` to `Iron_Type.enu`
- `src/analyzer/types.c` - Updated `iron_type_equals` and `iron_type_to_string` for generic enums; added `#include "parser/ast.h"`
- `src/parser/parser.c` - `iron_parse_enum_decl` parses generic params; `iron_parse_type_annotation` uses recursive calls for generic args
- `src/analyzer/typecheck.c` - Added `substitute_generic_type` helper, generic enum monomorphization in `resolve_type_annotation`, skip generic enums in variant_payload_types pre-pass, generic enum construction inference in `IRON_NODE_ENUM_CONSTRUCT`

## Decisions Made

- `types.c` includes `parser/ast.h` to access `Iron_EnumDecl->name` for `iron_type_to_string`. The alternative (adding a name field to `Iron_Type.enu`) would have been more complex and redundant.
- The variant_payload_types pre-pass skips generic enums entirely; monomorphization at use site is the authoritative population mechanism.
- Unit variant constructs on generic enums (e.g., `Option.None` without type annotation) return the base uninstantiated enum type. The variable's declared type annotation provides monomorphized context, which is acceptable for this phase.

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 3 - Blocking] Iron_Span initializer incomplete**
- **Found during:** Task 2 (typecheck.c)
- **Issue:** `Iron_Span` has 5 fields (filename, line, col, end_line, end_col) but `{0, 0, 0, 0}` only initializes 4 — Werror treated missing initializer as error
- **Fix:** Changed all synthetic span initializers to `(Iron_Span){0, 0, 0, 0, 0}`
- **Files modified:** src/analyzer/typecheck.c
- **Verification:** Build succeeded with zero warnings
- **Committed in:** `2d64ecc` (Task 2 commit)

---

**Total deviations:** 1 auto-fixed (1 blocking)
**Impact on plan:** Blocking fix required for correctness. No scope creep.

## Issues Encountered

None significant. The `iron_type_to_string` ENUM case originally returned `"<enum>"` (not the decl name); the plan correctly identified this needed updating, and including `parser/ast.h` in `types.c` was the right fix.

## Next Phase Readiness

- Plan 02 (C emitter and HIR registration for generic enums) can proceed
- `Iron_EnumDecl.generic_param_count`, `Iron_Type.enu.mangled_name`, and `variant_payload_types` substitution are all in place
- `Option[Int]` and `Option[String]` are now structurally distinct types via `iron_type_equals`

---
*Phase: 37-generic-enums*
*Completed: 2026-04-04*
