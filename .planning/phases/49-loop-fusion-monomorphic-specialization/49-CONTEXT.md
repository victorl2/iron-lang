# Phase 49: Loop Fusion & Monomorphic Specialization - Context

**Gathered:** 2026-04-07
**Status:** Ready for planning

<domain>
## Phase Boundary

Chained collection operations (`arr.map(...).filter(...).reduce(...)`) compile to a single fused loop per concrete type with no intermediate allocations. Collections proven to hold only one concrete type collapse to plain typed arrays with direct field access — no tag dispatch, no split collection overhead. Includes a `@fusible` annotation system with documentation spec. Does NOT add new collection methods or change existing method signatures.

</domain>

<decisions>
## Implementation Decisions

### Fusion chain recognition
- **Analysis level: LIR** — pattern-match sequences of CALL instructions in emit_c.c, extending the existing split collection method recognition (line 1890+)
- **Chain detection: def-use chain analysis** — build def-use graph, identify sequences where a collection method's result is used only as input to another collection method. More complete than simple result-feeds-self matching — handles intermediate variable storage
- **Fusible method set: `@fusible` annotation** — methods marked `@fusible` in stdlib are eligible for fusion. The compiler extracts the per-element body from annotated functions and composes them into a single fused loop
- Phase must produce `docs/fusible-annotation-spec.md` documenting the annotation, safety requirements, pattern constraints, and per-element body extraction rules
- Initial `@fusible` methods: map, filter, reduce, forEach, sum

### Partial vs full-chain fusion
- **Break at non-fusible boundary** — if a chain contains a non-fusible call (e.g. `map().customFunc().filter()`), fuse the subchains on either side independently. Two fused loops instead of three separate ones.
- **Full fusion including reduce/sum/forEach** — `map(f).filter(g).reduce(init,h)` becomes one loop: transform, test predicate, accumulate. No intermediate array at all. forEach is also a fusible terminal.
- **Opt-in fusion diagnostics** — no warning by default. A compiler flag (`--warn-fusion-break` or similar) shows where chains were split by non-fusible calls.

### Monomorphic collapse
- **Whole-program analysis** — track which concrete types are actually pushed to each collection variable across all functions. Iron's closed-world model makes this feasible — the compiler sees all code.
- **Full collapse to plain array** — when a collection is proven monomorphic, replace `Iron_SplitList_Shape` with `Iron_List_Circle` entirely. No tag, no split overhead, no order array. Direct field access. Matches spec §8.3 exactly.
- **Specialization registry: stb_ds hash map** — key: `(function_name, concrete_type)`, value: emitted C function name. Check before emitting — if already emitted, reuse. Prevents duplicate specialized function bodies.

### Split collection fusion
- **Per-type fused loops** — `shapes.map(f).filter(g).reduce(init,h)` emits: one fused loop for circles, one for squares. Each applies map+filter+reduce in a single pass over that type's sub-array. Preserves cache locality.
- **Preserve split layout for interface results** — when a fused chain on a split collection returns an interface type, each per-type fused loop pushes to its own sub-array in the result split collection.
- **SoA-aware fusion** — if a collection uses SoA layout (Phase 48), fused loops access per-field arrays directly (`particle_x[]`, `particle_y[]`) instead of reconstructing structs.
- **Sequential accumulation for reduce/sum** — run the circle fused loop first (accumulating into acc), then the square loop (continuing from acc). Deterministic, matches non-fused behavior exactly.

### Claude's Discretion
- Exact def-use graph data structure and algorithm
- How `@fusible` body extraction parses the Iron function body at LIR level
- Intermediate allocation detection and elimination mechanics
- Monomorphic analysis propagation across return values and parameters
- How to handle edge cases in SoA-aware fusion (mixed SoA/AoS in same chain)

</decisions>

<canonical_refs>
## Canonical References

**Downstream agents MUST read these before planning or implementing.**

### Loop fusion spec
- `docs/static-interface-dispatch-spec.md` §8.1 (Loop Fusion) — Fusion as code structure decision, fused loop body examples, intermediate elimination
- `docs/static-interface-dispatch-spec.md` §8.3 (Monomorphic Collection Specialization) — Single-type collapse, plain typed array, direct calls

