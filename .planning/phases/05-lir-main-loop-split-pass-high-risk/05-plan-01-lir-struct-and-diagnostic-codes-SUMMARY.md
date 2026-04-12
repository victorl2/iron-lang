---
phase: 05-lir-main-loop-split-pass-high-risk
plan: 01
subsystem: lir-struct, diagnostics
tags: [lir, diagnostics, web-target, struct-extension, error-codes]
dependency_graph:
  requires: []
  provides:
    - IronLIR_Func.web_frame_captures field (src/lir/lir.h)
    - IronLIR_Func.web_frame_capture_count field (src/lir/lir.h)
    - IRON_ERR_WEB_MULTIPLE_MAIN_LOOPS (700) error code
    - IRON_ERR_WEB_NON_CANONICAL_MAIN_LOOP (701) error code
    - IRON_ERR_WEB_NESTED_MAIN_LOOP (702) error code
    - IRON_ERR_WEB_MAIN_LOOP_WRONG_FUNCTION (703) error code
  affects:
    - Plan 02 (web_main_loop_split pass writes to web_frame_captures)
    - Plan 03 (unit tests assert on IRON_ERR_WEB_* codes by symbol)
    - Phase 6 emit_web.c (reads web_frame_captures to synthesize frame state struct)
tech_stack:
  added: []
  patterns:
    - memset zero-init covers new struct fields (no allocation-site edits needed)
    - #define constants in diagnostics.h public catalog (not static-private like Phase 3's E0501)
key_files:
  created: []
  modified:
    - src/lir/lir.h
    - src/diagnostics/diagnostics.h
decisions:
  - "web_frame_captures kept separate from capture_metadata: capture_metadata signals lifted-lambda to emit_c.c; reusing it on Iron_main would corrupt native lambda emission (research trigger 3)"
  - "700 range chosen for Phase 5 web errors: 604 is current max, 700 range is free; upward from 704 reserved for Phase 6+"
  - "Single IRON_ERR_WEB_NON_CANONICAL_MAIN_LOOP code covers all non-while shapes (for-loops, compound conditions) — avoids HIR-level loop-shape plumbing into LIR (approach a per 05-RESEARCH.md open question 5)"
  - "No lir.c edits needed: memset(fn, 0, sizeof(*fn)) at lir.c:122 zero-initializes all new struct fields automatically"
metrics:
  duration: 1039s
  completed: "2026-04-11"
  tasks_completed: 2
  tasks_total: 2
  files_modified: 2
---

# Phase 05 Plan 01: LIR Struct and Diagnostic Codes Summary

Two purely additive header edits laying the foundation for the Phase 5 LIR main-loop split pass: `IronLIR_Func` extended with web frame-capture metadata fields, and four IRON_ERR_WEB_* error codes registered in the public diagnostics catalog.

## Tasks Completed

| Task | Name | Commit | Files |
|------|------|--------|-------|
| 1 | Extend IronLIR_Func with web_frame_captures metadata fields | a364cd0 | src/lir/lir.h |
| 2 | Define IRON_ERR_WEB_* error codes in the 700 range | 75554aa | src/diagnostics/diagnostics.h |

## What Was Built

### Task 1 — IronLIR_Func struct extension

Added two trailing fields to `struct IronLIR_Func` in `src/lir/lir.h` (after the existing `capture_metadata`/`capture_count` fields):

- `Iron_CaptureEntry *web_frame_captures` — populated by Plan 02's pass for the canonical `while(!WindowShouldClose())` loop; NULL on every other function
- `int web_frame_capture_count` — count of entries; 0 when NULL

The fields are zero-initialized automatically by the existing `memset(fn, 0, sizeof(*fn))` call in `iron_lir_func_create()` at `lir.c:122`. No allocation-site edits were needed or made. The comment block explains why these fields are deliberately separate from `capture_metadata` (to avoid corrupting lifted-lambda emission in `emit_c.c`).

### Task 2 — IRON_ERR_WEB_* error codes

Added a new section at the end of `src/diagnostics/diagnostics.h` (before the `#endif` guard) defining four codes in the 700 range:

| Code | Value | Meaning |
|------|-------|---------|
| IRON_ERR_WEB_MULTIPLE_MAIN_LOOPS | 700 | Function with InitWindow has 2+ canonical while(!WindowShouldClose()) loops |
| IRON_ERR_WEB_NON_CANONICAL_MAIN_LOOP | 701 | Loops found but none match the canonical shape (covers for-loops, compound conditions) |
| IRON_ERR_WEB_NESTED_MAIN_LOOP | 702 | Canonical loop found but nested inside another loop |
| IRON_ERR_WEB_MAIN_LOOP_WRONG_FUNCTION | 703 | Canonical loop is in a function other than the one containing InitWindow |

## Deviations from Plan

None — plan executed exactly as written.

## Verification Results

All success criteria confirmed:

- `grep -q "web_frame_captures" src/lir/lir.h` — PASSED
- `grep -q "web_frame_capture_count" src/lir/lir.h` — PASSED
- `grep -q "IRON_ERR_WEB_MULTIPLE_MAIN_LOOPS" src/diagnostics/diagnostics.h` — PASSED
- New fields are inside `struct IronLIR_Func` — PASSED
- `! grep -q "web_frame_captures" src/lir/lir.c` — PASSED (no lir.c edits)
- `! grep -q "web_frame_captures" src/lir/emit_c.c` — PASSED (emit_c.c untouched)
- `cmake --build build` exits 0 — PASSED
- `ctest --test-dir build -E benchmark_smoke` exits 0 (65 tests, 32/32 unit) — PASSED
- Pin discipline (`! grep -F "4.0.23" src/lir/lir.h`) — PASSED
- `git diff src/lir/emit_c.c` empty — PASSED

## Self-Check: PASSED
