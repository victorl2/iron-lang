---
phase: 04-comptime-game-dev-and-cross-platform
plan: "06"
subsystem: stdlib/vendor/runtime
tags: [raylib, threading, gap-closure, pthreads, cross-platform]
dependency_graph:
  requires: [04-01, 04-02, 04-03, 04-04, 04-05]
  provides: [parseable-raylib-iron, vendored-raylib-source, pthreads-only-runtime]
  affects: [src/stdlib, src/vendor, src/runtime, .github/workflows]
tech_stack:
  added: [raylib-5.5-vendored]
  patterns: [amalgamation-build, pthreads-everywhere]
key_files:
  created:
    - src/vendor/raylib/raylib.c
    - src/vendor/raylib/raylib.h
    - src/vendor/raylib/rcore.c
    - src/vendor/raylib/ (160 files, raylib 5.5 source)
  modified:
    - src/stdlib/raylib.iron
    - src/runtime/iron_runtime.h
    - src/runtime/iron_threads.c
    - .github/workflows/ci.yml
decisions:
  - "raylib.iron object declarations use one field per line (parser requires newline separators, not semicolons)"
  - "vendor/raylib/raylib.c is an amalgamation driver that #includes all raylib source modules; raylib 5.5 tarball does not ship a single-file amalgam"
  - "pthreads used unconditionally on all platforms; C11 threads.h path removed entirely; Windows uses pthreads4w via Chocolatey"
  - "iron_threads.c worker functions unified to void* return type with return NULL; _WIN32 signature guards removed"
metrics:
  duration_seconds: 167
  completed_date: "2026-03-26"
  tasks_completed: 3
  files_changed: 163
---

# Phase 4 Plan 6: Gap Closure — raylib.iron Syntax, Vendor Source, Pthreads Runtime Summary

**One-liner:** Fixed raylib.iron inline-semicolon syntax, vendored raylib 5.5 source as amalgamation driver, and switched runtime threading to pthreads-only (removing C11 threads.h), unblocking GAME-01.

## What Was Built

Three targeted fixes that close the remaining GAME-01 blockers:

1. **raylib.iron object syntax** — The `Vec2` and `Color` object declarations used inline semicolons as field separators (`var x: Float32; var y: Float32`). The Iron parser only accepts newline tokens between object fields. Rewrote both declarations to one field per line. `iron check src/stdlib/raylib.iron` now exits 0.

2. **Vendored raylib 5.5 source** — Downloaded the raylib 5.5 source tarball and extracted the `src/` directory into `src/vendor/raylib/`. Created `src/vendor/raylib/raylib.c` as an amalgamation driver that `#includes` all seven raylib source modules (rcore.c, rshapes.c, rtextures.c, rtext.c, rmodels.c, raudio.c, utils.c). The build pipeline at `src/cli/build.c:315` expects exactly this file path.

3. **Pthreads-only runtime** — Removed the `#ifdef _WIN32 / #include <threads.h>` block from `iron_runtime.h`. The header now unconditionally includes `pthread.h` and defines all `IRON_*` macros against `pthread_*` functions. In `iron_threads.c`, removed `_WIN32` guards on both `pool_worker` and `handle_thread_fn` — both now have `static void *fn(void *)` signatures and `return NULL` in all exit paths. Updated CI to install `pthreads4w` on Windows and pass its prefix path to CMake.

## Decisions Made

- **Amalgamation driver strategy:** Created `raylib.c` as a thin `#include` driver rather than downloading a non-existent pre-built amalgam. This is the standard approach for embedding raylib in single-TU builds.
- **Pthreads everywhere:** Removed the C11 `threads.h` path entirely per user directive. `pthreads4w` (pthreads-win32) provides the same API on Windows. No conditional compilation needed in `iron_runtime.h` for threading.
- **CI path for pthreads4w:** Used both `CMAKE_PREFIX_PATH` and explicit include/lib flags as belt-and-suspenders for the Windows CMake configure step, since Chocolatey pthreads4w layout can vary.

## Verification Results

| Check | Result |
|-------|--------|
| `iron check src/stdlib/raylib.iron` exits 0 | PASS |
| `src/vendor/raylib/raylib.h` exists (1708 lines) | PASS |
| `src/vendor/raylib/raylib.c` exists (amalgam driver) | PASS |
| `src/vendor/raylib/rcore.c` exists (4070 lines) | PASS |
| No `#include <threads.h>` or `thrd_t`/`mtx_t` in iron_runtime.h | PASS |
| `pthread.h` included unconditionally in iron_runtime.h | PASS |
| All 21 CTest unit tests pass on macOS | PASS |

## Human Verification Still Required

- **Raylib window on macOS:** A program using `import raylib` + `draw {}` compiles and opens a native window with graphics (requires GPU/display).
- **Windows CI green:** Push to main and verify `windows-latest` GitHub Actions job compiles with pthreads4w linked correctly.

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 1 - Bug] Created amalgamation driver for raylib.c**
- **Found during:** Task 2
- **Issue:** The plan noted "raylib does not ship a single .c amalgam in its repo" and provided the tar extraction command expecting `raylib-5.5/src/raylib.c` to exist. It does not — only `rcore.c`, `rshapes.c` etc. are present.
- **Fix:** After extraction, created `src/vendor/raylib/raylib.c` as a manual amalgamation driver that `#includes` each of the seven source modules. This is the correct pattern for single-TU raylib builds and matches what the build pipeline expects.
- **Files modified:** `src/vendor/raylib/raylib.c` (created, 20 lines)
- **Commit:** b5c3e45

## Self-Check: PASSED

All committed files verified present on disk and in git history.
