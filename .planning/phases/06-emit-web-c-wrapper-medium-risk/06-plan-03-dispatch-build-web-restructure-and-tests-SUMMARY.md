---
phase: 06-emit-web-c-wrapper-medium-risk
plan: 03
subsystem: cli/build + lir/emit_web + tests/unit + ci
tags: [web-target, emit, dispatch, test, ci]
dependency_graph:
  requires:
    - "06-plan-01: emit_c.c visibility promotion + emit_helpers.h"
    - "06-plan-02: emit_web_module() implementation"
  provides:
    - "iron_build() dispatches to emit_web_module on --target=web"
    - "Phase 2 stub short-circuit removed; full LIR pipeline runs for web target"
    - "test_emit_web Unity suite proves WEB-EMIT-05..08 end-to-end"
    - "web.yml CI triggers on all Phase 6 files"
  affects:
    - "src/cli/build.c (iron_build entry + emit step)"
    - "src/cli/build_web.c (stub stub message removed)"
    - "tests/unit/ (new test_emit_web target)"
    - ".github/workflows/web.yml (paths filter)"
tech_stack:
  added: []
  patterns:
    - "emit dispatch: if (opts.target == IRON_TARGET_WEB) -> emit_web_module else -> iron_lir_emit_c"
    - "TDD: write test with hand-built LIR fixture, run against live emit_web_module"
    - "Unity snapshot testing with strstr advancing-pointer pattern for textual order"
key_files:
  created:
    - tests/unit/test_emit_web.c
  modified:
    - src/cli/build.c
    - src/cli/build_web.c
    - src/cli/build_web.h
    - tests/unit/CMakeLists.txt
    - .github/workflows/web.yml
decisions:
  - "Option (a) chosen for build_web.c restructuring: kept iron_build_web as preflight (not renamed, not inlined). iron_build() calls iron_build_web() at entry when target==WEB; returns early on non-zero; falls through on 0. Rationale: smallest possible diff, no public header rename, Phase 7 can evolve iron_build_web into real emcc orchestration."
  - "IRON_SOURCE_DIR compile definition added to test_emit_web CMake target so the boundary invariant sub-test (test_emit_web_clean_boundary_invariant) can resolve ../src/lir/emit_web.c from the build directory."
  - "test_emit_web_clean_boundary_invariant uses TEST_IGNORE on fopen failure (not TEST_FAIL) — primary invariant is already guarded by Plan 02's verify command; the test sub-test is belt-and-braces."
metrics:
  duration: "1018s"
  completed: "2026-04-12"
  tasks_completed: 3
  files_changed: 5
---

# Phase 6 Plan 3: Dispatch + Build Web Restructure and Tests Summary

Close Phase 6 by wiring `emit_web_module` dispatch in `build.c`, removing the Phase 2 stub short-circuit, adding a 6-test Unity snapshot suite, and extending the web CI paths filter for all Phase 6 files.

## What Was Built

### Task 1: Wire dispatch in build.c, remove Phase 2 stub short-circuit

**`src/cli/build.c` edits:**

1. **Added include** at line 37: `#include "lir/emit_web.h"` (alongside `emit_c.h`).

2. **Removed Phase 2 stub short-circuit** at `iron_build()` entry (was lines 649-652):
   ```c
   // REMOVED:
   if (opts.target == IRON_TARGET_WEB) {
       return iron_build_web(source_path, output_path, opts);
   }
   ```

3. **Added preflight call** at `iron_build()` entry:
   ```c
   if (opts.target == IRON_TARGET_WEB) {
       int pre_rc = iron_build_web(source_path, output_path, opts);
       if (pre_rc != 0) return pre_rc;
       /* fall through — the pipeline runs for both targets */
   }
   ```

4. **Updated comment block** at the `iron_lir_web_main_loop_split` call site (was the "NOTE: today this code path runs only on native" comment). Updated to past-tense: "Phase 6 plan 03 removed the Phase 2 stub short-circuit at iron_build() entry, so this call now runs for BOTH targets."

5. **Wired emit dispatch** at step 8 (previously line 1066-1070):
   ```c
   const char *c_src;
   if (opts.target == IRON_TARGET_WEB) {
       c_src = emit_web_module(ir_module, &arena, &diags, &optimize_info,
                               &analysis.iface_registry,
                               opts.warn_fusion_break,
                               opts.report_compression);
   } else {
       c_src = iron_lir_emit_c(ir_module, &arena, &diags, &optimize_info,
                               &analysis.iface_registry,
                               opts.warn_fusion_break,
                               opts.report_compression);
   }
   ```

**`src/cli/build_web.c` edits:**

- Removed the Phase 2 stub banner print: `printf("Phase 2: CLI + TOML scaffold complete; real compilation in Phase 7\n")`.
- Replaced section 8 comment with `/* 8. Preflight success: iron_build() will run the main pipeline. */`.
- Removed `(void)opts;` from the original stub (it silenced unused-parameter on what was truly unused); added it back because `opts` is still unused in the preflight (opts consumed by caller for target check; Phase 7 uses the release flags). Compiler confirmed this is required.

**`src/cli/build_web.h`**: Updated doc comment from "dispatch function / stub" to "preflight" role.

**Restructuring option chosen: (a)** — kept `iron_build_web` as the preflight function name without renaming to `iron_web_preflight`. Rationale: zero public API churn, Phase 7 can evolve the function directly.

