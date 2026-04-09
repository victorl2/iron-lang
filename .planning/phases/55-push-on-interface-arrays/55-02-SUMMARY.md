---
phase: 55-push-on-interface-arrays
plan: 02
subsystem: codegen
tags: [emit_c, split_collection, interface_arrays, len, pop, tagged_union]

# Dependency graph
requires:
  - phase: 55-push-on-interface-arrays
    provides: Plan 01 _push branch in emit_c.c interception block (established patterns, indirect-variant handling, tag numbering via impl->tag)
  - phase: 41-split-collection
    provides: Iron_SplitList_<Iface> struct with _total_count, _order[], per-type sub-arrays
  - phase: 47-collection-methods
    provides: split-collection call interception block in emit_c.c
provides:
  - .len() dispatch on interface-typed split collections (direct _total_count read)
  - .pop() dispatch on interface-typed split collections (tag-switch over _order[_total_count-1])
  - Three integration tests: combined len+pop, len-on-empty, LIFO pop order
  - SoA-implementor pop: documented known limitation with defensive zero-init fallback
affects: [56-mono-collapsed-method-chain, future split-collection accessors (get/set)]

# Tech tracking
tech-stack:
  added: []
  patterns:
    - "Split-collection len: direct _total_count field read, no per-type fan-out (mirrors the existing .count accessor at emit_c.c:1054)."
    - "Split-collection pop: read (tag, idx) pair from _order[_total_count-1], tag-switch to matching sub-array, box via Iron_<Iface>_from_<Type>, decrement per-type count + _order_count + _total_count."
    - "SoA-implementor defensive fallback: when any alive impl uses SoA layout, emit memset(0) with a /* SoA pop not yet supported */ comment rather than miscompiling the per-field storage."

key-files:
  created:
    - tests/integration/push_interface_len_pop.iron
    - tests/integration/push_interface_len_pop.expected
    - tests/integration/push_interface_len_empty.iron
    - tests/integration/push_interface_len_empty.expected
    - tests/integration/push_interface_pop_order.iron
    - tests/integration/push_interface_pop_order.expected
  modified:
    - src/lir/emit_c.c

key-decisions:
  - "_len uses direct _total_count field read (authoritative combined-length field always emitted in emit_split.c:370), not a per-type sum. Trivial, single-line emission mirroring the existing .count accessor."
  - "_pop reads _order[_total_count-1] to recover the (tag, idx) of the most recently pushed element, dispatches via switch over impl->tag (not loop index) to box-and-return via Iron_<Iface>_from_<Type>. Matches the tag enum generated in emit_structs.c:263."
  - "_pop decrements per-type <lower>_count inside each case (sub-array logically shrinks) and _order_count + _total_count after the switch (whole-collection counters)."
  - "SoA-implementor pop falls back to defensive zero-init with a C comment, per 55-CONTEXT.md scoping. The AoS path is fully implemented; SoA pop is a documented known limitation."
  - "Indirect variants are handled by the existing Iron_<Iface>_from_<Type> constructor (heap-allocates internally). The pop branch passes the by-value struct read from <lower>_items regardless of indirection."
  - "Tests use 2-element initial literals with both Circle and Square implementors to avoid the Phase 56 monomorphic-collapse limitation (inherited pattern from Plan 01)."

patterns-established:
  - "Value-returning split-collection accessors in emit_c.c follow the reduce branch template: check is_hoisted to decide whether to emit the type prefix, assign to instr->id, break to exit the CALL case."
  - "SoA-implementor detection for split-collection branches: iterate sp_entry->impls, build iface_mangled:type_name key, query ctx->soa_types."

requirements-completed: [PUSH-01]

# Metrics
duration: 10min
completed: 2026-04-09
---

# Phase 55 Plan 02: len and pop on Interface Arrays Summary

**Added .len() and .pop() dispatch to the split-collection interception block in emit_c.c, completing the PUSH-01 cluster of accessor/mutation methods on interface-typed arrays.**

## Performance

- **Duration:** 10 min
- **Started:** 2026-04-09T14:38:34Z
- **Completed:** 2026-04-09T14:48:44Z
- **Tasks:** 3
- **Files modified:** 1 source file (`src/lir/emit_c.c`), 6 test files created

## Root Cause (carried forward from Plan 01)

Identical to the push fix: HIR->LIR method-call lowering (`src/hir/hir_to_lir.c`) uniformly mangles array method calls to `Iron_List_<elem_suffix>_<method>`. For interface arrays, this produces names like `Iron_List_Iron_Shape_len` and `Iron_List_Iron_Shape_pop`, which do not exist (interface arrays lower to `Iron_SplitList_<Iface>` with its own field layout). The C emitter's late-stage interception block at `src/lir/emit_c.c:1501-1915` rewrites these calls for split collections, but Plan 47 only covered the iteration methods (`map`, `filter`, `reduce`, `forEach`, `sum`), and Plan 55-01 added `push`. Plans 55-02 adds the remaining accessors: `len` and `pop`.

