---
phase: 02-cli-toml-scaffold
plan: "04"
subsystem: cli
tags: [web, emscripten, emcc, build-stub, version-detection]
dependency_graph:
  requires: ["02-01"]
  provides: ["iron_build_web() stub with emcc probe and Phase 2 exit-0 path"]
  affects: ["src/cli/build_web.h", "src/cli/build_web.c"]
tech_stack:
  added: [popen for emcc --version, posix access() for PATH probe]
  patterns: [static helper composition, runtime version reading, soft-warn on drift]
key_files:
  created:
    - src/cli/build_web.h
    - src/cli/build_web.c
  modified: []
decisions:
  - "popen() used for emcc --version capture (simpler than posix_spawnp + pipe for one-shot version read)"
  - "sscanf X.Y.Z extraction with %n consumed-byte tracking for reliable version parse"
  - "Comment-level 4.0.23 references replaced with X.Y.Z placeholder to satisfy pin discipline on literal grep"
metrics:
  duration: "~8 minutes"
  completed: "2026-04-11"
  tasks_completed: 2
  files_created: 2
  files_modified: 0
---

# Phase 2 Plan 04: Build Web Stub Summary

**One-liner:** `iron_build_web()` Phase 2 stub with runtime emcc PATH probe, X.Y.Z banner, .emsdk-version drift warning, install one-liner, and config validation — all version strings read from `.emsdk-version` at runtime, zero hardcoding.

## Tasks Completed

| Task | Description | Commit | Files |
|------|-------------|--------|-------|
| 1 | Create src/cli/build_web.h with iron_build_web() prototype | 03b1f8e | src/cli/build_web.h |
| 2 | Create src/cli/build_web.c with all stub helpers and entry point | f8fd3ff | src/cli/build_web.c |

## What Was Built

### `src/cli/build_web.h` (26 lines)
Single-prototype header exposing `iron_build_web()` with the same signature as `iron_build()`. Include guard `IRON_CLI_BUILD_WEB_H` follows project convention. Self-contained via `#include "cli/build.h"`. No helper declarations — all helpers are `static` in the `.c` file.

### `src/cli/build_web.c` (312 lines)

Five static helpers + one public entry point:

1. **`iron_read_pinned_emsdk_version(source_path)`** — reads `.emsdk-version` from cwd, falls back to `<dirname(source)>/../.emsdk-version`. Strips trailing newline. Returns heap-allocated string or NULL. No hardcoded version.

2. **`find_emcc()`** — walks `$PATH` colon-split entries, tests each `<dir>/emcc` with `access(..., X_OK)`. Returns malloc'd absolute path or NULL. Empty PATH component treated as `.` per POSIX. Linux/macOS only.

3. **`get_emcc_version(emcc_path)`** — `popen("<emcc_path> --version 2>&1", "r")`, reads first line, extracts first `X.Y.Z` sequence via `sscanf` with `%n` consumed tracking.

4. **`print_install_one_liner(pinned_version)`** — emits the multi-line `error: emcc not found in PATH` block to stderr with runtime-interpolated version from `.emsdk-version`. Fallback to shorter message if version is NULL.

5. **`validate_web_config(cfg)`** — Phase 2 sanity: non-empty title if set, non-negative initial_memory/stack_size, pthread_pool_size in [0,16].

6. **`iron_build_web(source_path, output_path, opts)`** — entry point. Parses `iron.toml` beside source, reads pinned version, probes PATH, prints banner `using emcc %s from %s`, soft-warns on drift, validates config, prints `Phase 2: CLI + TOML scaffold complete; real compilation in Phase 7`, returns 0. All allocations freed on every exit path.

**Windows guard:** `#ifdef _WIN32 #error` at top of file prevents accidental Windows builds.

## Key Invariants Verified

- `! grep -Fq "4.0.23" src/cli/build_web.c` — PASS (pin discipline)
- `! grep -Fq "4.0.23" src/cli/build_web.h` — PASS
- `grep -Fq "#ifdef _WIN32" src/cli/build_web.c && grep -Fq "#error"` — PASS
- All 5 static helpers present
- 23 `free()` calls (all cleanup paths covered)
- 312 lines (> 150 minimum)

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 1 - Bug] Removed hardcoded version from comments**
- **Found during:** Task 2 verification (pin discipline check)
- **Issue:** The docstring examples in `get_emcc_version()` used the literal version string to illustrate sample output, triggering the `! grep -Fq "4.0.23"` acceptance check
- **Fix:** Replaced `"4.0.23"` in two comment lines with `"X.Y.Z"` placeholder
- **Files modified:** src/cli/build_web.c
- **Commit:** f8fd3ff (same commit)

## Self-Check: PASSED

- src/cli/build_web.h — FOUND
- src/cli/build_web.c — FOUND
- commit 03b1f8e — FOUND
- commit f8fd3ff — FOUND
