---
phase: 07-build-web-c-emcc-orchestration
plan: 02
subsystem: cli
tags: [emscripten, wasm, build-system, safety-guard, flag-validation]

# Dependency graph
requires:
  - phase: 07-build-web-c-emcc-orchestration
    provides: iron_build_web_link with full argv construction (plan 01)
provides:
  - IRON_WEB_FORBIDDEN_FLAGS array with 9 verbatim forbidden substrings
  - is_forbidden_flag() static helper (NULL-safe, strstr-based linear scan)
  - Self-audit loop inside iron_build_web_link before posix_spawnp
affects:
  - 07-plan-03-build-c-dispatch
  - 07-plan-04-tests-ci-fixture
  - 07-plan-11-output-layout-overrides

# Tech tracking
tech-stack:
  added: []
  patterns:
    - Forbidden-flag substring guard pattern: strstr-based linear scan against static const array inserted before posix_spawnp in argv-building function

key-files:
  created: []
  modified:
    - src/cli/build_web.c

key-decisions:
  - "strstr substring match used (not strcmp) so -sMODULARIZE catches MODULARIZE, handles both -s<NAME> and -f<flag> forms uniformly"
  - "ASYNCIFY=1 (not bare ASYNCIFY) as forbidden substring preserves -sASYNCIFY=0 canonical flag without false positives"
  - "Cleanup on forbidden-flag hit mirrors Plan 01 error paths (free stdlib_i_flag, src_i_flag, abs_paths[], emcc_path)"

patterns-established:
  - "Forbidden-flag audit: walk final argv with is_forbidden_flag() BEFORE posix_spawnp; emit named diagnostic and return 1 without spawning"

requirements-completed:
  - WEB-BUILD-07
  - WEB-BUILD-08

# Metrics
duration: 12min
completed: 2026-04-12
---

# Phase 7 Plan 02: Forbidden Flag Guard Summary

**strstr-based forbidden-flag self-audit in iron_build_web_link rejects 9 dangerous emcc flag substrings before posix_spawnp, with named diagnostic and cleanup**

## Performance

- **Duration:** 12 min
- **Started:** 2026-04-12T01:05:23Z
- **Completed:** 2026-04-12T01:17:24Z
- **Tasks:** 1
- **Files modified:** 1

## Accomplishments
- Added `IRON_WEB_FORBIDDEN_FLAGS[]` const array with all 9 verbatim forbidden substrings (WEB-BUILD-07)
- Added `is_forbidden_flag()` static helper — NULL-safe, strstr linear scan, returns matched entry pointer or NULL
- Inserted self-audit loop in `iron_build_web_link` between argv completion and `posix_spawnp` — forbidden hit emits clear diagnostic naming the flag and returns 1 without spawning (WEB-BUILD-08)
- All 67 existing tests pass; Plan 01's canonical flags (including `-sASYNCIFY=0`) produce no false positives

## Task Commits

Each task was committed atomically:

1. **Task 1: Add forbidden-flag array + is_forbidden_flag helper + self-audit walk** - `4c30441` (feat)

**Plan metadata:** (docs commit follows)

## Files Created/Modified
- `src/cli/build_web.c` — Added Section I.2 with forbidden flags array, is_forbidden_flag() helper, and self-audit loop before posix_spawnp

## Decisions Made
- Used `strstr` (not `strcmp`) for substring matching so `-sMODULARIZE` catches forbidden `MODULARIZE` and `-fwasm-exceptions` is caught exactly — one uniform rule for both `-s<NAME>` and `-f<flag>` argument forms
- Forbidden entry is `ASYNCIFY=1` (value-included) not bare `ASYNCIFY` — ensures canonical `-sASYNCIFY=0` passes audit since `strstr("-sASYNCIFY=0", "ASYNCIFY=1")` returns NULL (trailing char differs)
- Cleanup block on forbidden-flag rejection mirrors existing Plan 01 error paths: `free(stdlib_i_flag)`, `free(src_i_flag)`, loop-free `abs_paths[]`, `free(emcc_path)`, return 1

## Deviations from Plan

None - plan executed exactly as written.

## Issues Encountered
None

## User Setup Required
None - no external service configuration required.

## Next Phase Readiness
- Forbidden-flag guard is live in `iron_build_web_link`
- Plan 03 (build.c dispatch) can wire `iron_build_web_link` into the build pipeline knowing the guard is in place
- Plan 04 (tests/CI/fixture) should add a unit test that passes a forbidden flag and verifies the diagnostic + return 1

---
*Phase: 07-build-web-c-emcc-orchestration*
*Completed: 2026-04-12*
