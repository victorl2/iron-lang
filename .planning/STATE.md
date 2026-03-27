---
gsd_state_version: 1.0
milestone: v1.1
milestone_name: High IR
status: defining
stopped_at: null
last_updated: "2026-03-27"
last_activity: "2026-03-27 — Milestone v1.1 started"
progress:
  total_phases: 0
  completed_phases: 0
  total_plans: 0
  completed_plans: 0
  percent: 0
---

# Project State

## Project Reference

See: .planning/PROJECT.md (updated 2026-03-27)

**Core value:** Every Iron language feature compiles to correct, working C code that produces a native binary
**Current focus:** Milestone v1.1 — High IR

## Current Position

Phase: Not started (defining requirements)
Plan: —
Status: Defining requirements
Last activity: 2026-03-27 — Milestone v1.1 started

## Accumulated Context

### Decisions

Decisions are logged in PROJECT.md Key Decisions table.
Recent decisions affecting current work:

- v1.0 codegen emits C directly from AST — this is the path being replaced
- Current codegen split across gen_stmts.c, gen_exprs.c with Iron_Codegen context struct
- Full codegen replacement chosen over dual-path — clean architectural break
- High IR should be SSA-form, C-agnostic, designed for future LLVM lowering
- Future pipeline: source → AST → High IR → Low IR (LLVM) → bytecode/C

### v1.0 Accumulated Context (preserved)

- stb_ds used throughout for hash maps and dynamic arrays
- Arena allocator is primary allocation strategy in compiler
- Codegen uses fprintf to emit C text directly
- Runtime library is separate CMake target (iron_runtime)
- Integration tests: iron build → execute binary → compare stdout
- Cross-platform: pthreads on all platforms (pthreads4w on Windows)

### Pending Todos

None yet.

### Blockers/Concerns

None yet.

## Session Continuity

Last session: 2026-03-27
Stopped at: Milestone v1.1 initialization
Resume file: None
