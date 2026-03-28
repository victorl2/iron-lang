---
phase: 12-binary-split-and-installation
plan: "02"
subsystem: cli
tags: [cmake, c, binary, subprocess, posix-spawn, ironc]

# Dependency graph
requires:
  - phase: 12-01
    provides: ironc binary as separate CMake target that iron discovers via sibling detection
provides:
  - iron package manager binary (src/pkg/main.c, build/iron)
  - find_ironc() sibling-binary discovery via platform-native self-path resolution
  - forward_to_ironc() using posix_spawnp (Unix) / CreateProcessA (Windows)
  - Cargo-style help output with Package Commands grouping
  - iron.toml package mode stub (returns helpful error; Phase 13 implements full support)
affects:
  - 12-03 (installation plan: tarball must include both iron and ironc in bin/)
  - 13 (iron.toml package mode to be implemented in iron binary)
  - 14 (any CI/release workflow referencing iron binary)

# Tech tracking
tech-stack:
  added: []
  patterns:
    - "Sibling-binary discovery: iron resolves own executable path via NSGetExecutablePath/readlink/GetModuleFileName, replaces 'iron' with 'ironc' in filename, falls back to PATH if sibling not found"
    - "Clean process boundary: iron binary has no compile-time link to iron_compiler — spawns ironc as subprocess"
    - "Cargo-style help: iron (no args) exits 0 with grouped help; ironc (no args) exits 1 with usage — intentional difference"

key-files:
  created:
    - src/pkg/main.c
  modified:
    - CMakeLists.txt

key-decisions:
  - "iron exits 0 for bare invocation (Cargo-style), unlike ironc which exits 1 — iron is user-facing, ironc is developer-facing"
  - "has_iron_file_arg() checks argv[2..] not argv[1..] — first arg is the subcommand (build/run/check/fmt/test)"
  - "No deprecation warning on iron build hello.iron — silent forwarding only, per CONTEXT.md backward compat decision"

patterns-established:
  - "iron CLI dispatch: global flags first, then subcommand routing, then file-arg check, then package-mode stub"
  - "Sibling binary detection: resolve_self_path() returns full path, caller replaces filename component"

requirements-completed:
  - CLI2-02
  - CLI2-03

# Metrics
duration: 12min
completed: "2026-03-27"
---

# Phase 12 Plan 02: iron Package Manager CLI Binary Summary

**Standalone iron binary with Cargo-style help, sibling ironc discovery via posix_spawnp, and silent single-file forwarding — 25 integration tests pass through iron -> ironc**

## Performance

- **Duration:** ~12 min
- **Started:** 2026-03-27
- **Completed:** 2026-03-27
- **Tasks:** 2
- **Files modified:** 2

## Accomplishments

- `src/pkg/main.c`: New standalone iron binary — finds ironc as sibling, forwards all .iron file subcommand invocations transparently
- `CMakeLists.txt`: `add_executable(iron src/pkg/main.c)` with no link to iron_compiler — clean process boundary maintained
- All 25 integration tests pass through `iron` -> `ironc` forwarding
- All 30 CTest tests pass (28 existing + 2 new: test_iron_help, test_iron_version)

## Task Commits

Each task was committed atomically:

1. **Task 1 + 2: Create iron package manager CLI binary and add CMake target** - `1f9d9ae` (feat)

**Plan metadata:** (docs commit follows)

## Files Created/Modified

- `src/pkg/main.c` - iron entry point: resolve_self_path(), find_ironc(), has_iron_file_arg(), forward_to_ironc(), print_help(), main()
- `CMakeLists.txt` - add_executable(iron), test_iron_help and test_iron_version CTest smoke tests added

## Decisions Made

- `iron` (no args) exits 0 with Cargo-style help — different from `ironc` (no args) which exits 1. iron is the user-facing tool, ironc is the raw compiler.
- `has_iron_file_arg()` starts from `argv[2]` not `argv[1]` because `argv[1]` is always the subcommand name (build/run/check/fmt/test)
- Tasks 1 and 2 collapsed into a single commit since the CMakeLists.txt iron target was required to build and verify the Task 1 acceptance criteria

## Deviations from Plan

None - plan executed exactly as written.

## Issues Encountered

None.

## Next Phase Readiness

- `iron` binary exists at `build/iron` and forwards single-file commands to `ironc` for all subcommands
- All integration tests verified against `iron` binary (25 passed, 0 failed)
- `iron.toml` package mode stub is in place — Phase 13 will implement full `iron init` and project workflow
- Installation plan (Phase 12-03 or 13) must place both `iron` and `ironc` in `~/.iron/bin/`

## Self-Check: PASSED

- `src/pkg/main.c` exists with `find_ironc`, `has_iron_file_arg`, `forward_to_ironc`, `posix_spawnp`, `CreateProcessA`, `The Iron package manager`, `See also: ironc`, `no iron.toml found`
- `build/iron` binary exists and functional
- `CMakeLists.txt` contains `add_executable(iron` with `src/pkg/main.c`
- `CMakeLists.txt` does NOT contain `target_link_libraries(iron iron_compiler`
- `CMakeLists.txt` contains `NAME test_iron_help` and `NAME test_iron_version`
- Commit `1f9d9ae` exists (Task 1+2 feat commit)
- `./build/iron --version` outputs `iron 0.0.2 (15e412f 2026-03-28)` — starts with `iron 0.0.`
- `./build/iron --help` contains `The Iron package manager`
- `./build/iron` (no args) shows `Package Commands:`
- `./build/iron build tests/integration/hello.iron` exits 0
- `./build/iron run tests/integration/hello.iron` exits 0
- `./build/iron build` (no file) exits 1 with `iron.toml` in stderr
- `ctest -R test_iron_help` and `ctest -R test_iron_version` both PASS
- Integration tests via `iron`: 25 passed, 0 failed

---
*Phase: 12-binary-split-and-installation*
*Completed: 2026-03-27*
