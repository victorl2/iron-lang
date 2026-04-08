# Requirements: Iron Compiler — Compiler Hardening & Refactoring

**Defined:** 2026-04-08
**Core Value:** The programmer writes polymorphic code against interfaces; the compiler emits monomorphic, data-oriented C code with no vtables, no heap indirection, and no pointer chasing.

## v1 Requirements

Requirements for v0.1.1-alpha. Each maps to roadmap phases.

### Collection Methods

- [x] **COLL-05**: `arr.map(func(x: T) -> U { ... })` returns a new array of transformed elements
- [x] **COLL-06**: `arr.filter(func(x: T) -> Bool { ... })` returns a new array of matching elements
- [x] **COLL-07**: `arr.reduce(init, func(acc: U, x: T) -> U { ... })` returns accumulated value
- [x] **COLL-08**: `arr.forEach(func(x: T) { ... })` iterates with side effects, no return value
- [x] **COLL-09**: `arr.sum()` returns the numeric total of all elements (Int or Float arrays)
- [x] **COLL-10**: Collection methods chain: `arr.map(...).filter(...).sum()` compiles and produces correct result
- [x] **COLL-11**: Collection methods work on interface-typed split collections, dispatching per concrete type

### Full Closure Capture

- [x] **CAPT-05**: Lambda captures a mutable `var` by reference; mutations inside the lambda are visible outside
- [x] **CAPT-06**: Closure returned from a function outlives the creating scope (heap-allocated environment)
- [x] **CAPT-07**: Closure stored as an object field retains its captured environment
- [x] **CAPT-08**: Lambda inside another lambda captures outer variables (nested closures)
- [x] **CAPT-09**: Recursive lambda via `var` self-reference compiles and terminates correctly
- [x] **CAPT-10**: Two closures sharing the same mutable variable see each other's mutations

### Layout Optimizations

- [x] **LAYOUT-01**: Compiler selects SoA layout when a loop accesses only 1-2 fields across many objects; selects AoS when all fields are accessed
- [x] **LAYOUT-02**: Fields declared on an implementor but never accessed through interface operations are excluded from collection storage structs
- [x] **LAYOUT-03**: Fields with same name, type, and position across all implementors are stored in a single shared array
- [x] **LAYOUT-04**: Non-collection interface variables store large variants indirectly when one implementor is significantly larger than others
- [x] **LAYOUT-05**: `layout: soa` and `layout: aos` annotations on collections override automatic selection; compiler warning when annotation contradicts analysis

### Loop Fusion

- [x] **FUSE-01**: `arr.map(...).filter(...).reduce(...)` compiles to a single fused loop per concrete type with no intermediate allocation — the compiler recognizes Iron-bodied stdlib collection methods as fusion targets
- [x] **FUSE-02**: Fused loops produce identical results to the non-fused path for all input types — tested with every collection method combination (map+filter, map+reduce, filter+sum, map+filter+reduce, map+filter+sum)
- [ ] **FUSE-03**: Fusion works on interface-typed split collections — chained operations on `[Shape]` fuse into per-type single-pass loops

### Monomorphic Specialization

- [x] **MONO-01**: A collection proven to hold only one concrete type collapses to a plain typed array with direct field access (no tag dispatch)
- [x] **MONO-02**: Specialization registry prevents duplicate function bodies for the same (function, concrete_type) pair

### Value Range & Arena

- [x] **VRC-01**: A field proven by whole-program analysis to fit in a smaller type is stored narrowed in collection structs (e.g., Int that only holds 0-255 stored as uint8_t)
- [x] **VRC-02**: Compiler inserts widening reads and narrowing writes at access sites for compressed fields
- [x] **ARENA-01**: Per-type sub-arrays in split collections use arena allocation with geometric growth (capacity doubling)
- [x] **ARENA-02**: Arena free releases all per-type sub-arrays in one operation

### Memory Investigation

- [x] **MEM-01**: ironc peak memory consumption during compilation of any integration test stays below 500MB
- [x] **MEM-02**: Generated C programs have no memory leaks — all allocated memory is freed or accounted for (verified by AddressSanitizer or valgrind)

### Emitter Refactoring

- [x] **EMIT-01**: Split collection emission (struct generation, push functions, free, iteration) extracted into dedicated `emit_split.c` module with clean API
- [x] **EMIT-02**: Fusion emission (fused loop generation, chain detection helpers) extracted into dedicated `emit_fusion.c` module
- [x] **EMIT-03**: Struct/layout emission (object struct bodies, SoA/AoS arrays, storage structs, type emission) extracted into dedicated `emit_structs.c` module
- [x] **EMIT-04**: EmitCtx fields documented, consistently named, and cleaned up via single `emit_ctx_cleanup()` function

