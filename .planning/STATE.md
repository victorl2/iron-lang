---
gsd_state_version: 1.0
milestone: v0.1.1-alpha
milestone_name: Collection Methods, Full Captures & Layout Optimizations
status: in_progress
stopped_at: "Completed 47-03-PLAN.md"
last_updated: "2026-04-07"
last_activity: 2026-04-07 -- Completed 47-03 method chaining and split collection dispatch
progress:
  total_phases: 5
  completed_phases: 1
  total_plans: 3
  completed_plans: 3
  percent: 20
---

# Project State

## Project Reference

See: .planning/PROJECT.md (updated 2026-04-07)

**Core value:** The programmer writes polymorphic code against interfaces; the compiler emits monomorphic, data-oriented C code with no vtables, no heap indirection, and no pointer chasing.
**Current focus:** Phase 46 - Full Closure Capture (ready to plan)

## Current Position

Phase: 47 of 50 (Collection Methods) -- COMPLETE
Plan: 3 of 3
Status: Phase 47 complete, ready for Phase 46
Last activity: 2026-04-07 -- Completed 47-03 method chaining and split collection dispatch

Progress: [▓▓░░░░░░░░] 20%

## Performance Metrics

**Velocity:**
- Total plans completed: 3
- Average duration: 25min
- Total execution time: 1.3 hours

| Phase | Plan | Duration | Tasks | Files |
|-------|------|----------|-------|-------|
| 47-collection-methods | 01 | 7min | 2 | 4 |
| 47-collection-methods | 02 | 20min | 2 | 15 |
| 47-collection-methods | 03 | 49min | 1 | 19 |

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
- [47-03]: Inline split collection dispatch in C emitter (not via function calls)
- [47-03]: Cross-type map/reduce generics via [U] method-level generic parameter
- [47-03]: Filter on split collections returns new split collection

### Pending Todos

None yet.

### Blockers/Concerns

- Lambda capture gaps: mutable var capture, closures returned from functions, closures as fields, nested lambdas -- all needed for collection methods to work fully
- Array extension method syntax added (func [T].method(...)) -- type checker resolves from decls with heuristic fallback
- SoA layout requires field-level access pattern analysis within loop bodies -- static analysis complexity

## Session Continuity

Last session: 2026-04-07
Stopped at: Completed 47-03-PLAN.md (method chaining and split collection dispatch)
Resume file: None
