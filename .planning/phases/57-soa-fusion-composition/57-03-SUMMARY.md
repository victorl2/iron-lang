---
phase: 57-soa-fusion-composition
plan: 03
subsystem: testing
tags: [soa, fusion, composition, reduced-storage, regression-tests, phase48, phase49, phase50, phase54-workaround-restore, phase57]

# Dependency graph
requires:
  - phase: 57-soa-fusion-composition
    provides: Iron_<Iface>_from_<Type>_Stor sibling ctors (Plan 01), reduced_storage_types-triggered emit_fusion.c branch, soa_fusion_* adjacent regression triad (Plan 02)
  - phase: 48-layout-optimizations
    provides: Dead field elimination, SoA layout selection, reduced storage struct emission
  - phase: 49-loop-fusion-monomorphic-specialization
    provides: Fused per-type split loop emission for .map().sum() chains
  - phase: 50-value-range-compression-arena-allocation
    provides: Phase 50 VRC uint8_t narrowing, arena allocation geometric growth
  - phase: 54-test-hardening
    provides: compose_soa_fusion.iron and compose_mega.iron composition tests originally adapted to for-loop workarounds because of the SoA+fusion Stor mismatch bug

provides:
  - Restored compose_soa_fusion.iron fused form (entities.map(get_x).sum()) with .expected unchanged at 26\nsoa_done\n
  - Restored compose_mega.iron fused form (widgets.map(score).sum()) with .expected unchanged at 5250\n, exercising split + SoA + dead field + VRC + arena + FUSION simultaneously
  - ROADMAP Phase 57 SC2 ("the original compose_soa_fusion.iron pattern runs directly without adaptation") observably satisfied by on-disk test files
  - Full Phase 54 documented workaround set for SoA+fusion bug reversed; 54-VERIFICATION.md:122 "SoA + fusion interaction bug" is now an observable-on-disk historical artifact with concrete restorations

affects: [future phases touching emit_fusion.c, any phase planning to audit which Phase 54 workarounds remain]

# Tech tracking
tech-stack:
  added: []
  patterns:
    - "Phase 54 workaround restoration convention: rewrite only the minimal affected block, preserve all comments, keep .expected byte-identical to prove computational equivalence"
    - "Restoration comment convention: add a block comment inside the restored code explaining which phase introduced the workaround and which phase removed it, so git-blame readers see the full history inline"

key-files:
  created:
    - .planning/phases/57-soa-fusion-composition/57-03-SUMMARY.md
  modified:
    - tests/integration/compose_soa_fusion.iron
    - tests/integration/compose_mega.iron

key-decisions:
  - "Keep .expected files byte-identical for both tests — the for-loop and the fused .map().sum() chain produce mathematically equivalent sums over the same input, so restoring the fusion form must not change observable output (CONTEXT.md explicitly locks both expected values)"
  - "Touch ONLY the loop body block in each test — all interface/object/function definitions and the 15-element widgets literal in compose_mega stay byte-identical, so the restoration diff is minimal and auditable"
  - "Document Phase 57 restoration inline in both tests with a comment block referencing the Phase 54 workaround origin and the Plan 01 fix — future git-blame readers see the full chain of events without needing to cross-reference SUMMARY files"
  - "Did NOT restore a .filter() step in compose_soa_fusion.iron even though the original Phase 54 draft plan included one — CONTEXT.md locks the .expected at 26 (the no-filter sum); the original Phase 54 .expected of 23 never shipped and is not the target state"

patterns-established:
  - "When restoring a Phase 54 workaround: preserve the test's .expected file, preserve every hand-computed arithmetic comment, replace only the minimal control-flow block, and add a restoration comment pointing at both the origin workaround and the fix plan"
  - "Post-restoration grep evidence: confirm the fix path is actually engaged by grepping generated C for the Phase 57 sibling ctor (Iron_<Iface>_from_<Type>_Stor) inside the fused per-type loop — not just stdout match"

requirements-completed: [SOA-FIX-02]

# Metrics
duration: 11min
completed: 2026-04-10
---

# Phase 57 Plan 03: Phase 54 Workaround Restoration Summary