### Collection method infrastructure (Phase 47)
- `src/lir/emit_c.c` lines 1890-2100 — Split collection method dispatch: inline per-type iteration for map/filter/reduce/forEach/sum. This is the code fusion extends.
- `src/runtime/iron_runtime.h` — `Iron_List_*_map/filter/reduce/forEach/sum` C runtime functions. Fusion replaces these with inlined/composed bodies.
- `src/runtime/iron_collections.c` — C runtime implementations of collection methods

### Layout optimization infrastructure (Phase 48)
- `src/lir/layout_analysis.h` — `IronLayoutKind` enum (SoA/AoS), `iron_layout_get_kind()`, `IRON_SOA_THRESHOLD`. Fusion must respect layout decisions.
- `src/lir/layout_analysis.c` — Field access analysis, SoA/AoS selection logic
- `src/lir/emit_c.c` SoA struct emission — per-field arrays that SoA-aware fusion reads directly

### Type system and analysis
- `src/analyzer/iface_collect.c` — Interface registry, alive implementor tracking. Foundation for monomorphic detection.
- `src/analyzer/typecheck.c` — Method call resolution, array type annotation handling

</canonical_refs>

<code_context>
## Existing Code Insights

### Reusable Assets
- **Split collection method dispatch** (`emit_c.c:1890+`): Already pattern-matches collection method calls on split collections and emits inline per-type iteration. Fusion extends this from "emit one operation per type" to "emit fused chain per type."
- **Iron_IfaceRegistry** (`iface_collect.c`): Tracks alive implementors per interface. Foundation for monomorphic detection — extend to track "which types are actually pushed."
- **Layout analysis module** (`layout_analysis.h/c`): Field access analysis, SoA/AoS detection. Fusion can query `iron_layout_get_kind()` to decide whether to emit SoA-aware or AoS fused loops.
- **stb_ds hash maps**: Used everywhere for tracking — `split_collection_ids`, `inline_eligible`, `reduced_storage_types`. Natural pattern for specialization registry.

### Established Patterns
- **Pre-scan in emit_func_body**: Already scans for split-eligible for-loops before emission. Chain detection can piggyback on this scan or run as a similar pre-pass.
- **Interprocedural analysis** (Phase 48): `layout_analysis.c` already scans all functions for field access patterns. Same infrastructure pattern applies to monomorphic type tracking.
- **C runtime → inline emission**: Phase 47 already replaces C runtime calls with inline emission for split collections. Fusion is the next step — replacing inline-per-operation with inline-fused-chain.

### Integration Points
- **emit_c.c**: Primary integration point. Chain detection, fused loop emission, monomorphic collapse all happen in the C emitter.
- **Parser**: `@fusible` annotation parsing on function declarations
- **Type checker**: Propagate `@fusible` attribute to resolved functions
- **LIR**: Def-use graph construction for chain detection

</code_context>

<specifics>
## Specific Ideas

- `@fusible` annotation spec document (`docs/fusible-annotation-spec.md`) must cover: annotation syntax, safety requirements (iterate self, per-element transformation, no inter-element dependencies), body extraction pattern, how the compiler verifies fusibility, examples of valid and invalid `@fusible` functions
- Phase 47 context explicitly states: "Phase 49 must recognize these as fusion targets and optimize chained calls into single-pass loops" and "Phase 49 must demonstrate fusion of these methods with full test coverage"
- The spec (§8.1) shows the expected transformation: 3 separate loops → 1 fused loop with map+filter+accumulate all inline
- Monomorphic collapse must produce identical results to the split collection path — test with both single-type and multi-type inputs to verify

</specifics>

<deferred>
## Deferred Ideas

- Lazy iterators — would enable natural fusion without compiler magic, but adds a new type system concept. Noted in Phase 47 context.
- Parallel fusion — per-type fused loops could run in parallel (separate accumulator per type, merge at end). Deferred to a parallelism phase.
- Profile-guided fusion thresholds — skip fusion for very short arrays where loop setup cost exceeds benefit. Needs runtime profiling data.

</deferred>

---

*Phase: 49-loop-fusion-monomorphic-specialization*
*Context gathered: 2026-04-07*
