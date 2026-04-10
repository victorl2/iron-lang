---
phase: 57-soa-fusion-composition
plan: 01
subsystem: codegen
tags: [soa, fusion, dead-field-elim, tagged-union, reduced-storage, phase48, phase49, phase50]

# Dependency graph
requires:
  - phase: 48-layout-optimizations
    provides: ctx->soa_types, ctx->reduced_storage_types, iron_layout_is_field_used, LayoutAnalysis
  - phase: 49-loop-fusion-monomorphic-specialization
    provides: Fused per-type split loop emission, ctx->soa_types probe wired but deferred
  - phase: 50-value-range-compression-arena-allocation
    provides: iron_vr_get_narrowed_type, Phase 50 VRC narrowed-type metadata
  - phase: 52-emitter-refactoring
    provides: emit_structs.c / emit_split.c / emit_fusion.c decomposition, EmitCtx in emit_helpers.h
  - phase: 54-test-hardening
    provides: Documented SoA+fusion bug at 54-VERIFICATION.md:122, compose_soa_fusion workaround to be restored

provides:
  - Iron_<Iface>_from_<Type>_Stor sibling tagged-union constructors for every reduced-storage (iface, impl) pair
  - Correct fused per-type loop element wrap for both SoA and AoS+dead-fields reduced storage
  - Primary regression test soa_fusion_map_sum.iron exercising Phase 48 dead-field elim + Phase 50 VRC + Phase 49 fusion end-to-end
  - Closed the (void)is_soa deferred marker in emit_fusion.c

affects: [57-02, 57-03, future SoA-related fusion work, any phase that touches emit_structs.c tagged-union constructor emission]

# Tech tracking
tech-stack:
  added: []
  patterns:
    - "Sibling tagged-union constructor for reduced storage (emit after split collection so both ctx->soa_types and ctx->reduced_storage_types are populated)"
    - "ctx->reduced_storage_types as the authoritative fusion-branch trigger (superset of soa_types)"

key-files:
  created:
    - tests/integration/soa_fusion_map_sum.iron
    - tests/integration/soa_fusion_map_sum.expected
    - .planning/phases/57-soa-fusion-composition/57-01-SUMMARY.md
  modified:
    - src/lir/emit_structs.c
    - src/lir/emit_fusion.c

key-decisions:
  - "Broadened the sibling emission + fused-loop branch trigger from ctx->soa_types to ctx->reduced_storage_types (the plan's original trigger was a proper subset)"
  - "Moved the sibling constructor emission to run AFTER emit_split_collection_for_iface() in emit_type_decls() so reduced_storage_types and soa_types are populated AND the Iron_<Type>_Stor typedef is in place before the sibling references it"
  - "Removed the is_soa local in emit_fusion.c entirely; kept the (void)is_soa reference only in a comment documenting the historical defer marker"
  - "Kept the full Iron_<Iface>_from_<Type> AoS constructor byte-identical; the Stor sibling is purely additive"

patterns-established:
  - "When emitting into struct_bodies after split collection, re-iterate entry->impls to pair each impl with its (now populated) soa/reduced state"
  - "iface_collection_vids stb_ds local built from ctx->split_collection_ids[i].value string compare, mirroring emit_split.c:125-133"
  - "VRC-narrowed alive fields get explicit (int64_t) widening cast on copy to the full Iron_<Type> data slot"
  - "Dead fields get literal = 0 zero-init in the sibling body (safe per Phase 48 elimination proof)"

requirements-completed: [SOA-FIX-01]

# Metrics
duration: 28min
completed: 2026-04-09
---

# Phase 57 Plan 01: SoA + Fusion Core Correctness Fix Summary

**Sibling Iron_<Iface>_from_<Type>_Stor tagged-union constructors + emit_fusion.c branch on reduced storage, unblocking every fused chain over a split collection whose per-type sub-arrays hold the reduced Stor variant (SoA and AoS+dead-fields alike).**

## Performance

