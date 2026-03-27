---
phase: 09-c-emission-and-cutover
plan: 04
subsystem: compiler-ir-lowering
tags: [ir-lowering, stdlib-dispatch, comptime, auto-static, method-call]

requires:
  - phase: 09-02
    provides: IR pipeline cutover with emit_c.c emitting function calls via mangle_func_name

provides:
  - Auto-static dispatch for stdlib module calls (Math.sin, IO.read_file, Time.now_ms, etc.)
  - Top-level val/comptime constant lowering via global_constants_map lazy resolution
  - Correct exclusion of empty-body stdlib method stubs from IR function registration

affects: [09-05, future-stdlib-modules, comptime-eval]

tech-stack:
  added: []
  patterns:
    - "IRON_SYM_TYPE check for auto-static dispatch: receiver is type symbol → emit Iron_<lower_type>_<method>()"
    - "global_constants_map lazy pattern: collect top-level vals in Pass 1.5, lower init lazily in IDENT handler, cache in val_binding_map"
    - "Empty-body method stub skip: stdlib method stubs with {} bodies excluded from IR registration to prevent void-param C emission"

key-files:
  created: []
  modified:
    - src/ir/lower_exprs.c
    - src/ir/lower.c
    - src/ir/lower_internal.h
    - src/ir/lower_types.c

key-decisions:
  - "Auto-static dispatch emits <lowercase_type>_<method> as func_ref name; mangle_func_name() in emit_c.c adds Iron_ prefix at emission, matching Iron_math_sin, Iron_io_read_file conventions"
  - "global_constants_map stores name->AST_init_node pairs; init expressions lowered lazily in the calling function's context, cached in val_binding_map for subsequent uses"
  - "Empty-body method stubs (func Timer.since(t: Timer) -> Int {}) skipped in lower_module_decls Pass 1f to prevent void param type emission for non-primitive types"

patterns-established:
  - "Auto-static vs instance dispatch: check obj_id->resolved_sym->sym_kind == IRON_SYM_TYPE before lowering receiver"
  - "Global constant lazy lowering: collect in dedicated pass, resolve in IDENT handler"

requirements-completed: [EMIT-01, EMIT-02]

duration: 41min
completed: 2026-03-27
---

# Phase 9 Plan 04: Stdlib Dispatch and Comptime Constants Summary

**Auto-static dispatch for Math/IO/Time/Timer/Log stdlib modules plus top-level comptime constant resolution, fixing 4 of 5 integration test failures (test_math, test_io, test_time, comptime_basic)**

## Performance

- **Duration:** ~41 min
- **Started:** 2026-03-27T17:00:00Z
- **Completed:** 2026-03-27T17:41:37Z
- **Tasks:** 2
- **Files modified:** 4

## Accomplishments

- Auto-static method dispatch: `Math.sin(x)` → `Iron_math_sin(x)`, `IO.read_file(p)` → `Iron_io_read_file(p)`, etc.
- Top-level comptime constant resolution: `val GREETING = comptime "hello comptime"` accessible across function bodies
- test_math, test_io, test_time, comptime_basic integration tests all pass
- Fixed empty-body stdlib method stubs causing void-param C emission for non-primitive types (Timer)
- extern_basic and test_range also unblocked as side effects of the stub-skip fix

## Task Commits

1. **Task 1: Auto-static dispatch for stdlib module calls** - `b7b1852` (feat)
2. **Task 2: Top-level comptime constant lowering** - `dc3598f` (feat)

**Plan metadata:** (this summary commit)

## Files Created/Modified

- `src/ir/lower_exprs.c` - Added IRON_SYM_TYPE check in METHOD_CALL, ctype.h include, global_constants_map lookup in IDENT
- `src/ir/lower.c` - Added lower_global_constants() function and Pass 1.5 call, shfree cleanup
- `src/ir/lower_internal.h` - Added global_constants_map field to IronIR_LowerCtx
- `src/ir/lower_types.c` - Skip empty-body method stubs in Pass 1f registration (Rule 1 auto-fix)

## Decisions Made

- Auto-static dispatch emits `<lowercase_type>_<method>` as func_ref name (e.g., `math_sin`). mangle_func_name() in emit_c.c adds the `Iron_` prefix, producing `Iron_math_sin`. IR names remain unmangled per established convention.
- global_constants_map uses lazy lowering: init expressions are lowered in the calling function's context (not in a separate global context), which naturally handles comptime literals that resolve to STRING_LIT, INT_LIT etc.
- Caching lowered global constants in val_binding_map prevents re-lowering the same expression for each reference in the same function body.

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 1 - Bug] Empty-body stdlib method stubs emitting void params for non-primitive types**
- **Found during:** Task 1 (test_time failure after auto-static dispatch fix)
- **Issue:** Stdlib method stubs like `func Timer.since(t: Timer) -> Int {}` in time.iron have empty bodies and non-primitive param types. lower_module_decls registered them as IR functions with NULL param types (lower_types_resolve_ann returns NULL for Timer). emit_c.c emits NULL type as "void", generating invalid C `int64_t Iron_Timer_since(void t)`.
- **Fix:** In lower_module_decls Pass 1f, skip method declarations with empty bodies. These stubs delegate to C functions via auto-static dispatch; they don't need IR function entries.
- **Files modified:** src/ir/lower_types.c
- **Verification:** test_time passes. extern_basic and test_range also unblocked.
- **Committed in:** b7b1852 (Task 1 commit)

---

**Total deviations:** 1 auto-fixed (Rule 1 - Bug)
**Impact on plan:** The empty-body stub skip was the direct cause of test_time failure after Task 1. Fix was localized to lower_types.c Pass 1f with no regressions.

## Issues Encountered

- Pre-existing snapshot test failure (`test_ir_lower` — `test_snapshot_if_else`): commit d5c91fc from plan 09-03 renamed block labels (dots to underscores) but did not update `tests/ir/snapshots/if_else.expected`. This is out of scope for 09-04; logged to deferred-items.md.

## Next Phase Readiness

- All planned integration tests pass: test_math, test_io, test_time, comptime_basic
- test_combined still fails due to parallel-for emission (iron_pool_parallel_for not included in header) — this is Plan 09-03's responsibility
- Auto-static dispatch pattern established for all stdlib modules: Math, IO, Time, Timer, Log
- Global constant pattern established for future comptime use cases

---
*Phase: 09-c-emission-and-cutover*
*Completed: 2026-03-27*
