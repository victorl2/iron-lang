---
phase: 38-concurrency-safety
plan: 02
subsystem: analyzer
tags: [concurrency, spawn, capture-analysis, data-race, warning]

# Dependency graph
requires:
  - phase: 38-concurrency-safety/01
    provides: expr_ident_name helper for root identifier extraction from field/index chains
provides:
  - Spawn block capture analysis detecting mutable outer-variable writes
  - IRON_WARN_SPAWN_DATA_RACE (604) warning for spawn data races
  - collect_spawn_refs recursive AST walker for spawn bodies
affects: [39-diagnostic-test-sweep]

# Tech tracking
tech-stack:
  added: []
  patterns: [spawn body capture analysis with bounded stack-allocated tracking (MAX_SPAWN_CAPTURES=64)]

key-files:
  created: []
  modified: [src/analyzer/concurrency.c, src/diagnostics/diagnostics.h, tests/unit/test_concurrency.c]

key-decisions:
  - "Reused collect_local_names + collect_spawn_refs two-pass approach: first collect locals, then walk for outer refs -- avoids false positives on spawn-local variables"
  - "Bounded capture tracking (MAX_SPAWN_CAPTURES=64) prevents unbounded allocation in pathological spawn bodies"
  - "Used has_warning() with level check for test assertions, separate from has_error() -- cleaner semantic distinction for warning vs error codes"

patterns-established:
  - "Spawn analysis pattern: save ctx state -> collect_local_names -> collect_spawn_refs -> emit warnings -> restore state (mirrors parallel-for pattern)"

requirements-completed: [CONC-03, CONC-04, CONC-05]

# Metrics
duration: 5min
completed: 2026-04-03
---

# Phase 38 Plan 02: Spawn Capture Analysis and Race Detection Summary

**Spawn block mutable-capture detection via collect_spawn_refs walker, emitting IRON_WARN_SPAWN_DATA_RACE for outer variable writes through bare idents, field access, and index expressions**

## Performance

- **Duration:** 5 min
- **Started:** 2026-04-03T11:24:08Z
- **Completed:** 2026-04-03T11:29:58Z
- **Tasks:** 2
- **Files modified:** 3

## Accomplishments
- Spawn blocks that write to outer variables now emit IRON_WARN_SPAWN_DATA_RACE (W0604)
- Field access (obj.x = val) and index (arr[0] = val) writes in spawn bodies detected via expr_ident_name from Plan 01
- Read-only captures and local-only mutations produce no false positives
- 5 new spawn-specific tests pass, all 14 concurrency tests pass, full 18-test unit suite zero regressions

## Task Commits

Each task was committed atomically:

1. **Task 1: Add spawn capture analysis and race detection** - `fb29514` (feat)
2. **Task 2: Unit tests for spawn capture analysis and race detection** - `e32df26` (test)

## Files Created/Modified
- `src/diagnostics/diagnostics.h` - Added IRON_WARN_SPAWN_DATA_RACE (604) warning code
- `src/analyzer/concurrency.c` - Extended ConcurrencyCtx with spawn state, added collect_spawn_refs walker, added IRON_NODE_SPAWN case in walk_stmt, added emit_warn helper
- `tests/unit/test_concurrency.c` - Added make_spawn_stmt builder, has_warning helper, 5 new test functions

## Decisions Made
- Reused collect_local_names + collect_spawn_refs two-pass approach: first collect local declarations from spawn body, then walk for outer variable references -- avoids false positives on spawn-local variables
- Bounded capture tracking (MAX_SPAWN_CAPTURES=64) prevents unbounded allocation in pathological spawn bodies
- Added emit_warn helper separate from emit_err to use IRON_DIAG_WARNING severity for spawn data races (spawn races are warnings, not errors, since they are potential rather than definite)

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 1 - Bug] Fixed has_warning field name**
- **Found during:** Task 2
- **Issue:** Used `.severity` field name but Iron_Diagnostic struct uses `.level` for diagnostic severity
- **Fix:** Changed `g_diags.items[i].severity` to `g_diags.items[i].level`
- **Files modified:** tests/unit/test_concurrency.c
- **Committed in:** e32df26 (part of Task 2 commit)

---

**Total deviations:** 1 auto-fixed (1 bug)
**Impact on plan:** Trivial field name correction. No scope creep.

## Issues Encountered
None

## User Setup Required
None - no external service configuration required.

## Next Phase Readiness
- Phase 38 (Concurrency Safety) fully complete: parallel-for mutation + spawn capture analysis
- Ready for Phase 39 diagnostic test sweep
- All concurrency diagnostics (E0208, W0604) operational with comprehensive tests

## Self-Check: PASSED

- All 3 modified files exist on disk
- Commit fb29514 (Task 1) verified in git log
- Commit e32df26 (Task 2) verified in git log
- 14/14 concurrency tests pass, 18/18 unit tests pass

---
*Phase: 38-concurrency-safety*
*Completed: 2026-04-03*