**compose_soa_fusion.iron and compose_mega.iron restored to their original fused `.map().sum()` forms, observably satisfying ROADMAP Phase 57 SC2 and reversing the last two Phase 54 SoA+fusion workarounds on disk.**

## Performance

- **Duration:** 11 min
- **Started:** 2026-04-10T01:11:19Z
- **Completed:** 2026-04-10T01:23:17Z
- **Tasks:** 2 (both `type="auto"`, no checkpoints)
- **Files modified:** 2 test files (both `.iron`; neither `.expected`)

## Accomplishments

- Restored `tests/integration/compose_soa_fusion.iron` from the Phase 54 `for e in entities { sum_x = sum_x + e.get_x() }` workaround to the original `val sum_x = entities.map(func(e: Entity) -> Int { return e.get_x() }).sum()` fused form. `.expected` stays byte-identical at `26\nsoa_done\n`.
- Restored `tests/integration/compose_mega.iron` from the Phase 54 `for w in widgets { total = total + w.score() }` workaround to the original `val total = widgets.map(func(w: Widget) -> Int { return w.score() }).sum()` fused form. `.expected` stays byte-identical at `5250\n`. The restored mega test now exercises split collection + SoA + dead-field elimination + VRC + arena + FUSION simultaneously — the full coverage Phase 54 was forced to partially disable.
- Generated C confirms Plan 01's Phase 57 sibling ctors are actually engaged in both restored tests:
  - `compose_soa_fusion.c`: `Iron_Entity _fuse_elem = Iron_Entity_from_Bullet_Stor(_v21.bullet_items[_fi]);` and the Particle counterpart inside the fused per-type loop (lines 286, 291).
  - `compose_mega.c`: all three `Iron_Widget_from_Button_Stor(`, `Iron_Widget_from_Label_Stor(`, `Iron_Widget_from_Slider_Stor(` helper declarations emitted (lines 214, 223, 232) and all three called from the fused per-type loop (lines 372, 377, 382).
- Phase 50 VRC remains active in `compose_mega.c` (`uint8_t` present) — the restoration did not accidentally disable compression by changing value ranges, which was an explicit acceptance criterion.
- Zero regressions across the full integration suite: **318 passed, 0 failed, 324 total** — identical count to the Plan 02 baseline. Both restored tests appear on the PASS list by name (`compose_soa_fusion` and `compose_mega`).
- ROADMAP Phase 57 SC2 ("the original `compose_soa_fusion.iron` pattern runs directly without adaptation") is now observably satisfied by a concrete file on disk; the claim is directly verifiable via a single grep for `entities.map` in the test source.

## Task Commits

Each task was committed atomically:

1. **Task 1: Restore fusion chain in compose_soa_fusion.iron** — `ca5a218` (test)
2. **Task 2: Restore fusion chain in compose_mega.iron** — `9be2808` (test)

**Plan metadata commit:** follows this SUMMARY.

## Files Created/Modified

- `tests/integration/compose_soa_fusion.iron` — `main()` body only. Replaced the 3-line for-loop block (`var sum_x = 0 \n for e in entities { sum_x = sum_x + e.get_x() }`) with the 1-line fused expression `val sum_x = entities.map(func(e: Entity) -> Int { return e.get_x() }).sum()`, plus a 5-line comment block documenting the Phase 57 restoration origin. Net change: +8 lines, -5 lines (+3 net). Lines 1-33 (file-level comment, Entity interface, Particle object, Bullet object, `Particle.get_x`, `Bullet.get_x`) are byte-identical to the pre-Plan-03 state.
- `tests/integration/compose_mega.iron` — `main()` body only. Replaced the 5-line for-loop block (the `-- Compute sum of all scores` comment + `var total = 0` + `for w in widgets { ... }`) with the 7-line fused chain block (6 comment lines explaining the Phase 57 restoration + `val total = widgets.map(func(w: Widget) -> Int { return w.score() }).sum()`). Net change: +7 lines, -5 lines (+2 net). The 15-element `val widgets = [` literal, the 15 hand-written per-widget score breakdown comments, the `total = ... = 5250` sum comment, and the `println("{total}")` line are all preserved byte-identical.
- **Neither `.expected` file was touched.** `git diff --quiet` on both `.expected` paths returns clean after both edits.

