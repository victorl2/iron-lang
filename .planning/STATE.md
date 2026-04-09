---
gsd_state_version: 1.0
milestone: v0.0
milestone_name: milestone
status: executing
stopped_at: Completed 56-02-PLAN.md
last_updated: "2026-04-09T22:55:26.779Z"
last_activity: 2026-04-09 -- Phase 56 Plan 02 complete (narrowing audit + Phase 55 workaround cleanup)
progress:
  total_phases: 20
  completed_phases: 10
  total_plans: 28
  completed_plans: 28
  percent: 100
---

# Project State

## Project Reference

See: .planning/PROJECT.md (updated 2026-04-09)

**Core value:** The programmer writes polymorphic code against interfaces; the compiler emits monomorphic, data-oriented C code with no vtables, no heap indirection, and no pointer chasing.
**Current focus:** Defining requirements for v0.1.3-alpha Known Limitations Cleanup

## Current Position

Phase: 56-monomorphic-method-chain
Plan: 02 complete (MONO-FIX-01/02 fully satisfied via Plan 01 + Plan 02 jointly — narrowing audit closes the silent-miscompilation gap)
Status: Executing v0.1.3-alpha
Last activity: 2026-04-09 -- Phase 56 Plan 02 complete (narrowing audit + Phase 55 workaround cleanup)

Progress: [██████████] 100%

## Performance Metrics

**Velocity:**
- Total plans completed: 22
- Average duration: 22min
- Total execution time: ~6.9 hours

| Phase | Plan | Duration | Tasks | Files |
|-------|------|----------|-------|-------|
| 47-collection-methods | 01 | 7min | 2 | 4 |
| 47-collection-methods | 02 | 20min | 2 | 15 |
| 47-collection-methods | 03 | 49min | 1 | 19 |
| 48-layout-optimizations | 01 | 24min | 2 | 6 |
| 48-layout-optimizations | 02 | 21min | 2 | 7 |
| 48-layout-optimizations | 03 | 28min | 2 | 11 |
| 48-layout-optimizations | 04 | 7min | 1 | 5 |
| 49-loop-fusion | 01 | 21min | 3 | 12 |
| 49-loop-fusion | 02 | 25min | 2 | 17 |
| 49-loop-fusion | 03 | 26min | 2 | 7 |
| 50-vrc-arena | 01 | 14min | 2 | 9 |
| 50-vrc-arena | 02 | 11min | 2 | 3 |
| 50-vrc-arena | 03 | 8min | 2 | 4 |
| 52-emitter-refactoring | 01 | 13min | 2 | 4 |
| 52-emitter-refactoring | 02 | 36min | 2 | 4 |
| 52-emitter-refactoring | 03 | 37min | 3 | 7 |
| 53-analysis-improvements | 01 | 39min | 2 | 6 |
| Phase 53 P02 | 52min | 2 tasks | 6 files |
| 53-analysis-improvements | 03 | 11min | 2 | 4 |
| Phase 54-test-hardening P01 | 5min | 2 tasks | 10 files |
| 54-test-hardening | 02 | 9min | 3 | 94 |
| 54-test-hardening | 03 | 6min | 2 | 10 |
| Phase 55-push-on-interface-arrays P01 | 27min | 3 tasks | 13 files |
| Phase 55-push-on-interface-arrays P02 | 10min | 3 tasks | 7 files |
| Phase 55-push-on-interface-arrays P03 | 18min | 3 tasks | 7 files |
| Phase 55.1-empty-typed-array-literal P01 | 23min | 3 tasks | 11 files |
| Phase 56-monomorphic-method-chain P01 | 55min | 3 tasks | 21 files |
| Phase 56-monomorphic-method-chain P02 | 33min | 2 tasks | 13 files |

## Accumulated Context

### Decisions

