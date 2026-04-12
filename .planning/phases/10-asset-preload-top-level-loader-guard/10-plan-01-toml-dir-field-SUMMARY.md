---
phase: 10-asset-preload-top-level-loader-guard
plan: 01
subsystem: toml
tags: [c, toml, iron-project, dirname, libgen, WEB-ASSET-04]

# Dependency graph
requires:
  - phase: 09-web-wiring-and-ci-smoke
    provides: IronProject struct and iron_toml_parse baseline from prior web TOML work
provides:
  - IronProject::toml_dir field (char*, heap, never NULL on success) populated by iron_toml_parse
  - Falls-back to "." for bare-filename paths with no slash component
  - iron_toml_free releases toml_dir
  - Two new unit tests covering both the absolute-path and bare-filename cases
affects: [10-plan-02, asset-path resolution, web-build, WEB-ASSET-04]

# Tech tracking
tech-stack:
  added: []
  patterns:
    - "strdup(path) -> dirname(copy) -> strdup(result) -> free(copy): POSIX-safe dirname pattern that never mutates caller's path"
    - "Sentinel fallback: if toml_dir allocation fails, strdup(\".\") ensures field is never NULL on success"

key-files:
  created: []
  modified:
    - src/cli/toml.h
    - src/cli/toml.c
    - tests/unit/test_toml_web.c

key-decisions:
  - "Used dirname() via <libgen.h> (already in project) over manual strrchr split — consistent with build_web.c pattern at line 250"
  - "toml_dir lives on IronProject, not IronWebConfig — it is a project-level concern (iron.toml location) not a [web]-section field"
  - "No mutation of caller's path argument: dirname operates on strdup copy; result is strdup'd again before storing on struct"
  - "Sentinel value is \".\" (not NULL) so downstream callers can unconditionally snprintf without NULL-check"

patterns-established:
  - "dirname safety pattern: always strdup input before dirname(), always strdup the result before freeing copy"
  - "free(proj->toml_dir) placed alongside free(proj->name) and other scalar frees in iron_toml_free"

requirements-completed: [WEB-ASSET-04]

# Metrics
duration: 15min
completed: 2026-04-12
---

# Phase 10 Plan 01: toml_dir Field Summary

**`IronProject` gains `char *toml_dir` populated via POSIX `dirname()` by `iron_toml_parse`, with `"."` fallback for bare-filename inputs and matching free in `iron_toml_free` — enabling Plan 02 to resolve `[web].assets` paths relative to iron.toml's location (WEB-ASSET-04)**

## Performance

- **Duration:** ~15 min
- **Started:** 2026-04-12T02:44:00Z
- **Completed:** 2026-04-12T02:59:41Z
- **Tasks:** 1 (TDD: RED + GREEN)
- **Files modified:** 3

## Accomplishments

- `IronProject` struct gains `char *toml_dir` with doc comment explaining lifecycle and WEB-ASSET-04 purpose
- `iron_toml_parse` computes directory via `dirname()` on a `strdup`'d copy so caller's `path` is never mutated; result is `strdup`'d again into `proj->toml_dir`
- Sentinel `strdup(".")` ensures `toml_dir` is never NULL on a successful parse, even when path has no slash component
- `iron_toml_free` releases `toml_dir` via `free(proj->toml_dir)` alongside the existing scalar frees
- `#include <libgen.h>` added to `toml.c` (consistent with `build_web.c` which already uses it)
- Two new unit tests: `test_toml_dir_is_populated` (absolute path into /tmp) and `test_toml_dir_relative_path` (bare filename, no slash -> ".")
- 34/34 unit tests pass (zero regressions)

## Task Commits

TDD task — two commits:

1. **RED — failing tests** - `e725db1` (test)
2. **GREEN — implementation** - `3a81ea0` (feat)

## Files Created/Modified

- `src/cli/toml.h` — `char *toml_dir` field appended to `IronProject` with doc comment
- `src/cli/toml.c` — `#include <libgen.h>`, `toml_dir` population in `iron_toml_parse`, `free(proj->toml_dir)` in `iron_toml_free`
- `tests/unit/test_toml_web.c` — two new test functions + `RUN_TEST` registrations

## Decisions Made

- **dirname() via libgen.h** over manual strrchr split — `dirname()` already used in `build_web.c` at line 250; consistent pattern is more maintainable.
- **toml_dir on IronProject, not IronWebConfig** — asset-path resolution is a project-level concern; `IronWebConfig` embeds by value and is not the right owner. Plan spec explicitly forbids touching `IronWebConfig`.
- **Sentinel "." instead of NULL** — downstream callers (Plan 02) can `snprintf("%s/%s", proj->toml_dir, relative)` unconditionally without a NULL-check guard.
- **Double-strdup pattern** — glibc's `dirname()` may mutate its input, so the input must be a copy; the output must then be re-`strdup`'d because dirname may return a pointer into the copy buffer (which gets freed).

## Deviations from Plan

None — plan executed exactly as written.

## Issues Encountered

None.

## User Setup Required

None — no external service configuration required.

## Next Phase Readiness

- `proj->toml_dir` is populated and freed correctly; Plan 02 (`build_web.c` asset-path resolution) can now consume it directly via `proj->toml_dir` after `iron_toml_parse`.
- No blockers.

---
*Phase: 10-asset-preload-top-level-loader-guard*
*Completed: 2026-04-12*
