---
phase: 03-runtime-audit-web-hardening
plan: 03
subsystem: analyzer
tags: [analyzer, web, await, WEB-RUNTIME-04, compile-time-error]
dependency_graph:
  requires:
    - "03-01 (iron_threads.c web guards)"
    - "03-04 (portability probe CI)"
    - "02-06 (IronBuildTarget enum in cli/build.h)"
  provides:
    - "IronBuildTarget threaded through iron_analyze() — reused by Phase 10 LoadTexture check"
    - "web_await_check pass: compile-time E0501 for await-on-web"
  affects:
    - "src/analyzer/analyzer.h (signature change — all callers updated)"
    - "src/cli/build.c (passes opts.target)"
    - "src/cli/check.c (passes IRON_TARGET_NATIVE)"
    - "tests/unit/test_comptime.c (updated call site)"
tech_stack:
  added: []
  patterns:
    - "BFS reachability from main entry point using stb_ds shmap for visited set and chain stack"
    - "Diagnostic code namespace: E0501 for web-specific await reachability error"
    - "Pass registration pattern: include + call after iron_concurrency_check, before iron_iface_collect"
key_files:
  created:
    - src/analyzer/web_await_check.h
    - src/analyzer/web_await_check.c
    - tests/unit/test_web_await_check.c
  modified:
    - src/analyzer/analyzer.h
    - src/analyzer/analyzer.c
    - src/cli/build.c
    - src/cli/check.c
    - CMakeLists.txt
    - tests/unit/CMakeLists.txt
    - tests/unit/test_comptime.c
    - .github/workflows/web.yml
decisions:
  - "Direct #include of cli/build.h in analyzer.h (not a forwarding header) — acceptable inversion for a single enum typedef; alternatives would be scope creep"
  - "Unit test calls iron_web_await_check directly (not iron_analyze) — mirrors test_concurrency.c pattern; avoids handcrafted-AST resolve failures in earlier pipeline steps"
  - "Ordering fix: E0501 block added before iron_iface_collect so early exit on web-await error does not leak iface registry work"
  - "test_comptime.c updated with IRON_TARGET_NATIVE as the new trailing arg — zero behavioral change, tests pass"
metrics:
  duration: "23 minutes"
  completed: "2026-04-11"
  tasks_completed: 2
  files_changed: 11
---

# Phase 3 Plan 3: Analyzer Target Plumbing and web_await_check Pass Summary

**One-liner:** BFS await-reachability pass emitting E0501 on `--target=web` with call-chain trace, wired via new `IronBuildTarget target` parameter on `iron_analyze()`.

## What Was Built

### Task 1: Analyzer target plumbing + web_await_check pass

Seven coordinated edits landing WEB-RUNTIME-04:

1. **`src/analyzer/analyzer.h`** — added `#include "cli/build.h"` and extended `iron_analyze()` signature with trailing `IronBuildTarget target` parameter. Updated doc comment to describe the new parameter.

2. **`src/analyzer/web_await_check.h`** (new) — public header declaring `iron_web_await_check(program, arena, diags, target)`.

3. **`src/analyzer/web_await_check.c`** (new, 206 lines) — full BFS reachability implementation:
   - `WebAwaitCtx` struct with stb_ds shmap for `func_by_name` and `visited`, plus stb_ds array for the call-chain stack
   - `emit_await_error()` renders the chain (`main -> A -> B -> fn`) and emits `IRON_DIAG_E0501_AWAIT_ON_WEB` (code 501) with all required substrings: `--target=web`, `in function`, `reached from main via`, `Emscripten`
   - `scan_node()` handles: `IRON_NODE_AWAIT` (emit + recurse), `IRON_NODE_CALL` (follow named callees via BFS), `IRON_NODE_BLOCK`, plus key statement containers (`VAL_DECL`, `VAR_DECL`, `RETURN`, `ASSIGN`, `IF`, `WHILE`, `FOR`)
   - `iron_web_await_check()` entry: zero-cost early return on `IRON_TARGET_NATIVE`; BFS from `main` function

4. **`src/analyzer/analyzer.c`** — added `#include "analyzer/web_await_check.h"`, updated `iron_analyze()` definition, inserted Step 5.5 call between `iron_concurrency_check` and `iron_iface_collect` with early-return on error.

