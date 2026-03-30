---
phase: 20-hir-lowering-pipeline-cutover
plan: "01"
subsystem: hir
tags: [hir, lowering, ast-to-hir, compiler-pipeline]
dependency_graph:
  requires: [hir.c, hir.h, ast.h, types.h, scope.h]
  provides: [iron_hir_lower, hir_lower.h, hir_lower.c]
  affects: [Phase 20 HIR pipeline, future HIR-to-LIR pass]
tech_stack:
  added: []
  patterns: [three-pass lowering, stb_ds scope stack, lazy global lowering, lift pending]
key_files:
  created:
    - src/hir/hir_lower.h
    - src/hir/hir_lower.c
    - tests/hir/test_hir_lower.c
  modified:
    - CMakeLists.txt
    - tests/hir/CMakeLists.txt
decisions:
  - stb_ds array ownership transfer to HIR constructors — callers must NOT arrfree arrays passed to iron_hir_expr_call/method_call/construct/array_lit/interp_string
  - Scope stack uses array of ScopeFrame* (stb_ds hash maps) for O(1) name lookup per scope
  - For-range desugaring: DOTDOT binary expr triggers while loop; single integer iterable also treated as range 0..n
  - Global constant lazy lowering injects STMT_LET at current block position and registers in scope to prevent re-injection
  - iron_hir_verify called at end of iron_hir_lower using stack-allocated arena (iron_arena_create value type)
metrics:
  duration_minutes: 9
  completed_date: "2026-03-30"
  tasks_completed: 2
  files_changed: 5
---

# Phase 20 Plan 01: AST-to-HIR Lowering Summary

Complete three-pass AST-to-HIR lowering with all Iron language constructs, four desugarings, lambda/spawn/pfor lifting, and 15 passing smoke tests.

## What Was Built

### Task 1: hir_lower.h + hir_lower.c (1,432 lines)

`iron_hir_lower()` converts a fully-analyzed `Iron_Program` to an `IronHIR_Module` in three passes:

**Pass 1 — `lower_module_decls_hir()`:**
- Registers all `IRON_NODE_FUNC_DECL` and `IRON_NODE_METHOD_DECL` as `IronHIR_Func*` entries in the module
- Methods get mangled names: `TypeName_methodName` with implicit `self` as first param
- Top-level `val`/`var` declarations collected into `global_constants_map` for lazy lowering
- Type annotations resolved to `Iron_Type*` via `resolve_type_ann()`

**Pass 2 — `lower_func_bodies_hir()`:**
- Lowers each function/method body into the pre-registered `IronHIR_Func*`
- Scope stack (stb_ds array of hash maps) provides `O(1)` name → VarId lookup
- Params assigned VarIds and registered in scope before body lowering

**Pass 3 — `lower_lift_pending_hir()`:**
- Lambda expressions → `__lambda_N` top-level functions
- Spawn statements → `__spawn_N` void functions
- Parallel-for statements → `__pfor_N` functions taking loop var as Int param

**Four desugarings:**
1. `elif` chains → nested `STMT_IF` in else branch
2. `for i in start..end` → `STMT_LET i = start` + `STMT_WHILE(i < end, { body; i = i + 1 })`
3. `+=`, `-=`, `*=`, `/=` → `STMT_ASSIGN(target, BINOP(op, target, value))`
4. String interpolation → kept as `IRON_HIR_EXPR_INTERP_STRING` (HIR supports it natively)

**Output verification:** `iron_hir_verify()` called at end; returns NULL on verify failure.

### Task 2: CMakeLists + 15 smoke tests

- `src/hir/hir_lower.c` added to `iron_compiler` static library in CMakeLists.txt
- `test_hir_lower` executable registered with `hir` label in tests/hir/CMakeLists.txt
- 740-line test file with 15 smoke tests covering all major lowering paths

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 1 - Bug] Use-after-free on stb_ds arrays passed to HIR constructors**
- **Found during:** Task 2 test execution (AddressSanitizer)
- **Issue:** `arrfree(args)` was called after passing `args` to `iron_hir_expr_call()`. The HIR expr stored the pointer directly — `arrfree` freed the backing memory, leaving `call.args` dangling.
- **Fix:** Removed `arrfree()` calls on `args`, `parts`, `elems`, `names`, `vals`, and `arms` arrays after passing them to HIR constructors. Ownership transfers to the HIR expression/statement.
- **Files modified:** `src/hir/hir_lower.c`
- **Commit:** 5569a5b (included in Task 2 commit)

## Self-Check: PASSED

All created files verified on disk:
- FOUND: src/hir/hir_lower.h
- FOUND: src/hir/hir_lower.c
- FOUND: tests/hir/test_hir_lower.c
- FOUND: .planning/phases/20-hir-lowering-pipeline-cutover/20-01-SUMMARY.md

All commits verified in git history:
- FOUND: bc8937b (Task 1 — feat: implement AST-to-HIR lowering pass)
- FOUND: 5569a5b (Task 2 — feat: add CMakeLists + 15 smoke tests)