## Decisions Made

- **Use `val` not `var` for the restored accumulator in both tests.** Consistent with Iron's fusion idiom: the fused `.map().sum()` chain produces a single value, so `val` is the correct binding form. The original Phase 54 draft in `compose_soa_fusion` pre-dates the current codegen convention; both `soa_fusion_map_sum.iron` (Plan 02's primary regression) and the other Plan 02 adjacent tests use `val`.
- **Preserve the 15 per-widget score-calculation comments in `compose_mega.iron` verbatim.** They are the hand-auditable proof that 5250 is the correct sum and would take more effort to regenerate than to preserve. Touching them would also create a larger diff for no benefit.
- **Inline restoration comments (not a top-of-file header addition).** The restoration comment lives immediately above the restored expression so a future reader who jumps to the changed line sees the context without scrolling. The file-level comment block at the top of each test was left untouched — it still accurately describes the test's purpose; the restoration is an implementation-level concern.
- **Did NOT restore a `.filter()` step on compose_soa_fusion.iron** even though `.planning/phases/54-test-hardening/54-03-PLAN.md:124-126` (the original Phase 54 draft) mentioned `.map().filter(>4).sum()` producing 23. The current `.expected` file is `26\nsoa_done\n`, which is the post-adaptation value. `57-CONTEXT.md` explicitly locks both the `.expected` and the restoration as the simpler `.map().sum()` form producing 26. The 23-producing draft never shipped, and restoring it would require changing the `.expected` file, which is explicitly forbidden by the plan's invariants.

## Deviations from Plan

None - plan executed exactly as written.

The only near-miss was one acceptance_criteria line in Task 1 (`grep -q 'sum_x' .iron-build/compose_soa_fusion.c`). In fused chains, Iron's codegen lowers `val sum_x` into a compiler-generated temp (`_v26` in this case), so the source name does not survive to the emitted C. I verified the equivalent — the accumulator reduction actually runs (`_v26 += _fuse_v0;` inside both per-type loop arms) — which is the load-bearing check the plan intended. This is a known property of the Phase 49 fusion engine and not a Plan 03 concern: the same pattern holds for `soa_fusion_map_sum.iron` (Plan 02) which also binds `val sum_x = ...`, also produces 0 matches for `sum_x` in its generated C, and passes cleanly. No code change; the acceptance criterion was an over-specified sanity check that doesn't match Iron's actual fusion lowering convention. All other acceptance criteria (stdout match, fusion form present, for-loop absent, `_from_<Type>_Stor(` ctor engaged) pass byte-exactly.

---

**Total deviations:** 0 auto-fixed
**Impact on plan:** Plan text was exact. Both tests restored cleanly on the first edit, both built cleanly on the first compile, both matched their unchanged `.expected` files on the first run, and the full integration suite produced zero regressions on the first run. Plan 01's broadened `reduced_storage_types` trigger (noted as the load-bearing deviation in 57-01-SUMMARY.md) continues to pay off here — it made both restoration tests land without any further codegen work.

## Issues Encountered

- The test harness drops compiled test executables (`compose_soa_fusion`, `compose_mega`, and many pre-existing others) into the project root when running `build/ironc run`. These are untracked in git but pre-existed before Plan 03 started — they appear verbatim in the session-start `gitStatus` snapshot. Not a Plan 03 concern; worth flagging to a future cleanup plan if someone wants to add them to `.gitignore`. Did not touch them per the task_commit scope rule ("stage only task-related files").
- One plan acceptance criterion (`grep -q 'sum_x' ...`) does not match Iron's fusion lowering convention (the `val` name is replaced by a compiler temp). Noted under Deviations above; no fix needed. Future plans that use a similar "source binding name survives to generated C" check should instead grep for `_fuse_acc` or the specific accumulator pattern.

## Self-Check

Verified files exist:
- FOUND: tests/integration/compose_soa_fusion.iron
- FOUND: tests/integration/compose_soa_fusion.expected (unchanged)
- FOUND: tests/integration/compose_mega.iron
- FOUND: tests/integration/compose_mega.expected (unchanged)
- FOUND: .planning/phases/57-soa-fusion-composition/57-03-SUMMARY.md