- [v0.1-alpha]: Core dispatch with tagged unions -- shipped
- [v0.1-alpha]: Collection splitting with per-type sub-arrays -- shipped
- [v0.1.1-alpha]: Method syntax for collection operations
- [v0.1.1-alpha]: Full closure capture before collection methods
- [Phase 49]: Monomorphic collapse to standard Iron_List path (not plain typed array)
- [Phase 49]: Conservative monomorphic detection: only ARRAY_LIT-local collections
- [Phase 50]: Conservative TOP for function call return values (no interprocedural call-site analysis)
- [Phase 50]: Inline pointer registry in SplitList instead of embedding full Iron_Arena
- [Phase 51]: Memory investigation resolved -- no critical leak found
- [Phase 52]: emit_ prefix convention for all shared emitter functions; emit_ctx_cleanup for consolidated resource freeing
- [Phase 52]: Type declaration emission isolated in emit_structs module; estimate_type_size renamed to emit_estimate_type_size
- [Phase 52]: Split collection emission isolated in emit_split module; fusion emission isolated in emit_fusion module
- [Phase 52]: emit_expr_to_buf made non-static for cross-module access from emit_fusion.c
- [Phase 53]: func_ref resolution via value_table for CALL-site return range propagation
- [Phase 53]: Block entry range replace semantics (not intersect) for correct cross-path analysis
- [Phase 53]: Conditional narrowing shared in both collect_return_ranges and analyze_function_ranges
- [Phase 53]: emit_type_to_c uses Iron_SplitList_ for interface arrays (fixes return type mismatch)
- [Phase 53]: Specialization heuristic: <=50 instrs, 1-2 callers, dispatch branches required
- [Phase 53]: Phase A.1 wires CALL results to coll_types via func_return_types for interprocedural type propagation
- [Phase 54]: Monomorphic collapse still emits SplitList infrastructure; verification checks single-type push not SplitList absence
- [Phase 54]: Single-implementor tests use for-loop (monomorphic + .map() chain is pre-existing compiler limitation)
- [Phase 54]: Large collection stress test uses typed Int array push loop (interface arrays don't support runtime push)
- [Phase 54]: Benchmark speed thresholds 1.5x->2.5x across 88 configs to tolerate CI runner variance
- [Phase 54]: SoA+fusion composition uses for-loop path (ordered iteration on SoA has Stor type mismatch bug)
- [Phase 54]: Mono+computation uses for-loop (mono + .map() chain is known compiler limitation)
- [Phase 54]: Mega test exercises split+SoA+dead field+compression+arena via for-loop; fusion deferred
- [Phase 55-push-on-interface-arrays]: Phase 55 Plan 01: Inline _push branch in emit_c.c split-collection interception block, ships both concrete (Mode a) and interface-typed (Mode b) dispatch
- [Phase 55-push-on-interface-arrays]: Phase 55 Plan 01: Mode b tag-switch uses impl->tag (not loop index) and honors ctx->indirect_variants for pointer-stored large payloads
- [Phase 55-push-on-interface-arrays]: Phase 55 Plan 01 tests: use 2-element initial literals with both implementors to avoid monomorphic-collapse path (scoped to Phase 56)
- [Phase 55-push-on-interface-arrays]: Phase 55 Plan 02: _len uses direct _total_count field read (not per-type sum) — authoritative combined-length field, mirrors .count accessor at emit_c.c:1054
- [Phase 55-push-on-interface-arrays]: Phase 55 Plan 02: _pop reads _order[_total_count-1] for (tag, idx), dispatches via switch over impl->tag, boxes via Iron_<Iface>_from_<Type>, decrements per-type count inside case and _order_count/_total_count after switch
- [Phase 55-push-on-interface-arrays]: Phase 55 Plan 02: SoA-implementor pop emits defensive zero-init fallback per 55-CONTEXT.md scoping; AoS is fully implemented, SoA pop is documented known limitation
- [Phase 55-push-on-interface-arrays]: Phase 55 Plan 03: _get mirrors _pop branch structure except no count/order decrement — pure read with tag switch over _order[i] and Iron_<Iface>_from_<Type> wrapping
- [Phase 55-push-on-interface-arrays]: Phase 55 Plan 03: _set uses same-type in-place overwrite with runtime tag check guard; different-type and interface-typed writes are documented silent no-op (known limitation)
- [Phase 55-push-on-interface-arrays]: Phase 55 Plan 03: Plans 01-03 together deliver full PUSH-01 broad scope (push + len + pop + get + set) for interface split collections; interception block now handles every stdlib array method
- [Phase 55.1-empty-typed-array-literal]: check_expr_with_expected helper threads expected types into empty array literal inference at 4 expression contexts; check_expr signature unchanged
- [Phase 55.1-empty-typed-array-literal]: Error code IRON_ERR_EMPTY_LITERAL_NO_TYPE=229 used (not 220 as in CONTEXT placeholder; 220 was already IRON_ERR_NO_SUCH_METHOD)
- [Phase 55.1-empty-typed-array-literal]: Call-arg test uses [Int] not [Shape] because passing interface arrays as function parameters hits a pre-existing codegen gap (deferred to future IFACE-PARAM-01 requirement)
- [Phase 56-monomorphic-method-chain]: Phase 56 Plan 01 emit_mono_list_decls pre-scan runs at END of emit_type_decls() and scans ARRAY_LIT elem_types directly (not just ctx->monomorphic_collections) because Iron type inference resolves [Circle(1),Circle(2)] to [Circle] concrete -- bypasses split_collection_ids and therefore monomorphic_collections
- [Phase 56-monomorphic-method-chain]: Phase 56 Plan 01 emits Iron_List_Iron_<T> struct typedef + IRON_LIST_DECL + IRON_LIST_IMPL for every concrete object element type; dedup via stb_ds set emitted_mono_list_types keyed on mangled name
- [Phase 56-monomorphic-method-chain]: Phase 56 Plan 01 tests use fusion chains for .map/.filter/.forEach -- IRON_LIST_COLL_IMPL macros can't work for struct element types (sum uses + operator, map cross-type return unrepresentable). Phase 49 fusion engine inlines the body into a single flat loop and bypasses the runtime COLL methods entirely.
- [Phase 56-monomorphic-method-chain]: Phase 56 Plan 01 fusion probe outcome: (a) passes out of the box -- Phase 49 fusion engine already supports plain Iron_List<Concrete> path; no emit_fusion.c extension needed
- [Phase 56-monomorphic-method-chain]: Standalone non-fusible .map/.filter/.forEach on struct-element lists is a deferred architectural gap (needs either inline emission mirroring split dispatch OR struct-aware COLL_IMPL variants). Workaround: chain with fusible terminal.
- [Phase 56-monomorphic-method-chain]: Phase 56 Plan 02 validates .push(arg) against array elem type via push_type_compatible helper hooked into both IRON_NODE_METHOD_CALL array branches (ident + chained); prevents silent miscompilation on narrowed mono collections that Plan 01 unblocked
- [Phase 56-monomorphic-method-chain]: Phase 56 Plan 02 adds type_display_name helper in typecheck.c (local to error emission sites) because iron_type_to_string returns literal '<object>' / '<interface>' placeholders; a global fix for the other 41 call sites is deferred to a future dedicated pass
- [Phase 56-monomorphic-method-chain]: Phase 56 Plan 02 introduces tests/compile_fail/ directory for negative tests; files live outside run_tests.sh's scan root and are invoked directly by the plan's verify command
- [Phase 56-monomorphic-method-chain]: Phase 56 Plan 02 Phase 55 audit tally: 1 rewritten (push_interface_len_empty -> Phase 55.1 empty annotated literal path), 7 annotated as load-bearing multi-type (get/set_same_type/len_pop/get_after_push/after_op/typed_var/prepopulated), 4 protected untouched (collection/multi_type/pop_order/loop_100)

### Roadmap Evolution

- Phase 51 inserted: Memory Investigation & Leak Audit (resolved)
- v0.1.2-alpha milestone created: Phases 52-54 (emitter refactoring, analysis improvements, test hardening)

### Pending Todos

None yet.

### Blockers/Concerns

- emit_c.c monolithic problem RESOLVED by Phase 52 (4 sub-modules extracted)
- Value range analysis uses conservative TOP at call boundaries -- RESOLVED by Phase 53 Plan 01
- Monomorphic detection is local-only -- Phase 53 extends it interprocedurally

## Session Continuity

Last session: 2026-04-09T22:55:26.776Z
Stopped at: Completed 56-02-PLAN.md
Resume file: None
