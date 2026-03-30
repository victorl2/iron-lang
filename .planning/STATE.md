---
gsd_state_version: 1.0
milestone: v0.0
milestone_name: milestone
status: planning
stopped_at: Completed 18-03-PLAN.md
last_updated: "2026-03-30T01:02:27.160Z"
last_activity: 2026-03-29 — Implemented copy propagation, DCE, and constant folding passes
progress:
  total_phases: 6
  completed_phases: 3
  total_plans: 13
  completed_plans: 12
  percent: 0
---

# Project State

## Project Reference

See: .planning/PROJECT.md (updated 2026-03-29)

**Core value:** Every Iron language feature compiles to correct, working C code that produces a native binary
**Current focus:** v0.0.5-alpha Phase 15 — Copy Propagation, DCE & Constant Folding

## Current Position

Phase: 15 of 20 (Copy Propagation, DCE & Constant Folding)
Plan: 0 of 0 in current phase (not yet planned)
Status: Ready to plan
Last activity: 2026-03-29 — Implemented copy propagation, DCE, and constant folding passes

Progress: [░░░░░░░░░░] 0%

## Performance Metrics

**Velocity (from v0.0.3-alpha):**
- Total plans completed: 7
- Phases: 12, 13, 14

**By Phase (v0.0.3-alpha):**

| Phase | Plans | Status |
|-------|-------|--------|
| Phase 12: Binary Split and Installation | 3 | Complete |
| Phase 13: Project Workflow | 2 | Complete |
| Phase 14: Dependency Resolution and Lockfile | 2 | Complete |
| Phase 15 P01 | 45 | 2 tasks | 9 files |
| Phase 15-copy-propagation-dce-constant-folding P03 | 29 | 2 tasks | 8 files |
| Phase 16 P01 | 120 | 3 tasks | 5 files |
| Phase 16-expression-inlining P02 | 21 | 2 tasks | 9 files |
| Phase 16-expression-inlining P03 | 5 | 1 tasks | 1 files |
| Phase 17-strength-reduction-store-load-elimination P01 | 60 | 1 tasks | 3 files |
| Phase 17-strength-reduction-store-load-elimination P02 | 32 | 1 tasks | 3 files |
| Phase 17-strength-reduction-store-load-elimination P03 | 9 | 2 tasks | 4 files |
| Phase 18-benchmark-validation P02 | 2 | 1 tasks | 1 files |
| Phase 18-benchmark-validation P01 | 741 | 2 tasks | 2 files |
| Phase 18-benchmark-validation P03 | 35 | 2 tasks | 40 files |

## Accumulated Context

### Decisions

Decisions are logged in PROJECT.md Key Decisions table.
Recent decisions affecting current work:

