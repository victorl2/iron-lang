---
phase: 34-hir-extensions-and-match-lowering
plan: "01"
subsystem: HIR/LIR ADT lowering and C emission
tags: [adt, hir, lir, emit_c, tagged-union, pattern-matching]
dependency_graph:
  requires: [33-01, 33-02]
  provides: [tagged-union C struct emission, HIR ENUM_CONSTRUCT/PATTERN kinds, AST-to-HIR lowering for ADT nodes]
  affects: [hir_to_lir.c (Plan 02 consumes IRON_HIR_EXPR_ENUM_CONSTRUCT and IRON_HIR_EXPR_PATTERN)]
tech_stack:
  added: []
  patterns: [has_payloads guard in emit_type_decls, tagged-union struct layout in struct_bodies, new HIR expr kinds with arena-allocated constructors]
key_files:
  created: []
  modified:
    - src/lir/emit_c.c
    - src/hir/hir.h
    - src/hir/hir.c
    - src/hir/hir_lower.c
    - src/hir/hir_print.c
decisions:
  - "ADT enum structs emitted into struct_bodies (not enum_defs) so per-variant payload structs resolve in C output order"
  - "IRON_HIR_EXPR_ENUM_CONSTRUCT and IRON_HIR_EXPR_PATTERN inserted before IRON_HIR_EXPR_IS in the kind enum"
  - "IRON_NODE_PATTERN lowering stores variant_index = -1 (resolved from match scrutinee type in hir_to_lir.c Plan 02)"
  - "Forward declaration for ADT struct typedef added to forward_decls so other structs can reference it"
metrics:
  duration: 4min
  completed: "2026-04-03"
  tasks: 2
  files: 5
requirements:
  - MATCH-06
---

# Phase 34 Plan 01: HIR Extensions and ADT C Emission Summary

Tagged-union C struct emission for ADT enums and HIR lowering of IRON_NODE_ENUM_CONSTRUCT / IRON_NODE_PATTERN from AST to two new HIR expression kinds.

## What Was Built

### Task 1: Tagged-union C struct emission (emit_c.c)

The `emit_type_decls()` function in `src/lir/emit_c.c` now splits ADT enums from plain enums using the `ed->has_payloads` flag.

**For `has_payloads == true`:** Writes to `ctx->struct_bodies` (ensuring correct C output order relative to forward declarations):
- A forward declaration `typedef struct {mangled} {mangled};` in `ctx->forward_decls`
- A tag enum: `typedef enum { Shape_TAG_Circle = 0, ... } Shape_Tag;`
- Per-variant payload structs (only for variants with `payload_count > 0`): `typedef struct { double _0; } Shape_Circle_data;`
- A union with mandatory `char _dummy` member: `typedef union { char _dummy; Shape_Circle_data Circle; } Shape_data_t;`
- The outer ADT struct: `struct Shape { Shape_Tag tag; Shape_data_t data; };`

**For `has_payloads == false`:** Existing plain `typedef enum { ... } T;` emission is completely unchanged.

The `IRON_LIR_CONSTRUCT` emission (both statement path at ~line 1803 and inline `emit_expr_to_buf` path at ~line 665) has a new branch for `IRON_TYPE_ENUM && has_payloads`. It extracts the variant index by reading the `CONST_INT` value of `field_vals[0]` from `fn->value_table`, looks up `ed->variants[variant_idx]->name`, then emits: `{ .tag = Shape_TAG_Circle, .data.Circle = { ._0 = val } }`.

### Task 2: HIR expression kinds and AST-to-HIR lowering

**hir.h additions:**
- Two new `IronHIR_ExprKind` values: `IRON_HIR_EXPR_ENUM_CONSTRUCT` and `IRON_HIR_EXPR_PATTERN` (inserted before `IRON_HIR_EXPR_IS`)
- `enum_construct` union member with fields: `type`, `enum_name`, `variant_name`, `variant_index`, `args`, `arg_count`
- `pattern` union member with fields: `enum_name`, `variant_name`, `variant_index`, `binding_names`, `nested_patterns`, `binding_count`
- Constructor declarations: `iron_hir_expr_enum_construct()` and `iron_hir_expr_pattern()`

**hir.c additions:**
- `iron_hir_expr_enum_construct()`: arena-allocates, sets `IRON_HIR_EXPR_ENUM_CONSTRUCT`, copies all fields
- `iron_hir_expr_pattern()`: arena-allocates, sets `IRON_HIR_EXPR_PATTERN`, `type = NULL` (patterns have no value type)

**hir_lower.c additions:**
- `case IRON_NODE_ENUM_CONSTRUCT`: iterates `ec->args`, calls `lower_expr_hir` on each, finds variant_index by name comparison, returns `iron_hir_expr_enum_construct()`
- `case IRON_NODE_PATTERN`: recursively lowers nested patterns via `lower_expr_hir`, copies `binding_names`, stores `variant_idx = -1` (resolved later in Plan 02 from scrutinee type)

**hir_print.c additions:**
- `IRON_HIR_EXPR_ENUM_CONSTRUCT`: prints `EnumConstruct(Shape.Circle)` with child args
- `IRON_HIR_EXPR_PATTERN`: prints `Pattern(Shape.Circle [r, _])` with nested patterns

## Verification

- `cmake --build build --target ironc` passes with no errors
- `iron check` on `adt_pattern_binding.iron`, `adt_else_arm.iron`, `adt_wildcard_pattern.iron`, `adt_enum_construct.iron` all pass
- `IRON_HIR_EXPR_ENUM_CONSTRUCT` confirmed in hir.h (line 83)
- `ed->has_payloads` guard confirmed in emit_c.c (line 3207)
- `_TAG_` emission confirmed in emit_c.c (lines 694, 1825, 3219)
- `_data_t` union typedef confirmed in emit_c.c (line 3254)
- `char _dummy` confirmed in emit_c.c (line 3245)
- `case IRON_NODE_ENUM_CONSTRUCT:` and `case IRON_NODE_PATTERN:` confirmed in hir_lower.c

## Deviations from Plan

None — plan executed exactly as written.

## Commits

| Task | Commit | Description |
|------|--------|-------------|
| 1 | 31b59cd | feat(34-01): emit tagged-union C structs for ADT enums in emit_c.c |
| 2 | 84b9b3c | feat(34-01): add HIR expression kinds and lower ENUM_CONSTRUCT + PATTERN to HIR |

## Self-Check: PASSED

All modified files exist. Both task commits verified in git log.
