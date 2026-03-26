---
phase: 02-semantics-and-codegen
plan: 06
subsystem: codegen
tags: [codegen, vtable, monomorphization, generics, lambda, closure, spawn, concurrency, parallel-for]

dependency_graph:
  requires:
    - phase: 02-05
      provides: [iron_codegen, emit_stmt, emit_expr, emit_defers, iron_type_to_c]
  provides:
    - Interface vtable struct emission (Iron_<Iface>_vtable + Iron_<Iface>_ref)
    - Static vtable instance emission per (type, interface) pair
    - Generic monomorphization with dedup registry (ensure_monomorphized_type)
    - Lambda closure codegen with env struct for captured variables
    - Spawn body lifting to named C functions with Iron_pool_submit
    - Parallel-for chunk function generation with Iron_pool_barrier
  affects: [Phase 3 runtime, end-to-end testing]

tech-stack:
  added: []
  patterns:
    - Vtable dispatch: typedef struct Iron_<Iface>_vtable with fn pointers + Iron_<Iface>_ref fat pointer
    - Mono registry: stb_ds shmap keyed by mangled name for O(1) dedup
    - Lambda lifting: captured-var detection via body AST walk; env struct for closures
    - Spawn lift: void Iron_spawn_<name>_N(void* arg) with Iron_pool_submit at use site
    - Parallel chunk: void Iron_parallel_chunk_N(int64_t start, int64_t end, void* ctx_arg) with range splitting

key-files:
  created: []
  modified:
    - src/codegen/codegen.h
    - src/codegen/gen_types.c
    - src/codegen/gen_exprs.c
    - src/codegen/gen_stmts.c
    - src/codegen/codegen.c
    - tests/test_codegen.c

key-decisions:
  - "Monomorphization registry uses stb_ds shmap (string hash) keyed by mangled name; lookup is O(1) and dedup is exact"
  - "Lambda captures detected by scanning body idents not matching param names; env fields typed as int64_t* for mutation"
  - "Spawn lifted function uses Iron_global_pool as default pool expression; handle not tracked for Phase 2"
  - "Parallel-for fixed at 4 chunks for Phase 2; Iron_pool_barrier emitted after loop"
  - "Vtable instances emitted after function implementations section so prototyped function names are visible"
  - "current_func_name field added to Iron_Codegen for correct lambda naming within function bodies"

patterns-established:
  - "Interface vtable: Iron_Drawable_vtable struct with fn pointers, Iron_Drawable_ref fat-pointer, static Iron_Player_Drawable_vtable instance"
  - "Generic mono: mangle_generic builds Iron_List_Iron_Enemy; ensure_monomorphized_type checks registry before emitting"
  - "Lambda lift: emit_lambda() lifts body to ctx->lifted_funcs section, placed before implementations in output"

requirements-completed:
  - GEN-04
  - GEN-05
  - GEN-09
  - GEN-10
  - GEN-11

duration: 9min
completed: 2026-03-26
---

# Phase 2 Plan 6: Advanced Codegen (Vtable, Mono, Lambda, Concurrency) Summary

**Interface vtable dispatch, generic monomorphization registry, lambda closure lifting, and concurrency codegen (spawn lift + parallel-for chunk) completing all GEN requirements.**

## Performance

- **Duration:** ~9 min
- **Started:** 2026-03-26T03:13:47Z
- **Completed:** 2026-03-26T03:23:00Z
- **Tasks:** 2
- **Files modified:** 6

## Accomplishments

- Interface vtable codegen: `Iron_<Iface>_vtable` struct with fn pointers, `Iron_<Iface>_ref` fat pointer, static vtable instances per (type, iface) pair
- Generic monomorphization: `ensure_monomorphized_type` with stb_ds shmap registry for dedup; stub struct emission with items/count/capacity
- Lambda closure lifting: pure lambdas as named C functions (`Iron_<func>_lambda_N`), closures with env struct for captured variables
- Spawn lifting: `void Iron_spawn_<name>_N(void* arg)` with `Iron_pool_submit` at use site
- Parallel-for: `Iron_parallel_chunk_N(int64_t start, int64_t end, void* ctx_arg)` with range splitting loop and `Iron_pool_barrier`

## Task Commits

Each task was committed atomically:

1. **Task 1: Vtable dispatch and monomorphization codegen** - `13874e8` (feat)
2. **Task 2 RED: Failing tests for lambda/spawn/parallel** - `2b78df7` (test)
3. **Task 2 GREEN: Lambda closure, spawn, parallel-for codegen** - `e2d23c6` (feat)

