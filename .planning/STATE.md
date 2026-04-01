---
gsd_state_version: 1.0
milestone: v0.0
milestone_name: milestone
status: completed
stopped_at: Completed 28-01-PLAN.md
last_updated: "2026-04-01T14:03:51.921Z"
last_activity: 2026-04-01 — Dead alloca elimination pass implemented; 36% reduction in count_components temporaries
progress:
  total_phases: 7
  completed_phases: 5
  total_plans: 6
  completed_plans: 6
  percent: 71
---

# Project State

## Project Reference

See: .planning/PROJECT.md (updated 2026-03-31)

**Core value:** Every Iron language feature compiles to correct, working C code that produces a native binary
**Current focus:** v0.0.7-alpha Performance Optimization — Phase 24 (Range Bound Hoisting) ready to plan

## Current Position

Phase: 28 of 30 (Phi Elimination Improvement)
Plan: 01 complete
Status: Phase complete
Last activity: 2026-04-01 — Dead alloca elimination pass implemented; 36% reduction in count_components temporaries

Progress: [██████████░░] 71%

## Performance Metrics

**Velocity (from v0.0.6-alpha):**
- Total plans completed: 5 (phases 21-23)
- Phases: 21, 22, 23

## Accumulated Context

### Decisions

Decisions are logged in PROJECT.md Key Decisions table.
Recent decisions affecting current work:

- [Phase 22-struct-codegen-fix]: Constructor detection must check callee FUNC_REF's own type not call result type
- [Phase 23-01]: mutable heap var alloca typed as RC(T); val_is_heap_ptr() follows LOAD->ALLOCA chain; method self-arg dereferenced at call site when callee expects value type
- [Phase 23-02]: CONST_NULL with IRON_TYPE_OBJECT emits zero-initialized struct ({0}); range-for defer wrap in IRON_HIR_STMT_BLOCK for correct scope ordering
- [v0.0.7-alpha roadmap]: Phase ordering: P1 (hoisting) -> P4 (fill) -> P5 (LOAD) -> P0 (inlining) -> P2 (phi) -> P3 (int32) -> benchmark. Research-recommended order. Phi and int32 come after all four primary passes are committed.
- [v0.0.7-alpha roadmap]: Function inlining (Phase 27) carries highest risk — mandatory value ID remap via fresh next_value_id++, recursion guard, array param mode restriction. Verify find_root param mode before planning.
- [Phase 24-range-bound-hoisting]: count_alloca placed in pre_header while active block, LOAD replaces GET_FIELD in header — bound computed once per loop entry
- [Phase 25-stack-array-promotion]: Apply constant-count gate in both optimizer AND emitter pre-scan: dynamic fills must be excluded in both places to prevent type mismatch for escaping fills
- [Phase 25-stack-array-promotion]: fill_hoisted map pattern: fill() declaration hoisting mirrors phi_hoisted — entry declarations, init-only at call site; reusable pattern for any array declaration hoisting
- [Phase 26-load-expression-inlining]: LOAD blanket exclusion removed: cross-block guard at lir_optimize.c:1779-1785 already handles dangerous case; blanket exclusion was redundant
- [Phase 26-load-expression-inlining]: bug_vla_goto_bypass pre-existing: confirmed failing before phase 26 on commit c333493; deferred to separate fix (VLA declaration hoisting needed)
- [Phase 27-function-inlining]: Threshold 30 (not 20): phi_eliminate adds ~9 extra param alloca/store pairs per function; source-level instruction count is unreliable for threshold decisions
- [Phase 27-function-inlining]: Result alloca must be inserted at call_block/call_idx BEFORE block split — placement in merge block causes C declaration-after-use errors
- [Phase 27-function-inlining]: Step 9 result_remap applied to ALL original caller blocks, not just cont — CALL results may be referenced in branch target blocks (earlier block indices)
- [Phase 27-function-inlining]: emit_c.c backward-ref hoisting extended to ALLOCA and all value-producing instrs; use_block_min requires comprehensive operand tracking across all instruction kinds
- [Phase 27]: is_hoisted guard must be applied to ALL value-producing instruction cases in emit_instr — any instruction can be backward-ref hoisted when inlining rearranges blocks
- [Phase 28-phi-elimination]: run_dead_alloca_elimination must be placed AFTER compute_escape_set in lir_optimize.c — C99 requires function to be declared before use (no implicit declaration allowed)
- [Phase 28-phi-elimination]: GET_INDEX/SET_INDEX/GET_FIELD/SET_FIELD uses of an alloca must mark it as live (same as LOAD) — prevents removing arrays mutated via index/field ops without explicit LOAD
- [Phase 28-phi-elimination]: Post-fixpoint single pass is sufficient for dead alloca elimination — copy-prop already removed single-store loads; inside-fixpoint placement adds marginal benefit only

### Pending Todos

None yet.

### Blockers/Concerns

- [Phase 27 planning]: Verify actual array parameter mode for `find_root(parent, x)` in connected_components before implementing inlining — if ARRAY_PARAM_LIST, the initial restriction blocks the primary benchmark target
- [Phase 25 planning]: Inspect generated C for connected_components before writing new code — stack array promotion path may already work for fill(50, 0) and only need declaration placement fix

## Session Continuity

Last session: 2026-04-01T13:45:00.000Z
Stopped at: Completed 28-01-PLAN.md
Resume file: None
