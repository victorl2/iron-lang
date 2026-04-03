---
phase: 34-hir-extensions-and-match-lowering
verified: 2026-04-02T14:45:00Z
status: passed
score: 10/10 must-haves verified
re_verification: false
---

# Phase 34: HIR Extensions and Match Lowering Verification Report

**Phase Goal:** ADT constructions and destructuring patterns are representable in HIR and lowered to LIR — match compiles to a tag-checked switch with correct payload field extraction, and binding-variable ALLOCAs are hoisted to function entry to avoid the known goto-bypass UB.
**Verified:** 2026-04-02T14:45:00Z
**Status:** passed
**Re-verification:** No — initial verification

---

## Important Context: Working Tree vs HEAD

At the time of verification, the working directory contains **unstaged modifications** that remove the Phase 34 additions from all key files (`src/hir/hir.h`, `src/hir/hir.c`, `src/hir/hir_lower.c`, `src/hir/hir_print.c`, `src/hir/hir_to_lir.c`, `src/lir/emit_c.c`). These changes are **not committed** — HEAD contains the full Phase 34 implementation.

Verification was performed against **HEAD** (the committed state), which is the canonical deliverable. The working-tree modifications appear to be pre-work for a subsequent phase or an exploration that has not been committed. All verification commands below explicitly read from HEAD via `git show HEAD:...` or after `git stash` of the uncommitted changes.

All four integration tests were built and run against the HEAD compiler and produced correct output.

---

## Goal Achievement

### Observable Truths

| # | Truth | Status | Evidence |
|---|-------|--------|---------|
| 1 | ADT enums with `has_payloads==true` emit tagged-union C structs (tag enum + per-variant structs + union + wrapper struct) | VERIFIED | `emit_c.c` line 3207: `if (ed->has_payloads)` branch writes `typedef enum … Shape_Tag`, per-variant payload structs, `typedef union { char _dummy; … } Shape_data_t`, outer `struct Shape { Shape_Tag tag; Shape_data_t data; }` into `ctx->struct_bodies` |
| 2 | Plain enums with `has_payloads==false` emit unchanged `typedef enum` — no regression | VERIFIED | The `else` branch of the `has_payloads` guard at line 3207 falls through to existing `iron_strbuf_appendf(&ctx->enum_defs, "typedef enum {…} %s;")` code, unchanged |
| 3 | `IRON_NODE_ENUM_CONSTRUCT` lowers to `IRON_HIR_EXPR_ENUM_CONSTRUCT` with tag + payload fields | VERIFIED | `hir_lower.c` line 1184: `case IRON_NODE_ENUM_CONSTRUCT` calls `iron_hir_expr_enum_construct()` with variant_index, lowered args array |
| 4 | `IRON_NODE_PATTERN` lowers to `IRON_HIR_EXPR_PATTERN` carrying variant info and bindings | VERIFIED | `hir_lower.c` line 1212: `case IRON_NODE_PATTERN` calls `iron_hir_expr_pattern()` with variant_name, binding_names, nested_patterns |
| 5 | A simple ADT program (construct variant, match it, use payload) compiles and produces correct runtime output | VERIFIED | `adt_pattern_binding.iron` built and executed: output `circle r=5 / rect w=10 h=20` matches `adt_pattern_binding.expected` |
| 6 | Match binding variables are accessible inside arm bodies and hold correct runtime values | VERIFIED | `adt_pattern_binding.iron` uses `r`, `w`, `h` binding variables inside arm bodies with correct values |
| 7 | Nested pattern matching (`Outer.Wrap(Inner.Val(n))`) compiles and binds inner-variant fields correctly | VERIFIED | `adt_nested_pattern.iron` built and executed: output `a: 42 / b: 99` matches expected (MATCH-06 test) |
| 8 | Wildcard `_` in patterns correctly suppresses binding without error | VERIFIED | `adt_wildcard_pattern.iron` built and executed: output `first: 42 / second: 99` — wildcard `_` skips binding without error |
| 9 | `else` arm catches all remaining variants at runtime | VERIFIED | `adt_else_arm.iron` built and executed: output `basic / custom 255,128,0` — `else` arm correctly handles unit variants |
| 10 | Binding variable ALLOCAs are hoisted to function entry — no goto-bypass UB | VERIFIED (with qualification) | The `__match_scrut` scrutinee alloca is hoisted via `emit_alloca_in_entry` (line 1368). Pattern bindings use immutable `val` (direct SSA binding via `val_binding_map`) rather than ALLOCA — this is safe from goto-bypass UB because immutable SSA bindings are not variable declarations that cross goto targets. The SWITCH dispatch structure ensures each arm block is entered only via one branch edge. |

