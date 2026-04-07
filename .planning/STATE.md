---
gsd_state_version: 1.0
milestone: v0.1.1-alpha
milestone_name: Collection Methods, Full Captures & Layout Optimizations
status: not_started
stopped_at: null
last_updated: "2026-04-07"
last_activity: 2026-04-07 -- Milestone v0.1.1-alpha started
progress:
  total_phases: 0
  completed_phases: 0
  total_plans: 0
  completed_plans: 0
  percent: 0
---

# Project State

## Project Reference

See: .planning/PROJECT.md (updated 2026-04-07)

**Core value:** The programmer writes polymorphic code against interfaces; the compiler emits monomorphic, data-oriented C code with no vtables, no heap indirection, and no pointer chasing.
**Current focus:** Defining requirements for v0.1.1-alpha

## Current Position

Phase: Not started (defining requirements)
Plan: —
Status: Defining requirements
Last activity: 2026-04-07 — Milestone v0.1.1-alpha started

Progress: [░░░░░░░░░░] 0%

## Performance Metrics

**Velocity:**
- Total plans completed: 0
- Average duration: --
- Total execution time: 0 hours

## Accumulated Context

### Decisions

- [v0.1-alpha]: Core dispatch with tagged unions — shipped
- [v0.1-alpha]: Collection splitting with per-type sub-arrays — shipped
- [v0.1-alpha]: Prefetch insertion for split loops — shipped
- [v0.1-alpha]: OpenMP removed — Iron's own threading model should be used instead
- [v0.1.1-alpha]: Method syntax for collection operations (arr.map(...).filter(...).sum())
- [v0.1.1-alpha]: Full closure capture support included in this milestone
- [v0.1.1-alpha]: Basic captures only insufficient — need mutable var by ref, nested, recursive, closures-as-fields

### Pending Todos

None yet.

### Blockers/Concerns

- Lambda capture gaps: mutable var capture, closures returned from functions, closures as fields, nested lambdas — all needed for collection methods to work fully
- Collection methods need generic method dispatch on array types — Iron doesn't have method syntax on built-in types yet
- SoA layout requires field-level access pattern analysis within loop bodies — static analysis complexity

## Session Continuity

Last session: 2026-04-07
Stopped at: Defining requirements for v0.1.1-alpha
Resume file: None
