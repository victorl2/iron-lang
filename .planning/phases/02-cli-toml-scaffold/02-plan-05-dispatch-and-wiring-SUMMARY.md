---
phase: 02-cli-toml-scaffold
plan: 05
subsystem: cli-build-dispatch
tags: [dispatch, cmake, release-flag, native-build]
dependency_graph:
  requires:
    - "02-01: IronBuildTarget enum + target/release fields in IronBuildOpts"
    - "02-03: --target/--release flag parsing in main.c"
    - "02-04: iron_build_web() stub in build_web.c"
  provides:
    - "iron_build() dispatches to iron_build_web() when target == IRON_TARGET_WEB"
    - "Native --release builds get -O2 appended to clang argv"
    - "build_web.c linked into the ironc binary via CMake"
  affects:
    - "src/cli/build.c (dispatch + -O2 branch)"
    - "CMakeLists.txt (build_web.c source registration)"
tech_stack:
  added: []
  patterns:
    - "Last-wins clang -O flag: append -O2 after -O3 for release; -O2 wins"
    - "Single-line dispatch at function entry: short-circuit before any native machinery"
key_files:
  created: []
  modified:
    - src/cli/build.c
    - CMakeLists.txt
decisions:
  - "Used Interpretation A for -O2 (append after -O3, clang last-wins) per CONTEXT.md literal phrasing"
  - "Dispatch placed as absolute first statement in iron_build(), before even base_dir resolution"
metrics:
  duration: "2 minutes"
  completed: "2026-04-11T15:31:00Z"
  tasks_completed: 3
  files_modified: 2
---

# Phase 02 Plan 05: Dispatch and Wiring Summary

**One-liner:** Wired `iron_build()` dispatch to `iron_build_web()` for `--target=web`, added clang `-O2` append for `--release` native builds, and registered `build_web.c` in the CMake `ironc` target — completing the Wave 3 synthesis step.

## Tasks Completed

| Task | Name | Commit | Files |
|------|------|--------|-------|
| 1 | Add iron_build_web dispatch at top of iron_build() | 16e4d8c | src/cli/build.c |
| 2 | Add --release -O2 branch to build_src_list native clang argv | 2db2885 | src/cli/build.c |
| 3 | Add src/cli/build_web.c to ironc CMake target | d93a449 | CMakeLists.txt |

## What Was Built

### Task 1: Web dispatch in iron_build()
- Added `#include "cli/build_web.h"` to build.c's include block (after `cli/iron_import_detect.h`)
- Added single-line dispatch as first statement in `iron_build()`:
  ```c
  if (opts.target == IRON_TARGET_WEB) {
      return iron_build_web(source_path, output_path, opts);
  }
  ```
- No platform guards — `IRON_TARGET_WEB` is a runtime flag available on all hosts

### Task 2: Native -O2 release branch
- Added `if (opts.release)` block after the existing `-O3` in `build_src_list()` Unix branch:
  ```c
  if (opts.release) {
      /* Phase 2: --release appends -O2 to native builds; clang's last-wins
       * argv parsing means this overrides the -O3 above. */
      argv_buf[ai++] = "-O2";
  }
  ```
- Existing `-O3` preserved — release just appends `-O2` which wins per clang last-wins semantics
- Windows `clang-cl` branch untouched

### Task 3: CMake registration
- Added `src/cli/build_web.c` to `add_executable(ironc ...)` immediately after `src/cli/build.c`
- `iron` package-manager target untouched
- No new `target_include_directories` or `target_link_libraries` needed — existing `src/` include path and libc covers `build_web.c`

## Verification Results

All plan success criteria verified:
- `cmake --build build` succeeds: all sources compiled, `ironc` binary linked cleanly with `build_web.o`
- `build/ironc --version` prints version string
- `grep -q "IRON_TARGET_WEB" src/cli/build.c` — PASS
- `grep -q "iron_build_web" src/cli/build.c` — PASS
- `grep -F '"-O2"' src/cli/build.c` — PASS
- `grep -q "build_web.c" CMakeLists.txt` — PASS
- `! grep -Fq "4.0.23" src/cli/build.c` — PASS (pin discipline maintained)
- Smoke test: `build/ironc build --target=unknown foo.iron 2>&1 | grep -q "valid targets: web, native"` — PASS
- `build/ironc 2>&1 | grep -q -- '--target=<t>'` — PASS (usage text includes new flag)

## Deviations from Plan

### Awk check false positive (non-blocking)

The plan's acceptance criteria for Task 1 included this awk check:
```
awk '/opts.target == IRON_TARGET_WEB/{found=1} /base_dir = get_iron_lib_dir/{if (!found) exit 1; exit 0}' src/cli/build.c
```
This check failed because there is a second `base_dir = get_iron_lib_dir()` call at line 359 in a *different* function (`build_src_list`) that appears before `iron_build()` in the file. The awk pattern matched this earlier occurrence first, then never saw the `IRON_TARGET_WEB` pattern before it, and exited 1.

The actual code is correct: in `iron_build()`, the dispatch at line 644 is before `base_dir` at line 649. Verified manually by reading the file and by successful build. No code change needed.

## Key Decisions Made

1. **Interpretation A for -O2:** Appended `-O2` after fixed `-O3` (clang last-wins) rather than making `-O3` conditional. Smaller diff, matches CONTEXT.md literal phrasing verbatim.
2. **Dispatch position:** Placed as absolute first statement before even `base_dir = get_iron_lib_dir()`. Web path fully owns its own execution without touching any native machinery.

## Self-Check

- [x] `src/cli/build.c` modified with both dispatch and -O2 branch
- [x] `CMakeLists.txt` modified with `src/cli/build_web.c`
- [x] Three commits exist: 16e4d8c, 2db2885, d93a449
- [x] `cmake --build build` succeeded — `ironc` binary at `build/ironc`
- [x] All plan verification commands pass

## Self-Check: PASSED
