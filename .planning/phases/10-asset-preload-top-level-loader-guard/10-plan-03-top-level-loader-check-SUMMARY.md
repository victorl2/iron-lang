---
phase: 10-asset-preload-top-level-loader-guard
plan: "03"
subsystem: analyzer
tags: [analyzer, web, diagnostics, raylib, asset-preload]
dependency_graph:
  requires:
    - src/parser/ast.h
    - src/diagnostics/diagnostics.h
    - src/util/arena.h
    - src/cli/build.h
    - src/analyzer/analyzer.c
  provides:
    - src/analyzer/web_top_level_loader_check.h
    - src/analyzer/web_top_level_loader_check.c
  affects:
    - src/analyzer/analyzer.c
    - tests/unit/test_web_top_level_loader_check.c
    - tests/unit/CMakeLists.txt
    - CMakeLists.txt
    - .github/workflows/web.yml
tech_stack:
  added: []
  patterns:
    - "Single-purpose analyzer pass (sibling to web_await_check.c, not an extension)"
    - "Target-gated early return: zero overhead on IRON_TARGET_NATIVE"
    - "Module-level AST walk skipping IRON_NODE_FUNC_DECL entirely"
    - "strcmp exact-match on callee name (case-sensitive, rejects LoadTexture2D)"
    - "Diagnostic code #502 mirrors #501 convention from web_await_check"
key_files:
  created:
    - src/analyzer/web_top_level_loader_check.h
    - src/analyzer/web_top_level_loader_check.c
    - tests/unit/test_web_top_level_loader_check.c
  modified:
    - src/analyzer/analyzer.c
    - tests/unit/CMakeLists.txt
    - CMakeLists.txt
    - .github/workflows/web.yml
    - .planning/ROADMAP.md
    - .planning/REQUIREMENTS.md
decisions:
  - "Pass is implemented as infrastructure: Iron v1.1.0-alpha has no module-level executable statements, so the check is vacuously true in practice. Tests use synthetic VAL_DECL + call fixtures that the parser never produces."
  - "IRON_NODE_FUNC_DECL nodes are skipped at the top-level loop, not recursed into. This is the key correctness invariant: calls inside function bodies are allowed; only calls outside any function body are forbidden."
  - "Error code 502 defined locally in web_top_level_loader_check.c as IRON_DIAG_E0502_TOP_LEVEL_LOADER_ON_WEB, mirroring the 501 pattern in web_await_check.c."
  - "Pass registered as Step 5.6 immediately after iron_web_await_check (Step 5.5) in analyzer.c, sharing the same error-count short-circuit pattern."
metrics:
  duration: "~20 minutes"
  completed: "2026-04-11"
  tasks_completed: 1
  files_created: 3
  files_modified: 6
---

# Phase 10 Plan 03: Top-Level Loader Check Summary

**One-liner**: New `iron_web_top_level_loader_check` analyzer pass emitting E0502 when `LoadTexture`/`LoadSound`/`LoadFont`/`LoadModel` appears at module level on `--target=web`, registered as pipeline Step 5.6 after the `await` check.

## What Was Built

A new single-purpose analyzer pass (`web_top_level_loader_check.{h,c}`) that gates raylib resource-loader calls from appearing at module level when targeting web. The pass mirrors the design of `web_await_check.c`: target-gated early return on native, top-level AST walk, `iron_diag_emit` with a unique error code (502), and a clear actionable message.

The pass is registered in `iron_analyze()` immediately after `iron_web_await_check` as Step 5.6. Four Unity unit tests cover: (1) top-level `LoadTexture` on web emits E0502 with correct message text, (2) same fixture on native emits nothing, (3) `LoadTexture` inside `func main()` on web emits nothing (the function-body skip invariant), (4) all four forbidden names at top-level on web each emit an error.

## Implementation Note: Vacuous Check

As of Iron v1.1.0-alpha the language has no module-level executable statements. All executable code lives inside function bodies. The `for` loop over `program->decls[]` will only ever see `IRON_NODE_FUNC_DECL` nodes from valid parsed programs, all of which are skipped. The check is therefore vacuously a no-op for the current language surface.

The pass is implemented as infrastructure: if Iron gains module-level `val`/`var` declarations with call-expression initialisers (or standalone expression statements at module level), the guard will fire without any further changes. The unit tests build synthetic AST fixtures (top-level `VAL_DECL` with a call as `init`) to verify the detection logic exercises the intended code paths despite the current language restriction.

## Tasks Completed

| Task | Description | Outcome |
|------|-------------|---------|
| 1 | Create `web_top_level_loader_check.{h,c}` | Done — 31-line header, 185-line implementation |
| 2 | Register pass in `analyzer.c` after `iron_web_await_check` | Done — Step 5.6 with error-count short-circuit |
| 3 | Update `CMakeLists.txt` iron_compiler sources | Done — added after `web_await_check.c` |
| 4 | Write 4-case Unity test `test_web_top_level_loader_check.c` | Done — all 4 pass |
| 5 | Register test in `tests/unit/CMakeLists.txt` | Done — after `test_web_await_check` block |
| 6 | Update `web.yml` paths filter | Done — 3 new entries in both push and pull_request |
| 7 | Mark Phase 10 complete in ROADMAP.md | Done — [x], 3/3, completed 2026-04-11 |
| 8 | Mark WEB-ASSET-01..05 Complete in REQUIREMENTS.md | Done — all 5 requirement rows updated |

## Deviations from Plan

**1. [Rule 1 - Bug] Iron_ValDecl has `declared_type` not `resolved_type`**

- **Found during:** Building the test file (first compile attempt)
- **Issue:** The test helper `make_top_level_val_decl` initialised `vd->resolved_type = NULL` but the struct field is named `declared_type` (set by type checker, per ast.h line 271)
- **Fix:** Changed to `vd->declared_type = NULL`
- **Files modified:** `tests/unit/test_web_top_level_loader_check.c`
- **Commit:** included in the single atomic commit

## Self-Check: PASSED

- `src/analyzer/web_top_level_loader_check.h` — EXISTS
- `src/analyzer/web_top_level_loader_check.c` — EXISTS
- `tests/unit/test_web_top_level_loader_check.c` — EXISTS
- `ctest -L unit` — 35/35 PASSED (including 1 new test: `test_web_top_level_loader_check`)
- ROADMAP.md Phase 10: `[x]`, `3/3`, `Complete`, `2026-04-11`
- REQUIREMENTS.md WEB-ASSET-01..05: all `[x]` and `Complete` in coverage table
