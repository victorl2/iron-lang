---
phase: 09-c-emission-and-cutover
plan: 03
subsystem: ir/emit_c
tags: [bug-fix, c-emission, parallel, extern, block-labels]
dependency_graph:
  requires: [09-02]
  provides: [extern_basic-pass, test_range-pass, test_parallel-pass]
  affects: [src/ir/emit_c.c, src/ir/lower_stmts.c, src/ir/lower_exprs.c]
tech_stack:
  added: []
  patterns: [range-splitting-parallel-for, extern-c-name-resolution, label-sanitization]
key_files:
  created: []
  modified:
    - src/ir/emit_c.c
    - src/ir/lower_stmts.c
    - src/ir/lower_exprs.c
    - tests/ir/snapshots/if_else.expected
decisions:
  - resolve_func_c_name() helper centralizes extern/mangle logic for FUNC_REF and CALL paths
  - Iron_String args to extern calls need iron_string_cstr() coercion at emit time
  - PARALLEL_FOR emits context struct + wrapper function (void*) to bridge IR chunk signature to Iron_pool_submit
  - Function params emitted as _v{id} names to match IR value references (not plain param names)
  - Label sanitization: fix at lowerer (underscores) + safety belt sanitize_label() in emit_c.c
metrics:
  duration: 35 min
  completed: 2026-03-27
  tasks: 2
  files_changed: 4
---

# Phase 9 Plan 03: emit_c.c Bug Fixes Summary

Fixed three emit_c.c bugs causing extern_basic, test_range, and test_parallel integration tests to fail.

## What Was Built

**Task 1: FUNC_REF extern handling + block label sanitization**

- Added `resolve_func_c_name(EmitCtx *ctx, const char *ir_name)` helper that checks `is_extern`/`extern_c_name` before mangling; used in FUNC_REF case and indirect CALL path
- Added `sanitize_label(label, arena)` helper that replaces dots with underscores; applied in `emit_func_body` block label emission and threaded through `resolve_label()` at all goto sites
- Fixed extern function call argument emission: when calling an extern function (either via `func_decl` or FUNC_REF-detected indirect call), `Iron_String` arguments are converted to `const char*` via `iron_string_cstr(&arg)` — required because `Iron_String` is a 24-byte SSO struct, not a `const char*` alias
- Replaced all dot-containing block labels with underscores in `lower_stmts.c` (for_init, for_header, while_header, if_merge, match_join, etc.) and `lower_exprs.c` (and_rhs, or_merge, etc.)
- Updated `if_else.expected` snapshot to reflect renamed labels

**Task 2: PARALLEL_FOR emission with range-splitting pattern**

- Replaced `iron_pool_parallel_for`/`iron_default_pool()`/`Iron_ChunkFunc` (all non-existent) with correct runtime API
- Emits `__pfor_N_ctx` struct typedef (with `start`, `end` fields) and a `static void __pfor_N_wrapper(void*)` trampoline function into the `lifted_funcs` section; wrapper unpacks the struct, loops `[start, end)`, calls `Iron___pfor_N(int64_t i)` per iteration, and frees the context
- Emits inline range-splitting: `Iron_pool_thread_count(Iron_global_pool)` → chunk size → submission loop with `malloc'd` context per chunk → `Iron_pool_submit(Iron_global_pool, wrapper, ctx)` → `Iron_pool_barrier(Iron_global_pool)`
- Fixed SPAWN emission: replaced `iron_default_pool()`/`Iron_Future*`/`Iron_TaskFunc` with `Iron_global_pool`/`(void (*)(void*))` cast
- Fixed function parameter emission: params now emitted as `_v{id}` (e.g., `int64_t _v1`) to match IR value ID references in the function body (was emitting raw param names like `i` causing undefined `_v1`)

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 1 - Bug] extern call Iron_String → const char* coercion**
- **Found during:** Task 1 (extern_basic still failing after FUNC_REF name fix)
- **Issue:** `puts(_v2)` where `_v2: Iron_String` (24-byte struct) — clang rejected incompatible type
- **Fix:** Added extern call detection in CALL emission; wraps String-typed args with `iron_string_cstr(&arg)`
- **Files modified:** src/ir/emit_c.c
- **Commit:** d5c91fc

**2. [Rule 1 - Bug] Function params emitted with wrong names**
- **Found during:** Task 2 (chunk function `__pfor_0` had `_v2 = _v1` where `_v1` was undefined)
- **Issue:** `emit_func_signature` emitted params with plain names (`int64_t i`) but IR body references them as `_v{id}` (`_v1`); mismatch caused undefined identifier
- **Fix:** Changed `emit_func_signature` to emit params as `_v{1+i}` instead of `fn->params[i].name`
- **Files modified:** src/ir/emit_c.c
- **Commit:** 6dde380

**3. [Rule 1 - Bug] PARALLEL_FOR wrapper called unmangled chunk function name**
- **Found during:** Task 2 (wrapper called `__pfor_0` but C function is `Iron___pfor_0`)
- **Issue:** `chunk_func` IR name is `__pfor_0`, gets mangled to `Iron___pfor_0` in C; wrapper must use mangled name
- **Fix:** Use `mangle_func_name(chunk_func, ctx->arena)` for the wrapper call
- **Files modified:** src/ir/emit_c.c
- **Commit:** 6dde380

**4. [Rule 1 - Bug] Snapshot test failure after block label rename**
- **Found during:** Task 1 post-verification (unit test `test_snapshot_if_else` fails)
- **Issue:** `tests/ir/snapshots/if_else.expected` contained old `if.then`/`if.else`/`if.merge` labels
- **Fix:** Updated snapshot file to use `if_then`/`if_else`/`if_merge`
- **Files modified:** tests/ir/snapshots/if_else.expected
- **Commit:** 6dde380

## Self-Check: PASSED

- SUMMARY.md exists at `.planning/phases/09-c-emission-and-cutover/09-03-SUMMARY.md`
- Commit d5c91fc exists (Task 1)
- Commit 6dde380 exists (Task 2)
- All 3 target integration tests pass (extern_basic, test_range, test_parallel)
- All 24 unit tests pass (no regressions)