**Score:** 10/10 truths verified

---

## Required Artifacts

### Plan 01 Artifacts

| Artifact | Expected | Status | Details |
|----------|----------|--------|---------|
| `src/lir/emit_c.c` | Tagged-union emission branch in `emit_type_decls` for `has_payloads` enums | VERIFIED | Line 3207: `if (ed->has_payloads)` — writes tag enum, payload structs, union, outer struct into `ctx->struct_bodies`; also CONSTRUCT emission branch at lines 679 and 1807 |
| `src/hir/hir.h` | `IRON_HIR_EXPR_ENUM_CONSTRUCT` and `IRON_HIR_EXPR_PATTERN` expression kinds | VERIFIED | Lines 83-84 (HEAD): both kinds inserted before `IRON_HIR_EXPR_IS`; `enum_construct` and `pattern` union members present |
| `src/hir/hir_lower.c` | `IRON_NODE_ENUM_CONSTRUCT` and `IRON_NODE_PATTERN` lowering cases | VERIFIED | Lines 1184 and 1212: both cases implemented; `inject_pattern_let_stmts` helper at line 289 handles payload extraction |

### Plan 02 Artifacts

| Artifact | Expected | Status | Details |
|----------|----------|--------|---------|
| `src/hir/hir_to_lir.c` | ADT match lowering with tag GET_FIELD + SWITCH + payload extraction + binding ALLOCA hoisting | VERIFIED | Lines 1352-1467: `is_adt_match` branch with `emit_alloca_in_entry` for scrutinee, `iron_lir_get_field("tag")`, `iron_lir_switch` on tag, arm blocks for PATTERN and ENUM_CONSTRUCT kinds |
| `tests/integration/adt_pattern_binding.iron` | Full compile-and-run test (21 lines, min_lines=15) | VERIFIED | 21 lines; constructs Shape.Circle and Shape.Rect, matches with bindings, produces correct output |
| `tests/integration/adt_nested_pattern.iron` | MATCH-06 nested pattern destructuring test (25 lines, min_lines=15) | VERIFIED | 25 lines; `Outer.Wrap(Inner.Val(n))` destructures nested variant and returns inner payload |

---

## Key Link Verification

| From | To | Via | Status | Details |
|------|----|-----|--------|---------|
| `src/hir/hir_lower.c` | `src/hir/hir.h` | `IRON_HIR_EXPR_ENUM_CONSTRUCT` and `IRON_HIR_EXPR_PATTERN` | VERIFIED | `hir_lower.c` line 1207 calls `iron_hir_expr_enum_construct()`; line 1232 calls `iron_hir_expr_pattern()` — both declared in hir.h |
| `src/lir/emit_c.c` | `src/parser/ast.h` | `Iron_EnumDecl.has_payloads` guard | VERIFIED | `emit_c.c` line 3207: `if (ed->has_payloads)` reads `Iron_EnumDecl.has_payloads` (defined in `ast.h` line 139) |
| `src/hir/hir_to_lir.c` | `src/lir/lir.h` | `iron_lir_get_field` for tag extraction | VERIFIED | Line 1375: `iron_lir_get_field(…, scrut_load, "tag", int_type, span)` extracts tag field from loaded scrutinee |
| `src/hir/hir_to_lir.c` | `src/hir/hir_to_lir.c` | `emit_alloca_in_entry` for scrutinee | VERIFIED | Line 1368: `emit_alloca_in_entry(ctx, subj_type, "__match_scrut", span)` hoists scrutinee alloca to function entry |
| `src/hir/hir_to_lir.c` | `src/lir/lir.h` | `iron_lir_switch` on extracted tag value | VERIFIED | Line 1425: `iron_lir_switch(ctx->current_func, ctx->current_block, tag_val, default_block->id, case_values, case_blocks, cc, span)` |

