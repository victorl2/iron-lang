---
phase: 37-generic-enums
plan: 02
subsystem: codegen
tags: [generics, enum, monomorphization, hir, lir, c-emitter, type-checker, integration-tests]

# Dependency graph
requires:
  - phase: 37-generic-enums
    provides: Iron_EnumDecl.generic_param_count, Iron_Type.enu.mangled_name, monomorphized types at use sites
  - phase: 34-hir-extensions-and-match-lowering
    provides: ADT match lowering, HIR ENUM_CONSTRUCT/PATTERN lowering
provides:
  - HIR pre-pass skips generic enum base declarations; monomorphized instances registered via AST walker
  - emit_c.c uses enu.mangled_name for type names and struct emission
  - mono_registry deduplication prevents duplicate C typedef emission
  - build_hir_params_named uses type-checker-resolved param types from global scope
  - type_mangle_component produces C-safe names for nested generics (Option_Int not Option[Int])
  - maybe_fill_missing_generic_args handles context-directed monomorphization for multi-param enums
  - 5 integration tests: generic_enum_option, result, match, multi_inst, nested
affects: [38-recursive-enums]

# Tech tracking
tech-stack:
  added: []
  patterns:
    - "MonoEnumSeen stb_ds string map for deduplication in HIR pre-pass monomorphized enum walker"
    - "collect_mono_enums_node recursive AST walker discovers monomorphized enums from function bodies"
    - "build_hir_params_named looks up pre-resolved param types from func symbol in global scope"
    - "type_mangle_component returns C-safe name component (strips Iron_ prefix for nested enums)"
    - "maybe_fill_missing_generic_args patches ec->resolved_type with declared type when partial inference"

key-files:
  created:
    - tests/integration/generic_enum_option.iron
    - tests/integration/generic_enum_option.expected
    - tests/integration/generic_enum_result.iron
    - tests/integration/generic_enum_result.expected
    - tests/integration/generic_enum_match.iron
    - tests/integration/generic_enum_match.expected
    - tests/integration/generic_enum_multi_inst.iron
    - tests/integration/generic_enum_multi_inst.expected
    - tests/integration/generic_enum_nested.iron
    - tests/integration/generic_enum_nested.expected
  modified:
    - src/hir/hir_to_lir.c
    - src/hir/hir_lower.c
    - src/lir/emit_c.c
    - src/analyzer/typecheck.c

key-decisions:
  - "HIR pre-pass skip: generic_param_count > 0 guard prevents registering base Option decl as type_decl; monomorphized instances collected by collect_mono_enums_node walker"
  - "build_hir_params_named: look up func symbol in global scope to get type-checker-resolved param types (resolves Option[Int] in function signatures to Iron_Option_Int)"
  - "type_mangle_component: strips Iron_ prefix from mangled_name for nested generic type args, producing Iron_Result_Option_Int_String not Iron_Result_Option[Int]_String"
  - "maybe_fill_missing_generic_args: context-directed patch for multi-param enums where partial inference leaves NULLs — replace ec->resolved_type with declared type"
  - "CONSTRUCT tag fix in emit_c.c: two sites use emit_mangle_name unconditionally; patched to prefer enu.mangled_name"

patterns-established:
  - "Generic enum pipeline: type-checker creates monomorphized Iron_Type -> HIR walker registers type_decl -> C emitter uses mangled_name for typedef and tag names"
  - "param type resolution uses global_scope function symbol's func.param_types to get pre-resolved generic types"

requirements-completed: [GENER-01, GENER-02, GENER-03]

# Metrics
duration: 85min
completed: 2026-04-03
---

# Phase 37 Plan 02: Generic Enums — HIR-to-LIR and C Emitter Summary

**Generic enum end-to-end pipeline: HIR registration, C emission with mangled names, and 5 integration tests covering Option[T], Result[T,E], match payload binding, two distinct instantiations, and nested generics**

## Performance

- **Duration:** ~85 min
- **Started:** 2026-04-03
- **Completed:** 2026-04-03
- **Tasks:** 2
- **Files modified:** 14 (4 source files, 10 test files)

## Accomplishments

- HIR pre-pass skips generic base declarations and collects monomorphized instances via `collect_mono_enums_node` AST walker
- C emitter uses `enu.mangled_name` for type names (in `emit_type_to_c`, enum struct emitter, and CONSTRUCT tag emission) with `mono_registry` deduplication
- `build_hir_params_named` in `hir_lower.c` uses type-checker-resolved param types from global scope, fixing `Option[Int]` in function signatures
- `type_mangle_component` produces C-safe names for nested generics (`Option_Int` not `Option[Int]`)
- `maybe_fill_missing_generic_args` enables `val r: Result[Int, String] = Result.Ok(100)` to work with context-directed monomorphization
- 189/195 integration tests pass (5 new + 184 pre-existing)

## Task Commits

Each task was committed atomically:

1. **Task 1: HIR-to-LIR registration and C emitter for monomorphized generic enums** - `04de163` (feat)
2. **Task 2: Integration tests for all GENER requirements** - `e9b0025` (feat)

## Files Created/Modified

