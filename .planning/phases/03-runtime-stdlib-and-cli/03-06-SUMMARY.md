---
phase: 03-runtime-stdlib-and-cli
plan: "06"
subsystem: cli
tags: [cli, build, run, check, diagnostics, posix_spawn]
dependency_graph:
  requires: [03-01, 03-02, 03-03, 03-04, 03-05]
  provides: [iron-binary, build-command, run-command, check-command]
  affects: [CMakeLists.txt]
tech_stack:
  added: [posix_spawn, mkstemps]
  patterns: [full-pipeline-orchestration, runtime-source-compilation, cmake-source-dir-injection]
key_files:
  created:
    - src/cli/main.c
    - src/cli/build.h
    - src/cli/build.c
    - src/cli/check.h
    - src/cli/check.c
  modified:
    - CMakeLists.txt
decisions:
  - "IRON_SOURCE_DIR macro baked in at CMake configure time so build.c can locate runtime sources without path resolution at runtime"
  - "Runtime sources compiled inline alongside generated C (not linked as .a) so the user binary is fully self-contained in one clang invocation"
  - "posix_spawn used for both clang invocation and binary execution; posix_spawn (not posix_spawnp) used for run_after so ./binary path works without PATH search"
  - "WILL_FAIL TRUE removed from test_cli_help CTest property; PASS_REGULAR_EXPRESSION alone handles expected-failure-with-output pattern correctly"
  - "verbose output goes to stdout, diagnostics/status to stderr — consistent with clang/rustc conventions"
metrics:
  duration_minutes: 6
  completed_date: "2026-03-26"
  tasks_completed: 2
  files_changed: 6
---

# Phase 3 Plan 6: CLI Commands (build, run, check) Summary

Iron gets a usable `iron` CLI that orchestrates the full lex-parse-analyze-codegen-clang pipeline with Rust-style diagnostics, `--verbose` mode showing generated C, and `--debug-build` for retaining temp files.

## Tasks Completed

| Task | Name | Commit | Files |
|------|------|--------|-------|
| 1 | Implement CLI main dispatcher with build, run, and check commands | dfa4b21 | src/cli/main.c, src/cli/build.h, src/cli/build.c, src/cli/check.h, src/cli/check.c |
| 2 | Update CMakeLists.txt for CLI binary and integration smoke test | 09feedf | CMakeLists.txt |

## What Was Built

**src/cli/main.c** — Entry point parsing `iron <cmd>` with global flags (`--verbose`, `--debug-build`), dispatches to `iron_build` or `iron_check`. Unrecognized commands and `fmt`/`test` stubs print usage and return 1.

**src/cli/build.c** — Full pipeline orchestration:
1. `fopen`/`fread` reads `.iron` source
2. Arena + DiagList allocation
3. `iron_lex_all` → token array
4. `iron_parser_create` + `iron_parse` → AST
5. `iron_analyze` → `Iron_AnalyzeResult` with global scope
6. `iron_codegen` → C source string
7. `mkstemps` for temp `.c` file (or `.iron-build/` with `--debug-build`)
8. `posix_spawnp("clang", ...)` compiles generated C alongside all runtime/stdlib sources (injected via `IRON_SOURCE_DIR` macro) → standalone binary
9. For `iron run`: `posix_spawn("./binary")` executes the result, propagates exit code
10. Diagnostic errors at any stage call `iron_diag_print_all` (which uses `isatty(STDERR_FILENO)` for ANSI color gating)

**src/cli/check.c** — Same pipeline minus codegen and clang; returns 0/1 based on error count.

**CMakeLists.txt** — `add_executable(iron ...)` linked to `iron_compiler iron_runtime iron_stdlib m pthread`; `target_compile_definitions` injects `IRON_SOURCE_DIR`; `test_cli_help` CTest target verifies usage output.

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 1 - Bug] iron_arena_destroy does not exist**
- **Found during:** Task 1, first build attempt
- **Issue:** Plan template used `iron_arena_destroy()` but the arena API only exposes `iron_arena_free()`
- **Fix:** All instances replaced with `iron_arena_free()`
- **Files modified:** src/cli/build.c, src/cli/check.c

**2. [Rule 1 - Bug] Generated binary missing runtime symbols**
- **Found during:** Task 1 verification (iron build test)
- **Issue:** Generated C calls `Iron_println`, `iron_runtime_init`, `iron_string_from_literal` etc. but clang invocation only passed `-lm -lpthread` — no runtime symbols
- **Fix:** Updated `invoke_clang()` to compile all runtime/stdlib source files alongside generated C using `IRON_SOURCE_DIR` macro (injected by CMake). Also added `IRON_SOURCE_DIR` definition to CMakeLists.txt
- **Files modified:** src/cli/build.c, CMakeLists.txt

**3. [Rule 1 - Bug] iron run could not find compiled binary**
- **Found during:** Task 1 verification (iron run test)
- **Issue:** `posix_spawnp` searches `PATH` — relative binary names like `test_hello` were not found
- **Fix:** When binary_name has no `/`, prefix with `./` and use `posix_spawn` (exact path) instead of `posix_spawnp` (PATH search)
- **Files modified:** src/cli/build.c

**4. [Rule 1 - Bug] test_cli_help failed due to WILL_FAIL + PASS_REGULAR_EXPRESSION interaction**
- **Found during:** Task 2 test run
- **Issue:** CMake `WILL_FAIL TRUE` inverts the pass/fail decision *after* `PASS_REGULAR_EXPRESSION` check. When regex matched (pass), WILL_FAIL inverted it to fail.
- **Fix:** Removed `WILL_FAIL TRUE` — `PASS_REGULAR_EXPRESSION` alone passes the test when output matches regardless of exit code
- **Files modified:** CMakeLists.txt

## Verification Results

All 19 tests pass:
```
100% tests passed, 0 tests failed out of 19
Total Test time (real) = 0.78 sec
```

Manual smoke tests:
- `iron build hello.iron` → produces `test_hello` binary, prints "Built: test_hello"
- `./test_hello` → prints "hello from iron"
- `iron run hello.iron` → prints "hello from iron", exit 0
- `iron check bad.iron` (type error) → prints `error[E0202]: type mismatch: expected 'Int', got 'String'` with source snippet and arrow
- `iron build --verbose hello.iron` → prints generated C source to stdout before building
- `iron` (no args) → prints usage, exit 1

## Self-Check: PASSED

Files created:
- src/cli/main.c — FOUND
- src/cli/build.c — FOUND
- src/cli/build.h — FOUND
- src/cli/check.c — FOUND
- src/cli/check.h — FOUND

Commits:
- dfa4b21 — FOUND (feat(03-06): implement CLI commands)
- 09feedf — FOUND (feat(03-06): add iron CLI binary to CMake)