- **Duration:** 28 min
- **Started:** 2026-04-10T00:17:55Z
- **Completed:** 2026-04-10T00:46:00Z
- **Tasks:** 2 (both `type="auto"`)
- **Files modified:** 2 source + 2 new tests = 4 total

## Accomplishments

- Closed the (void)is_soa SoA-aware access deferred marker in emit_fusion.c that has stood open since Phase 49
- Emitted per-(iface, impl) sibling constructors Iron_<Iface>_from_<Type>_Stor that expand Iron_<Type>_Stor back into full Iron_<Iface> tagged unions with dead-field zero-init and Phase 50 VRC widening
- Broadened the plan's SoA-only trigger to reduced_storage_types, catching the AoS+dead-fields subcase that the plan's diagnosis missed
- Landed tests/integration/soa_fusion_map_sum.iron as the minimal primary regression test; matches 26\nsoa_fusion_done\n exactly
- Preserved every pre-existing passing integration test: 315 passed, 0 failed across the full suite

## Task Commits

Each task was committed atomically:

1. **Task 1: Emit _from_<Type>_Stor sibling constructors in emit_structs.c** — `64b2b30` (feat)
2. **Task 2: Branch fused per-type loop on reduced storage + ship soa_fusion_map_sum regression test** — `96590bc` (fix)

## Files Created/Modified

