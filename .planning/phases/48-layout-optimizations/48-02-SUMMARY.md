---
phase: 48-layout-optimizations
plan: 02
subsystem: compiler-backend
tags: [lir, emit-c, soa-layout, aos-layout, field-access-analysis, common-field, split-collections]

# Dependency graph
requires:
  - phase: 48-layout-optimizations
    provides: "LayoutAnalysis module, dead field elimination, reduced storage structs"
provides:
  - "Automatic SoA/AoS layout selection based on field access ratios"
  - "IronLayoutKind enum and IRON_SOA_THRESHOLD configurable constant"
  - "Per-field SoA arrays in split collection structs"
  - "Common field detection across interface implementors"
  - "iron_layout_select(), iron_layout_get_kind(), iron_layout_get_common_fields() API"
affects: [48-03-layout-annotations, 48-04-benchmark-suite]

# Tech tracking
tech-stack:
  added: []
  patterns:
    - "SoA per-field arrays: <lower>_<field> naming for field-specific arrays in SplitList"
    - "Access ratio analysis: interprocedural field counting through method implementations"
    - "Object reconstruction from SoA: zero-init + per-field array reads"

key-files:
  created:
    - tests/integration/layout_soa_select.iron
    - tests/integration/layout_soa_select.expected
    - tests/integration/layout_common_field.iron
    - tests/integration/layout_common_field.expected
  modified:
    - src/lir/layout_analysis.h
    - src/lir/layout_analysis.c
    - src/lir/emit_c.c

key-decisions:
  - "Common field shared arrays disabled when any implementor uses SoA layout (per-type and shared array counts diverge)"
  - "SoA selection uses interprocedural field_used data combined with direct loop body field access counting"
  - "Arena-allocated keys for soa_types hash map to avoid stack-use-after-scope with stb_ds"

patterns-established:
  - "SoA struct emission: per-field arrays with shared count/cap per type"
  - "Layout decision lookup: iron_layout_get_kind(iface_mangled, type_name) for conditional emission paths"

requirements-completed: [LAYOUT-01, LAYOUT-03]

# Metrics
duration: 21min
completed: 2026-04-07
---

# Phase 48 Plan 02: SoA/AoS Layout Selection Summary

**Automatic SoA layout selection when loops access fewer than 50% of fields, with per-field contiguous arrays replacing struct arrays for cache-optimal iteration**

## Performance

- **Duration:** 21 min
- **Started:** 2026-04-07T21:59:09Z
- **Completed:** 2026-04-07T22:20:09Z
- **Tasks:** 2
- **Files modified:** 7

## Accomplishments
- Extended layout_analysis with per-loop field access ratio analysis and SoA/AoS layout decision infrastructure
- SoA struct generation: per-field arrays (e.g., `particle_x`, `bullet_x`) instead of struct arrays when access ratio < 50%
- Common field detection identifies fields shared across all alive implementors of an interface
- Full SoA emission pipeline: struct generation, push functions, loop reconstruction, free functions
- Two integration tests: SoA selection for partial-field access (1/4 fields), common field factoring for AoS

## Task Commits

Each task was committed atomically:

1. **Task 1: Extend layout analysis with SoA/AoS selection and common field detection** - `78ea639` (feat)
2. **Task 2: SoA struct generation, common field shared arrays, and integration tests** - `e682c6d` (feat)

## Files Created/Modified
- `src/lir/layout_analysis.h` - IronLayoutKind enum, CommonField struct, IRON_SOA_THRESHOLD, iron_layout_select/get_kind/get_common_fields API
- `src/lir/layout_analysis.c` - Per-loop access ratio computation, common field detection, layout_annotation_to_c helper
- `src/lir/emit_c.c` - SoA struct emission, SoA push functions, SoA loop reconstruction, SoA free, soa_types tracking map
- `tests/integration/layout_soa_select.iron` - 4-field objects with 1-field access loop (25% ratio -> SoA)
- `tests/integration/layout_soa_select.expected` - Expected: 18, done
- `tests/integration/layout_common_field.iron` - Shared id field across Cat/Dog implementors
- `tests/integration/layout_common_field.expected` - Expected: 6, done

## Decisions Made
- **Common fields disabled for SoA**: When any implementor of an interface uses SoA layout, common field shared arrays are suppressed. The per-type counts would diverge from the shared count, making index-based access incorrect. Common field factoring remains available for AoS-only interfaces.
- **Arena-allocated soa_types keys**: The stb_ds hash map stores pointers to string keys. Stack-allocated keys cause use-after-scope when the block exits. Fixed by arena-allocating the key strings.
- **SoA access ratio uses interprocedural data**: The access ratio counts fields accessed both directly in the loop body and through method calls (via the interprocedural field_used analysis from Plan 01).

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 1 - Bug] Stack-use-after-scope for soa_types hash map keys**
- **Found during:** Task 2 (first SoA test compilation)
- **Issue:** `char soa_key[768]` declared in block scope, pointer stored in stb_ds hash map, ASAN detected use-after-scope
- **Fix:** Arena-allocate key string via `iron_arena_strdup` before `shput`
- **Files modified:** src/lir/emit_c.c
- **Verification:** ASAN-clean compilation and execution
- **Committed in:** e682c6d (Task 2 commit)

**2. [Rule 1 - Bug] Common field shared arrays incompatible with SoA per-type indexing**
- **Found during:** Task 2 (SoA test produced wrong output: 25 instead of 18)
- **Issue:** Common field array (`entity_x`) indexed by per-type loop counter `_sp_i`, but entries are interleaved across types. Bullet at index 0, Particle at index 1 in shared array, but per-type loop expects sequential per-type data.
- **Fix:** Disabled common field shared arrays when any implementor uses SoA layout. Each type stores all its own fields in per-type arrays.
- **Files modified:** src/lir/emit_c.c
- **Verification:** SoA test output correct (18), all 253 integration tests pass
- **Committed in:** e682c6d (Task 2 commit)

---

**Total deviations:** 2 auto-fixed (2 bugs)
**Impact on plan:** Both fixes necessary for correctness. Common field factoring for SoA deferred to future work requiring per-type indexed shared arrays. No scope creep.

## Issues Encountered
None beyond the deviations documented above.

## User Setup Required
None - no external service configuration required.

## Next Phase Readiness
- SoA/AoS layout infrastructure complete for Plans 03 (layout annotations) and 04 (benchmark suite)
- Common field detection API available for AoS-only interfaces
- 253 integration tests passing, clean foundation for layout annotation parsing

## Self-Check: PASSED

All files verified present, all commits verified in git log.

---
*Phase: 48-layout-optimizations*
*Completed: 2026-04-07*