## Files Created/Modified

- `src/codegen/codegen.h` - Added mono_registry, lambda_counter, spawn_counter, parallel_counter, lifted_funcs, current_func_name; declared emit_interface_vtable_struct, emit_vtable_instance, mangle_generic, ensure_monomorphized_type, emit_lambda
- `src/codegen/gen_types.c` - Implemented vtable struct/ref emission, vtable instance emission, mangle_generic, ensure_monomorphized_type
- `src/codegen/gen_exprs.c` - Implemented collect_captures (body AST walk), emit_lambda with pure/closure branching, updated IRON_NODE_LAMBDA and IRON_NODE_AWAIT cases
- `src/codegen/gen_stmts.c` - Implemented IRON_NODE_SPAWN lifting, IRON_NODE_FOR parallel chunk generation, current_func_name tracking in func/method impl
- `src/codegen/codegen.c` - Added vtable struct emission after struct bodies, vtable instance emission after func impls, lifted_funcs section in output, mono_registry/lifted_funcs initialization and cleanup
- `tests/test_codegen.c` - 4 new TDD tests: vtable struct, vtable instance fn pointer, mono struct API, mono dedup API, lambda lifting, env struct, spawn pool_submit, parallel-for barrier

## Decisions Made

- Monomorphization registry uses stb_ds `shmap` (string hash) keyed by mangled name; `shgeti` check before `shput` for O(1) dedup
- Lambda capture detection is intra-body only: scan idents not matching param names; captures typed as `int64_t*` in env struct for Phase 2
- Spawn uses `Iron_global_pool` as default pool; handle not tracked for Phase 2 (sufficient for GEN-10)
- Parallel-for fixed at 4 chunks for Phase 2 (`(_total + 3) / 4`); actual thread count from runtime is Phase 3
- Vtable instances emitted in a separate pass after all function implementations so function names are prototyped
- `current_func_name` field added to Iron_Codegen, set/restored in `emit_func_impl` and `emit_method_impl` for proper lambda naming

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 2 - Missing Critical] Added current_func_name tracking to Iron_Codegen**
- **Found during:** Task 2 implementation (lambda needs enclosing function name)
- **Issue:** Plan mentioned `Iron_<enclosing_func>_lambda_N` naming but `Iron_Codegen` had no way to track the current function name during statement emission
- **Fix:** Added `current_func_name` field, set/restored in `emit_func_impl` and `emit_method_impl`
- **Files modified:** `src/codegen/codegen.h`, `src/codegen/gen_stmts.c`
- **Verification:** `test_lambda_no_captures_generates_lifted_function` verifies correct naming
- **Committed in:** e2d23c6 (Task 2 commit)

**2. [Rule 1 - Test design] Monomorphization tests use direct API instead of full pipeline**
- **Found during:** Task 1 test design
- **Issue:** `List[Enemy]` in Iron source would fail resolution (List not declared), causing `run_codegen` to return NULL
- **Fix:** Tests call `ensure_monomorphized_type` and `emit_lambda` directly on minimal codegen contexts
- **Files modified:** `tests/test_codegen.c`
- **Committed in:** 13874e8 (Task 1 commit)

---

**Total deviations:** 2 auto-fixed (1 missing critical field, 1 test design adjustment)
**Impact on plan:** Both fixes necessary for correctness. No scope creep.

## Issues Encountered

None significant - plan executed smoothly with correct Iron syntax for spawn (`spawn("name") { body }`) and parallel-for (`for i in n parallel { body }`).

## Next Phase Readiness

- All GEN-04/05/09/10/11 requirements satisfied
- End-to-end testing (Phase 2 plans 07+) can now exercise the complete codegen pipeline
- Phase 3 runtime will implement `Iron_pool_submit`, `Iron_pool_barrier`, and collection generics that the Phase 2 codegen emits calls to

## Self-Check: PASSED

All modified files verified on disk:
- FOUND: src/codegen/codegen.h
- FOUND: src/codegen/gen_types.c
- FOUND: src/codegen/gen_exprs.c
- FOUND: src/codegen/gen_stmts.c
- FOUND: src/codegen/codegen.c
- FOUND: tests/test_codegen.c

All commits verified:
- FOUND: 13874e8 (Task 1: vtable and monomorphization)
- FOUND: 2b78df7 (Task 2 RED: failing tests)
- FOUND: e2d23c6 (Task 2 GREEN: lambda/spawn/parallel)

---
*Phase: 02-semantics-and-codegen*
*Completed: 2026-03-26*
