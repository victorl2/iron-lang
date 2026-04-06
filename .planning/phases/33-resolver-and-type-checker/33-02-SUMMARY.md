---
phase: 33-resolver-and-type-checker
plan: 02
subsystem: analyzer
tags: [type-checker, adt, exhaustiveness, pattern-matching, enum-construct]
dependency_graph:
  requires: [33-01]
  provides: [variant-payload-type-population, enum-construct-type-resolution, exhaustiveness-checking, binding-type-assignment]
  affects: [33-03, hir_lower]
tech_stack:
  added: []
  patterns: [covered-variant bitset for exhaustiveness, tc_push_scope/tc_pop_scope per arm, pre-pass for enum type population]
key_files:
  created:
    - tests/integration/adt_pattern_binding.iron
    - tests/integration/adt_pattern_binding.expected
    - tests/integration/adt_wildcard_pattern.iron
    - tests/integration/adt_wildcard_pattern.expected
    - tests/integration/adt_else_arm.iron
    - tests/integration/adt_else_arm.expected
  modified:
    - src/analyzer/typecheck.c
    - tests/unit/test_typecheck.c
decisions:
  - "variant_payload_types population runs as a dedicated pre-pass in iron_typecheck before function signatures and bodies, ensuring types are available when IRON_NODE_ENUM_CONSTRUCT and IRON_NODE_MATCH are processed"
  - "IRON_NODE_MATCH exhaustiveness checking only fires when subject_type->kind == IRON_TYPE_ENUM && ed->has_payloads — integer matches are unaffected"
  - "IRON_DIAG_NOTE (code 0) emitted for else-arm catch list — not an error or warning, just informational"
  - "IRON_NODE_MATCH_CASE uses tc_push_scope/tc_pop_scope mirroring the resolver; binding types assigned via tc_define using ptypes from variant_payload_types"
metrics:
  duration: 22min
  completed: "2026-04-02"
  tasks: 3
  files_changed: 8
---

# Phase 33 Plan 02: ADT Type Checker Extension Summary

**One-liner:** ADT type checker — variant payload type population, enum construct type resolution with arity checking, exhaustiveness via covered-variant bitset, binding type assignment, and 8 new unit tests backed by 3 new integration tests.

## What Was Built

Extended the Iron type checker (Pass 3) to fully support ADT enum types: populating `variant_payload_types`, type-checking construction expressions, enforcing exhaustiveness on match statements, and assigning types to pattern bindings.

### typecheck.c — Five additions (1597 lines → 1823 lines)

**1. `find_variant_index` helper (static, after forward declarations)**
Searches an `Iron_EnumDecl` for a variant by name; returns index or -1. Used by all three ADT-related cases.

**2. Pre-pass in `iron_typecheck`: populate `variant_payload_types`**
A new dedicated loop over `program->decls` (runs after the val/var pre-pass and before the function-signature pre-pass) processes every `IRON_NODE_ENUM_DECL` with `has_payloads == true`. For each such enum it:
- Allocates `Iron_Type ***vpt` (outer array of `Iron_Type **` pointers, one per variant)
- For each variant with `payload_count > 0`, resolves each `payload_type_anns[j]` via `resolve_type_annotation` and stores into a per-variant `Iron_Type **` row
- Sets `ty->enu.variant_payload_types = vpt` on the enum's `Iron_Type`

This runs before function bodies so that `IRON_NODE_ENUM_CONSTRUCT` and `IRON_NODE_MATCH` processing can read fully-populated payload types.

**3. `IRON_NODE_ENUM_CONSTRUCT` case in `check_expr`**
- Looks up enum type in global scope by `ec->enum_name`
- Finds variant index via `find_variant_index`
- Checks `ec->arg_count == ev->payload_count` — emits `IRON_ERR_PATTERN_ARITY` (225) on mismatch
- Type-checks each argument against `variant_payload_types[vi][j]` — emits `IRON_ERR_ARG_TYPE` (217) on type mismatch
- Sets `ec->resolved_type = enum_type`; returns the enum type

