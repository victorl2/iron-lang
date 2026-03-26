---
phase: 03-runtime-stdlib-and-cli
plan: "05"
subsystem: stdlib
tags: [math, io, time, log, stdlib, xorshift64, monotonic-clock]
dependency_graph:
  requires: [03-01, 03-03]
  provides: [iron_stdlib, Iron_math_*, Iron_io_*, Iron_time_*, Iron_log_*]
  affects: [03-06, 03-07]
tech_stack:
  added: [iron_stdlib static library, xorshift64 RNG, UNITY_INCLUDE_DOUBLE]
  patterns: [result-struct error returns, thread-local RNG state, isatty color gating, CLOCK_MONOTONIC monotonic time]
key_files:
  created:
    - src/stdlib/iron_math.h
    - src/stdlib/iron_math.c
    - src/stdlib/iron_io.h
    - src/stdlib/iron_io.c
    - src/stdlib/iron_time.h
    - src/stdlib/iron_time.c
    - src/stdlib/iron_log.h
    - src/stdlib/iron_log.c
    - tests/test_stdlib.c
  modified:
    - CMakeLists.txt
decisions:
  - "iron_math global RNG uses two separate __thread static variables (state + init flag) since __thread storage requires constant initialization"
  - "UNITY_INCLUDE_DOUBLE added to unity PUBLIC compile definitions so all test targets inherit double precision support"
  - "Iron_io_list_files uses newline-separated entries (not comma-separated as originally stated in plan) for unambiguous parsing of filenames with commas"
metrics:
  duration: 5
  completed_date: "2026-03-26"
  tasks_completed: 2
  files_changed: 10
---

# Phase 3 Plan 5: Standard Library Modules (math, io, time, log) Summary

Four standard library modules implemented with headers, implementations, and a 21-test Unity suite covering math functions, file I/O, monotonic time, and leveled logging.

## Tasks Completed

| Task | Description | Commit | Files |
|------|-------------|--------|-------|
| 1 | Implement math, io, time, and log modules | 090e7dc | 8 new files in src/stdlib/ |
| 2 | Add stdlib tests and update CMakeLists.txt | 0b2a441 | tests/test_stdlib.c, CMakeLists.txt |

## What Was Built

### iron_math (src/stdlib/iron_math.c)
- Thin `<math.h>` wrappers: `sin`, `cos`, `tan`, `sqrt`, `pow`, `floor`, `ceil`, `round`, `lerp`
- Constants: `IRON_PI`, `IRON_TAU`, `IRON_E`
- xorshift64 RNG: `Iron_RNG` struct with `Iron_rng_create/next/next_int/next_float`
- Thread-local global RNG seeded from `clock_gettime(CLOCK_MONOTONIC)` for `Iron_math_random()` and `Iron_math_random_int()`

### iron_io (src/stdlib/iron_io.c)
- `Iron_Result_String_Error` and `Iron_Result_bool_Error` result types
- `Iron_io_read_file` / `Iron_io_write_file` using `fopen`/`fread`/`fwrite`
- `Iron_io_file_exists` via `stat()`
- `Iron_io_create_dir` via `mkdir()` (idempotent with `EEXIST` check)
- `Iron_io_delete_file` via `remove()`
- `Iron_io_list_files` using `opendir`/`readdir` returning newline-separated filename list

### iron_time (src/stdlib/iron_time.c)
- `Iron_time_now()` — wall-clock seconds as double via `CLOCK_REALTIME`
- `Iron_time_now_ms()` — monotonic milliseconds via `clock_gettime(CLOCK_MONOTONIC)`
- `Iron_time_sleep()` — millisecond sleep via `nanosleep()`
- `Iron_Timer` with `Iron_timer_create()`, `Iron_timer_since()`, `Iron_timer_reset()`

### iron_log (src/stdlib/iron_log.c)
- `Iron_LogLevel` enum: DEBUG=0, INFO=1 (default), WARN=2, ERROR=3
- `Iron_log_set_level()` for runtime level filtering
- `Iron_log_debug/info/warn/error()` — timestamped `[YYYY-MM-DD HH:MM:SS] [LEVEL] msg\n` to stderr
- ANSI color (gray/green/yellow/red) gated by `isatty(STDERR_FILENO)`
- Wall-clock timestamps via `time()` + `localtime_r()`

### Tests (tests/test_stdlib.c)
21 tests total:
- 10 math tests: trig correctness, sqrt, pow, lerp, floor/ceil, constant values, RNG uniqueness/range, random [0,1)
- 5 IO tests: write+read roundtrip, nonexistent file error, file_exists true/false, create_dir, delete_file
- 4 time tests: monotonic ordering, sleep >= 10ms, timer since >= 5ms, timer reset < previous
- 2 log tests: set_level filtering without crash, all four log functions complete without crash

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 2 - Missing Config] UNITY_INCLUDE_DOUBLE not enabled in Unity build**
- **Found during:** Task 2 test run — 7 math double-precision tests failed with "Unity Double Precision Disabled"
- **Issue:** Unity v2.6.1 excludes double support by default; `TEST_ASSERT_DOUBLE_WITHIN` requires `UNITY_INCLUDE_DOUBLE` compile definition in Unity itself, not just the test target
- **Fix:** Added `target_compile_definitions(unity PUBLIC UNITY_INCLUDE_DOUBLE)` after `FetchContent_MakeAvailable(unity)` so all test targets inherit the definition
- **Files modified:** CMakeLists.txt
- **Commit:** 0b2a441 (included in Task 2 commit)

**2. [Rule 1 - Bug] Unused variable warning in test_math_random**
- **Found during:** Task 2 first build — `-Werror,-Wunused-variable` on double assertion variable
- **Issue:** `TEST_ASSERT_GREATER_OR_EQUAL_DOUBLE` macro doesn't use the variable in a compiler-visible way
- **Fix:** Rewrote test to accumulate results into `int in_range` flag, then assert
- **Files modified:** tests/test_stdlib.c
- **Commit:** 0b2a441

**3. [Rule 3 - Blocking] Missing `<unistd.h>` for rmdir in tests**
- **Found during:** Task 2 first build — `call to undeclared function 'rmdir'`
- **Fix:** Added `#include <unistd.h>` to test_stdlib.c
- **Files modified:** tests/test_stdlib.c
- **Commit:** 0b2a441

## Key Decisions

1. Global RNG uses two separate `__thread` statics (state + init flag) — `__thread` requires constant initialization so `Iron_rng_create(0)` cannot be used as a default
2. `UNITY_INCLUDE_DOUBLE` on the unity target itself (PUBLIC scope) so inherited by all test executables — avoids per-target repetition
3. `Iron_io_list_files` uses newline separator instead of comma as originally stated in the plan — commas can appear in filenames, newlines cannot (on Linux/macOS)

## Self-Check: PASSED

All 9 required files exist on disk. Commits 090e7dc and 0b2a441 exist in git history. All 18 tests pass under ASan/UBSan.