- `src/lir/emit_structs.c` — Added the Phase 57 sibling emission loop inside emit_type_decls()'s per-interface block, placed AFTER emit_split_collection_for_iface(). The loop iterates alive impls, skips pointer-indirect variants, checks reduced_storage_types, and emits a static inline Iron_<Iface>_from_<Type>_Stor whose body copies alive fields (with (int64_t) widening casts on Phase 50 VRC-narrowed fields) and zero-inits dead fields.
- `src/lir/emit_fusion.c` — Replaced lines 312-332 of the fused per-type loop element wrap emission. The is_soa local is removed. A new is_reduced local probes ctx->reduced_storage_types. A ctor_suffix local selects "_Stor" when reduced storage is in use and "" otherwise. The appendf format changes from %s_from_%s( (3 args) to %s_from_%s%s( (4 args) so the suffix lands after impl->type_name.
- `tests/integration/soa_fusion_map_sum.iron` — Primary regression test mirroring 57-CONTEXT.md's reproduction snippet verbatim. Entity interface with Particle/Bullet (4 fields each, only get_x exposed), `entities.map(func(e) -> e.get_x()).sum()` fused chain, hand-computed sum 26.
- `tests/integration/soa_fusion_map_sum.expected` — `26\nsoa_fusion_done\n`

## Decisions Made

- **Sibling emission position:** Placed AFTER emit_split_collection_for_iface() rather than BEFORE (as the plan's `<action>` suggested). Required because emit_split_collection_for_iface is the only site that populates ctx->reduced_storage_types and ctx->soa_types and emits the Iron_<Type>_Stor typedef. At the plan's proposed site (inside the wrap-ctor loop), all three were still uninitialized, so the guards would always evaluate false. See Deviations Rule 3 below.
- **Trigger broadening (soa_types → reduced_storage_types):** The plan's diagnosis framed the bug as SoA-only, but emit_split.c line 343-353 shows even AoS-path sub-arrays use <Type>_Stor *items when reduced_storage_types is set (dead-field elimination). The soa_fusion_map_sum repro actually hits this AoS+dead-fields path (no for_pre loop means iron_layout_select never runs, so layout falls through to AoS) while still hitting reduced storage. Triggering on soa_types alone would have left the test failing. Triggering on reduced_storage_types — which is a strict superset — covers both cases under one rule with zero loss of specificity.
- **Keep Iron_<Iface>_from_<Type> AoS ctor unchanged:** Non-fusion AoS collection code paths still call the full-type ctor via the existing emit_c.c manual struct reconstruction. The sibling is purely additive.
- **Use hmlen (not shlen) for split_collection_ids iteration:** split_collection_ids is an int-keyed stb_ds hmap, so hmlen is the correct length macro. The plan's example used shlen (appropriate for string-keyed maps only). Matches the canonical pattern in emit_split.c:129 and emit_c.c:4803.

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 3 - Blocking] Sibling emission position had to move after emit_split_collection_for_iface**
- **Found during:** Task 1 verification (layout_soa_select.iron)
- **Issue:** The plan specified emitting the `_from_<Type>_Stor` sibling inside the existing per-impl wrap-ctor loop at emit_structs.c:454-494. At that site, ctx->soa_types is empty (population happens later in emit_split_collection_for_iface at emit_structs.c:497), ctx->reduced_storage_types is empty, and the Iron_<Type>_Stor typedef hasn't been emitted yet. Result: my first attempt emitted sibling calls that would compile cleanly on no-SoA tests but zero on every reduced-storage test because the guards rejected every impl.
- **Fix:** Moved the sibling emission to a fresh scoped block AFTER the `emit_split_collection_for_iface(ctx, iface_mangled, entry);` call at line 498, re-iterating entry->impls. struct_bodies is still emitted before any function body, so the sibling still precedes every caller. The `sb`, `iface_mangled`, and `entry` locals are all in scope at that point.
- **Files modified:** src/lir/emit_structs.c (sibling block moved, wrapped in its own `{ }` scope with a fresh iface_collection_vids57 local)
- **Verification:** After move, `build/ironc build --debug-build tests/integration/layout_soa_select.iron` emits `static inline Iron_Entity Iron_Entity_from_Bullet_Stor(Iron_Bullet_Stor val)` and the Particle sibling, with correct dead-field zero-init and (int64_t) widening cast on the VRC-narrowed x field.
- **Committed in:** 64b2b30 (Task 1 commit)

**2. [Rule 1 - Bug] Broaden sibling + fusion trigger from soa_types to reduced_storage_types**
- **Found during:** Task 2 verification (soa_fusion_map_sum.iron compile)
- **Issue:** With the Task 1 fix in place, `build/ironc build tests/integration/soa_fusion_map_sum.iron` still failed clang compilation with `passing 'Iron_Bullet_Stor' to parameter of incompatible type 'Iron_Bullet'`. Inspection of the generated C showed that Bullet_Stor/Particle_Stor typedefs WERE emitted and bullet_items WAS typed `Iron_Bullet_Stor *` — but ctx->soa_types was empty, so my guards on soa_types rejected the emission entirely. The root cause: soa_fusion_map_sum.iron has no for_pre loop (only `entities.map(...).sum()`), and `iron_layout_select` in layout_analysis.c:444-507 only scans `for_pre`-prefixed blocks. So the layout decision fell through to AoS, but dead-field elimination at emit_split.c:158-201 (which is layout-independent) still set `reduced_storage_types["Particle"]` and `reduced_storage_types["Bullet"]`. Result: sub-arrays are Stor but ctx->soa_types empty.
- **Fix:** Replaced `ctx->soa_types` with `ctx->reduced_storage_types` (string-keyed by impl->type_name, not `<iface>:<type>`) as the trigger in BOTH sites:
  1. emit_structs.c sibling emission guard
  2. emit_fusion.c ctor_suffix branch
  Removed the is_soa local in emit_fusion.c entirely since the only remaining reader was gone. Updated comments in both files to document the dual-cause (SoA OR AoS+dead-fields) and the plan-vs-implementation discrepancy.
- **Files modified:** src/lir/emit_structs.c, src/lir/emit_fusion.c
- **Verification:**
  - `build/ironc build --debug-build tests/integration/soa_fusion_map_sum.iron` exits 0
  - `build/ironc run tests/integration/soa_fusion_map_sum.iron` matches .expected exactly
  - Generated C grep: `Iron_Entity_from_Bullet_Stor(` present, `Iron_Entity_from_Particle_Stor(` present
  - Negative grep: `Iron_Entity_from_Bullet(_v21.bullet_items` absent, `Iron_Entity_from_Particle(_v21.particle_items` absent
  - Full integration suite: 315 passed, 0 failed
- **Committed in:** 96590bc (Task 2 commit)

---

**Total deviations:** 2 auto-fixed (1 blocking position fix, 1 bug in plan's diagnostic framing)
**Impact on plan:** Both auto-fixes were required for the fix to land at all. No scope creep; both kept the plan's "locked helper body shape" intact. The plan's 57-01 truths and the primary acceptance test were all satisfied by the broader trigger. Plan 57-02 and 57-03 can assume the broader trigger is in place; I'll note in Plan 57-02 prep that the soa_fusion_dead_field test case they add is now doubly motivated — it exercises the very path (AoS + dead-fields + fusion) the deviation surfaced.

## Issues Encountered

- `build/ironc build` silently cleans the `.iron-build/` directory on success, which confused initial verification until I discovered the `--debug-build` flag via `ironc --help`. Used `--debug-build` thereafter for every grep-the-generated-C check. Not a deviation; just an ergonomic note for future plans that need to grep generated C.
- `build/iron` (the package manager that `tests/run_tests.sh` uses) is a separate CMake target from `build/ironc`. Running `cmake --build build --target ironc --clean-first` deletes `build/iron` as a side effect. Had to re-run `cmake --build build --target iron` to restore it before the final integration suite run. Not a deviation; just a build-system quirk.

## Self-Check

Verified files exist:
- FOUND: src/lir/emit_structs.c (modified, contains `_from_%s_Stor` sibling emission block)
- FOUND: src/lir/emit_fusion.c (modified, contains `ctor_suffix = is_reduced`)
- FOUND: tests/integration/soa_fusion_map_sum.iron
- FOUND: tests/integration/soa_fusion_map_sum.expected
- FOUND: .planning/phases/57-soa-fusion-composition/57-01-SUMMARY.md

Verified commits exist:
- FOUND: 64b2b30 feat(57-01): emit Iron_<Iface>_from_<Type>_Stor sibling constructors
- FOUND: 96590bc fix(57-01): branch fused per-type loop on reduced storage and ship soa_fusion_map_sum regression test

Verified build + tests:
- Clean `cmake --build build --target ironc --clean-first` succeeds with zero new warnings
- `build/ironc build --debug-build tests/integration/soa_fusion_map_sum.iron` exits 0
- `diff <(build/ironc run tests/integration/soa_fusion_map_sum.iron) tests/integration/soa_fusion_map_sum.expected` produces no output
- Generated C contains `Iron_Entity_from_Bullet_Stor(` AND `Iron_Entity_from_Particle_Stor(`
- Generated C does NOT contain `Iron_Entity_from_Bullet(_v[0-9]+.bullet_items` or the Particle variant
- Source contains no bare `(void)is_soa;` statement (only an explanatory mention inside a code comment)
- `bash tests/run_tests.sh integration` reports 315 passed, 0 failed

## Self-Check: PASSED

## Next Phase Readiness

**Plan 57-02 can now proceed.** The broadened reduced_storage_types trigger directly enables the Plan 57-02 adjacent tests:
- `soa_fusion_dead_field.iron` (explicit dead-field-elim test) now exercises the very path the Rule 1 deviation fixed — this test is doubly load-bearing post-Phase 57-01
- `soa_fusion_compressed.iron` (VRC widening) will exercise the `(int64_t)val.<field>` cast path in the sibling body; layout_soa_select already proved this works for uint8_t→int64_t
- `soa_fusion_many_types.iron` will exercise the sibling emission loop for 3+ implementors in a single interface

**Plan 57-03 can also proceed.** The core fix is in place, so restoring the fusion chains in compose_soa_fusion.iron and compose_mega.iron is purely a test-editing task from 57-03's perspective — the codegen fix is atomic in this plan.

**Plan 57-02 planning note:** the soa_fusion_dead_field test must explicitly NOT use a for_pre loop anywhere in the same file so it hits the AoS+dead-fields path specifically (not the SoA path). If it accidentally adds a loop, layout_select will pick SoA and the test becomes redundant with soa_fusion_map_sum. Worth mentioning in the 57-02 plan.

**No blockers for subsequent plans.**

---
*Phase: 57-soa-fusion-composition*
*Completed: 2026-04-09*
