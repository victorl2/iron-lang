# Phase 48: Layout Optimizations - Context

**Gathered:** 2026-04-07
**Status:** Ready for planning

<domain>
## Phase Boundary

Automatic SoA/AoS layout selection per (collection, concrete type) based on field access pattern analysis. Dead field elimination from collection storage structs. Common field factoring across implementors. Small/large variant split for non-collection interface variables. Layout annotations (`layout: soa`, `layout: aos`, `unordered`). Includes a benchmark suite to validate and tune the SoA/AoS threshold.

</domain>

<decisions>
## Implementation Decisions

### SoA/AoS selection strategy
- **Default threshold: field ratio < 50%** — SoA when the loop body accesses fewer than half the struct's fields; AoS when half or more are accessed
- The threshold is a configurable constant in the compiler source, not hardcoded deep in logic
- **Benchmark-validated** — Phase 48 includes a benchmark suite that tests different access ratios (1 field, half, all fields) across structs with 2, 4, 6, 8 fields, measuring AoS vs SoA throughput
- The benchmark data informs whether 50% is the right crossover or if it should be tuned
- Analysis is **per loop body per collection** — the same collection could use SoA in one loop and AoS in another (per the spec's "context-dependent" design)

### Dead field elimination
- **Per-collection, maximum aggression** — each split collection instance gets a custom storage struct with only the fields accessed through that specific collection's operations
- If `shapes1` only calls `.area()` (accesses radius/side) and `shapes2` calls `.name()` (accesses name), they get different storage structs
- The **primary object struct retains all fields** — direct concrete-type access (`my_circle.debug_name`) always works
- Whole-program scan: analyze all for-loops, collection method calls, and field accesses to build the used-field set per collection

### Common field factoring
- Fields with the same name, type, and position across ALL alive implementors of an interface are stored in a single shared array
- Example: if all entities have `x: Int` as first field, store `entity_x[]` once instead of per-type
- Only factors fields at the same struct offset position to avoid alignment issues

### Small/large variant split
- **Combined threshold: >2x smallest AND >64 bytes** — both conditions must be true for indirect storage
- Avoids pointer overhead for small structs even when size ratios are high
- Large variants stored via heap-allocated pointer in the tagged union
- Only applies to **non-collection** interface variables (split collections already avoid the padding problem)

### Layout annotations
- **Type annotation syntax**: `val particles: [Particle, layout: soa] = [...]`
- Also: `val shapes: [Shape, layout: aos] = [...]` and `val shapes: [Shape, unordered] = [...]`
- Annotations override the compiler's automatic selection
- **Explanatory warnings** when annotation contradicts analysis: `"warning: 'layout: soa' on shapes may reduce performance — loop at line 12 accesses all 4 fields (AoS recommended for full-struct access). Annotation honored."`
- Annotations parse as extra type parameters in the array type annotation

### Claude's Discretion
- How to represent SoA storage in the generated C (separate named arrays vs struct of arrays)
- Field access pattern analysis implementation (walk LIR blocks vs annotate at HIR level)
- How to propagate per-collection field usage through function boundaries
- Benchmark suite structure and reporting format

</decisions>

<canonical_refs>
## Canonical References

**Downstream agents MUST read these before planning or implementing.**

### Static dispatch spec
- `docs/static-interface-dispatch-spec.md` §5 (Memory Layout Optimizations) — Common field factoring, small/large variant split
- `docs/static-interface-dispatch-spec.md` §6 (Context-Dependent Field-Level Splitting) — SoA/AoS selection, semantic context analysis, conversion strategies, programmer hints

### Existing split collection code
- `src/lir/emit_c.c` — `Iron_SplitList_<Iface>` struct generation (~line 4230+), per-type loop emission (~line 4150+), split collection tracking via `split_collection_ids` map
- `src/lir/emit_c.c` `emit_object_struct_body()` — Object struct emission with all fields
- `src/lir/lir_optimize.c` — Inline eligibility exclusions for interface arrays

### Type system
- `src/analyzer/typecheck.c` — Array type annotation parsing, method call resolution
- `src/parser/parser.c` — Type annotation parsing (need to extend for `layout:` and `unordered`)

</canonical_refs>

<code_context>
## Existing Code Insights

### Reusable Assets
- **Split collection struct generation** in emit_c.c: already generates per-type sub-arrays. SoA extends this to per-field-per-type arrays.
- **emit_object_struct_body()**: Emits C structs for objects. Can be adapted to emit reduced storage structs.
- **Iron_IfaceRegistry**: Tracks alive implementors per interface. Provides the implementor list for common field factoring.
- **Benchmark infrastructure**: `tests/benchmark/` directory with CMake integration for benchmark_smoke test.

### Established Patterns
- **Pre-scan in emit_func_body**: Already scans for split-eligible for-loops before emission. Field access analysis can piggyback on this scan.
- **stb_ds hash maps**: Used throughout for tracking (split_collection_ids, inline_eligible, etc.). Field usage sets can use the same pattern.

### Integration Points
- **Parser**: Type annotation parsing needs `layout:` and `unordered` keyword support
- **Type checker**: Annotations need to propagate to the array type
- **emit_c.c**: SoA struct generation, field access analysis, dead field elimination all happen here
- **LIR optimizer**: May need field access tracking annotations on ARRAY_LIT and GET_FIELD instructions

</code_context>

<specifics>
## Specific Ideas

- Benchmark suite: Iron programs with varying field counts and access patterns, compiled with AoS and SoA, timing compared. Output: table of access_ratio vs speedup.
- SoA in C: `double *particle_x; double *particle_y;` (separate malloc per field array) rather than a struct of arrays, to maximize cache line utilization.
- Dead field detection: scan all GET_FIELD instructions in all functions, build set per collection ValueId, only include accessed fields in storage struct.

</specifics>

<deferred>
## Deferred Ideas

- Dual-representation maintenance (SoA stored, AoS views on demand) — LAYOUT-06, v2
- Profile-guided layout selection — LAYOUT-07, v2
- Layout conversion between AoS and SoA within a function — complex, defer to when benchmark data shows it's needed

</deferred>

---

*Phase: 48-layout-optimizations*
*Context gathered: 2026-04-07*
