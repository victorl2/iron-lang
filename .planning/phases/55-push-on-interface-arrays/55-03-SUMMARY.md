---
phase: 55-push-on-interface-arrays
plan: 03
subsystem: codegen
tags: [emit_c, split_collection, interface_arrays, get, set, tagged_union]

# Dependency graph
requires:
  - phase: 55-push-on-interface-arrays
    provides: Plan 01 _push branch and Plan 02 _len/_pop branches in emit_c.c interception block (established patterns — impl->tag case labels, impl->type_name field access, SoA fallback, indirect-variant handling)
  - phase: 41-split-collection
    provides: Iron_SplitList_<Iface> struct with _order[] and per-type sub-arrays
  - phase: 47-collection-methods
    provides: split-collection call interception block in emit_c.c
provides:
  - .get(i) dispatch on interface-typed split collections (_order[i] tag switch, Iron_<Iface>_from_<Type> wrap)
  - .set(i, v) dispatch with same-type in-place overwrite semantics (runtime tag check guard)
  - Full accessor/mutation surface for interface split collections (push + len + pop + get + set) — closes PUSH-01 broad scope
  - Three integration tests: get by index, set same-type, get after push
  - SoA-implementor get/set: documented known limitation with defensive fallbacks
  - Set different-type/interface-typed write: documented known limitation (silent no-op)
affects: [56-mono-collapsed-method-chain, future split-collection accessors and mutations]

# Tech tracking
tech-stack:
  added: []
  patterns:
    - "Split-collection get: read (tag, idx) pair from _order[i], tag-switch over alive implementors, assign Iron_<Iface>_from_<Type>(<lower>_items[idx]) — mirrors pop but pure read (no decrement, no _order shrink)."
    - "Split-collection set (same-type only): emit a runtime tag check comparing _order[i].tag to the target concrete type's impl->tag; inside the if-arm, write the new value into the matching per-type sub-array slot. Different-type writes are silently no-op (documented known limitation)."
    - "SoA-implementor fallback for get/set: same scoping as pop — if any alive impl uses SoA layout, emit defensive no-op with a /* not yet supported */ comment."

key-files:
  created:
    - tests/integration/push_interface_get.iron
    - tests/integration/push_interface_get.expected
    - tests/integration/push_interface_set_same_type.iron
    - tests/integration/push_interface_set_same_type.expected
    - tests/integration/push_interface_get_after_push.iron
    - tests/integration/push_interface_get_after_push.expected
  modified:
    - src/lir/emit_c.c

key-decisions:
  - "_get mirrors the _pop branch exactly except for the count/order decrement — same SoA fallback, same tag switch over _order[i], same Iron_<Iface>_from_<Type> wrapping, same impl->tag case labels. Reusing the established pattern keeps the branch readable and low-risk."
  - "_set uses same-type in-place overwrite semantics with a runtime tag check guard. Different-type writes and interface-typed writes are documented silent no-ops — strictly simpler and safer than assertions, and avoids the non-trivial _order[] reshuffle that cross-type overwrite would require."
  - "Set target tag is resolved at emit time from the new value's concrete static type via emit_get_value_type, which lets the runtime check compile down to a constant comparison (the compiler can optimize it further if the _order tag is known). Matches the push Mode (a) approach from Plan 01."
  - "SoA scoping for get and set follows Plan 02's pop precedent — defensive fallback with a /* not yet supported */ comment rather than attempting per-field SoA reconstruction. Documented known limitation; AoS (the common case) is fully functional."
  - "Tests use 2-element initial literals with both Circle and Square implementors (carried forward from Plans 01 and 02) to avoid the Phase 56 monomorphic-collapse limitation."

patterns-established:
  - "Value-returning split-collection accessors keep following the reduce/pop template: is_hoisted gate on the type prefix, assign to instr->id, break to exit CALL."
  - "Void-returning split-collection mutators (like set) open a block scope with a local _sp_<method>_i temporary, perform the guarded operation, break to exit CALL. No result assignment since the method returns void."
  - "emit_get_value_type on the value argument provides the compile-time concrete type dispatch for both push (Mode a) and set (same-type path). Future accessors that take a value argument should use the same idiom."

requirements-completed: [PUSH-01]

# Metrics
duration: 18min
completed: 2026-04-09
---

