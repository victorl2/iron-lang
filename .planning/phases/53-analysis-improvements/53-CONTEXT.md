# Phase 53: Analysis Improvements - Context

**Gathered:** 2026-04-08
**Status:** Ready for planning

<domain>
## Phase Boundary

Strengthen the compiler's analysis passes to cross function boundaries. Monomorphic detection tracks concrete types through function return values and parameters with heuristic-gated specialization. Value range analysis propagates return ranges to call sites and narrows ranges through conditional branches (including AND chains). Does NOT add new language features, change syntax, or modify the emitter sub-modules from Phase 52.

</domain>

<decisions>
## Implementation Decisions

### Interprocedural monomorphic detection
- **Track returns + parameters** — if `buildCircles()` only returns `[Circle]`, callers get monomorphic collapse. If all callers of `process(shapes: [Shape])` pass `[Circle]`, the parameter is monomorphic inside the function.
- **Heuristic-gated specialization** — when a function parameter is monomorphic at a subset of call sites, generate specialized copies ONLY when:
  - Function is small (under ~50 LIR instructions)
  - Few call sites (1-2)
  - Function body has significant dispatch overhead (for-loops, type switches)
  - When conditions are NOT met: use union of all call-site types (conservative, no duplication)
- **Existing `specialization_registry`** (Phase 49) prevents duplicate specialized function bodies

### Return range propagation
- **Single-pass scan** — scan all functions once, collect return ranges from RETURN instructions. For each function, return range = union of all its return statement ranges. Call sites use that range. O(n), no iteration.
- **One-level unrolling for recursion** — for recursive functions, analyze the base case (non-recursive return paths) and use that range. Ignore recursive return paths. More precise than TOP for simple recursion patterns.

### Conditional range narrowing
- **Simple comparisons + AND chains** — handle: `if x < 100` → [min,99] in true, [100,max] in false. `if x == 5` → [5,5] in true. Also accumulate narrowings across `&&` short-circuit block chains (block_0 narrows, block_1 inherits + narrows further). Standard SSA dominator-based propagation.
- **Skip OR union merging** — `||` requires union-at-join complexity. Deferred to future work.
- **Else branches get complementary range** — `if x < 100` means else branch gets [100, max]. Both branches benefit.

### Claude's Discretion
- Exact LIR instruction threshold for specialization heuristic
- How to detect "significant dispatch overhead" in function bodies
- Call graph construction approach (scan all CALL instructions vs build formal graph)
- How to handle indirect calls (closures) — likely conservative TOP

</decisions>

<canonical_refs>
## Canonical References

**Downstream agents MUST read these before planning or implementing.**

### Analysis modules
- `src/lir/value_range.h` — Current value range API: `iron_vr_analyze`, `iron_vr_get_narrowed_type`, `iron_vr_free`
- `src/lir/value_range.c` — Current implementation: per-function analysis, CONST_INT/CONSTRUCT/SET_FIELD/PHI/ADD/SUB/MUL handling, type ladder. Must be extended for conditional narrowing and call-site return ranges.
- `src/lir/emit_c.c` — Monomorphic detection pre-scan (~line 5090-5150): `monomorphic_collections` hash map, local ARRAY_LIT analysis. Must be extended for interprocedural tracking.

### Specialization infrastructure (Phase 49)
- `src/lir/emit_c.c` — `specialization_registry` hash map, dispatch function deduplication

### Emitter sub-modules (Phase 52)
- `src/lir/emit_helpers.h` — EmitCtx struct with all hash map fields
- `src/lir/emit_split.c` — Split collection emission (affected by monomorphic collapse)

### LIR infrastructure
- `src/lir/lir.h` — LIR instruction kinds (BRANCH_IF, CMP_LT, CMP_GT, CMP_EQ, RETURN, CALL)
- `src/lir/lir.c` — Value table, block structure, function representation

</canonical_refs>

<code_context>
## Existing Code Insights

### Reusable Assets
- **ValueRangeAnalysis** (`value_range.h/c`): Already handles per-function dataflow with overflow-safe arithmetic. Extend with conditional narrowing and interprocedural return ranges.
- **monomorphic_collections** (`emit_c.c`): Existing local detection. Extend with cross-function tracking.
- **specialization_registry** (`emit_c.c`): Already prevents duplicate specialized functions. Reuse for parameter-specialized copies.
- **Iron_IfaceRegistry** (`iface_collect.h`): Tracks alive implementors per interface. Used for type set analysis.

### Established Patterns
- **Pre-scan passes in emit_c.c**: Monomorphic detection is a pre-scan. Interprocedural extension fits this pattern — scan all functions first, then apply results.
- **stb_ds hash maps**: Used for all tracking. Return range map, call-site type map follow same pattern.

### Integration Points
- **value_range.c**: Add conditional narrowing to existing per-function analysis. Add return range collection.
- **emit_c.c**: Extend monomorphic pre-scan with cross-function data. Add specialization heuristic.
- **CMakeLists.txt**: No new files expected — extending existing modules.

</code_context>

<specifics>
## Specific Ideas

- Return range collection can be a separate pass before the per-function analysis — compute function return ranges first, then per-function analysis uses them at CALL instruction sites
- Conditional narrowing happens at BRANCH_IF instructions — check if the condition is a CMP with a constant, narrow the variable's range in the target block
- AND chain accumulation: when block B is dominated by block A and both branch on the same variable, B's entry range is A's narrowed range
- The specialization heuristic could count BRANCH_IF instructions that dispatch on collection element tags as "significant dispatch overhead"

</specifics>

<deferred>
## Deferred Ideas

- OR union merging at join points — needs phi-like range merging, deferred
- Per-call-site specialization without heuristic gate — full monomorphization, defer to benchmarking phase
- Closure parameter tracking — indirect calls through `Iron_Closure` are too complex for this phase

</deferred>

---

*Phase: 53-analysis-improvements*
*Context gathered: 2026-04-08*