### Analysis Improvements

- [x] **ANAL-01**: Monomorphic detection tracks concrete types across function boundaries — helper functions returning single-type collections trigger collapse at call site
- [x] **ANAL-02**: Value range analysis tracks return value ranges through function calls instead of conservative TOP
- [x] **ANAL-03**: Value range analysis narrows ranges through conditional branches (if x < 100 produces [min, 99] in true branch)

### Test Hardening

- [ ] **TEST-01**: Edge case test suite covers empty collections, all-filtered-out, single element, zero-field structs, single-implementor interfaces
- [ ] **TEST-02**: Stress tests validate correctness with 10K+ element collections, 10+ implementors, deeply nested fusion chains
- [ ] **TEST-03**: Composition tests verify optimization combinations: SoA + fusion, dead field + compression, monomorphic + fusion, arena + SoA

## v2 Requirements

Deferred to future release. Tracked but not in current roadmap.

### Layout Enhancements

- **LAYOUT-06**: Full dual-representation maintenance (SoA stored, AoS views reconstructed on demand) for mixed-access collections
- **LAYOUT-07**: Profile-guided layout selection as a second-pass optimization

### Compilation Model

- **COMP-01**: Incremental compilation — adding a new implementor triggers recompilation of only affected unions, collections, and dispatch sites
- **COMP-02**: Branch deduplication and outlining for dispatch sites with many implementors

## Out of Scope

Explicitly excluded. Documented to prevent scope creep.

| Feature | Reason |
|---------|--------|
| Runtime vtable fallback | Closed-world model is a design requirement |
| Pre-compiled binary library support | Source-only distribution required for whole-program analysis |
| Profiling-guided layout (PGO) | Static analysis only; programmer hints cover edge cases |
| Per-call-site specialization for all types | Exponential code size without proof of benefit |
| Concurrency captures (spawn with mutable) | Separate concern from collection methods |

## Traceability

Which phases cover which requirements. Updated during roadmap creation.

| Requirement | Phase | Status |
|-------------|-------|--------|
| CAPT-05 | Phase 46 | Pending |
| CAPT-06 | Phase 46 | Pending |
| CAPT-07 | Phase 46 | Pending |
| CAPT-08 | Phase 46 | Pending |
| CAPT-09 | Phase 46 | Pending |
| CAPT-10 | Phase 46 | Pending |
| COLL-05 | Phase 47 | Complete |
| COLL-06 | Phase 47 | Complete |
| COLL-07 | Phase 47 | Complete |
| COLL-08 | Phase 47 | Complete |
| COLL-09 | Phase 47 | Complete |
| COLL-10 | Phase 47 | Complete |
| COLL-11 | Phase 47 | Complete |
| LAYOUT-01 | Phase 48 | Complete |
| LAYOUT-02 | Phase 48 | Complete |
| LAYOUT-03 | Phase 48 | Complete |
| LAYOUT-04 | Phase 48 | Complete |
| LAYOUT-05 | Phase 48 | Complete |
| FUSE-01 | Phase 49 | Complete |
| FUSE-02 | Phase 49 | Complete |
| MONO-01 | Phase 49 | Complete |
| MONO-02 | Phase 49 | Complete |
| VRC-01 | Phase 50 | Complete |
| VRC-02 | Phase 50 | Complete |
| ARENA-01 | Phase 50 | Complete |
| ARENA-02 | Phase 50 | Complete |
| MEM-01 | Phase 51 | Complete |
| MEM-02 | Phase 51 | Complete |
| EMIT-01 | Phase 52 | Complete |
| EMIT-02 | Phase 52 | Complete |
| EMIT-03 | Phase 52 | Complete |
| EMIT-04 | Phase 52 | Complete |
| ANAL-01 | Phase 53 | Complete |
| ANAL-02 | Phase 53 | Complete |
| ANAL-03 | Phase 53 | Complete |
| TEST-01 | Phase 54 | Pending |
| TEST-02 | Phase 54 | Pending |
| TEST-03 | Phase 54 | Pending |

**Coverage:**
- v1 requirements: 38 total
- Mapped to phases: 38
- Unmapped: 0

---
*Requirements defined: 2026-04-07*
*Last updated: 2026-04-08 after roadmap creation (phases 52-54 for v0.1.2-alpha)*