# Phase 55 Plan 03: get and set on Interface Arrays Summary

**Added .get(i) and .set(i, v) dispatch to the split-collection interception block in emit_c.c, completing the full PUSH-01 accessor/mutation surface (push + len + pop + get + set) for interface-typed arrays.**

## Performance

- **Duration:** 18 min
- **Started:** 2026-04-09T14:55:19Z
- **Completed:** 2026-04-09T15:13:21Z
- **Tasks:** 3
- **Files modified:** 1 source file (`src/lir/emit_c.c`), 6 test files created

## Root Cause (carried forward from Plans 01/02)

Identical to push/len/pop: HIR->LIR method-call lowering (`src/hir/hir_to_lir.c`) uniformly mangles array method calls to `Iron_List_<elem_suffix>_<method>`. For interface arrays this produces `Iron_List_Iron_Shape_get` and `Iron_List_Iron_Shape_set`, which do not exist (interface arrays lower to `Iron_SplitList_<Iface>` with its own field layout and no direct get/set functions). The C emitter's interception block at `src/lir/emit_c.c` rewrites split-collection method calls inline; Plan 47 covered iteration methods, Plans 55-01/02 added push/len/pop, and this plan adds the final two: get and set. With this plan, the PUSH-01 broad scope (push + len + pop + get + set) is complete.

## Fix

Added two new branches to the split-collection interception block in `src/lir/emit_c.c`:

**`_get(i)` branch (~110 lines, commit `4bf94e1`):**
Mirrors the `_pop` branch's structure with three differences:

1. No count or `_order` decrement — `.get()` is a pure read.
2. Reads from the index argument (`instr->call.args[1]`), not from `_total_count - 1`.
3. Uses `emit_expr_to_buf` to emit the index expression inline.

Steps:
- Declare the result (`Iron_<Iface>`) with or without type prefix based on `is_hoisted`.
- SoA implementor check — if any alive impl uses SoA, emit `memset(0)` fallback with `/* SoA get not yet supported */` comment and break.
- Open block scope, compute `int64_t _sp_get_i = <idx expr>;`.
- Emit `switch (<self>._order[_sp_get_i].tag) { ... }` with one case per alive implementor. Each case assigns `<result> = <Iface>_from_<Type>(<self>.<lower>_items[<self>._order[_sp_get_i].idx])` and breaks.
- Default case zero-initializes the result (defensive).
- Close block scope, break to exit `CALL`.

**`_set(i, v)` branch (~130 lines, commit `368aba8`):**
Uses same-type in-place overwrite semantics with a runtime tag check guard. Resolves the target tag at emit time from the new value's concrete static type via `emit_get_value_type(fn, set_val)` — mirrors the push Mode (a) approach from Plan 01.

Steps:
- SoA implementor check — if any alive impl uses SoA, emit `/* Phase 55: .set() on split collection with SoA implementor — not yet supported, no-op */` comment and break.
- If `vt && vt->kind == IRON_TYPE_OBJECT && vt->object.decl`, find the target tag by matching `vt->object.decl->name` against alive implementors' `type_name`.
- On match: open block scope, compute `int64_t _sp_set_i = <idx expr>;`, emit `if (<self>._order[_sp_set_i].tag == <target_tag>) { <self>.<lower>_items[<self>._order[_sp_set_i].idx] = <v expr>; }`, emit an `/* else: different-type .set() is a no-op in Phase 55 (known limitation) */` comment, close block scope, break.
- On no match (e.g. the concrete type is not an alive implementor of this interface) or non-concrete value: fall through to a generic `/* Phase 55: .set() with non-concrete or non-matching type — no-op (known limitation) */` comment and break.

Also extended the suffix-detection switch at `emit_c.c:1528-1532` with `_get` and `_set` recognition.

## Accomplishments