5. **`src/cli/build.c`** — threaded `opts.target` as the new trailing argument.

6. **`src/cli/check.c`** — passed `IRON_TARGET_NATIVE` (ironc check is not target-aware).

7. **`CMakeLists.txt`** — `src/analyzer/web_await_check.c` added immediately after `src/analyzer/concurrency.c` in the `iron_compiler` source list.

**Additional fix:** `tests/unit/test_comptime.c` had two `iron_analyze` call sites with the old 7-arg signature; both updated to pass `IRON_TARGET_NATIVE`.

### Task 2: Unity test + CMake registration + web.yml paths extension

1. **`tests/unit/test_web_await_check.c`** (new, 249 lines) — 3 Unity test cases using handcrafted AST nodes (matching `test_concurrency.c` pattern):
   - `test_await_in_main_on_web_errors`: main body contains `IRON_NODE_AWAIT`; asserts `error_count >= 1`, `has_code(501)`, `msg_contains("--target=web")`
   - `test_await_in_main_on_native_no_501`: same fixture, `IRON_TARGET_NATIVE`; asserts no code 501
   - `test_no_await_on_web_ok`: program with `helper()` and `main()` both empty; asserts no code 501 on web

2. **`tests/unit/CMakeLists.txt`** — registered `test_web_await_check` executable, ctest target, and `unit` label.

3. **`.github/workflows/web.yml`** — extended `paths:` filter in both `push:` and `pull_request:` blocks with 15 new entries covering all Phase 3 source files and the new analyzer + test files.

## Verification

```
cmake --build build                            EXIT 0
ctest -R test_web_await_check -V               3/3 PASS
ctest -E benchmark_smoke                       63/63 PASS (0 regressions)
grep -q "IronBuildTarget target" analyzer.h    PASS
grep -q "iron_web_await_check" analyzer.c      PASS
grep -q "web_await_check.c" CMakeLists.txt     PASS
grep -q "test_web_await_check.c" web.yml       PASS
! grep -Fq "4.0.23" web_await_check.c          PASS (pin discipline)
```

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 1 - Bug] Wrong field names in scan_node for Iron_IfStmt**
- **Found during:** Task 1 first build
- **Issue:** Used `then_block` and `else_block` — actual AST struct has `body` and `else_body`; `elif_conds`/`elif_bodies` also needed
- **Fix:** Corrected to `is->body`, `is->else_body`, added elif iteration
- **Files modified:** `src/analyzer/web_await_check.c`
- **Commit:** d8a98b8 (included in Task 1)

**2. [Rule 1 - Bug] Non-existent IRON_NODE_EXPR_STMT / Iron_ExprStmt**
- **Found during:** Task 1 first build
- **Issue:** Attempted to handle `IRON_NODE_EXPR_STMT` — this node kind does not exist in the Iron AST
- **Fix:** Removed the case; expression statements in Iron are already unwrapped at the block level
- **Files modified:** `src/analyzer/web_await_check.c`
- **Commit:** d8a98b8 (included in Task 1)

**3. [Rule 1 - Bug] test_comptime.c call sites not updated**
- **Found during:** Task 1 build (compilation failure)
- **Issue:** `tests/unit/test_comptime.c` had 2 direct `iron_analyze()` calls with the old 7-arg signature; the plan did not list this file
- **Fix:** Both sites updated to pass `IRON_TARGET_NATIVE` as the trailing argument
- **Files modified:** `tests/unit/test_comptime.c`
- **Commit:** d8a98b8 (included in Task 1)

**4. [Rule 3 - Blocking] CMake reconfigure needed after test CMakeLists.txt update**
- **Found during:** Task 2 build
- **Issue:** `cmake --build build --target test_web_await_check` failed with "No rule to make target" because CMake cache was stale
- **Fix:** Ran `cmake -S . -B build` to reconfigure before building the new target
- **Impact:** None — build succeeded after reconfigure

## Self-Check: PASSED

Files verified:
- `src/analyzer/web_await_check.h` — EXISTS
- `src/analyzer/web_await_check.c` — EXISTS
- `tests/unit/test_web_await_check.c` — EXISTS
- Task 1 commit d8a98b8 — EXISTS
- Task 2 commit ac975ef — EXISTS