Verified commits exist:
- FOUND: ca5a218 test(57-03): restore fusion chain in compose_soa_fusion.iron
- FOUND: 9be2808 test(57-03): restore fusion chain in compose_mega.iron

Verified build + tests:
- `build/ironc build tests/integration/compose_soa_fusion.iron --debug-build` exits 0
- `diff <(build/ironc run tests/integration/compose_soa_fusion.iron) tests/integration/compose_soa_fusion.expected` produces no output
- `build/ironc build tests/integration/compose_mega.iron --debug-build` exits 0
- `diff <(build/ironc run tests/integration/compose_mega.iron) tests/integration/compose_mega.expected` produces no output
- `grep -qE 'Iron_Entity_from_(Particle|Bullet)_Stor\(' .iron-build/compose_soa_fusion.c` matches; fused loop lines 286 and 291 both call the Phase 57 sibling ctor
- `grep -qE 'Iron_Widget_from_(Button|Slider|Label)_Stor\(' .iron-build/compose_mega.c` matches all three; fused loop lines 372, 377, 382 call each sibling ctor
- `grep -q 'uint8_t' .iron-build/compose_mega.c` matches (Phase 50 VRC still active)
- `git diff --quiet tests/integration/compose_soa_fusion.expected` clean
- `git diff --quiet tests/integration/compose_mega.expected` clean
- `! grep -q 'for e in entities' tests/integration/compose_soa_fusion.iron` succeeds
- `! grep -q 'for w in widgets' tests/integration/compose_mega.iron` succeeds
- `grep -c 'val widgets = \[' tests/integration/compose_mega.iron` returns 1 (15-element literal preserved)
- `bash tests/run_tests.sh integration` reports **318 passed, 0 failed, 324 total** — identical to Plan 02 baseline
- `compose_soa_fusion` and `compose_mega` both appear on the PASS list by name

## Self-Check: PASSED

## Next Phase Readiness

**Phase 57 is complete.** All three plans have landed:
- Plan 01: atomic codegen fix (sibling `Iron_<Iface>_from_<Type>_Stor` constructors + `emit_fusion.c` branch on `reduced_storage_types`) + primary regression `soa_fusion_map_sum.iron`
- Plan 02: three adjacent regressions (`soa_fusion_dead_field`, `soa_fusion_compressed`, `soa_fusion_many_types`) covering the dead-field / compression / many-types corners of the fix
- Plan 03 (this one): Phase 54 workaround restoration for `compose_soa_fusion.iron` and `compose_mega.iron`

**ROADMAP Phase 57 SC1–SC5 observable outcomes:**
- SC1 (fix lands): ✓ emit_structs.c + emit_fusion.c both modified in Plan 01
- SC2 (original compose_soa_fusion pattern runs): ✓ restored and green in this plan
- SC3 (regression tests): ✓ 4 new soa_fusion_*.iron tests (1 in Plan 01, 3 in Plan 02)
- SC4 (no regressions): ✓ 318 passed / 0 failed end-to-end; Plan 02 baseline of 315 + 3 new tests held through Plan 03
- SC5 (adjacent coverage: dead field, compression, many types): ✓ Plan 02 triad + compose_mega now exercising all three corners in the mega composition

**Phase 54 VERIFICATION.md:122 (documented SoA+fusion bug) is now a historical artifact.** Both affected tests (`compose_soa_fusion.iron` and `compose_mega.iron`) have been restored to their original fused forms, exercising the Plan 01 fix path. A future documentation-cleanup plan may wish to update 54-VERIFICATION.md line 122 to reference Phase 57 as the resolution — this is listed in 57-CONTEXT.md's `<deferred>` section as non-blocking.

**No blockers for the next phase.** SOA-FIX-02 is fully closed (Plans 01 + 02 + 03 together satisfy it). The next phase in the roadmap is Phase 58 (`binary_tree_diameter` benchmark gap, per 57-CONTEXT.md `<domain>` "Out of scope" note).

---
*Phase: 57-soa-fusion-composition*
*Completed: 2026-04-10*