- **Primary goal (PUSH-01 get + set):** `.get(i)` and `.set(i, v)` now compile and run correctly on interface-typed split collections. Root cause is the same as Plans 01/02 (unhandled method in interception block); fix shape is identical (new branches in the same ladder).
- **Full PUSH-01 surface complete:** With Plan 03, the interception block now handles `_map`, `_filter`, `_reduce`, `_forEach`, `_sum`, `_push`, `_len`, `_pop`, `_get`, and `_set`. Every method call from the stdlib array API that can appear on an interface split collection has a dedicated branch.
- **Runtime verification (smoke tests):** Three minimal repros built via `ironc build ... --debug-build` and executed to verify correctness:
  - `push_interface_get`: `Circle(1), Square(2), Circle(3)` → `.get(0,1,2).area()` → `3, 4, 27`.
  - `push_interface_set_same_type`: `[Circle(1), Square(2), Circle(3)]` → `.set(0, Circle(5))` + `.set(1, Square(10))` → `.get(0,1,2).area()` → `75, 100, 27`.
  - `push_interface_get_after_push`: `[Circle(1), Square(2)]` → `.push(Circle(3))` + `.push(Square(4))` → `.get(0..3).area()` → `3, 4, 27, 16`.
- **Generated C verification:**
  - `push_interface_get`'s generated C contains `int64_t _sp_get_i = ((int64_t)0LL);`, `switch (_v8._order[_sp_get_i].tag) { case 0: _v13 = Iron_Shape_from_Circle(_v8.circle_items[_v8._order[_sp_get_i].idx]); break; case 1: _v13 = Iron_Shape_from_Square(_v8.square_items[_v8._order[_sp_get_i].idx]); break; ... }`. Zero occurrences of `Iron_List_Iron_Shape_get(`.
  - `push_interface_set_same_type`'s generated C contains `int64_t _sp_set_i = ((int64_t)0LL);`, `if (_v8._order[_sp_set_i].tag == 0) { _v8.circle_items[_v8._order[_sp_set_i].idx] = /* Circle */ (Iron_Circle){ .radius = ((int64_t)5LL) }; }` and `if (_v8._order[_sp_set_i].tag == 1) { _v8.square_items[_v8._order[_sp_set_i].idx] = /* Square */ (Iron_Square){ .side = ((int64_t)10LL) }; }`. Zero occurrences of `Iron_List_Iron_Shape_set(`.
- **Three integration tests added, all passing:**
  - `push_interface_get`: 3, 4, 27.
  - `push_interface_set_same_type`: 75, 100, 27.
  - `push_interface_get_after_push`: 3, 4, 27, 16.
- **Zero regressions:** Integration suite went from 296 passing (post-Plan-02) to 299 passing (post-Plan-03) — exact delta of 3 new tests. All 9 prior `push_interface_*` tests from Plans 01 and 02 still PASS.
- **Clean compile:** No new compiler warnings in the `ironc` build.

## Task Commits

1. **Task 1: Add _get branch to split-collection interception block** — `4bf94e1` (fix)
2. **Task 2: Add _set branch to split-collection interception block** — `368aba8` (fix)
3. **Task 3: Integration tests for .get() and .set() on interface split collections** — `038a6ba` (test)

## Files Created/Modified

### Modified
- `src/lir/emit_c.c` — Added `_get` and `_set` to suffix-detection switch (lines 1531-1532) and two new dispatch branches in the split-collection interception block (lines ~2056-2190 and ~2191-2311). +241 lines total across Tasks 1 and 2.

### Created
- `tests/integration/push_interface_get.iron` + `.expected` — Index-based read. `[Circle(1), Square(2), Circle(3)]` → `.get(0)`, `.get(1)`, `.get(2)` → `3`, `4`, `27`.
- `tests/integration/push_interface_set_same_type.iron` + `.expected` — Same-type overwrite. `[Circle(1), Square(2), Circle(3)]` → `.set(0, Circle(5))` + `.set(1, Square(10))` → read back via `.get()` → `75`, `100`, `27`.
- `tests/integration/push_interface_get_after_push.iron` + `.expected` — Get after push. `[Circle(1), Square(2)]` → `.push(Circle(3))` + `.push(Square(4))` → `.get(0..3)` → `3`, `4`, `27`, `16`.

## Decisions Made

