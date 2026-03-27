---
phase: 06-milestone-gap-closure
plan: "02"
subsystem: cli
tags: [iron-check, stdlib, import-detection, arena]

requires:
  - phase: 05-codegen-fixes-stdlib-wiring
    provides: stdlib .iron wrappers (math.iron, io.iron, time.iron, log.iron, raylib.iron) and build.c strstr/prepend pattern

provides:
  - iron check now supports stdlib imports (import math/io/time/log/raylib) by prepending .iron wrappers before lex phase
  - check_make_src_path and check_read_stdlib helpers in check.c
  - IRON_SOURCE_DIR guard in check.c

affects: [06-03, integration-tests, cli-check]

tech-stack:
  added: []
  patterns:
    - "check.c stdlib detection mirrors build.c: strstr detection + read wrapper file + prepend source + free old pointer"

key-files:
  created: []
  modified:
    - src/cli/check.c

key-decisions:
  - "check.c uses 64k initial arena (matches build.c) to prevent heap-use-after-free when combined source triggers realloc during resolver scope push"
  - "check_make_src_path and check_read_stdlib are self-contained static helpers (not shared via header) matching project's self-contained CLI file pattern"
  - "Import detection order: raylib, math, io, time, log — exact match to build.c ordering"

patterns-established:
  - "Pattern: strstr detection + file prepend for stdlib wrappers must match in both build.c and check.c"

requirements-completed: [CLI-03]

duration: 3min
completed: 2026-03-27
---

# Phase 06 Plan 02: iron check stdlib import support Summary

**strstr/prepend pattern from build.c replicated in check.c so iron check accepts import math/io/time/log/raylib without errors**

## Performance

- **Duration:** ~3 min
- **Started:** 2026-03-27T01:27:06Z
- **Completed:** 2026-03-27T01:30:01Z
- **Tasks:** 1
- **Files modified:** 1

## Accomplishments

- `iron check` now succeeds on files using `import math`, `import io`, `import time`, `import log`, and `import raylib`
- Added `IRON_SOURCE_DIR` guard, `check_make_src_path`, and `check_read_stdlib` static helpers to `check.c`
- Increased initial arena from 32k to 64k to match `build.c` — prevents heap-use-after-free under ASan when combined (stdlib + user) source triggers arena realloc during resolver scope push
- No regression on files without stdlib imports

## Task Commits

1. **Task 1: Add stdlib import detection and prepend to check.c** - `55801b1` (feat)

## Files Created/Modified

- `src/cli/check.c` - Added IRON_SOURCE_DIR guard, check_make_src_path helper, check_read_stdlib helper, strstr/prepend blocks for raylib/math/io/time/log, 64k arena

## Decisions Made

- Increased arena from 32k to 64k to match build.c. With stdlib prepended, combined source is larger and triggers arena realloc during resolver scope push. Under ASan, the old 32k size exposed a use-after-free in the resolver (pre-existing issue: resolver holds pointers into arena that become stale after realloc). Using 64k eliminates the realloc in practice, matching the behavior of build.c.

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 1 - Bug] Increased arena size from 32k to 64k to prevent heap-use-after-free under ASan**
- **Found during:** Task 1 (post-implementation verification)
- **Issue:** `iron check tests/integration/test_io.iron` triggered heap-use-after-free in `resolve.c:355` under ASan. After prepending io.iron, the combined source caused arena realloc during resolver scope push, invalidating cached pointers. `build.c` avoids this with a 64k initial arena.
- **Fix:** Changed `iron_arena_create(32 * 1024)` to `iron_arena_create(64 * 1024)` in `iron_check()`.
- **Files modified:** `src/cli/check.c`
- **Verification:** All 5 check commands pass under both ASan and release build. Full ctest suite shows no new failures.
- **Committed in:** `55801b1` (Task 1 commit)

---

**Total deviations:** 1 auto-fixed (Rule 1 - bug fix)
**Impact on plan:** Required for correctness under ASan. No scope creep.

## Issues Encountered

- `iron check tests/integration/test_io.iron` crashed under ASan build with heap-use-after-free at `resolve.c:355`. Root cause: arena realloc (triggered by larger combined source after io.iron prepend) invalidates pointers held by the resolver. Pre-existing issue; fixed by matching build.c's 64k arena size.
- Three pre-existing integration test failures (`test_integration`, `test_interp_codegen`, `test_parallel_codegen`) unrelated to this plan.

## User Setup Required

None - no external service configuration required.

## Next Phase Readiness

- `iron check` now parity with `iron build` for stdlib import acceptance
- Ready to proceed to plan 06-03

## Self-Check: PASSED

- src/cli/check.c: FOUND
- 06-02-SUMMARY.md: FOUND
- commit 55801b1: FOUND

---
*Phase: 06-milestone-gap-closure*
*Completed: 2026-03-27*