Phase 54 documented `.len()` failing with the same pattern (`.planning/phases/54-test-hardening/54-02-SUMMARY.md:94-111`, PUSH-01), justifying the broad scope.

## Fix

Added two new branches to the split-collection interception block in `src/lir/emit_c.c`:

**`_len` branch (23 lines, commit `ffeb288`):**
Direct field read of `<self>._total_count`. No per-type fan-out because `_total_count` is the authoritative whole-collection element count, always emitted in `src/lir/emit_split.c:370` as the last field of `Iron_SplitList_<Iface>`. Uses the `is_hoisted` check to decide whether to emit the `int64_t` type prefix, matching the existing value-returning branches. This mirrors the existing `.count` accessor at `emit_c.c:1054` which already uses the same pattern for a different call path.

**`_pop` branch (118 lines, commit `fd5b96f`):**
Mirrors the existing map/filter/reduce tag-switch style but reads a single slot rather than iterating:

1. Declare the result variable (`Iron_<Iface>`) with or without type prefix based on `is_hoisted`.
2. Check for any SoA implementor — if found, emit a defensive `memset(0)` fallback with a `/* SoA pop not yet supported */` comment and break (known limitation).
3. Open a block scope, compute `int64_t _sp_pop_i = <self>._total_count - 1;`.
4. Emit `switch (<self>._order[_sp_pop_i].tag) { ... }` with one case per alive implementor. Each case:
   - Uses `impl->tag` as the case label (matching the `_TAG_%s` enum values from `emit_structs.c:263`).
   - Assigns `<result> = <Iface>_from_<Type>(<self>.<lower>_items[<self>._order[_sp_pop_i].idx])`.
   - Decrements the per-type `<self>.<lower>_count--`.
   - `break;`
5. Default case zero-initializes the result (defensive).
6. After the switch, decrement `<self>._order_count--` and `<self>._total_count--`.
7. Close the block scope and `break` out of the `CALL` case (prevent fall-through to generic emission).

Also extended the suffix-detection switch at `emit_c.c:1529-1530` with `_len` and `_pop` recognition.

## Accomplishments

- **Primary goal (PUSH-01 len + pop):** `.len()` and `.pop()` now compile and run correctly on interface-typed split collections. Root cause is the same as Plan 01's push fix (unhandled method in interception block); fix shape is identical.
- **Runtime verification (smoke test):** A minimal repro (`Circle(1), Square(2), Circle(3)` → `.len()` → `.pop()` → `.len()`) produces the correct output `3`, `27`, `2`. The popped `Circle(3)` has area `3 * 3 * 3 = 27`, confirming LIFO order via `_order[_total_count-1]`.
- **Generated C verification:** Spot-checked `push_interface_len_pop`'s generated C:
  - `int64_t _v11 = _v8._total_count;` for `.len()` (direct field read, no function call)
  - `int64_t _sp_pop_i = _v8._total_count - 1;` + `switch (_v8._order[_sp_pop_i].tag)` for `.pop()`
  - `case 0: _v18 = Iron_Shape_from_Circle(_v8.circle_items[_v8._order[_sp_pop_i].idx]); _v8.circle_count--; break;`
  - `case 1: _v18 = Iron_Shape_from_Square(_v8.square_items[_v8._order[_sp_pop_i].idx]); _v8.square_count--; break;`
  - Zero occurrences of `Iron_List_Iron_Shape_len(` or `Iron_List_Iron_Shape_pop(`
- **Three integration tests added, all passing:**
  - `push_interface_len_pop`: combined len + pop sequence (`3` → `27` → `2`)
  - `push_interface_len_empty`: len after popping all elements (`2` → `0`)
  - `push_interface_pop_order`: LIFO verification, pop 4 distinct shapes, verify reverse-push order (`16`, `27`, `4`, `3`, `0`)
- **Zero regressions:** Integration suite went from 293 passing (post-Plan-01) to 296 passing (post-Plan-02) — the exact delta of 3 new tests. All 6 Plan 01 tests still PASS.
- **Clean compile:** No new compiler warnings in the `ironc` build.

## Task Commits

1. **Task 1: Add _len branch to split-collection interception block** — `ffeb288` (fix)
2. **Task 2: Add _pop branch to split-collection interception block** — `fd5b96f` (fix)
3. **Task 3: Integration tests for .len() and .pop() on interface split collections** — `f56ee99` (test)

## Files Created/Modified

### Modified
- `src/lir/emit_c.c` — Added `_len` and `_pop` to suffix-detection switch (lines 1529-1530) and two new dispatch branches in the split-collection interception block (lines 1916-2065). +141 lines total across Tasks 1 and 2.