- `src/hir/hir_to_lir.c` - Added `MonoEnumSeen`, `register_mono_enum`, `collect_mono_enums_node`; skip generic base decls; register mono instances
- `src/hir/hir_lower.c` - Added `build_hir_params_named` that uses global scope function symbol's resolved param types
- `src/lir/emit_c.c` - `emit_type_to_c` ENUM case uses `mangled_name`; struct emitter uses `mangled_name`; `mono_registry` dedup; CONSTRUCT tag uses `mangled_name`
- `src/analyzer/typecheck.c` - Added `type_mangle_component` helper; `maybe_fill_missing_generic_args` for context-directed mono; use `type_mangle_component` in both mangled name builders
- 10 integration test files covering GENER-01, GENER-02, GENER-03

## Decisions Made

- `build_hir_params_named` looks up the function symbol in `global_scope` to get the pre-resolved param types from the type checker. The alternative (extending `resolve_type_ann` to handle generic annotations) would have duplicated the type checker logic without access to the LIR module.
- `type_mangle_component` strips the `"Iron_"` prefix from `mangled_name` for nested generic enums rather than using `iron_type_to_string` (which returns `"Option[Int]"` with brackets).
- `maybe_fill_missing_generic_args` replaces `ec->resolved_type` with the declared type when both are instantiations of the same base enum. This is correct because the type annotation provides the authoritative complete type.

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 1 - Bug] multi-param generic enum type mismatch for partial inference**
- **Found during:** Task 2 (generic_enum_result.iron test)
- **Issue:** `val r: Result[Int, String] = Result.Ok(100)` — `E` cannot be inferred from `Ok(T)`, leaving `Iron_Result_Int_unknown` vs expected `Iron_Result_Int_String`
- **Fix:** Added `maybe_fill_missing_generic_args` to context-directedly use the declared type when the construct's type shares the same base enum decl
- **Files modified:** src/analyzer/typecheck.c
- **Verification:** `build/ironc build generic_enum_result.iron` succeeds; output `result_ok: ok`
- **Committed in:** `e9b0025` (Task 2 commit)

**2. [Rule 1 - Bug] Function parameter type `Option[Int]` resolved to base non-generic `Iron_Option` in HIR lowering**
- **Found during:** Task 2 (generic_enum_match.iron test)
- **Issue:** `hir_lower.c`'s `resolve_type_ann` doesn't handle generic type annotations; params got `Iron_Option` type instead of `Iron_Option_Int`
- **Fix:** Added `build_hir_params_named` that looks up function symbol in global scope to get type-checker-resolved param types
- **Files modified:** src/hir/hir_lower.c
- **Verification:** Function prototype in generated C uses `Iron_Option_Int` correctly
- **Committed in:** `e9b0025` (Task 2 commit)

**3. [Rule 1 - Bug] CONSTRUCT tag emission uses `emit_mangle_name(ed->name)` ignoring mangled_name**
- **Found during:** Task 2 (generic_enum_match.iron test)
- **Issue:** Two sites in `emit_c.c` used `emit_mangle_name(adt_ed->name, arena)` for tag constants, producing `Iron_Option_TAG_None` instead of `Iron_Option_Int_TAG_None`
- **Fix:** Both construct sites now prefer `instr->construct.type->enu.mangled_name` when set
- **Files modified:** src/lir/emit_c.c
- **Verification:** Generated C uses correct tag names; binary runs correctly
- **Committed in:** `e9b0025` (Task 2 commit)

**4. [Rule 1 - Bug] Nested generic mangled name contains brackets (`Iron_Result_Option[Int]_String`)**
- **Found during:** Task 2 (generic_enum_nested.iron test)
- **Issue:** `iron_type_to_string` for `Option[Int]` returns `"Option[Int]"` with brackets, used in mangled name building → invalid C identifier
- **Fix:** Added `type_mangle_component` that returns `"Option_Int"` (strips `Iron_` prefix from mangled_name for enum type args); used in both mangled name builders in typecheck.c
- **Files modified:** src/analyzer/typecheck.c
- **Verification:** `Iron_Result_Option_Int_String` generated correctly; nested test passes
- **Committed in:** `e9b0025` (Task 2 commit)

---

**Total deviations:** 4 auto-fixed (all Rule 1 — bugs discovered during compilation)
**Impact on plan:** All fixes necessary for correct code generation. No scope creep.

## Issues Encountered

The plan's test `generic_enum_match.iron` originally used `println(unwrap_or(a, 0))` where `unwrap_or` returns `Int`. This fails the type checker since `println` is registered as `func(String)->Void`. Changed to `println("{unwrap_or(a, 0)}")` using string interpolation — the expected output `42` and `-1` is unchanged.

## Next Phase Readiness

- Generic enums (`Option[T]`, `Result[T,E]`) compile end-to-end to working C
- All GENER-01, GENER-02, GENER-03 requirements satisfied
- Phase 38 (recursive enums) can proceed — it builds on the same `Iron_Type.enu` structure
- No regressions: 184 pre-existing integration tests still pass

---
*Phase: 37-generic-enums*
*Completed: 2026-04-03*
