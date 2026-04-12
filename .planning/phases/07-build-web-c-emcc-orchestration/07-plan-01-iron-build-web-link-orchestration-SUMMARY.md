---
phase: 07-build-web-c-emcc-orchestration
plan: 01
subsystem: cli
tags: [emscripten, emcc, wasm, web, posix_spawnp, build-orchestration]

# Dependency graph
requires:
  - phase: 06-emit-web-c-wrapper-medium-risk
    provides: emit_web_module() producing emitted C file; iron_build_web preflight pattern
  - phase: 04-wasm-safe-time-shim
    provides: iron_time_web.c (linked instead of iron_time.c on web target)
  - phase: 02-cli-toml-scaffold
    provides: IronWebConfig, IronBuildOpts.release, find_emcc, posix_spawnp pattern in build_web.c
provides:
  - iron_build_web_link(c_file_path, opts, cfg) in build_web.c — full emcc link orchestration
  - mkdir_p static helper for recursive dist/web/ creation
  - 12-flag canonical emcc argv (WEB-BUILD-03) hardcoded in IRON_WEB_CANONICAL_FLAGS[]
  - Release/debug flag branches in argv
  - 13 web-compatible source file paths linked (3 util + 5 runtime + 5 stdlib w/ iron_time_web.c)
affects: [07-plan-02 forbidden-flag-validation, 07-plan-03 build-c-dispatch, 07-plan-04 tests-ci]

# Tech tracking
tech-stack:
  added: []
  patterns:
    - "static const char *const[] for canonical flag set makes flags grep-auditable in diff"
    - "CWD-relative iron_src_dir='src' mirrors invoke_clang pattern in build.c"
    - "Plan 02 will insert forbidden-flag self-audit between argv construction and posix_spawnp"

key-files:
  created: []
  modified:
    - src/cli/build_web.c
    - src/cli/build_web.h

key-decisions:
  - "CWD-relative 'src' for iron_src_dir rather than get_iron_lib_dir() duplication — matches invoke_clang convention, CI-safe from repo root"
  - "IRON_WEB_SRC_COUNT=13 macro makes file count explicit — compiler catches inconsistency if list diverges"
  - "stdlib_i_flag (-Isrc/stdlib) added alongside -Isrc so stdlib headers resolve without path prefix"
  - "cfg cast to (void)cfg in Phase 7 — Phase 11 will wire IronWebConfig overrides"

patterns-established:
  - "Section H / Section I naming continues the lettered section pattern from Sections B-G"
  - "Canonical flags in static const array so Plan 02's is_forbidden_flag can walk same storage"

requirements-completed: [WEB-BUILD-01, WEB-BUILD-03, WEB-BUILD-04, WEB-BUILD-06]

# Metrics
duration: 509s
completed: 2026-04-12
---

# Phase 7 Plan 01: iron_build_web_link Orchestration Summary

**emcc link orchestration added to build_web.c: full canonical 12-flag argv construction with mkdir_p, release/debug branches, 13 web-safe source files, posix_spawnp invocation targeting dist/web/index.html**

## Performance

- **Duration:** ~9 min
- **Started:** 2026-04-12T00:54:18Z
- **Completed:** 2026-04-12T01:02:47Z
- **Tasks:** 1
- **Files modified:** 2

## Accomplishments
- Added `iron_build_web_link(c_file_path, opts, cfg)` to `build_web.c` (new Section I) and declared in `build_web.h`
- Added `mkdir_p` static helper (new Section H) for portable recursive `dist/web/` creation
- Hardcoded all 12 canonical emcc flags in `IRON_WEB_CANONICAL_FLAGS[]` array (grep-auditable)
- Release mode: `-Oz -flto -sASSERTIONS=0`; debug mode: `-O0 -g -sASSERTIONS=1`
- Links 13 source files (3 util + 5 runtime + 5 stdlib including `iron_time_web.c`, excluding `iron_time.c`, `iron_net_init.c`, `iron_net.c`)
- Pin discipline preserved (`4.0.23` absent); Windows `#error` guard preserved
- All 67 tests pass; phase2-invariant (pin discipline) and web smoke tests confirmed green

## Task Commits

1. **Task 1: mkdir_p helper + iron_build_web_link + canonical argv** - `8081e70` (feat)

**Plan metadata:** (to be added after state update commit)

## Files Created/Modified
- `src/cli/build_web.c` — Added Section H (`mkdir_p`) + Section I (`IRON_WEB_CANONICAL_FLAGS[]` + `iron_build_web_link`); 319 lines added
- `src/cli/build_web.h` — Added `#include "cli/web_config.h"` and `iron_build_web_link` prototype with full doc comment

## Decisions Made
- CWD-relative `"src"` for iron_src_dir instead of duplicating `get_iron_lib_dir()` — consistent with `invoke_clang`'s calling convention in `build.c`; CI runs from repo root
- Added `-Isrc/stdlib` alongside `-Isrc` to allow stdlib headers to resolve without `stdlib/` prefix in `#include` directives
- `(void)cfg` in function body to suppress unused-parameter warning — Phase 11 will apply IronWebConfig overrides when wiring user-facing flag control
- `IRON_WEB_SRC_COUNT=13` as a `#define` so the compiler rejects inconsistency between the count and the array initializer

## Deviations from Plan

None - plan executed exactly as written.

## Issues Encountered
None.

## User Setup Required
None - no external service configuration required.

## Next Phase Readiness
- `iron_build_web_link` is implemented and declared; callable but not yet dispatched from `iron_build()` (Plan 03 wires it)
- Plan 02 will add forbidden-flag self-audit (`is_forbidden_flag`) just before the `posix_spawnp` call — a comment placeholder is in place
- Phase 8 raylib linkage will add source files and flags to the argv; `max_argv=48` provides headroom

---
*Phase: 07-build-web-c-emcc-orchestration*
*Completed: 2026-04-12*