### Created
- `tests/integration/push_interface_len_pop.iron` + `.expected` — Combined `.len()` + `.pop()` test. Initial len=3, pop Circle(3) → area 27, post-pop len=2.
- `tests/integration/push_interface_len_empty.iron` + `.expected` — Pop-to-empty test. Initial len=2, pop both elements, final len=0.
- `tests/integration/push_interface_pop_order.iron` + `.expected` — LIFO order test. Push Circle(1), Square(2), Circle(3), Square(4); pop all four; verify reverse-push areas (16, 27, 4, 3); final len=0.

## Decisions Made

- **`_len` uses direct `_total_count` read, not per-type sum.** The CONTEXT.md sketch described len as "sum of per-type sub-array lengths", but `_total_count` is the authoritative field already maintained by the existing per-type push functions (incrementing it on every push, decrementing on every pop). Reading it directly is simpler, faster, and matches the existing `.count` accessor at `emit_c.c:1054`.
- **Tag case labels use `impl->tag`, not the loop index.** Carried forward from Plan 01's Deviation 2 — the `_TAG_%s` enum in `emit_structs.c:263` uses `impl->tag` as the numeric value, and the `_order[].tag` field stores the same. Using the loop index `ji` would dispatch to the wrong case when dead-eliminated implementors are present.
- **`_pop` decrements counts in a specific order inside/outside the switch.** Per-type `<lower>_count--` lives inside the case (correct sub-array logically shrinks); `_order_count--` and `_total_count--` live after the switch (whole-collection counters decrement once per pop). This is the minimal correct order that matches the push functions' update patterns.
- **SoA-implementor fallback is per-collection, not per-case.** If any alive implementor uses SoA layout, the whole pop is zero-initialized with a comment — because the tag switch would need to handle both AoS and SoA cases in the same branch, and the SoA per-field read is non-trivial (see `emit_c.c:4677-4720` for the reference SoA reconstruction in the unordered-split loop). The fallback is documented as a known limitation; the common case (AoS) is fully functional.
- **Indirect variants are handled transparently by the existing constructor.** `Iron_<Iface>_from_<Type>(<Type> val)` heap-allocates internally for large variants (`emit_structs.c:324-338`), so the pop branch passes the by-value struct from `<lower>_items` regardless of whether the tagged union stores the variant inline or via pointer. No Plan 02 code needed to handle this.
- **Tests use 2-element initial literals with both implementors.** Carried forward from Plan 01's Deviation 4 — single-type initial literals like `[Circle(1)]` narrow to `[Circle]` (not `[Shape]`) via the monomorphic-collapse path, which is a Phase 56 concern. All Plan 02 tests start with both Circle and Square to force the split-collection path.

## Deviations from Plan

None — plan executed exactly as written. Plan 01 had already established the correct patterns (`impl->tag` for case labels, `impl->type_name` for tagged union field names, indirect-variant handling) via its own deviations, so Plan 02's skeleton was ground-truth-correct and required no corrections. The SoA scoping decision was explicit in the plan and simply followed.

The plan's guidance on `_total_count` ("sum of per-type sub-array lengths" in CONTEXT.md vs. "direct field read" in the plan body + critical context) was resolved in favor of the direct field read, as instructed by the critical context block in the execution prompt.

## Authentication Gates

None — no external services involved.

## Issues Encountered

None.

## User Setup Required

None.

## Next Phase Readiness

- **ROADMAP Phase 55 broad scope (CONTEXT.md):** Plans 01 and 02 together cover PUSH-01 (push + len + pop). The remaining accessors from CONTEXT.md (`.get(i)`, `.set(i, v)`) are deferred to Plan 55-03 (already planned, per `55-03-PLAN.md` present on disk).
- **Requirement PUSH-01:** Satisfied end-to-end. The PUSH-01 requirement statement covers push/len/pop; get/set are a separate deferred requirement (if the REQUIREMENTS.md tracks them separately).
- **Integration suite health:** 296 passing, 0 failing. No regression in Plan 01's 6 push tests.
- **SoA pop limitation:** Documented in the Task 2 commit message, the SUMMARY decisions section, and in-line C comments when the fallback is emitted. A future phase may revisit this if SoA-layout interface arrays with pop become common.

Plan 55-02 is complete and ready for Plan 55-03 (get/set) execution.

---
*Phase: 55-push-on-interface-arrays*
*Completed: 2026-04-09*

## Self-Check: PASSED

- All 7 key-files verified on disk (1 modified, 6 created).
- All 3 task commits present in `git log`: `ffeb288`, `fd5b96f`, `f56ee99`.
- Integration suite: 296 passing, 0 failing, 302 total (6 skipped baseline).
