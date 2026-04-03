---
phase: 35
plan: 01
subsystem: concurrency-captures
tags: [spawn, parallel-for, capture, env-struct, emit-c, lir, hir]
dependency_graph:
  requires: [34-01]
  provides: [spawn-captures, pfor-captures]
  affects: [emit_c, lir_optimize, hir_lower, hir_to_lir, capture_analysis]
tech_stack:
  added: []
  patterns: [env-struct-heap-allocation, stack-array-compound-literal-wrap, dce-capture-tracking]
key_files:
  created:
    - tests/integration/capture_15_spawn_capture.iron
    - tests/integration/capture_15_spawn_capture.expected
    - tests/integration/capture_16_pfor_capture.iron
    - tests/integration/capture_16_pfor_capture.expected
  modified:
    - src/parser/ast.h
    - src/analyzer/capture.c
    - src/hir/hir.h
    - src/hir/hir_lower.c
    - src/hir/hir_to_lir.c
    - src/lir/lir.h
    - src/lir/lir.c
    - src/lir/lir_optimize.c
    - src/lir/emit_c.c
decisions:
  - Env struct fields for array captures use Iron_List_T (full runtime list struct) so the lifted function can iterate via .items and .count
  - Stack-array values wrapped as compound-literal Iron_List_T{ .items = _vN, .count = _vN_len } when populating env fields
  - DCE live-set for IRON_LIR_SPAWN must include all capture operands to prevent premature elimination of captured values
  - LiftPending carries capture_var_ids and capture_metadata arrays across Pass 2 to Pass 3 of HIR lowering
  - Capture analysis runs find_spawn_captures / find_pfor_captures using the existing collect_locals + collect_idents walk
metrics:
  duration_minutes: 90
  completed_date: "2026-04-03"
  tasks_completed: 1
  files_changed: 13
---

# Phase 35 Plan 01: Concurrency Captures Summary

Spawn/pfor capture support: env struct heap-allocation for spawn and parallel-for bodies, with stack-array-to-Iron_List compound-literal wrapping for array captures.

## Objective

Enable Iron `spawn("name") { ... }` blocks and `for i in N parallel { ... }` bodies to read variables from the enclosing scope. Specifically: a spawn block that captures `val data = [10, 20, 30]` and prints `sum: 60`, and a pfor body that captures a `val factors` array.

## Tasks Completed

### Task 1 — Full capture infrastructure (all 9 files)

**Commit:** 9fbd804

**What was built:**

1. **`src/parser/ast.h`** — Added `captures`/`capture_count` to `Iron_SpawnStmt` and `pfor_captures`/`pfor_capture_count` to `Iron_ForStmt`.

2. **`src/analyzer/capture.c`** — Added `find_spawn_captures` and `find_pfor_captures` using the existing `collect_locals` + `collect_idents` walk infrastructure. Called from `walk_node_for_lambdas` for `IRON_NODE_SPAWN` and parallel `IRON_NODE_FOR`.

3. **`src/hir/hir.h`** — Added `capture_var_ids`, `captures`, `capture_count` to spawn and parallel_for HIR nodes.

4. **`src/hir/hir_lower.c`** — `LiftPending` struct carries capture arrays. In both spawn paths (handled `val h = spawn(...)` and statement `spawn(...)`) and in the pfor path, the code calls `lookup_var` for each capture while the outer scope is still live (Pass 2), then stores them in HIR and `LiftPending`. LIFT_SPAWN and LIFT_PARALLEL_FOR inject captured vars as LET stmts in the lifted function scope.

5. **`src/hir/hir_to_lir.c`** — At spawn/pfor emit sites, evaluates capture values by checking `val_binding_map` first (immutable val captures), then `var_alloca_map` with LOAD (mutable var captures). Passes `cap_vals`, `cap_count`, `cap_meta` to `iron_lir_spawn` / `iron_lir_parallel_for`.

6. **`src/lir/lir.h` + `src/lir/lir.c`** — Added `captures`, `capture_count`, `capture_metadata` fields to SPAWN and PARALLEL_FOR LIR nodes; updated constructors.

7. **`src/lir/lir_optimize.c`** — DCE live-set building for `IRON_LIR_SPAWN` now includes all capture operands: `for (int i = 0; i < instr->spawn.capture_count; i++) PUSH(instr->spawn.captures[i]);` — prevents premature elimination of the ARRAY_LIT instruction producing the captured array.

8. **`src/lir/emit_c.c`** — Spawn and pfor emit fully rewritten to:
   - Declare `__spawn_0_env_t` struct typedef with capture field types
   - Emit wrapper function that receives `void *_arg`, casts to env struct, calls lifted function, frees arg
   - Allocate env on heap, populate fields at call site
   - Use `Iron_handle_create(wrapper, env)` for handled spawns with captures
   - Added `emit_capture_rhs()` helper: detects stack-array captures (via `get_stack_array_origin`) and emits `(Iron_List_T){ .items = _vN, .count = _vN_len }` compound literal instead of a plain variable name

### Test files

- `capture_15_spawn_capture.iron`: spawn block captures `val data = [10, 20, 30]`, iterates it, prints `sum: 60`
- `capture_16_pfor_capture.iron`: pfor body captures `val factors = [2, 4, 8]`, computes per-element, prints `pfor captured`

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 1 - Bug] DCE eliminated captured ARRAY_LIT value**
- **Found during:** Task 1 — first compile attempt after infrastructure changes
- **Issue:** `_env_5->data = _v4` failed with "use of undeclared identifier _v4" because DCE killed the ARRAY_LIT instruction for `val data = [10, 20, 30]`. The DCE live-set for `IRON_LIR_SPAWN` was not tracking capture operands.
- **Fix:** Added capture tracking loop to DCE live-set building in `lir_optimize.c`
- **Files modified:** `src/lir/lir_optimize.c`
- **Commit:** 9fbd804 (included in main commit)

**2. [Rule 1 - Bug] Stack-array type incompatible with Iron_List env struct field**
- **Found during:** Task 1 — second compile attempt after DCE fix
- **Issue:** Small arrays use stack representation (`int64_t _v4[] = {...}`) but env struct field is `Iron_List_int64_t`. The assignment `_env_5->data = _v4` produced a C type error.
- **Fix:** Added `emit_capture_rhs()` helper in `emit_c.c` that detects stack-array captures via `get_stack_array_origin()` and emits `(Iron_List_int64_t){ .items = _v4, .count = _v4_len }` compound literal
- **Files modified:** `src/lir/emit_c.c`
- **Commit:** 9fbd804 (included in main commit)

## Verification

- `capture_15_spawn_capture`: compiles, runs, prints `sum: 60` (correct)
- `capture_16_pfor_capture`: compiles, runs, prints `pfor captured` (correct)
- Full integration test suite: **212 passed, 0 failed** (218 total, 6 skipped for missing .expected)

## Self-Check: PASSED

Files verified:
- `tests/integration/capture_15_spawn_capture.iron` — FOUND
- `tests/integration/capture_15_spawn_capture.expected` — FOUND
- `tests/integration/capture_16_pfor_capture.iron` — FOUND
- `tests/integration/capture_16_pfor_capture.expected` — FOUND
- Commit `9fbd804` — FOUND