### Task 2: New Unity test suite test_emit_web.c

**`tests/unit/test_emit_web.c`** — 6 Unity test functions:

| Test | What It Asserts |
|------|-----------------|
| `test_emit_web_emits_emscripten_include` | `#include <emscripten/emscripten.h>` present (WEB-EMIT-06) |
| `test_emit_web_emits_main_loop_arg_with_zero_zero` | `emscripten_set_main_loop_arg(` present; `, 0, 0)` within 200 chars (WEB-EMIT-07) |
| `test_emit_web_shutdown_branch_textual_order` | Inside `_frame_cb(void *state_arg)`: `WindowShouldClose()` → `CloseWindow()` → `iron_runtime_shutdown()` → `free(` → `emscripten_cancel_main_loop()` in order (WEB-EMIT-08) |
| `test_emit_web_frame_state_struct_named_after_func` | `FrameState_main_loop` and `score` present (WEB-EMIT-05) |
| `test_emit_web_no_iron_main_function` | `void Iron_main_loop(` absent; `int main(` present |
| `test_emit_web_clean_boundary_invariant` | `emit_web.c` does not `#include "lir/emit_c.h"` (WEB-EMIT-05 structural) |

Fixture: function `main_loop` with captured local `score`, canonical `while(!WindowShouldClose())` CFG.

`ctest -R test_emit_web`: **6/6 PASS** (0 failures, 0 ignored after adding `IRON_SOURCE_DIR` to CMake target).

**`tests/unit/CMakeLists.txt`**: registered `test_emit_web` with `iron_compiler unity` and `IRON_SOURCE_DIR` compile definition.

### Task 3: Extend .github/workflows/web.yml paths filter

Added 5 entries to **both** `push.paths` and `pull_request.paths` filters (10 new lines total):

```yaml
- 'src/lir/emit_c.c'
- 'src/lir/emit_helpers.h'
- 'src/lir/emit_web.c'
- 'src/lir/emit_web.h'
- 'tests/unit/test_emit_web.c'
```

Each entry appears exactly 2 times in the file (verified: `grep -c ... == 2` for all 5).

## Verification Results

- `cmake --build build`: clean, no new errors or warnings (pre-existing linker dedup warning for `-lpthread` is unrelated).
- `ctest --test-dir build -R "^test_emit_web$"`: 6/6 PASS.
- `ctest --test-dir build -E benchmark_smoke`: **100% (67 tests passing)** — no regression from removing the stub short-circuit.
- `nm build/libiron_compiler.a | grep emit_web_module`: shows `T _emit_web_module` (symbol exported with T linkage).
- `grep -c "emit_web_module" src/cli/build.c`: 4 (comment + call site + emit comment + include reference).
- `grep -c "Phase 2 stub complete" src/cli/build_web.c`: 0.
- `grep -c "src/lir/emit_web.c" .github/workflows/web.yml`: 2.
- Pin discipline: `grep -rF "4.0.23"` over all edited files returns empty.

## Expected Failures After Phase 6

On `--target=web` for a real Iron source file, `iron_build()` now runs the full pipeline through `emit_web_module()` at step 12 successfully. However, step 13 (`invoke_clang`) fails because native `clang` cannot find `<emscripten/emscripten.h>`. This is expected and correct — Phase 7 replaces `invoke_clang` with `emcc` orchestration.

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 1 - Bug] Restored (void)opts; in build_web.c**
- **Found during:** Task 1 build
- **Issue:** The plan instructed to remove `(void)opts;` (which silenced unused-parameter when the stub was a no-op). The function signature still takes `IronBuildOpts opts` but nothing in the preflight body reads from it directly. The compiler issued `-Wunused-parameter -Werror` and rejected the build.
- **Fix:** Added `(void)opts;` back with updated comment explaining the Phase 7 intent.
- **Files modified:** `src/cli/build_web.c`
- **Commit:** included in `5728e25`

**2. [Rule 2 - Missing functionality] Added IRON_SOURCE_DIR define to test_emit_web CMake target**
- **Found during:** Task 2 test run
- **Issue:** The boundary invariant test (`test_emit_web_clean_boundary_invariant`) used `TEST_IGNORE` when `emit_web.c` was not accessible from the build working directory. The `IRON_SOURCE_DIR` macro is only defined for `ironc`, not test targets.
- **Fix:** Added `target_compile_definitions(test_emit_web PRIVATE IRON_SOURCE_DIR="${CMAKE_SOURCE_DIR}/src")` to CMakeLists.txt. This made the boundary invariant test run as PASS instead of IGNORE.
- **Files modified:** `tests/unit/CMakeLists.txt`
- **Commit:** included in `1100965`

## Self-Check: PASSED

- `/Users/victor/code/worker-3/iron-lang/tests/unit/test_emit_web.c`: FOUND
- `/Users/victor/code/worker-3/iron-lang/.planning/phases/06-emit-web-c-wrapper-medium-risk/06-plan-03-dispatch-build-web-restructure-and-tests-SUMMARY.md`: FOUND
- Commit `5728e25`: feat(06-03): wire emit_web_module dispatch — FOUND
- Commit `1100965`: test(06-03): add test_emit_web snapshot suite — FOUND
- Commit `5f0fd20`: chore(06-03): extend web.yml paths filter — FOUND