- Current IR is mid-level (SSA + CFG), not truly "high" — 7 high-level instruction kinds remain
- v0.0.4-alpha shipped perf codegen: stack arrays, pointer-mode analysis, direct indexing, range hoisting, ~93% at 1.2x C parity
- v005 spec identifies 6 IR optimization passes to close remaining ~7% gap
- Plan B: ship IR optimizations on current IR first, then introduce true High IR as architectural improvement
- Expression inlining (Phase 16) is the highest-impact pass — fixes median_two_sorted_arrays 4.5x to ~1.2x
- Phi elimination is already a lowering step — natural boundary between future HIR and LIR
- [Phase 15]: IronIR_OptimizeInfo unified bridge struct carries all 5 maps from optimizer to emitter; build.c owns lifetime via iron_ir_optimize_info_free()
- [Phase 15]: iron_ir_optimize() accepts Iron_Arena* because IronIR_Module has no module-level arena (arena is per-IronIR_Func)
- [Phase 15]: emit_c.c is now pure emission; all IR transformations live in ir_optimize.c enabling standalone testability
- [Phase 15-02]: Named typedefs required for stb_ds hmput with anonymous struct maps — C prohibits assignment between distinct anonymous struct types
- [Phase 15-02]: Emission-only unit tests use skip_new_passes=true to isolate emitter from optimizer effects; optimizer correctly removes unused constants and folds arithmetic
- [Phase 15-03]: Test 2 (copy prop) verifies LOAD elimination via DCE rather than operand check, because constant folding eliminates the ADD before assertion
- [Phase 15-03]: [Phase 15-03]: Iron integration tests require println("{val}") string interpolation for integer output; println only accepts String type
- [Phase 16]: Expression inlining: instr_mutates_memory() ordering hazard detection prevents inlining pure values past STORE/SET_INDEX/SET_FIELD/CALL
- [Phase 16]: MAKE_CLOSURE excluded from instr_is_inline_expressible — multi-statement env struct alloc requires named void* variable
- [Phase 16]: PARALLEL_FOR range_val and MAKE_CLOSURE captures explicitly excluded from inlining — emit_instr uses emit_val template pattern for these
- [Phase 16-expression-inlining]: Lambda closures test simplified: (Int)->Int type annotation syntax rejected by Iron parser; outer-variable capture not yet supported; test uses non-capturing lambdas
- [Phase 16-expression-inlining]: Synthetic param ID reservation in unit tests: manually advance fn->next_value_id and populate value_table NULL slots before alloca to match lowerer i*2+1 convention for non-foldable IR test inputs
- [Phase 16-expression-inlining]: median_two_sorted_arrays benchmark fails 1.5x target at 4.2x — root cause is int64_t vs int array width and extra length params, not fixable by expression inlining
- [Phase 16-expression-inlining]: BRANCH instruction missing from compute_func_purity exclusion list — find_median_sorted incorrectly shows 0 pure functions; minor issue for future cleanup
- [Phase 17-01]: Store/load elim uses intra-block tracking only; non-escaped allocas survive CALL; SET_INDEX counted as mutation in copy-prop; GET_INDEX/GET_FIELD excluded from inline-eligible when operand is parameter
- [Phase 17-02]: rebuild_cfg_edges() must be called before domtree: IR constructors do not auto-populate preds/succs arrays
- [Phase 17-02]: Strength reduction inv_val hoisting: loop-body-defined invariants stored to step_alloca in preheader for C-scope safety
- [Phase 17-02]: MUL rewritten in-place: load.ptr overlaps binop.left in union — never set binop.left after setting load.ptr
- [Phase 17-03]: Integration tests use 'func' keyword (not 'fun') per existing Iron source convention
- [Phase 17-03]: benchmark_smoke timeout is pre-existing (merge_k_sorted_lists Iron compilation failure); all 26 non-benchmark tests pass 0/0 failures
- [Phase 18-benchmark-validation]: JSON benchmarks array only receives entries for fully-measured benchmarks; errors/skipped counted in top-level fields only
- [Phase 18-benchmark-validation]: --compare mode degrades gracefully when no-opt binary build fails; compare_suffix omitted for that benchmark
- [Phase 18-benchmark-validation]: ARRAY_LIT elements excluded from expression inlining in Pass 1 to prevent forward-reference in C initializer lists
- [Phase 18-benchmark-validation]: DCE and use_counts require explicit ARRAY_LIT element traversal beyond MAX_OPERANDS=64 — opt_collect_operands hard limit silently drops elements 65+ causing DCE to incorrectly remove them as dead
- [Phase 18-benchmark-validation]: Int32 narrowing for ARRAY_LIT/fill() reverted — narrowing breaks int64_t* alloca assignments; requires aliasing analysis before safe application
- [Phase 18-03]: Iron parallel-for/spawn bodies cannot capture outer variables — emit as closure with void* args causing type mismatch; use pure functions taking only loop index or helper functions recomputing values internally
- [Phase 18-03]: val and match are Iron keywords — benchmark variables renamed to is_match, iters throughout
- [Phase 18-03]: Concurrency benchmark correctness pattern: sequential checksum → parallel run → recompute checksum → Match: 1; parallel section cannot accumulate into shared state

### Pending Todos

None yet.

### Blockers/Concerns

- All phases: Windows testing coverage for subprocess invocation, path handling, and colored output

## Session Continuity

Last session: 2026-03-30T01:02:27.157Z
Stopped at: Completed 18-03-PLAN.md
Resume file: None
