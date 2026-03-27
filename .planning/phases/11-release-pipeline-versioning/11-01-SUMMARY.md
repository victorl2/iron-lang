---
phase: 11-release-pipeline-versioning
plan: 01
subsystem: infra
tags: [cmake, versioning, compile-time, build-system]

# Dependency graph
requires:
  - phase: 09-c-emission-and-cutover
    provides: Working iron CLI binary with build/run/check/fmt/test commands
provides:
  - CMakeLists.txt with project(iron VERSION 0.1.1 LANGUAGES C) and compile-time IRON_VERSION_STRING/IRON_GIT_HASH/IRON_BUILD_DATE defines
  - iron --version outputting "iron 0.1.1 (HASH DATE)" in rustc format
affects: [release-pipeline, ci]

# Tech tracking
tech-stack:
  added: []
  patterns: [CMake execute_process() for compile-time git hash capture, string(TIMESTAMP) for build date baking, #ifndef fallback guards for non-CMake builds]

key-files:
  created: []
  modified:
    - CMakeLists.txt
    - src/cli/main.c

key-decisions:
  - "Version baked at CMake configure time via execute_process(git rev-parse --short HEAD) — not runtime detection"
  - "PROJECT_VERSION flows from project(VERSION 0.1.1) to IRON_VERSION_STRING define — single source of truth"
  - "#ifndef fallback guards in main.c protect against non-CMake builds"

patterns-established:
  - "Compile-time version injection: CMake defines pass git hash and build date as string literals to C code"

requirements-completed: [REL-03, REL-04]

# Metrics
duration: 2min
completed: 2026-03-27
---

# Phase 11 Plan 01: Version Baking Summary

**CMake compile-time version injection: iron --version now outputs "iron 0.1.1 (abc1234 2026-03-27)" using git hash and build date baked at configure time**

## Performance

- **Duration:** 2 min
- **Started:** 2026-03-27T22:34:39Z
- **Completed:** 2026-03-27T22:36:55Z
- **Tasks:** 2
- **Files modified:** 2

## Accomplishments
- Version 0.1.1 defined once in CMakeLists.txt project() call, flows to binary via PROJECT_VERSION
- Git commit hash captured at CMake configure time via execute_process(git rev-parse --short HEAD)
- Build date captured at configure time via string(TIMESTAMP ... UTC)
- print_version() updated to rustc format: iron VERSION (HASH DATE)
- All 27 existing tests continue to pass

## Task Commits

Each task was committed atomically:

1. **Task 1: Add version, git hash, and build date to CMake build** - `c63eea0` (feat)
2. **Task 2: Update print_version() to use CMake-injected defines in rustc format** - `8458b47` (feat)

**Plan metadata:** (docs: complete plan — see below)

## Files Created/Modified
- `CMakeLists.txt` - project() updated to VERSION 0.1.1 LANGUAGES C; execute_process() for git hash; string(TIMESTAMP) for build date; target_compile_definitions extended with IRON_VERSION_STRING, IRON_GIT_HASH, IRON_BUILD_DATE
- `src/cli/main.c` - Removed hardcoded IRON_VERSION define; added #ifndef fallback guards; updated print_version() to printf("iron %s (%s %s)\n", ...) format

## Decisions Made
- Version baked at CMake configure time via execute_process(git rev-parse --short HEAD), not runtime detection — matches REL-04 requirement
- PROJECT_VERSION from project(VERSION 0.1.1) is the single source of truth for version string
- #ifndef fallback guards in main.c protect correctness if binary is ever compiled without CMake

## Deviations from Plan

None - plan executed exactly as written.

## Issues Encountered

None.

## User Setup Required

None - no external service configuration required.

## Next Phase Readiness
- Version infrastructure complete for release pipeline
- iron --version outputs correct rustc-format string with real git hash and build date
- All tests pass; no regressions introduced

---
*Phase: 11-release-pipeline-versioning*
*Completed: 2026-03-27*
