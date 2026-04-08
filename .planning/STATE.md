---
gsd_state_version: 1.0
milestone: v0.1.2-alpha
milestone_name: Compiler Hardening & Refactoring
status: defining_requirements
stopped_at: Milestone v0.1.2-alpha started
last_updated: "2026-04-08T14:00:00.000Z"
last_activity: 2026-04-08 -- Milestone v0.1.2-alpha started
progress:
  total_phases: 0
  completed_phases: 0
  total_plans: 0
  completed_plans: 0
  percent: 0
---

# Project State

## Project Reference

See: .planning/PROJECT.md (updated 2026-04-08)

**Core value:** The programmer writes polymorphic code against interfaces; the compiler emits monomorphic, data-oriented C code with no vtables, no heap indirection, and no pointer chasing.
**Current focus:** Defining requirements for v0.1.2-alpha Compiler Hardening & Refactoring

## Current Position

Phase: Not started (defining requirements)
Plan: —
Status: Defining requirements
Last activity: 2026-04-08 -- Milestone v0.1.2-alpha started

Progress: [░░░░░░░░░░] 0%

## Performance Metrics

**Velocity:**
- Total plans completed: 7
- Average duration: 22min
- Total execution time: 2.6 hours

| Phase | Plan | Duration | Tasks | Files |
|-------|------|----------|-------|-------|
| 47-collection-methods | 01 | 7min | 2 | 4 |
| 47-collection-methods | 02 | 20min | 2 | 15 |
| 47-collection-methods | 03 | 49min | 1 | 19 |
| Phase 48-layout-optimizations P01 | 24min | 2 tasks | 6 files |
| Phase 48-layout-optimizations P02 | 21min | 2 tasks | 7 files |
| Phase 48-layout-optimizations P03 | 28min | 2 tasks | 11 files |
| Phase 48-layout-optimizations P04 | 7min | 1 task | 5 files |
| Phase 49-loop-fusion-monomorphic-specialization P01 | 21min | 3 tasks | 12 files |
| Phase 49-loop-fusion-monomorphic-specialization P02 | 25min | 2 tasks | 17 files |
| Phase 49 P03 | 26min | 2 tasks | 7 files |
| Phase 50-value-range-compression-arena-allocation P01 | 14min | 2 tasks | 9 files |
| Phase 50-value-range-compression-arena-allocation P02 | 11min | 2 tasks | 3 files |
| Phase 50-value-range-compression-arena-allocation P03 | 8min | 2 tasks | 4 files |

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
- [Phase 48]: Interprocedural method analysis: scan interface method implementations for self.field accesses to determine used fields in split collections
- [Phase 48]: Object reconstruction from reduced storage: zero-init + field copy for tagged union dispatch compatibility
- [Phase 48]: Common field shared arrays disabled when any implementor uses SoA (per-type vs shared count divergence)
- [Phase 48]: SoA per-field arrays: <lower>_<field> naming pattern for cache-contiguous field storage
- [Phase 48]: Layout annotations as comma-separated array type attributes: [T, layout: soa/aos] and [T, unordered]
- [Phase 48]: Large variant pointer indirection: >2x smallest AND >64 bytes triggers heap allocation in tagged union
- [Phase 48]: Arena-allocated keys for stb_ds string hash maps to prevent stack-use-after-scope
- [Phase 49]: @fusible annotation via IRON_TOK_AT token + identifier check; chain detection pre-scan with STORE/LOAD propagation and escape analysis; chain-interior skip deferred to Plan 02
- [Phase 49]: Fused loop emission placed before emit_instr as static helper function
- [Phase 49]: Split collection pre-population with STORE/LOAD propagation moved before chain detection
- [Phase 49]: Per-node type tracking arrays for correct lambda typedef casting in fused loops
- [Phase 49]: Monomorphic collapse to standard Iron_List path (not plain typed array) preserves type compatibility with downstream interface dispatch
- [Phase 49]: Conservative monomorphic detection: only ARRAY_LIT-local collections, no parameters, escape analysis via CALL/RETURN/SET_FIELD/MAKE_CLOSURE
- [Phase 50]: Conservative TOP for function call return values (no interprocedural call-site analysis)
- [Phase 50]: Union semantics for field ranges across all functions; any TOP path kills compression
- [Phase 50]: STORE/LOAD tracking via per-alloca range map for variable-based value flow
- [Phase 50]: Emit tracking helpers as static inline in generated C (not linked from compiler arena.c)

### Roadmap Evolution

- Phase 51 added: Memory Investigation & Leak Audit — user reported ironc consuming 50GB+ memory during compilation
- [Phase 50]: Inline pointer registry in SplitList (_tracked/count/cap) instead of embedding full Iron_Arena
- [Phase 50]: 1.5x geometric growth factor for split collection sub-arrays (reduced from 2x)
- [Phase 50]: VRC integration test requires dead field to trigger Stor struct emission where compression applies

### Pending Todos

None yet.

### Blockers/Concerns

- Lambda capture gaps: mutable var capture, closures returned from functions, closures as fields, nested lambdas -- all needed for collection methods to work fully
- Array extension method syntax added (func [T].method(...)) -- type checker resolves from decls with heuristic fallback
- SoA layout requires field-level access pattern analysis within loop bodies -- static analysis complexity

## Session Continuity

Last session: 2026-04-08T11:41:52.294Z
Stopped at: Completed 50-03-PLAN.md
Resume file: None
