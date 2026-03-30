---
phase: 19-lir-rename-hir-foundation
plan: "03"
subsystem: hir
tags: [hir, printer, verifier, testing, snapshots]
dependency_graph:
  requires: ["19-02"]
  provides: ["iron_hir_print", "iron_hir_verify"]
  affects: ["phase-20-lowering"]
tech_stack:
  added: []
  patterns: ["stb_ds hmput/hmget scope tracking", "golden snapshot testing", "collect-all error accumulation"]
key_files:
  created:
    - src/hir/hir_print.c
    - src/hir/hir_verify.c
    - tests/hir/test_hir_print.c
    - tests/hir/test_hir_verify.c
    - tests/hir/snapshots/simple_func.txt
    - tests/hir/snapshots/control_flow.txt
    - tests/hir/snapshots/closures_concurrency.txt
  modified:
    - src/hir/hir.h
    - src/diagnostics/diagnostics.h
    - CMakeLists.txt
    - tests/hir/CMakeLists.txt
decisions:
  - "Printer handles NULL types gracefully by printing '?' — enables testing without full type system setup"
  - "Verifier uses stb_ds hmput/hmget int-keyed hash maps for scope frames — avoids linear scan per lookup"
  - "Scope stack is dynamic array of hash-map pointers (ScopeEntry**) — allows O(depth) lookup, O(1) push/pop"
  - "HIR error codes in 500 range to avoid collision with LIR (300), lowering (400), and semantic (200) codes"
metrics:
  duration_seconds: 840
  completed_date: "2026-03-30"
  tasks_completed: 2
  files_created_or_modified: 10
---

# Phase 19 Plan 03: HIR Printer and Verifier Summary

HIR printer producing indented tree dumps and HIR verifier enforcing scope/structural correctness, with 21 passing tests.

## What Was Built

### Task 1: HIR Printer and Verifier (src/hir/)

**`src/hir/hir_print.c`** — Implements `iron_hir_print(const IronHIR_Module*)` returning a heap-allocated string:
- 2-space indentation per depth level per locked project convention
- Handles all 13 statement kinds and all 27 expression kinds
- Uses `Iron_StrBuf` for output building (same pattern as LIR printer)
- Temporary arena for `iron_type_to_string` calls
- NULL types print as `?` for graceful handling during early-phase testing

**`src/hir/hir_verify.c`** — Implements `iron_hir_verify(const IronHIR_Module*, Iron_DiagList*, Iron_Arena*)`:
- Scope tracking via stb_ds int-keyed hash maps (ScopeEntry** stack)
- `push_scope`/`pop_scope`/`declare_var`/`lookup_var` helpers
- Detects: use-before-def, duplicate bindings, scope isolation violations, NULL structural pointers
- Collects ALL errors without early abort (verified by `test_hir_verify_collects_all_errors` requiring >= 3 errors)

**`src/diagnostics/diagnostics.h`** — Added HIR verifier error codes in the 500 range:
- `IRON_ERR_HIR_NULL_POINTER` (500)
- `IRON_ERR_HIR_USE_BEFORE_DEF` (501)
- `IRON_ERR_HIR_DUPLICATE_BINDING` (502)
- `IRON_ERR_HIR_TYPE_MISMATCH` (503)
- `IRON_ERR_HIR_ARG_COUNT_MISMATCH` (504)
- `IRON_ERR_HIR_INVALID_SCOPE` (505)
- `IRON_ERR_HIR_MISSING_RETURN_VALUE` (506)
- `IRON_ERR_HIR_STRUCTURAL` (507)

**`src/hir/hir.h`** — Added declarations for `iron_hir_print` and `iron_hir_verify`.

### Task 2: Tests and Snapshots

**`tests/hir/test_hir_print.c`** — 3 snapshot comparison tests:
1. `test_hir_print_simple_func` — function with params, let binding, binop, return
2. `test_hir_print_control_flow` — if/else, while loop, for loop
3. `test_hir_print_closures_concurrency` — closure expr, defer stmt, spawn stmt

**`tests/hir/test_hir_verify.c`** — 8 verifier tests:
1. `test_hir_verify_valid_tree` — well-formed tree with params, let, if/else, return
2. `test_hir_verify_use_before_def` — ghost VarId never declared
3. `test_hir_verify_duplicate_binding` — same VarId declared twice in same scope
4. `test_hir_verify_scope_isolation` — variable from then-branch used after if
5. `test_hir_verify_null_pointer` — binop with NULL left operand
6. `test_hir_verify_nested_scopes` — inner var used after inner block closes
7. `test_hir_verify_closure_scope` — closure param used outside closure body
8. `test_hir_verify_collects_all_errors` — 3 independent errors, all reported

## Deviations from Plan

None — plan executed exactly as written.

## Test Results

- All 21 HIR tests pass: 10 data + 3 print + 8 verify
- Full LIR regression suite unaffected (LIR data, print, verify, emit, optimize, lower all green)

## Self-Check

**Created files exist:**
- src/hir/hir_print.c: FOUND
- src/hir/hir_verify.c: FOUND
- tests/hir/test_hir_print.c: FOUND
- tests/hir/test_hir_verify.c: FOUND
- tests/hir/snapshots/simple_func.txt: FOUND
- tests/hir/snapshots/control_flow.txt: FOUND
- tests/hir/snapshots/closures_concurrency.txt: FOUND

**Commits exist:**
- 6587ec3: feat(19-03): implement HIR printer and verifier
- dce0ee0: feat(19-03): add HIR printer snapshot tests and verifier unit tests

## Self-Check: PASSED
