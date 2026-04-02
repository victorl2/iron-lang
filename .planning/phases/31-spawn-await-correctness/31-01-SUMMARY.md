---
phase: 31-spawn-await-correctness
plan: 01
subsystem: compiler
tags: [spawn, await, concurrency, runtime, typecheck, emit_c, hir_to_lir, integration-tests]

# Dependency graph
requires:
  - phase: 30-benchmark-validation
    provides: benchmark infrastructure and validated test suite baseline

provides:
  - "iron_future_await() runtime function: blocks caller, returns spawned result, destroys handle"
  - "iron_handle_create_self_ref() runtime: creates handle with self as arg for wrapper capture pattern"
  - "Parser: val h = spawn(...) and var h = spawn(...) set handle_name on SpawnStmt"
  - "Typechecker: spawn body return type propagated to await via spawn_result_types map"
  - "Typechecker: fire-and-forget spawn emits IRON_WARN_SPAWN_NO_HANDLE warning"
  - "HIR-to-LIR: handled spawns use IRON_TYPE_NULL to distinguish from void spawns"
  - "emit_c.c: handled spawns generate wrapper function + iron_handle_create_self_ref call"
  - "Integration tests: spawn_await, spawn_await_return, spawn_await_multiple, spawn_pfor_implicit_join"

affects:
  - 31-02-benchmark-updates
  - any future concurrency features

# Tech tracking
tech-stack:
  added: []
  patterns:
    - "Wrapper function pattern: static void wrapper(void *_arg) casts _arg to Iron_Handle*, calls lifted fn, stores (void*)(intptr_t)result into _h->result"
    - "Self-ref handle: iron_handle_create_self_ref passes the handle itself as the wrapper arg"
    - "stb_ds shmap for spawn_result_types: handle_name -> Iron_Type* propagated from spawn body to await"

key-files:
  created:
    - tests/integration/spawn_await_return.iron
    - tests/integration/spawn_await_return.expected
    - tests/integration/spawn_await_multiple.iron
    - tests/integration/spawn_await_multiple.expected
    - tests/integration/spawn_pfor_implicit_join.iron
    - tests/integration/spawn_pfor_implicit_join.expected
  modified:
    - src/runtime/iron_threads.c
    - src/runtime/iron_runtime.h
    - src/diagnostics/diagnostics.h
    - src/parser/parser.c
    - src/analyzer/typecheck.c
    - src/hir/hir_to_lir.c
    - src/lir/emit_c.c
    - tests/integration/spawn_await.iron
    - tests/integration/spawn_await.expected

key-decisions:
  - "IRON_TYPE_NULL used for handled spawn LIR type (not IRON_TYPE_OBJECT): NULL kind is a distinct primitive that emits as void* and is != IRON_TYPE_VOID; avoids conflating object type with handle pointer"
  - "Wrapper function emitted into lifted_funcs section: handles result capture without changing lifted HIR function signature (void no-arg stays void no-arg)"
  - "iron_handle_create_self_ref passes handle as its own arg: eliminates need for separate arg struct, making wrapper pattern simpler"
  - "stb_ds sh_new_strdup map for spawn_result_types: handle_name key maps to Iron_Type* body return type; initialized in check_program, freed at end"

patterns-established:
  - "Handle-wrapper emit pattern: any spawn with handle_name gets wrapper + iron_handle_create_self_ref at call site"
  - "spawn_result_types shmap: type propagation from spawn body through await expression"

requirements-completed: []

# Metrics
duration: 45min
completed: 2026-04-01
---

# Phase 31 Plan 01: Spawn/Await Correctness — Runtime + Compiler Pipeline Summary

**spawn/await end-to-end: iron_future_await runtime, wrapper-function emit pattern, type propagation via spawn_result_types shmap, and 4 integration tests passing**

## Performance

- **Duration:** ~45 min
- **Started:** 2026-04-01
- **Completed:** 2026-04-01T20:35:09Z
- **Tasks:** 3
- **Files modified:** 9 modified, 6 created

## Accomplishments