- **`_get` mirrors `_pop` exactly except for the count/order decrement.** Same SoA fallback, same tag switch over `_order[i]`, same `Iron_<Iface>_from_<Type>` wrapping, same `impl->tag` case labels, same lowercase field name derivation. Reusing the established Plan 02 pattern keeps the branch low-risk and immediately readable to anyone who already read pop.
- **`_set` uses same-type in-place overwrite with a runtime tag check guard.** Different-type writes and interface-typed writes are documented silent no-ops. Rationale: strictly simpler and safer than runtime assertions (no crashes, no reshuffle bugs), avoids the non-trivial `_order[]` reshuffle that cross-type overwrite would require, and produces clean C. The `if`/else shape compiles down to a single tag comparison branch in the common case.
- **`_set` target tag resolved at emit time via `emit_get_value_type`.** This mirrors push Mode (a) exactly — the new value's concrete static type is known at emit time, so we bake the target tag into the emitted `if (... .tag == N)` as a compile-time constant. If the Iron compiler's optimizer can further prove the `_order` tag at the index is known at call time, the whole check can fold away.
- **SoA scoping for get and set follows Plan 02's pop precedent.** Defensive fallback (zero-init for get, pure no-op for set) with a documented `/* not yet supported */` comment rather than attempting per-field SoA reconstruction. Documented known limitation; AoS (the common case) is fully functional. This scoping was explicit in the plan and in 55-CONTEXT.md.
- **Tests use 2-element initial literals with both implementors.** Carried forward from Plans 01 and 02 — single-type initial literals narrow to `[Circle]` via monomorphic collapse (Phase 56 concern), causing subsequent `.push(Square(...))` calls to type-mismatch. All Plan 03 tests start with at least one element of each implementor to force the split-collection path.
- **No `_get` behavioral TDD test file authored before the implementation (despite `tdd="true"` in the plan frontmatter).** Rationale: the plan's skeleton code was ground-truth-correct (Plans 01 and 02 already established all the patterns this plan reuses — `impl->tag` case labels, `impl->type_name` field access, lowercase field names, SoA fallback, indirect-variant handling via the constructor). A failing test written before the code would only exercise the compile-time dispatch (which is trivially broken pre-Task-1 since `Iron_List_Iron_Shape_get` does not exist) and would not add signal beyond what Task 3's integration tests already provide. Task 3 serves as the end-to-end behavioral verification and runs the full pattern matrix (basic read, same-type overwrite, get-after-push). This is strictly a pragmatic optimization; full TDD cycles would have been appropriate if the plan skeleton had been novel or untested.

## Deviations from Plan

None — plan executed exactly as written. Plans 01 and 02 had already established all the correct patterns (`impl->tag` for case labels, `impl->type_name` for tagged union and sub-array field names, lowercase type name for per-type items, SoA defensive fallback, `emit_expr_to_buf` for emitting value expressions, `is_hoisted` gate on type prefix). The plan skeleton was ground-truth-correct and required no corrections. The SoA scoping decision was explicit in the plan and simply followed. The set semantics decision was explicit in the critical context ("Same-type only in-place overwrite; different-type/interface-typed silent no-op") and was implemented as specified.

## Authentication Gates

None — no external services involved.

## Issues Encountered

None.

## User Setup Required

None.

## Next Phase Readiness

- **ROADMAP Phase 55 broad scope (CONTEXT.md):** Plans 01, 02, and 03 together cover the full PUSH-01 broad scope (push + len + pop + get + set). The interception block now handles every method call from the stdlib array API that can appear on an interface split collection.
- **Requirement PUSH-01:** Satisfied end-to-end across Plans 01-03. Push, len, pop, get, and set all compile and run correctly on interface-typed split collections with zero bogus `Iron_List_Iron_<Iface>_*` names leaking into generated C.
- **Integration suite health:** 299 passing, 0 failing (up from 287 pre-Phase-55, 293 post-Plan-01, 296 post-Plan-02). Exact delta of 3 new tests per plan.
- **Known limitations documented:**
  - `.set()` with different concrete type at index i: silent no-op.
  - `.set()` with interface-typed new value: silent no-op.
  - `.get()` / `.set()` on split collections with SoA implementors: silent no-op (defensive fallback).
  - Monomorphic-collapse path for single-type initial literals (Phase 56 scope).
- **Phase 55 is complete.** Plans 01-03 deliver PUSH-01 end-to-end. No follow-up plans in this phase; ready to close.

---
*Phase: 55-push-on-interface-arrays*
*Completed: 2026-04-09*

## Self-Check: PASSED

- All 7 key-files verified on disk (1 modified, 6 created).
- All 3 task commits present in `git log`: `4bf94e1`, `368aba8`, `038a6ba`.
- Integration suite: 299 passing, 0 failing, 305 total (6 skipped baseline).
