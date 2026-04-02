---
phase: 22-struct-codegen-fix
plan: 01
subsystem: HIR/LIR codegen
tags: [struct, codegen, hir-to-lir, emit-c, bug-fix]
dependency_graph:
  requires: []
  provides: [STRUCT-01, STRUCT-02]
  affects: [game_loop_headless, quicksort, hash_map, concurrent_hash_map, graph_bfs_dfs]
tech_stack:
  added: []
  patterns: [defensive clamp, remove incorrect special case]
key_files:
  modified:
    - src/hir/hir_to_lir.c
    - src/lir/emit_c.c
decisions:
  - "Removed spurious OBJECT-returning-call-as-constructor special case entirely — IRON_HIR_EXPR_CONSTRUCT already handles real constructors"
  - "Both CONSTRUCT emission sites in emit_c.c were clamped (second site discovered during implementation)"
  - "effective_field_count clamp accounts for field_start offset to handle struct inheritance correctly"
metrics:
  duration: 78s
  completed: 2026-03-31
  tasks_completed: 2
  files_modified: 2
---

# Phase 22 Plan 01: Struct Codegen Fix Summary

**One-liner:** Removed spurious CALL-as-CONSTRUCT detection in hir_to_lir.c and added defensive field_count clamp in emit_c.c to fix struct codegen memory corruption.

## Tasks Completed

| Task | Name | Commit | Files |
|------|------|--------|-------|
| 1 | Remove spurious constructor detection in hir_to_lir.c | afb0b7c | src/hir/hir_to_lir.c |
| 2 | Add defensive field_count clamp in emit_c.c CONSTRUCT emission | 8276101 | src/lir/emit_c.c |

## Changes Made

### Task 1 — hir_to_lir.c

Deleted the 17-line block at lines 714–730 that incorrectly treated all function calls returning `IRON_TYPE_OBJECT` as struct constructors. This block checked `type->kind == IRON_TYPE_OBJECT` and emitted a `CONSTRUCT` instruction regardless of whether the callee was actually a type name or a regular function returning a struct. After removal, all function calls (including those returning structs) fall through to normal `CALL` lowering at line 732+. Real constructors continue to be handled by the `IRON_HIR_EXPR_CONSTRUCT` case at line ~903 (shifted up by 17 lines).

### Task 2 — emit_c.c

Added `effective_field_count` clamp to both CONSTRUCT emission sites (line 653 and line 1714). The clamp ensures the loop iterates at most `od->field_count + field_start` times, preventing out-of-bounds reads into `od->fields[]` when `instr->construct.field_count` exceeds the declared struct field count. The `field_start` offset correctly accounts for the `_base` field in structs with inheritance (`extends_name`).

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 1 - Bug] Second CONSTRUCT emission site also required the clamp**

- **Found during:** Task 2
- **Issue:** The plan identified one CONSTRUCT emission site (line 654). During implementation, `replace_all` revealed a second identical site at line 1714 in emit_c.c — likely for a different emission context (e.g., emit_stmt vs emit_expr).
- **Fix:** Applied `effective_field_count` clamp to both sites using `replace_all: true`.
- **Files modified:** src/lir/emit_c.c
- **Commit:** 8276101

## Verification Results

- `grep "object constructor call TypeName" src/hir/hir_to_lir.c` — 0 matches (confirmed removed)
- `grep "effective_field_count" src/lir/emit_c.c` — 8 matches (3 per site × 2 + 2 declarations)
- `ninja` in build/ — completed with exit code 0, no compilation errors
- `IRON_HIR_EXPR_CONSTRUCT` case confirmed untouched at line ~903

## Self-Check: PASSED

- src/hir/hir_to_lir.c modified: confirmed
- src/lir/emit_c.c modified: confirmed
- Commit afb0b7c exists: confirmed
- Commit 8276101 exists: confirmed
- Build clean: confirmed
