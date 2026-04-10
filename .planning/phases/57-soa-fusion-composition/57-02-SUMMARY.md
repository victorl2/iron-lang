---
phase: 57-soa-fusion-composition
plan: 02
subsystem: testing
tags: [soa, fusion, dead-field-elim, vrc, value-range-compression, regression-tests, phase48, phase49, phase50, phase57]

# Dependency graph
requires:
  - phase: 57-soa-fusion-composition
    provides: Iron_<Iface>_from_<Type>_Stor sibling ctors (Plan 01), reduced_storage_types-triggered emit_fusion.c branch
  - phase: 48-layout-optimizations
    provides: Dead field elimination, SoA layout selection
  - phase: 49-loop-fusion-monomorphic-specialization
    provides: Fused per-type split loop emission for .map().sum() chains
  - phase: 50-value-range-compression-arena-allocation
    provides: Phase 50 VRC narrowing to uint8_t for small-range fields in _Stor structs

provides:
  - Adjacent regression test soa_fusion_dead_field.iron covering SC5 "SoA + fusion + dead field" (5-field implementor, 4 dead fields per element)
  - Adjacent regression test soa_fusion_compressed.iron covering SC5 "SoA + fusion + compression" (Phase 50 VRC uint8_t storage + (int64_t) widening cast path)
  - Adjacent regression test soa_fusion_many_types.iron covering SC5 "SoA + fusion on split with many types" (4 implementors, 4 distinct sibling ctors emitted and called)
  - Full SOA-FIX-02 regression surface closed (Plans 01 + 02 together)

affects: [57-03, future phases touching emit_structs.c sibling ctor emission or emit_fusion.c per-type loop wrap]

# Tech tracking
tech-stack:
  added: []
  patterns:
    - "Adjacent regression test triad: one test per corner of a fix (dead-field, compression, many-types)"
    - "Generated-C grep verification alongside runtime stdout match (Phase 54 test convention)"

key-files:
  created:
    - tests/integration/soa_fusion_dead_field.iron
    - tests/integration/soa_fusion_dead_field.expected
    - tests/integration/soa_fusion_compressed.iron
    - tests/integration/soa_fusion_compressed.expected
    - tests/integration/soa_fusion_many_types.iron
    - tests/integration/soa_fusion_many_types.expected
    - .planning/phases/57-soa-fusion-composition/57-02-SUMMARY.md
  modified: []

key-decisions:
  - "Dead-field test uses 5 fields per implementor (above primary test's 4) with large sentinel values 5001-9004 so any dead-field leakage would blow the sum to tens of thousands instead of 500"
  - "Compressed test uses priority values in [1..9] and other fields in [1..50] guaranteeing Phase 50 VRC narrows to uint8_t; validated against generated C"
  - "Many-types test uses 4 implementors (Soldier/Archer/Mage/Healer) instead of the minimum 3, cleanly proving the sibling emission loop handles a count above the edge"
  - "All three tests use `.map(get_*).sum()` rather than `.map().filter().reduce()` — simpler, same fusion engagement, keeps the test focus on Plan 01's sibling ctor path"
  - "No plan deviations needed: Plan 01's reduced_storage_types trigger generalization (noted in 57-01-SUMMARY.md as the critical deviation) meant all three acceptance-criteria grep patterns landed on the first build, no probe-and-adapt required"

patterns-established:
  - "Three-test adjacent regression triad: cover dead-field corner, compression corner, and many-types corner of a fix in parallel"
  - "Hand-compute expected sums with intermediate per-type breakdown inside comments so the test's acceptance is human-auditable"
  - "Assert on both runtime stdout AND generated-C grep patterns in acceptance_criteria (confirms the fix path is actually exercised, not just that the numbers line up)"

requirements-completed: [SOA-FIX-02]

# Metrics
duration: 12min
completed: 2026-04-10
---

# Phase 57 Plan 02: SoA + Fusion Adjacent Regression Triad Summary

**Three adjacent regression tests (dead-field zero-init, VRC widening cast, 4-implementor sibling loop) closing the SOA-FIX-02 regression surface, all green end-to-end with Plan 01's reduced_storage_types-triggered sibling constructors.**

## Performance

- **Duration:** 12 min
- **Started:** 2026-04-10T00:53:02Z
- **Completed:** 2026-04-10T01:05:05Z
- **Tasks:** 3 (all `type="auto"`, no checkpoints)
- **Files created:** 6 test files (3 `.iron` + 3 `.expected`) + 1 SUMMARY

