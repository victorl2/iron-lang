---
phase: 04-wasm-safe-time-shim
plan: "01"
subsystem: stdlib/time
tags: [emscripten, wasm, time, stdlib, web-runtime]
dependency_graph:
  requires: []
  provides: [src/stdlib/iron_time_web.c]
  affects: [web-build-link-step, CI-portability-probe]
tech_stack:
  added: [emscripten_get_now, emscripten_date_now]
  patterns: [ifdef-__EMSCRIPTEN__-guard, self-contained-translation-unit, spin-loop-sleep]
key_files:
  created: [src/stdlib/iron_time_web.c]
  modified: []
decisions:
  - "emscripten_sleep absent from file even in comments — Asyncify-free policy enforced textually"
  - "iron_time_now_ns uses ms-precision-in-ns-units (emscripten_get_now()*1e6); W3C performance.now() cap accepted per Phase 4 SC1"
  - "Iron_time_sleep implemented as spin loop — blocking main thread is acceptable; game code uses emscripten_set_main_loop"
  - "Timer helpers duplicated verbatim from iron_time.c so iron_time_web.c is a self-contained translation unit (zero link-time dependency on iron_time.c)"
metrics:
  duration: 712s
  completed: "2026-04-11"
  tasks_completed: 1
  files_changed: 1
requirements_satisfied: [WEB-RUNTIME-05]
---

# Phase 4 Plan 1: iron_time_web.c — Emscripten Web-Target Time Implementation Summary

**One-liner:** Emscripten web-target time shim using emscripten_get_now/emscripten_date_now with spin-loop sleep, all 9 iron_time.h symbols under __EMSCRIPTEN__ guard.

## What Was Built

Created `src/stdlib/iron_time_web.c` — the WebAssembly counterpart of `src/stdlib/iron_time.c`. The file implements the complete `iron_time.h` contract (9 public symbols) using Emscripten's monotonic and wall-clock APIs instead of POSIX `clock_gettime`/`nanosleep`.

## Tasks Completed

| Task | Name | Commit | Files |
|------|------|--------|-------|
| 1 | Create src/stdlib/iron_time_web.c — mirror native file body with emscripten time APIs under __EMSCRIPTEN__ guard | 6aaaa04 | src/stdlib/iron_time_web.c (created, 121 lines) |

## Implementation Details

### Time Source Mapping

| Function | Native (iron_time.c) | Web (iron_time_web.c) |
|----------|---------------------|----------------------|
| Iron_time_now | clock_gettime(CLOCK_REALTIME) | emscripten_date_now()/1000.0 |
| Iron_time_now_ms | clock_gettime(CLOCK_MONOTONIC) ms | (int64_t)emscripten_get_now() |
| Iron_time_now_ns | clock_gettime(CLOCK_MONOTONIC) ns | (int64_t)(emscripten_get_now()*1e6) — ms precision |
| Iron_time_sleep | nanosleep() | spin loop on emscripten_get_now() |
| Iron_time_since | Iron_time_now()-start | duplicated verbatim (pure arithmetic) |
| Iron_time_Timer + helpers | pure arithmetic | duplicated verbatim (pure arithmetic) |

### Key Design Constraints Upheld

- `#ifdef __EMSCRIPTEN__` wraps entire body with `#else #error` branch for loud native-build failure
- No reference to `emscripten_sleep` anywhere in file (not even in comments) — Asyncify-banned
- No version string `4.0.23` in file (pin discipline)
- `iron_time.c` and `iron_time.h` have zero diff (WEB-RUNTIME-06 invariant)
- No CMake target changes — Phase 7 owns web build wiring

## Verification Results

All structural checks passed:
- File exists with 121 lines (above min_lines: 60)
- `#ifdef __EMSCRIPTEN__` guard + `#else #error` branch present
- All 9 symbols defined: Iron_time_now, Iron_time_now_ms, Iron_time_now_ns, Iron_time_sleep, Iron_time_since, Iron_time_Timer, Iron_timer_done, Iron_timer_update, Iron_timer_reset
- emscripten_date_now and emscripten_get_now both present
- No emscripten_sleep (Asyncify-banned) — absent from file including comments
- No "4.0.23" version pin string
- `git diff --exit-code src/stdlib/iron_time.c src/stdlib/iron_time.h` exits 0 (WEB-RUNTIME-06)
- Native build clean: `cmake --build build` — 100% targets built
- Full test suite green: 63/63 tests passed, 0 failures

## Deviations from Plan

**1. [Rule 1 - Bug] Removed emscripten_sleep references from file comments**
- **Found during:** Task 1 verification
- **Issue:** The plan's provided code template contained references to `emscripten_sleep()` in comments. The plan's critical rules explicitly state "The string `emscripten_sleep` MUST NOT appear in the new file — not even in a comment, to avoid future 'let's enable Asyncify' drift."
- **Fix:** Replaced both comment occurrences with "Asyncify-based sleep" phrasing that conveys the same policy rationale without naming the banned function.
- **Files modified:** src/stdlib/iron_time_web.c
- **Commit:** 6aaaa04 (included in same task commit)

## Self-Check: PASSED

- src/stdlib/iron_time_web.c: FOUND
- SUMMARY.md: FOUND
- Commit 6aaaa04: FOUND
