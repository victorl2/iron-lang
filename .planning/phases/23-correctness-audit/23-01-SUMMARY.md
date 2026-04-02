---
phase: 23-correctness-audit
plan: 01
subsystem: hir-pipeline
tags: [audit, correctness, heap, method-call, field-access]
dependency_graph:
  requires: []
  provides: [audit-checklist, heap-ptr-correctness, method-call-on-heap]
  affects: [hir_to_lir.c, emit_c.c]
tech_stack:
  added: []
  patterns: [RC-typed alloca for heap pointer, val_is_heap_ptr LOAD chain tracing, heap-ptr deref at method call site]
key_files:
  created:
    - .planning/phases/23-correctness-audit/audit-checklist.md
    - tests/integration/bug_audit_mutable_heap_field.iron
    - tests/integration/bug_audit_mutable_heap_field.expected
    - tests/integration/bug_audit_heap_method_call.iron
    - tests/integration/bug_audit_heap_method_call.expected
  modified:
    - src/hir/hir_to_lir.c
    - src/lir/emit_c.c
decisions:
  - "[23-01]: mutable heap var alloca typed as RC(T) (=T*) to match heap_alloc pointer semantics in C"
  - "[23-01]: val_is_heap_ptr() follows LOAD -> ALLOCA chain to detect pointer-holding vars"
  - "[23-01]: method call on heap object derefs self arg (*ptr) when callee param is IRON_TYPE_OBJECT"
  - "[23-01]: EXPR_IS is POISON (known unimplemented feature) - safe to leave as documented limitation"
  - "[23-01]: MAKE_CLOSURE (ret(*)()) no-prototype cast is known limitation, not a correctness bug"
metrics:
  duration: "~60 minutes"
  completed: "2026-03-31"
  tasks: 1
  files: 7
---

# Phase 23 Plan 01: HIR Pipeline Correctness Audit Summary

Systematic correctness audit of every HIR instruction kind through the HIR->LIR->C pipeline, resulting in the discovery and fix of 2 correctness bugs and production of a comprehensive written checklist.

## What Was Done

Traced all 41 HIR instruction kinds (13 statements, 28 expressions) through:
- `lower_stmt()` / `lower_expr()` in `hir_to_lir.c`
- LIR instruction kinds in `lir.h`
- `emit_instr()` in `emit_c.c`

Verified the 5 high-risk emission paths: CONSTRUCT, PARALLEL_FOR, MAKE_CLOSURE, GET_FIELD arrow/dot, IS_CHECK.

## Bugs Found and Fixed

### Bug 1: Mutable heap variable alloca wrong type (Rule 1)

**Found during:** IRON_HIR_STMT_LET audit + GET_FIELD high-risk review

**Issue:** `var p = heap T(...)` created an alloca typed as `T` (value), but `heap_alloc` emits `T*` (pointer) in C. The STORE instruction `_v1 = _v5` failed C compilation with "assigning to 'T' from incompatible type 'T*'". Additionally, GET_FIELD on loaded heap pointers used `.` instead of `->`.

**Root cause:** `emit_alloca_in_entry` received the HIR variable type (inner object type), not a pointer type. `obj_is_ptr` detection only checked `HEAP_ALLOC | RC_ALLOC` instruction kinds, missing LOAD results from pointer-holding allocas.

**Fix:**
- `hir_to_lir.c` STMT_LET: when `is_mut` and init is `IRON_HIR_EXPR_HEAP` or `IRON_HIR_EXPR_RC`, wrap alloca type as `iron_type_make_rc(lir_arena, type)` so emit_c declares it as `T*`
- `emit_c.c`: add `val_is_heap_ptr()` helper that checks HEAP_ALLOC, RC_ALLOC, and LOAD from RC-typed ALLOCA (via `fn->value_table[ptr]->kind == IRON_LIR_ALLOCA && alloc_type->kind == IRON_TYPE_RC`)
- `emit_c.c` GET_FIELD/SET_FIELD: replace inline `obj_is_ptr` checks with `val_is_heap_ptr()`
- `emit_c.c` LOAD emission: when loading from an RC-typed alloca, use the alloca's RC type (T*) for the C declaration

**Files:** `src/hir/hir_to_lir.c`, `src/lir/emit_c.c`
**Test:** `tests/integration/bug_audit_mutable_heap_field.iron`

### Bug 2: Method call on heap object passes T* where T expected (Rule 1)

**Found during:** IRON_HIR_EXPR_METHOD_CALL + EXPR_CALL high-risk review

**Issue:** When calling `heap_obj.method()`, the self value was a `T*` (HEAP_ALLOC or LOAD from RC-typed alloca) but the method signature expected `T` (value type). This caused C compilation error: "passing 'Iron_T *' to parameter of incompatible type 'Iron_T'".

**Root cause:** METHOD_CALL lowering passes `lower_expr(object)` directly as first arg without considering pointer dereference. The CALL emission passed the raw pointer value without the needed `*`.

**Fix:**
- `emit_c.c` CALL argument emission: if `val_is_heap_ptr(fn, arg_id)` AND the callee function (looked up via `find_ir_func`) has parameter `i` of kind `IRON_TYPE_OBJECT`, emit `(*arg_val)` instead of `arg_val`

**Files:** `src/lir/emit_c.c`
**Test:** `tests/integration/bug_audit_heap_method_call.iron`

## Known Limitations Documented

1. **EXPR_IS (IS type test)**: Emits POISON. The IS expression for interface/enum type tests has no runtime tag infrastructure. Silent UB if used in condition. Zero existing tests exercise this path.

2. **MAKE_CLOSURE no-prototype cast**: `(ret (*)())` generates `-Wdeprecated-non-prototype` in Clang. Requires parameter type propagation at indirect call sites to fix properly.

3. **PFOR captures always zero**: pfor bodies cannot access outer variables. Outer access causes compile error (not silent wrong behavior). Intentional design.

4. **STMT_LEAK auto_free interaction**: Leak stmt evaluates without storing; heap_alloc auto_free=true could still trigger free at return. Edge case not tested.

## Test Results

All existing tests pass with zero regressions:
- `test_integration`: 110+ integration tests PASSED
- `test_algorithms`: 13 algorithm tests PASSED
- `test_composite`: composite tests PASSED

## Decisions Made

- RC-typed alloca is the correct vehicle for "pointer-holding variable" without adding a new LIR instruction or Iron type
- `val_is_heap_ptr()` follows LOAD->ALLOCA chain using kind check guard to avoid union UB
- Method self-arg dereference emitted at call site (emit_c.c) rather than lowering (hir_to_lir.c) to keep LIR clean and avoid adding a DEREF instruction

## Self-Check: PASSED
