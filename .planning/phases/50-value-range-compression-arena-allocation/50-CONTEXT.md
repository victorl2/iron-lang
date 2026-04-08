# Phase 50: Value Range Compression & Arena Allocation - Context

**Gathered:** 2026-04-08
**Status:** Ready for planning

<domain>
## Phase Boundary

Fields in collection storage structs (`Iron_<Type>_Stor`) proven by whole-program dataflow analysis to fit in narrower types are stored compressed (e.g., `int64_t` → `uint8_t`). Widening reads and narrowing writes are inserted at access sites. Per-type sub-arrays in split collections use arena allocation with geometric growth and bulk free. Does NOT add new annotations, type system features, or change standalone object struct layouts.

</domain>

<decisions>
## Implementation Decisions

### Range analysis scope
- **Full dataflow analysis with conservative bounds** — track value ranges through literal assignments, constant propagation, arithmetic (conservative widening), conditionals (narrow on true branch, union at merge), and function returns (interprocedural)
- **No false precision** — when any code path can't prove a bound, the field keeps its original type. Unknown sources (user input, file reads, casts from wider type) → full type range → no compression
- **Interprocedural** — analyze function return ranges and parameter constraints across calls. Iron's closed-world model makes this feasible.
- **New analysis module: `value_range.h/c`** — separate from layout_analysis, called from emit_c.c before struct emission. Similar pattern to how layout_analysis was added in Phase 48.

### Compression thresholds
- **Full type ladder** — pick the smallest safe type: int64 → int32 → int16 → int8/uint8. e.g., range [0, 50000] → uint16, range [-100, 100] → int8
- **Collection storage structs only** — compress fields in `Iron_<Type>_Stor` structs and nested structs within collections. Leave standalone object structs uncompressed (bulk data is where savings matter)
- **Opt-in diagnostic** — a flag like `--report-compression` shows which fields were narrowed and to what type. Silent by default.

### Arena allocation
- **Per-collection arena** — one arena per split collection. All sub-arrays (circle[], square[], order[]) allocated from the same arena block. Single free destroys everything.
- **Extend existing Iron_Arena** — `util/arena.h` already has `iron_arena_alloc()` with bump allocation. Add geometric growth and bulk sub-allocation for collection use cases.
- **Initial capacity: 8 elements, growth factor: 1.5x** — larger start, slower growth. Less reallocations for small collections, less memory waste for large ones.

### Programmer control
- **Purely automatic** — no range annotations. If the programmer knows the range, they use sized integer types (`Int8`, `Int16`, etc. from Phase 29). The compiler handles collection-level compression transparently.
- **No opt-out needed** — compression is safe (widening/narrowing produce identical results). If full-width storage is wanted, use a wider type in source.

### Claude's Discretion
- Exact dataflow lattice representation for value ranges
- How to handle range merges at phi nodes in LIR
- Arena block size strategy (one large block vs linked list of smaller blocks)
- How to propagate ranges through generic collection method calls
- Whether to compress Bool fields to bit-packing or leave as uint8_t

</decisions>

<canonical_refs>
## Canonical References

**Downstream agents MUST read these before planning or implementing.**

### Spec sections
- `docs/static-interface-dispatch-spec.md` §8.4 (Arena Allocation Per Type) — Arena-backed field arrays, geometric growth, bulk deallocation
- `docs/static-interface-dispatch-spec.md` §8.7 (Value Range Compression) — Range analysis, narrowed storage, widening casts, static provability requirement

### Existing collection infrastructure
- `src/lir/emit_c.c` — Split collection struct generation (`Iron_SplitList_*`), `Iron_<Type>_Stor` structs, per-type sub-array malloc/realloc/free, push functions
- `src/lir/layout_analysis.h` — `LayoutAnalysis` type, field access analysis (Phase 48). New `value_range` module follows same pattern.
- `src/lir/layout_analysis.c` — Interprocedural analysis pattern (method body scanning) — same approach for range analysis

### Arena infrastructure
- `src/util/arena.h` — `Iron_Arena`, `iron_arena_alloc()`, `iron_arena_free()`. Extend for collection sub-allocation.
- `src/util/arena.c` — Current bump allocator implementation

### Type system
- `src/analyzer/types.h` — Iron type kinds, sized integer support (Int8, Int16, Int32, Int64 from Phase 29)
- `src/lir/lir.h` — LIR instruction types, value table, GET_FIELD/SET_FIELD

</canonical_refs>

<code_context>
## Existing Code Insights

### Reusable Assets
- **Iron_Arena** (`util/arena.h/c`): Existing bump allocator with page-based allocation. Can be extended with geometric growth for collection sub-arrays.
- **Layout analysis module** (`layout_analysis.h/c`): Phase 48 added interprocedural field access analysis. Value range analysis follows the same architectural pattern — new module, called from emit_c.c, queries per-field.
- **Sized integer types** (Phase 29): `IRON_TYPE_INT8`, `IRON_TYPE_INT16`, `IRON_TYPE_INT32` already exist in the type system. Compression maps to these existing types.
- **`Iron_<Type>_Stor` structs**: Reduced storage structs from Phase 48 are already emitted with field selection. Value range compression extends this to type narrowing per field.

### Established Patterns
- **Pre-scan in emit_c.c**: Multiple pre-scan passes run before emission (stack arrays, heap arrays, split collection detection, chain detection). Range analysis is a natural addition.
- **stb_ds hash maps**: Used throughout for tracking per-field/per-collection state. Range info follows the same pattern.
- **Interprocedural analysis** (Phases 48-49): Scanning all functions for field patterns, method body analysis. Range analysis extends this to value tracking.

### Integration Points
- **emit_c.c struct emission**: Where `Iron_<Type>_Stor` fields are emitted — use range info to select narrower C types
- **emit_c.c push functions**: Where values are stored to sub-arrays — insert narrowing casts
- **emit_c.c loop body**: Where values are loaded from sub-arrays — insert widening casts
- **emit_c.c split collection allocation**: Where malloc/realloc/free happen — replace with arena calls

</code_context>

<specifics>
## Specific Ideas

- The spec (§8.7) example: `kind: u64` proven to hold 0-7 → stored as `uint8_t*` in `ParticleStorage`. Access widens: `uint64_t k = (uint64_t)particle_kind[i]`.
- The spec (§8.4) example: `CircleStorage` with arena-allocated field arrays, `circle_storage_destroy()` freeing all arrays.
- Range analysis must handle the common case: enum-like integer fields used as tags or categories (always small values).
- Arena free must handle both AoS and SoA layouts — AoS frees one array per type, SoA frees one array per field per type.

</specifics>

<deferred>
## Deferred Ideas

- Profile-guided range hints — gather runtime ranges and feed back as assertions. Spec mentions this as optional, deferred.
- Bit-packing for boolean arrays — store 8 booleans per byte instead of 1 byte each. Significant savings for boolean-heavy structs but adds bit manipulation overhead.
- Arena pooling across collections — reuse arena memory across different collections. Complex lifetime management, defer.

</deferred>

---

*Phase: 50-value-range-compression-arena-allocation*
*Context gathered: 2026-04-08*