## Accomplishments

- Landed `soa_fusion_dead_field.iron` — 5-field Ledger/Note implementors with only `get_value()` exposed, `.map(get_value).sum()` fused chain, hand-computed total 500. Generated C proves the sibling constructor writes `u.data.Ledger.label_a = 0;` through `label_d = 0;` and the symmetric `tag_a..d` zero-init for Note, with `(int64_t)val.value` widening copy on the single alive field.
- Landed `soa_fusion_compressed.iron` — 4-field Job/Chore implementors with priority values in `[1..9]`, forcing Phase 50 VRC to narrow the `_Stor` priority field to `uint8_t`. The `.map(get_priority).sum()` chain hand-computes to 26. Generated C proves both `uint8_t priority` in the `_Stor` struct AND `(int64_t)val.priority` widening cast in the sibling ctor body.
- Landed `soa_fusion_many_types.iron` — 4-implementor split collection (Soldier/Archer/Mage/Healer) with `.map(get_power).sum()` fused chain hand-computed to 86. Generated C contains 4 distinct `Iron_Unit_from_<Type>_Stor(` sibling ctor declarations AND all 4 are called from the fused per-type loop.
- Zero regressions across the full integration suite: **318 passed, 0 failed** (Plan 01 was at 315; our 3 new tests add exactly 3 more passers).

## Task Commits

Each task was committed atomically:

1. **Task 1: Create soa_fusion_dead_field.iron + .expected** — `ef04b99` (test)
2. **Task 2: Create soa_fusion_compressed.iron + .expected** — `67de731` (test)
3. **Task 3: Create soa_fusion_many_types.iron + .expected** — `f8ce5c5` (test)

**Plan metadata commit:** will follow after this SUMMARY lands.

## Files Created/Modified

- `tests/integration/soa_fusion_dead_field.iron` — 55-line integration test: interface Entry, 5-field Ledger, 5-field Note, `entries.map(func(e: Entry) -> Int { return e.get_value() }).sum()`. Values chosen so dead-field leakage would produce obviously wrong output (sentinels 5001-9004 vs expected total 500).
- `tests/integration/soa_fusion_dead_field.expected` — `500\ndead_field_done\n`
- `tests/integration/soa_fusion_compressed.iron` — 58-line integration test: interface Task, 4-field Job, 4-field Chore, priority values in `[1..9]`, other fields in `[1..50]` to guarantee Phase 50 VRC narrows to uint8_t. `tasks.map(func(t: Task) -> Int { return t.get_priority() }).sum()`.
- `tests/integration/soa_fusion_compressed.expected` — `26\ncompressed_done\n`
- `tests/integration/soa_fusion_many_types.iron` — 81-line integration test: interface Unit, 4-field Soldier/Archer/Mage/Healer each, `army.map(func(u: Unit) -> Int { return u.get_power() }).sum()`. Eight elements total (2 of each type) to force the fused loop to visit each sibling ctor at least twice.
- `tests/integration/soa_fusion_many_types.expected` — `86\nmany_types_done\n`

## Decisions Made

- **Test chain: `.map(get_*).sum()` in all three tests.** The plan left the fusion terminal open (`.sum()` or `.reduce()`) and I picked `.sum()` everywhere because it's the simplest fusible terminal that engages the Phase 49 fusion engine and is unambiguous about what it computes. All three tests hand-compute a single Int sum, which is trivially auditable. No need for `.filter()` since none of these tests is about filtering — they're all about whether the sibling ctor copies the surviving field(s) correctly.
- **Sentinel values in dead_field test:** Ledger dead fields carry 5001-9004, Note dead fields carry 5001-8004. Picked these intentionally large to make a leakage-bug diagnosis obvious: if any dead field contributed to the sum instead of being zeroed, the sum would be in the tens of thousands (minimum), not 500. Easier debugging surface than tiny sentinel values.
- **Compressed test: values in `[1..50]`, priority in `[1..9]`.** Small enough to guarantee uint8_t narrowing (Phase 50 narrows at <256 range). Priority's tighter range `[1..9]` is doubly narrow so even if some edge in Phase 50 uses a different narrowing threshold, priority will still narrow. Generated-C grep confirms both uint8_t storage AND the widening cast actually fire.
- **Many-types test: 4 implementors, not 3.** The plan said "3+"; I picked 4 for a cleaner test (four is a round number with breathing room above the minimum, and produces 4 sibling ctor declarations, making the count check unambiguous). Eight elements total (2 per implementor) guarantees the fused loop visits each sibling ctor's call site at least twice, which is a stronger correctness proof than 1-per-type.
- **All tests use the same pattern (`.map(get_<field>).sum()`) for the fused chain.** Keeps the test triad uniform and makes the three tests directly comparable — any one of them failing points cleanly to the corresponding corner of the fix (dead-field, compression, or per-impl loop).

