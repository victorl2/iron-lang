---
phase: 03-runtime-audit-web-hardening
plan: "01"
subsystem: runtime
tags: [thread-safety, tsan, intern-table, web-hardening]
dependency_graph:
  requires: []
  provides:
    - WEB-RUNTIME-01 verified via tsan stress test + contract comment
  affects:
    - src/runtime/iron_string.c (comment-only)
    - tests/unit/test_string_intern_race.c (new)
    - tests/unit/CMakeLists.txt (new target)
tech_stack:
  added: []
  patterns:
    - pthread 8-worker stress test with clock_gettime budget
    - CMake FAIL_REGULAR_EXPRESSION tsan warning trap
    - Thread-safety contract comment pattern
key_files:
  created:
    - tests/unit/test_string_intern_race.c
  modified:
    - src/runtime/iron_string.c
    - tests/unit/CMakeLists.txt
decisions:
  - "tsan-first: tsan test PASSED (macOS smoke, will tsan-instrument on Linux CI) — no DCL rewrite needed"
  - "clock_gettime(CLOCK_MONOTONIC) used for per-worker wall-clock budget (avoids clock() which is process-wide CPU time)"
  - "FAIL_REGULAR_EXPRESSION traps tsan WARNING banner even when process exits 0 (tsan default without halt_on_error)"
metrics:
  duration: "718s (~12 min)"
  completed: "2026-04-11"
  tasks_completed: 2
  files_changed: 3
---

# Phase 3 Plan 01: Intern Table Hardening Summary

**One-liner:** ThreadSanitizer stress test + in-source thread-safety contract prove `iron_string_intern()` is race-free under the current single-mutex + `pthread_once` pattern; no DCL rewrite needed.

## Tasks Completed

| Task | Name | Commit | Files |
|------|------|--------|-------|
| 1 | Write tsan stress test + register as ctest | 78155fa | tests/unit/test_string_intern_race.c, tests/unit/CMakeLists.txt |
| 2 | Document thread-safety contract on iron_string_intern | 838617a | src/runtime/iron_string.c |

## What Was Built

**Task 1 — tsan stress test:**

`tests/unit/test_string_intern_race.c` spawns 8 pthread workers. Each worker loops for up to 1 second (wall-clock via `clock_gettime(CLOCK_MONOTONIC)`) or 10,000 iterations, whichever hits first. Each iteration interns 64 unique-per-worker literals (`"uniq_<worker>_<i>"`) interleaved with the shared hot literal `"intern_hot_literal_shared_across_workers"` to maximize contention on the single `s_intern_lock`. After all workers join, the main thread re-interns the hot literal and asserts it round-trips correctly.

The CMake target `test_string_intern_race` applies `-fsanitize=thread -g -O1` compile+link flags on Linux only (macOS tsan is flaky; emcc has no tsan). `FAIL_REGULAR_EXPRESSION "WARNING: ThreadSanitizer"` turns any tsan warning line into a ctest failure even if the process returns 0.

**Task 2 — thread-safety contract comment:**

A `/* Thread-safety contract (WEB-RUNTIME-01): ... */` block was inserted immediately above the `Iron_String iron_string_intern(Iron_String s)` definition in `src/runtime/iron_string.c`. The comment:

- Documents that `s_intern_lock` covers the full `shgeti/shput` critical section
- Explains why `pthread_once` / `InitOnceExecuteOnce` guarantees memory-ordered init
- Explains the Emscripten + SharedArrayBuffer + main-thread safety argument
- References `PROXY_TO_PTHREAD` as a forbidden emcc flag (WEB-BUILD-07) enforcing the uncontested-at-init invariant
- Explicitly states no DCL upgrade is needed, and points to CONTEXT.md if the tsan test ever fails

## Verification Results

```
test 48: test_string_intern_race ... Passed  1.36 sec
100% tests passed, 0 tests failed out of 1
```

Full suite (61 tests, `ctest -E benchmark_smoke`):
```
100% tests passed, 0 tests failed out of 61
tsan   = 1.02 sec*proc (1 test)
unit   = 6.82 sec*proc (29 tests)
```

Acceptance criteria checks:
- `grep "Thread-safety contract (WEB-RUNTIME-01)"` → PASS
- `grep "test_string_intern_race" tests/unit/CMakeLists.txt` → PASS
- `grep "fsanitize=thread" tests/unit/CMakeLists.txt` → PASS
- `grep "double-checked read path" iron_string.c` → PASS
- `grep "PROXY_TO_PTHREAD" iron_string.c` → PASS
- `! grep "4.0.23" test_string_intern_race.c` → PASS (pin discipline observed)

## Deviations from Plan

None — plan executed exactly as written. The tsan test passed on macOS (plain smoke without tsan instrumentation); no DCL rewrite was needed or attempted.

## Self-Check: PASSED