- `val h = spawn("name") { return 42 }; val r = await h` compiles and r == 42 end-to-end
- `iron_future_await()` and `iron_handle_create_self_ref()` added to runtime (iron_threads.c + iron_runtime.h)
- emit_c.c generates a `static void wrapper(void *_arg)` function for each handled spawn that captures the lifted function's return value into the Iron_Handle's result field
- Parser modified to set `handle_name` on SpawnStmt when spawn appears as val/var initializer
- Typechecker propagates spawn body return type to await expression via `spawn_result_types` stb_ds shmap
- Fire-and-forget spawn emits `IRON_WARN_SPAWN_NO_HANDLE` (600) warning
- All 173 integration tests pass; all 13 algorithm tests pass

## Task Commits

1. **Task 1: Runtime functions + diagnostics + parser + typecheck** - `aa7692a` (feat)
2. **Task 2: HIR-to-LIR + emit_c.c spawn emission** - folded into `aa7692a` (feat)
3. **Task 3: Integration tests for spawn/await** - `ecd166f` (test)

## Files Created/Modified

- `src/runtime/iron_threads.c` - Added `iron_future_await()` and `iron_handle_create_self_ref()` implementations
- `src/runtime/iron_runtime.h` - Added declarations for `iron_future_await` and `iron_handle_create_self_ref`
- `src/diagnostics/diagnostics.h` - Added `IRON_WARN_SPAWN_NO_HANDLE 600` warning code
- `src/parser/parser.c` - Modified `iron_parse_val_decl` and `iron_parse_var_decl` to detect spawn and set `handle_name`
- `src/analyzer/typecheck.c` - Added `spawn_result_types` shmap; spawn case stores body return type; await case looks it up; fire-and-forget emits warning
- `src/hir/hir_to_lir.c` - Handled spawns use `IRON_TYPE_NULL` type; spawn LIR value bound to `handle_var` via `val_binding_map`
- `src/lir/emit_c.c` - SPAWN case generates wrapper function + `iron_handle_create_self_ref` for handled spawns
- `tests/integration/spawn_await.iron` - Rewritten to use `val h = spawn(...) { return value }` + `await h` pattern
- `tests/integration/spawn_await.expected` - Updated expected output for new test
- `tests/integration/spawn_await_return.iron` - New: single spawn + await, verifies result value 45
- `tests/integration/spawn_await_return.expected` - Expected: `45`
- `tests/integration/spawn_await_multiple.iron` - New: 3 concurrent fibonacci spawns, verifies all 3 results
- `tests/integration/spawn_await_multiple.expected` - Expected: fib(10)=55, fib(15)=610, fib(20)=6765
- `tests/integration/spawn_pfor_implicit_join.iron` - New: parallel-for implicit join regression test
- `tests/integration/spawn_pfor_implicit_join.expected` - Expected: parallel-for complete / done

## Decisions Made

- IRON_TYPE_NULL for handled spawn LIR type: keeps spawn handle representation as void* pointer without conflating with the OBJECT type used for struct constructors
- Wrapper function emitted per spawn in `lifted_funcs` section: no HIR signature change required; clean separation between lifted logic and handle capture
- Self-referential handle: passing handle as its own arg avoids a second allocation for the wrapper context

## Deviations from Plan

None - plan executed exactly as written. The implementation was completed in a prior session and all tests pass. This execution verified correctness (173/173 integration tests, 13/13 algorithm tests).

## Issues Encountered

None.

## User Setup Required

None - no external service configuration required.

## Next Phase Readiness

- spawn/await pipeline is complete and tested
- Phase 31-02 can proceed: update concurrency benchmarks to use `val h = spawn(...); await h` instead of fire-and-forget, re-validate thresholds

---
## Self-Check: PASSED

All created files exist. Both task commits verified:
- `aa7692a` (feat(31-01): implement spawn/await pipeline and runtime functions) — FOUND
- `ecd166f` (test(31-01): add spawn/await integration tests) — FOUND

173/173 integration tests pass. 13/13 algorithm tests pass.

---
*Phase: 31-spawn-await-correctness*
*Completed: 2026-04-01*
