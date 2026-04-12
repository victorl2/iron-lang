---
phase: 02-cli-toml-scaffold
plan: "01"
subsystem: cli
tags: [c, headers, structs, types, web-config, toml, build-opts]

# Dependency graph
requires:
  - phase: 01-bootstrap-guardrails
    provides: .emsdk-version pin and CI scaffold; no source types needed
provides:
  - IronWebConfig struct with 7 fields (assets/asset_count/title/shell/initial_memory/stack_size/pthread_pool_size) in src/cli/web_config.h
  - IRON_WEB_DEFAULT_* #defines (128 MiB initial_memory, 1 MiB stack_size, pool_size=4)
  - IronBuildTarget enum (IRON_TARGET_NATIVE=0, IRON_TARGET_WEB=1) in src/cli/build.h
  - IronBuildOpts.target and IronBuildOpts.release fields
  - IronProject.web embedded value field (IronWebConfig) in src/cli/toml.h
affects:
  - 02-plan-02 (TOML parser reads IronWebConfig fields)
  - 02-plan-03 (default #defines consumed for emcc flag construction)
  - 02-plan-04 (main.c flag parsing writes target/release onto IronBuildOpts)
  - 02-plan-05 (CMake wires new sources; build.c dispatch uses IronBuildTarget)
  - phase-07 (build_web.c consumes IronBuildOpts.release for emcc flag set)

# Tech tracking
tech-stack:
  added: []
  patterns:
    - "Header-only type declaration: new web types live in dedicated src/cli/web_config.h with no .c counterpart"
    - "Append-only struct extension: new fields appended at end of IronBuildOpts so designated-initializer callers zero-init defaults"
    - "Value embedding: IronWebConfig web embedded by value on IronProject; lifetime follows IronProject; calloc zero-init means NULL/0 == not-set"
    - "Pin discipline: version strings live exclusively in .emsdk-version; never hardcoded in source files"

key-files:
  created:
    - src/cli/web_config.h
  modified:
    - src/cli/build.h
    - src/cli/toml.h

key-decisions:
  - "IronWebConfig is a value field on IronProject (not pointer); zero-init from calloc provides NULL/0 == not-set semantics without extra allocation"
  - "char **assets + int asset_count chosen over flexible array member for compatibility with existing int-count pattern in IronProject"
  - "int used for numeric fields (initial_memory, stack_size, pthread_pool_size) matching IronProject.dep_count plain-int style, not size_t"
  - "IronBuildTarget enum literals 0/1 made explicit to guarantee downstream grep invariant from plan acceptance criteria"

patterns-established:
  - "Include path pattern: cli/web_config.h (not just web_config.h) mirrors how build.c uses cli/toml.h"
  - "Default constant naming: IRON_WEB_DEFAULT_<FIELDNAME> in web_config.h as single source of truth for numeric defaults"

requirements-completed:
  - WEB-MANIFEST-01

# Metrics
duration: 2min
completed: 2026-04-11
---

# Phase 2 Plan 01: Type Foundations Summary

**IronWebConfig struct (7 fields, 3 default #defines), IronBuildTarget enum (NATIVE/WEB), and IronProject.web value embedding — pure header declarations, zero runtime changes, project builds clean**

## Performance

- **Duration:** ~2 min
- **Started:** 2026-04-11T15:19:09Z
- **Completed:** 2026-04-11T15:21:25Z
- **Tasks:** 3
- **Files modified:** 3 (1 created, 2 extended)

## Accomplishments

- New `src/cli/web_config.h` declares `IronWebConfig` with all 6 manifest fields plus `asset_count`, and the three `IRON_WEB_DEFAULT_*` #defines
- `src/cli/build.h` gains `IronBuildTarget` enum (NATIVE=0, WEB=1) and two new fields (`target`, `release`) appended at end of `IronBuildOpts`; all 11 existing fields untouched
- `src/cli/toml.h` includes `cli/web_config.h` and embeds `IronWebConfig web;` by value at end of `IronProject`; function prototypes and `IronDep` struct unchanged
- Full CMake build passes cleanly (100% targets built, no errors)

## Task Commits

Each task was committed atomically:

1. **Task 1: Create src/cli/web_config.h** - `a8a2384` (feat)
2. **Task 2: Extend IronBuildOpts with target enum + release** - `57a3764` (feat)
3. **Task 3: Embed IronWebConfig on IronProject** - `ac6519a` (feat)

## Files Created/Modified

- `src/cli/web_config.h` — New header: IronWebConfig struct + IRON_WEB_DEFAULT_* #defines (27 lines)
- `src/cli/build.h` — Extended: IronBuildTarget enum + target/release fields appended to IronBuildOpts
- `src/cli/toml.h` — Extended: #include "cli/web_config.h" + IronWebConfig web; field on IronProject

## Decisions Made

- `IronWebConfig` embedded by value (not pointer) on `IronProject` — follows CONTEXT.md rationale: lifetime follows parent struct, calloc zero-init gives correct NULL/0 defaults for all optional fields
- `int` used for `initial_memory`, `stack_size`, `pthread_pool_size` — matches existing `dep_count`/`dep_capacity` plain-int style in the codebase
- Explicit `= 0` and `= 1` on enum literals to satisfy plan's verbatim grep acceptance criteria for downstream plans

## Deviations from Plan

None - plan executed exactly as written.

## Issues Encountered

Minor: macOS system `grep` does not support `-P` (PCRE) flag; acceptance criteria checks that used `grep -Pq` were adapted to `grep -Eq` with POSIX character classes. The file contents fully satisfy all acceptance criteria.

## User Setup Required

None - no external service configuration required.

## Next Phase Readiness

- All three Phase 2 downstream plans (02, 03, 04) can now reference `IronWebConfig`, `IronBuildTarget`, `IronBuildOpts.target`, `IronBuildOpts.release`, and `IronProject.web` directly without reading CONTEXT.md for the struct shapes
- Pin discipline maintained: `! grep -r "4.0.23" src/cli/` passes cleanly

---
*Phase: 02-cli-toml-scaffold*
*Completed: 2026-04-11*
