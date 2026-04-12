---
phase: 07-build-web-c-emcc-orchestration
plan: 03
subsystem: cli
tags: [emcc, emrun, web-build, posix_spawnp, iron_build_web_link, run-after]

# Dependency graph
requires:
  - phase: 07-build-web-c-emcc-orchestration plan-01
    provides: iron_build_web_link() in build_web.c — emcc orchestration function
  - phase: 07-build-web-c-emcc-orchestration plan-02
    provides: is_forbidden_flag() self-audit in build_web.c — canonical flag validation
provides:
  - "iron_build() step 13 dispatches to iron_build_web_link when target == IRON_TARGET_WEB"
  - "iron run --target=web spawns emrun dist/web/index.html via posix_spawnp; friendly ENOENT fallback"
  - "Phase 7 end-to-end web pipeline fully wired: preflight (plan-06) + emit (plan-06) + link (plan-01) + dispatch (plan-03)"
affects:
  - 07-plan-04-ci-integration
  - phases-08-onward-raylib-shell-assets

# Tech tracking
tech-stack:
  added: []
  patterns:
    - "Target-branched dispatch at linker step: if (opts.target == IRON_TARGET_WEB) { iron_build_web_link } else { invoke_clang }"
    - "posix_spawnp (PATH search) for emrun vs posix_spawn (direct path) for native binary — deliberate distinction"
    - "Return 0 on emrun ENOENT: build success != run success; graceful fallback prints manual instruction"

key-files:
  created: []
  modified:
    - src/cli/build.c

key-decisions:
  - "cfg=NULL passed to iron_build_web_link: Phase 7 hardcodes canonical flags; [web] config overrides deferred to Phase 11"
  - "posix_spawnp used for emrun (PATH-resolved) vs posix_spawn for native binary (has explicit path)"
  - "return 0 on emrun not found: build artifact exists on disk; run is best-effort affordance, not a build requirement"
  - "Web emrun branch returns early before native posix_spawn block; no code duplication, clean separation"

patterns-established:
  - "Target-branched run-after: web branch returns early, native falls through to existing code"

requirements-completed: [WEB-BUILD-01]

# Metrics
duration: 11min
completed: 2026-04-12
---

# Phase 7 Plan 03: Dispatch and Emrun Summary

**`iron_build_web_link` wired into build.c step 13 via target-branched dispatch, and `iron run --target=web` added with `emrun` via posix_spawnp and graceful PATH-miss fallback**

## Performance

- **Duration:** 11 min
- **Started:** 2026-04-12T01:19:13Z
- **Completed:** 2026-04-12T01:30:26Z
- **Tasks:** 1
- **Files modified:** 1

## Accomplishments
- Step 13 in `iron_build()` now branches on `opts.target`: web path calls `iron_build_web_link(c_file_path, opts, NULL)`, native path calls `invoke_clang` unchanged
- `iron run --target=web` spawns `emrun dist/web/index.html` via `posix_spawnp`; if emrun is not on PATH, prints a friendly note and returns 0 (build succeeded)
- Phase 7 end-to-end pipeline is fully closed: preflight (Phase 6) → emit_web_module (Phase 6) → write_temp_c → iron_build_web_link (Plan 01) → emrun (this plan)
- All 67 ctest targets (100%) green; web smoke test still accepts emcc-found/emcc-not-found both as passing

## Task Commits

Each task was committed atomically:

1. **Task 1: Dispatch iron_build_web_link at step 13 + add iron run --target=web emrun flow** - `7cccfec` (feat)

**Plan metadata:** (docs commit follows)

## Files Created/Modified
- `src/cli/build.c` - Two new target-branched decision points: step 13 link dispatch + step 16 emrun run-after

## Decisions Made
- **cfg=NULL:** Phase 7 canonical flags are hardcoded in `iron_build_web_link`; no `iron.toml` `[web]` overrides consumed at link time. Phase 11 will plumb parsed `IronWebConfig` through when overrides matter.
- **posix_spawnp for emrun:** `emrun` is PATH-resolved (same as `emcc`). Native binary branch uses `posix_spawn` with an explicit `./`-prefixed path; deliberately different.
- **return 0 on emrun ENOENT:** The build artifacts (`dist/web/index.{html,js,wasm}`) exist. `iron run` is an affordance; returning 1 would incorrectly signal build failure to CI/scripts.

## Deviations from Plan

None — plan executed exactly as written. Both edits matched the plan's code snippets precisely; `#include "cli/build_web.h"` and `extern char **environ` were already present from prior phases.

## Issues Encountered

None.

## User Setup Required

None — no external service configuration required.

## Next Phase Readiness

- Phase 7 plan 04 (CI integration) can now validate the full `iron build --target=web` end-to-end flow in CI
- `iron_build_web_link` is no longer dead code — it is reachable via `iron build --target=web <source>`
- Raylib linkage (Phase 8) can extend `iron_build_web_link`'s argv construction without touching `build.c`

---
*Phase: 07-build-web-c-emcc-orchestration*
*Completed: 2026-04-12*