## Deviations from Plan

None - plan executed exactly as written.

Plan 01's decision to broaden the trigger from `ctx->soa_types` to `ctx->reduced_storage_types` was load-bearing here: all three tests use fusion-only code (no `for_pre` loops), so Phase 48's layout_select doesn't run and `soa_types` would have been empty in all three cases. The `reduced_storage_types`-triggered path is exactly what made Plan 02's tests work on the first build without any plan-vs-reality probing. Acknowledged in the plan's execution notes and the 57-01-SUMMARY.md Rule 1 deviation — consumed as intended here.

---

**Total deviations:** 0
**Impact on plan:** Plan text was exact. All three test files landed as specified; all runtime stdouts match hand-computed values; all grep acceptance criteria passed on the first build.

## Issues Encountered

- `build/ironc --debug-build build ...` (flag before subcommand) fails with "unknown command '--help'"-style error; the correct invocation is `build/ironc build <file> --debug-build` (flag after the positional argument). Minor ergonomic quirk, already noted in 57-01-SUMMARY.md. Used the trailing-flag form throughout. Not a deviation.

## Self-Check

Verified files exist:
- FOUND: tests/integration/soa_fusion_dead_field.iron
- FOUND: tests/integration/soa_fusion_dead_field.expected
- FOUND: tests/integration/soa_fusion_compressed.iron
- FOUND: tests/integration/soa_fusion_compressed.expected
- FOUND: tests/integration/soa_fusion_many_types.iron
- FOUND: tests/integration/soa_fusion_many_types.expected
- FOUND: .planning/phases/57-soa-fusion-composition/57-02-SUMMARY.md

Verified commits exist:
- FOUND: ef04b99 test(57-02): add soa_fusion_dead_field adjacent regression
- FOUND: 67de731 test(57-02): add soa_fusion_compressed adjacent regression
- FOUND: f8ce5c5 test(57-02): add soa_fusion_many_types adjacent regression

Verified build + tests:
- `build/ironc build tests/integration/soa_fusion_dead_field.iron --debug-build` exits 0; stdout matches expected; 10 grep acceptance criteria all pass
- `build/ironc build tests/integration/soa_fusion_compressed.iron --debug-build` exits 0; stdout matches expected; 4 grep acceptance criteria (widening cast, both sibling ctors, VRC narrowing) all pass
- `build/ironc build tests/integration/soa_fusion_many_types.iron --debug-build` exits 0; stdout matches expected; 7 grep acceptance criteria (4 sibling ctor calls + count>=4 + no (void)is_soa residue) all pass
- `bash tests/run_tests.sh integration` reports **318 passed, 0 failed, 324 total** — all 3 new tests pass, zero regressions from Plan 01's baseline of 315 passers

## Self-Check: PASSED

## Next Phase Readiness

**SOA-FIX-02 fully closed.** Plans 01 and 02 together deliver the full Phase 57 fix surface:
- Plan 01: atomic codegen fix (sibling Iron_<Iface>_from_<Type>_Stor ctors + emit_fusion.c branch on reduced_storage_types) + primary regression `soa_fusion_map_sum.iron`
- Plan 02: three adjacent regressions covering the three corners of the fix (dead-field, compression, many-types)

**Plan 57-03 can proceed.** Plan 03's scope is restoring the fusion chain portions of `compose_soa_fusion.iron` and `compose_mega.iron` (Phase 54 workarounds). The codegen fix is already atomic and proven across 4 regression tests, so Plan 03 is purely a test-editing task.

**No blockers for Plan 03.** All Phase 57 codegen work is complete; only test restoration remains.

---
*Phase: 57-soa-fusion-composition*
*Completed: 2026-04-10*