**4. `IRON_NODE_MATCH` case in `check_stmt` (replaced)**
- Calls `check_expr` on subject (capturing `subject_type`)
- Processes all case arms via `check_stmt`
- If `subject_type->kind == IRON_TYPE_ENUM && ed->has_payloads`:
  - Allocates `bool *covered[variant_count]` via arena
  - Walks each `IRON_NODE_PATTERN` arm: marks `covered[vi] = true` or emits `IRON_ERR_UNREACHABLE_ARM` (226) for duplicates; also checks `binding_count == payload_count` emitting `IRON_ERR_PATTERN_ARITY` (225) on mismatch
  - If no else arm: collects uncovered variants and emits `IRON_ERR_NONEXHAUSTIVE_MATCH` (224) with variant names
  - If else arm present and uncovered variants remain: emits `IRON_DIAG_NOTE` listing "else catches: ..."

**5. `IRON_NODE_MATCH_CASE` case in `check_stmt` (replaced)**
- Calls `tc_push_scope(ctx, IRON_SCOPE_BLOCK)` at entry
- For `IRON_NODE_PATTERN` patterns: looks up enum type (qualified via `enum_name` or via global scope variant lookup), then calls `tc_define` for each non-wildcard binding with the corresponding `variant_payload_types[vi][j]` type
- For other patterns (integer literals etc.): `check_expr`
- Checks body via `check_stmt`
- Calls `tc_pop_scope(ctx)` at exit

### tests/unit/test_typecheck.c — 8 new tests (26 existing → 34 total)

| Test | What it verifies | Expected |
|------|-----------------|---------|
| `test_tc_adt_enum_construct_type` | `Shape.Circle(1.0)` produces no error | 0 errors |
| `test_tc_adt_enum_construct_arity_error` | `Shape.Circle(1.0, 2.0)` (1 payload, 2 args) | error 225 |
| `test_tc_adt_exhaustive_error` | Missing `Rect` arm in 2-variant match | error 224 |
| `test_tc_adt_exhaustive_with_else` | Circle arm + else — exhaustiveness satisfied | 0 errors |
| `test_tc_adt_unreachable_arm` | Circle arm repeated twice | error 226 |
| `test_tc_adt_exhaustive_all_variants` | Both Circle and Rect covered | 0 errors |
| `test_tc_adt_wildcard_in_pattern` | `Rect(_, h)` — wildcard skipped, `h` typed | 0 errors |
| `test_tc_adt_integer_match_unchanged` | Integer match with else — no exhaustiveness error | 0 errors |

### tests/integration/ — 6 new files (3 .iron + 3 .expected)

All three integration tests are deliberately minimal (enum decl + `println` in main) because HIR lowering for ADT match is not yet implemented (Phase 34+35). They verify that enum declarations with payloads compile and run correctly with the new type checker pre-pass active.

| Test | Enum | Notes |
|------|------|-------|
| `adt_pattern_binding` | Shape { Circle(Float), Rect(Float, Float) } | tests payload types population |
| `adt_wildcard_pattern` | Shape { Rect(Float, Float) } | tests single-payload enum |
| `adt_else_arm` | Color { Red, Green, Blue, Custom(Int, Int, Int) } | tests mixed plain/payload enum |

## Deviations from Plan

None — plan executed exactly as written.

## Verification Results

- Compiler build: PASSED (`cmake --build build --target iron_compiler`)
- Type checker unit tests: PASSED (34/34 — 26 existing + 8 new, 0 failures)
- Resolver unit tests: PASSED (unchanged, all passing)
- Full unit suite: PASSED (17/17 test targets, 0 failures)
- Integration tests: 178/187 passed — 3 pre-existing failures (smoke_test, test_combined, test_io — unrelated to this plan), 3 skips (no .expected file), 3 new ADT tests all PASS

## Self-Check: PASSED
