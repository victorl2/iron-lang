---
gsd_state_version: 1.0
milestone: v0.1.1-alpha
milestone_name: Collection Methods, Full Captures & Layout Optimizations
status: in_progress
stopped_at: "Completed 47-02-PLAN.md"
last_updated: "2026-04-07"
last_activity: 2026-04-07 -- Completed 47-02 collection method runtime implementations
progress:
  total_phases: 5
  completed_phases: 0
  total_plans: 3
  completed_plans: 2
  percent: 14
---

# Project State

## Project Reference

See: .planning/PROJECT.md (updated 2026-04-07)

**Core value:** The programmer writes polymorphic code against interfaces; the compiler emits monomorphic, data-oriented C code with no vtables, no heap indirection, and no pointer chasing.
**Current focus:** Phase 46 - Full Closure Capture (ready to plan)

## Current Position

Phase: 47 of 50 (Collection Methods)
Plan: 2 of 3
Status: In progress
Last activity: 2026-04-07 -- Completed 47-02 collection method runtime implementations

Progress: [▓▓░░░░░░░░] 14%

## Performance Metrics

**Velocity:**
- Total plans completed: 2
- Average duration: 14min
- Total execution time: 0.5 hours

| Phase | Plan | Duration | Tasks | Files |
|-------|------|----------|-------|-------|
| 47-collection-methods | 01 | 7min | 2 | 4 |
| 47-collection-methods | 02 | 20min | 2 | 15 |

## Accumulated Context

### Decisions

- [v0.1-alpha]: Core dispatch with tagged unions -- shipped
- [v0.1-alpha]: Collection splitting with per-type sub-arrays -- shipped
- [v0.1-alpha]: Prefetch insertion for split loops -- shipped
- [v0.1-alpha]: OpenMP removed -- Iron's own threading model should be used instead
- [v0.1.1-alpha]: Method syntax for collection operations (arr.map(...).filter(...).sum())
- [v0.1.1-alpha]: Full closure capture before collection methods (lambdas needed as callbacks)
- [v0.1.1-alpha]: Layout optimizations parallel to collection methods (both build on v0.1-alpha split collections)

- [47-01]: __Array sentinel type_name for array extension methods (minimal downstream impact)
- [47-01]: Helper functions for decl-based type resolution + heuristic fallback (backward compatible)
- [47-02]: C runtime implementations for collection methods (compiler lacks generic function monomorphization)
- [47-02]: memcpy-based closure fn pointer casting to avoid -Wcast-function-type-mismatch
- [47-02]: Skip type checking of array extension method stubs (generic T not resolvable in global scope)

### Pending Todos

None yet.

### Blockers/Concerns

- Lambda capture gaps: mutable var capture, closures returned from functions, closures as fields, nested lambdas -- all needed for collection methods to work fully
- Array extension method syntax added (func [T].method(...)) -- type checker resolves from decls with heuristic fallback
- SoA layout requires field-level access pattern analysis within loop bodies -- static analysis complexity

## Session Continuity

Last session: 2026-04-07
Stopped at: Completed 47-02-PLAN.md (collection method runtime implementations)
Resume file: None
