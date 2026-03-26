---
phase: 03-runtime-stdlib-and-cli
plan: 07
subsystem: cli
tags: [iron-fmt, iron-test, iron-toml, ast-pretty-printer, test-discovery, toml-parser]

# Dependency graph
requires:
  - phase: 01-frontend
    provides: AST pretty-printer (iron_print_ast) reused by iron fmt
  - phase: 03-01
    provides: iron_runtime library used in compiled test binaries
  - phase: 03-04
    provides: iron_build API used by test runner to compile each test file
  - phase: 03-06
    provides: CLI build.c and check.c as integration pattern reference

provides:
  - iron fmt command: in-place source formatting with safe atomic write
  - iron test command: test_*.iron discovery, compile, run with pass/fail reporting
  - iron.toml parser: minimal project config reader (name, version, entry, raylib)
  - CLI dispatcher updated to handle all 5 commands: build, run, check, fmt, test

affects:
  - 03-08 (CI/hardening phase may add fmt/test to pipeline)
  - 04 (Phase 4 raylib projects will use iron.toml with raylib=true)

# Tech tracking
tech-stack:
  added: []
  patterns:
    - Safe atomic file replacement via temp write + rename() system call
    - Test runner pattern: discover -> compile -> run -> report
    - Minimal hand-written TOML parser for project config subset

key-files:
  created:
    - src/cli/fmt.h
    - src/cli/fmt.c
    - src/cli/test_runner.h
    - src/cli/test_runner.c
    - src/cli/toml.h
    - src/cli/toml.c
  modified:
    - src/cli/main.c
    - CMakeLists.txt

key-decisions:
  - "iron fmt uses rename() for atomic file replacement — original never touched until temp write fully verified"
  - "Test runner uses fork+posix_spawn with exit-code-based pass/fail determination"
  - "iron.toml parser hand-written (minimal subset, not full TOML) — only project name/version/entry/raylib=true"
  - "Test runner uses StrVec dynamic array + qsort for alphabetical deterministic ordering"
  - "ANSI color output gated by isatty(STDOUT_FILENO) consistent with existing diagnostics pattern"

patterns-established:
  - "Safe in-place write: write to <path>.iron.tmp, verify fwrite+fflush, then rename() atomically"
  - "iron fmt refuses to format files with syntax errors — prints diagnostics + refusal message"
  - "Test discovery: opendir+readdir, strncmp('test_', 5) prefix + '.iron' suffix matching"

requirements-completed: [CLI-04, CLI-05]

# Metrics
duration: 3min
completed: 2026-03-26
---

# Phase 3 Plan 7: CLI fmt, test, and iron.toml Summary

**iron fmt with atomic safe in-place formatting, iron test runner with colorized pass/fail reporting, and minimal iron.toml project config parser**

## Performance

- **Duration:** 3 min
- **Started:** 2026-03-26T13:15:43Z
- **Completed:** 2026-03-26T13:18:35Z
- **Tasks:** 3
- **Files modified:** 8

## Accomplishments

- `iron fmt file.iron` reformats in-place using Phase 1 AST pretty-printer; refuses to overwrite on parse errors via temp file + rename() atomic pattern
- `iron test dir/` discovers test_*.iron files, compiles each with iron_build, runs via posix_spawn, reports colorized [PASS]/[FAIL] with summary line
- iron.toml minimal hand-written parser reads project name, version, entry, and raylib=true/false
- All 19 existing tests continue to pass; iron binary builds cleanly

## Task Commits

Each task was committed atomically:

1. **Task 1: Implement iron fmt command** - `df3a11c` (feat)
2. **Task 2: Implement iron test runner and iron.toml parser** - `2bbc595` (feat)
3. **Task 3: Wire fmt and test into CLI dispatcher, update CMakeLists** - `28bcb64` (feat)

## Files Created/Modified

- `src/cli/fmt.h` - iron_fmt() function declaration
- `src/cli/fmt.c` - Format command: lex+parse, check errors, pretty-print, write temp, rename
- `src/cli/test_runner.h` - iron_test() function declaration
- `src/cli/test_runner.c` - Test discovery (opendir/readdir), compile (iron_build), run (posix_spawn), colorized reporting
- `src/cli/toml.h` - IronProject struct + iron_toml_parse/free declarations
- `src/cli/toml.c` - Minimal line-by-line TOML parser for [project] and [dependencies] sections
- `src/cli/main.c` - Added iron_fmt/iron_test dispatch; fmt shows usage error on missing arg; test defaults to "."
- `CMakeLists.txt` - Added fmt.c, test_runner.c, toml.c to iron executable sources

## Decisions Made

- `iron fmt` temp file path uses `<original>.iron.tmp` suffix (predictable, visible on crash)
- Test runner calls `iron_build()` directly (shared library function) rather than spawning `iron build` subprocess — avoids PATH lookup issues and keeps output cleaner
- `posix_spawn()` (not `posix_spawnp()`) for running compiled test binaries since full path is provided
- StrVec dynamic array used instead of stb_ds for test file list — simpler and avoids stb_ds_impl dependency in CLI translation unit

## Deviations from Plan

None - plan executed exactly as written.

## Issues Encountered

None - all three files compiled without warnings on first attempt.

## Next Phase Readiness

- All 5 CLI commands operational: build, run, check, fmt, test
- iron.toml parser ready for Phase 4 raylib dependency detection
- Test runner can be used to validate Iron programs during Phase 4 raylib integration

---
*Phase: 03-runtime-stdlib-and-cli*
*Completed: 2026-03-26*
