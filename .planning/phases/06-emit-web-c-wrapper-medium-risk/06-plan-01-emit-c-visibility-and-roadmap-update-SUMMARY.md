---
phase: 06-emit-web-c-wrapper-medium-risk
plan: "01"
subsystem: lir-emission
tags: [visibility, static-to-extern, emit_helpers, roadmap]
dependency_graph:
  requires: [Phase 5 web_frame_captures metadata]
  provides: [emit_func_signature extern, emit_func_body extern, emit_instr extern]
  affects: [src/lir/emit_c.c, src/lir/emit_helpers.h]
tech_stack:
  added: []
  patterns: [extern declaration in shared header, zero-behavior visibility promotion]
key_files:
  created: []
  modified:
    - src/lir/emit_c.c
    - src/lir/emit_helpers.h
decisions:
  - "Removed static from emit_instr, emit_func_signature, and emit_func_body — pure visibility change, zero behavioral impact"
  - "Extern declarations placed immediately before the #endif guard in emit_helpers.h under a Phase 6-labeled section"
  - "Task 2 (ROADMAP.md SC3/SC4) executed as no-op verify — planner had pre-populated the correct language"
metrics:
  duration: 681s
  completed: "2026-04-11"
  tasks_completed: 2
  files_changed: 2
---

# Phase 6 Plan 01: emit_c.c Visibility Promotion and ROADMAP Update Summary

Promoted `emit_func_signature`, `emit_func_body`, and `emit_instr` from `static` to public in `src/lir/emit_c.c` and declared them in `src/lir/emit_helpers.h`. This is a pure visibility change — zero behavioral impact — that enables Plan 02's `emit_web.c` to reuse the native emitter's function-body and per-instruction dispatch paths via `#include "lir/emit_helpers.h"` alone.

## Tasks Completed

| Task | Description | Commit | Files |
|------|-------------|--------|-------|
| 1 | Promote 3 static helpers to extern + declare in emit_helpers.h | 744c57e | src/lir/emit_c.c, src/lir/emit_helpers.h |
| 2 | Verify ROADMAP.md Phase 6 SC3/SC4 language (no-op: already correct) | — (no file change) | .planning/ROADMAP.md |

## Exact Changes Made

### src/lir/emit_c.c — 3 static keyword removals

| Function | Original line | Change |
|----------|---------------|--------|
| `emit_instr` | 687 | `static void emit_instr(` → `void emit_instr(` |
| `emit_func_signature` | 4163 | `static void emit_func_signature(` → `void emit_func_signature(` |
| `emit_func_body` | 4209 | `static void emit_func_body(` → `void emit_func_body(` |

No other lines in emit_c.c were touched.

### src/lir/emit_helpers.h — insertion point

Declarations inserted immediately before the final `#endif /* IRON_EMIT_HELPERS_H */` guard, after the existing `emit_ctx_cleanup` declaration. A new section header `/* ── Function + instruction emission (shared with emit_web.c, Phase 6) ── */` was added to make the promotion self-documenting.

### .planning/ROADMAP.md — Task 2 outcome

Task 2 was a **no-op verify**. All four invariant checks passed without any edits:
- `grep -c "PR #17 merged"` → 2 (>= 1 required)
- `grep -c "non-web emission behavior is unchanged"` → 1 (>= 1 required)
- `grep -c "zero touched lines"` → 0 (required)
- `grep -c "emit_web_module"` → 4 (>= 1 required)

The planner had pre-populated the target SC3/SC4 language during plan finalization.

## Verification Results

### Build
`cmake --build build` — clean build, zero new warnings vs. pre-edit baseline.
Only pre-existing warning: `ld: warning: ignoring duplicate libraries: '-lpthread', 'libiron_runtime.a'` (pre-existing, unrelated).

### Test Suite
`ctest --test-dir build -E benchmark_smoke` — **100% tests passed, 0 tests failed out of 66**.

### Symbol Linkage (nm check)
```
00000000000100a0 T _emit_func_body
000000000000fd90 T _emit_func_signature
0000000000002800 T _emit_instr
```
All three promoted symbols show `T` (global text) linkage, not `t` (local/static). Confirmed callable from external translation units.

### negated static grep
`! grep -E '^static.*emit_func_(signature|body)|^static.*emit_instr' src/lir/emit_c.c` — passes (no static definitions remain for these functions).

### Pin discipline
`grep -F "4.0.23" src/lir/emit_c.c src/lir/emit_helpers.h` — returns empty (no new occurrences of the pinned version string).

## Deviations from Plan

None — plan executed exactly as written. Task 2 was pre-identified as potentially a no-op, and it was.

## Self-Check: PASSED

- [x] `src/lir/emit_c.c` exists and contains `void emit_func_body(EmitCtx *ctx` (no static)
- [x] `src/lir/emit_helpers.h` exists and contains `void emit_func_body(EmitCtx`
- [x] Commit `744c57e` exists in git log
- [x] 66/66 tests passing confirmed
- [x] All three symbols show `T` linkage in `nm`