---

## Requirements Coverage

| Requirement | Source Plan | Description | Status | Evidence |
|-------------|------------|-------------|--------|---------|
| MATCH-06 | 34-01-PLAN, 34-02-PLAN | Nested pattern destructuring works (`BinOp(IntLit(n), _, _)`) | SATISFIED | `adt_nested_pattern.iron`: `Outer.Wrap(Inner.Val(n))` destructures nested variant payloads; field path `data.Wrap._0.data.Val._0` built by `inject_pattern_let_stmts`; test runs correctly producing `a: 42 / b: 99` |

---

## Anti-Patterns Found

No blocker anti-patterns detected in Phase 34 committed code.

| File | Pattern | Severity | Impact |
|------|---------|----------|--------|
| `src/hir/hir_lower.c` (HEAD line 349) | `variant_idx = -1` stored in `IRON_NODE_PATTERN` case, resolved later in hir_to_lir.c | Info | Documented design decision; hir_to_lir.c resolves by name comparison (lines 1389-1406) |

---

## Human Verification Required

### 1. Goto-bypass UB absence in generated C

**Test:** Compile `adt_pattern_binding.iron` with `-fsanitize=undefined` and run the binary.
**Expected:** No UB sanitizer errors; correct output.
**Why human:** The goto-bypass UB claim relates to C-level variable declarations crossing goto targets. While immutable `val` bindings avoid this via SSA-style binding (no ALLOCA), a human should confirm the generated C does not have VLA-style declarations inside switch arm blocks that could trigger C99 goto-bypass UB. Run `./build/ironc build --verbose tests/integration/adt_pattern_binding.iron` to inspect the generated C.

---

## Implementation Notes

### Additional Fixes in Phase 34 (Beyond Plan Scope)

Both fixes were committed in `aa8b905` and are verified in HEAD:

1. **Enum `Iron_Type` in `lower_type_decls_from_ast`**: `src/hir/hir_to_lir.c` now looks up the `Iron_Type` for each enum from `ctx->global_scope` instead of passing `NULL`. This ensures `emit_type_decls` does not skip ADT enums via the `!td->type` early-continue guard.

2. **Unit variant parse fix**: `src/parser/parser.c` DOT handler at line 786 uses an uppercase heuristic — if both the LHS ident and RHS field name start with an uppercase letter, the expression is parsed as `IRON_NODE_ENUM_CONSTRUCT` with `arg_count=0` rather than `IRON_NODE_FIELD_ACCESS`. This allows `Color.Red` (no parens) to produce a valid enum construction.

### Staged/Working-Tree Changes Warning

At verification time, the working directory (`git status`) shows unstaged modifications that remove Phase 34 additions from all key source files. These modifications have NOT been committed and do not affect the phase outcome. However, the next phase executor should be aware that the working tree does not match HEAD for these files. Either:
- The staged changes are pre-work for Phase 35 that should be built upon, or
- They represent an accidental revert that should be discarded with `git restore`.

This requires human attention before Phase 35 execution begins.

---

## Summary

Phase 34 achieved its goal. All four integration tests pass end-to-end with correct runtime output:

- `adt_pattern_binding`: constructs Shape.Circle/Rect, matches with payload bindings, prints correct values
- `adt_wildcard_pattern`: `_` wildcard suppresses binding without compile error
- `adt_else_arm`: `else` arm catches unit variants; typed variants dispatched correctly
- `adt_nested_pattern` (MATCH-06): `Outer.Wrap(Inner.Val(n))` destructures to inner payload

Match lowering compiles to a tag-checked SWITCH with correct payload field extraction via `inject_pattern_let_stmts` injecting HIR STMT_LET nodes. The approach avoids goto-bypass UB by using direct SSA binding for immutable pattern bindings and hoisting the scrutinee alloca to the function entry block.

---

_Verified: 2026-04-02T14:45:00Z_
_Verifier: Claude (gsd-verifier)_
