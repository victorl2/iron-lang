---
phase: 04-comptime-game-dev-and-cross-platform
plan: 05
subsystem: infra
tags: [windows, cross-platform, threading, pthreads, c11-threads, ci, github-actions, cmake]

# Dependency graph
requires:
  - phase: 04-comptime-game-dev-and-cross-platform
    provides: runtime threading (Iron_Pool, Iron_Handle, Iron_Channel, Iron_Mutex) and build pipeline (invoke_clang with posix_spawn)
provides:
  - Platform-abstracted threading macros (IRON_MUTEX_INIT/LOCK/UNLOCK, IRON_THREAD_CREATE/JOIN, IRON_COND_WAIT/SIGNAL/BROADCAST)
  - Windows-compatible build pipeline using clang-cl and CreateProcess
  - .exe extension on Windows in derive_output_name
  - Three-platform CI matrix (ubuntu-latest, macos-latest, windows-latest)
  - CMakeLists.txt pthread linking and integration test guards for NOT WIN32
affects: [future Windows users, CI pipeline, runtime porting]

# Tech tracking
tech-stack:
  added: [C11 threads (threads.h) for Windows, GitHub Actions windows-latest runner, clang-cl]
  patterns: [IRON_MUTEX/COND/THREAD macros abstract pthreads vs C11 threads; #ifdef _WIN32 / #else / #endif wrapping preserves Unix behavior]

key-files:
  created: []
  modified:
    - src/runtime/iron_runtime.h
    - src/runtime/iron_threads.c
    - src/cli/build.c
    - .github/workflows/ci.yml
    - CMakeLists.txt

key-decisions:
  - "Threading abstraction uses iron_thread_t/iron_mutex_t/iron_cond_t typedefs + IRON_* macros; Unix structs map to pthread types, Windows to C11 threads.h types"
  - "build.c Windows path uses clang-cl + /std:c11 + /Fe<output> + CreateProcess rather than posix_spawnp"
  - "Windows GetTempPath/GetTempFileName replaces mkstemps for temp C file creation"
  - "Integration tests skipped on Windows in both CMakeLists.txt and CI (shell-script based)"
  - "fail-fast: false in CI matrix so all three platforms report independently"
  - "pthread link guard in CMakeLists.txt: target_link_libraries conditional on NOT WIN32"

patterns-established:
  - "Platform guard pattern: #ifdef _WIN32 ... #else ... #endif wrapping all OS-specific code"
  - "Iron_* threading types stay unchanged; only the underlying typedef and macro implementation switches"

requirements-completed: [GAME-04, RT-08]

# Metrics
duration: 6min
completed: 2026-03-26
---

# Phase 4 Plan 5: Windows Cross-Platform Parity Summary

**Platform-abstracted threading (pthreads/C11 threads) with clang-cl build pipeline and three-platform GitHub Actions CI**

## Performance

- **Duration:** ~6 min
- **Started:** 2026-03-26T17:08:40Z
- **Completed:** 2026-03-26T17:14:55Z
- **Tasks:** 2
- **Files modified:** 5

## Accomplishments
- Replaced raw pthread types in iron_runtime.h with iron_thread_t/iron_mutex_t/iron_cond_t and IRON_MUTEX/COND/THREAD macros — on Unix maps to pthreads, on Windows to C11 threads.h
- Converted all pthread calls in iron_threads.c (Iron_Pool, Iron_Handle, Iron_Channel, Iron_Mutex, Lock/CondVar) to use abstraction macros exclusively
- Added Windows code path in build.c: clang-cl invocation via CreateProcess, .exe extension in derive_output_name, Windows temp file via GetTempPath/GetTempFileName
- Extended CI from ubuntu+macOS to three-platform matrix (ubuntu-latest, macos-latest, windows-latest) with fail-fast: false
- Updated CMakeLists.txt to conditionally link pthread and skip integration tests on Windows

## Task Commits

1. **Task 1: Threading abstraction macros and Windows-compatible build pipeline** - `9ef5136` (feat)
2. **Task 2: Three-platform CI matrix and CMake Windows adjustments** - `ec394a9` (feat)

## Files Created/Modified
- `src/runtime/iron_runtime.h` - Added IRON_* threading macros block; updated Iron_Handle, Iron_Mutex, Iron_Lock, Iron_CondVar to use abstract types
- `src/runtime/iron_threads.c` - Replaced all pthread_* calls with IRON_MUTEX/COND/THREAD macros; Windows cpu count via GetSystemInfo
- `src/cli/build.c` - Windows includes, dirname/basename shims, Windows temp file, clang-cl argv via CreateProcess, .exe in derive_output_name
- `.github/workflows/ci.yml` - Added windows-latest to matrix; platform-conditional steps; fail-fast: false
- `CMakeLists.txt` - pthread link and integration test wrapped in NOT WIN32 guards

## Decisions Made
- Threading abstraction uses iron_thread_t/iron_mutex_t/iron_cond_t typedefs + IRON_* macros so all call sites in iron_threads.c only use macros — portable without changing logic
- Windows build path uses clang-cl + /std:c11 + /Fe<output> flag (MSVC output convention) via CreateProcess (no posix_spawnp on Windows)
- GetTempPath/GetTempFileName replaces mkstemps for temp C file creation on Windows
- Integration tests skipped on Windows in both CMakeLists.txt and CI since they are shell-script based
- fail-fast: false in CI matrix ensures all three platforms always report independently

## Deviations from Plan

None - plan executed exactly as written.

## Issues Encountered
- `#include <windows.h>` inside a function body (for GetSystemInfo in iron_threads_init) is not valid C; moved include to file-level `#ifdef _WIN32` block. Fixed inline without separate commit.

## User Setup Required
None - no external service configuration required.

## Next Phase Readiness
- All runtime threading is now platform-abstracted and will compile on Windows with MSVC 17.8+ (VS 2022) using C11 threads
- build.c handles clang-cl on Windows and clang on Unix
- CI validates all three platforms on every push to main and PR

---
*Phase: 04-comptime-game-dev-and-cross-platform*
*Completed: 2026-03-26*
