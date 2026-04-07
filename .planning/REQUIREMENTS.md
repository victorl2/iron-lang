# Requirements: Iron Compiler — Collection Methods, Full Captures & Layout Optimizations

**Defined:** 2026-04-07
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

- [ ] **LAYOUT-01**: Compiler selects SoA layout when a loop accesses only 1-2 fields across many objects; selects AoS when all fields are accessed
- [ ] **LAYOUT-02**: Fields declared on an implementor but never accessed through interface operations are excluded from collection storage structs
- [ ] **LAYOUT-03**: Fields with same name, type, and position across all implementors are stored in a single shared array
- [ ] **LAYOUT-04**: Non-collection interface variables store large variants indirectly when one implementor is significantly larger than others
- [ ] **LAYOUT-05**: `layout: soa` and `layout: aos` annotations on collections override automatic selection; compiler warning when annotation contradicts analysis

### Loop Fusion

- [ ] **FUSE-01**: `arr.map(...).filter(...).reduce(...)` compiles to a single fused loop per concrete type with no intermediate allocation — the compiler recognizes Iron-bodied stdlib collection methods as fusion targets
- [ ] **FUSE-02**: Fused loops produce identical results to the non-fused path for all input types — tested with every collection method combination (map+filter, map+reduce, filter+sum, map+filter+reduce, map+filter+sum)
- [ ] **FUSE-03**: Fusion works on interface-typed split collections — chained operations on `[Shape]` fuse into per-type single-pass loops

### Monomorphic Specialization

- [ ] **MONO-01**: A collection proven to hold only one concrete type collapses to a plain typed array with direct field access (no tag dispatch)
- [ ] **MONO-02**: Specialization registry prevents duplicate function bodies for the same (function, concrete_type) pair

### Value Range & Arena

- [ ] **VRC-01**: A field proven by whole-program analysis to fit in a smaller type is stored narrowed in collection structs (e.g., Int that only holds 0-255 stored as uint8_t)
- [ ] **VRC-02**: Compiler inserts widening reads and narrowing writes at access sites for compressed fields
- [ ] **ARENA-01**: Per-type sub-arrays in split collections use arena allocation with geometric growth (capacity doubling)
- [ ] **ARENA-02**: Arena free releases all per-type sub-arrays in one operation

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
| LAYOUT-01 | Phase 48 | Pending |
| LAYOUT-02 | Phase 48 | Pending |
| LAYOUT-03 | Phase 48 | Pending |
| LAYOUT-04 | Phase 48 | Pending |
| LAYOUT-05 | Phase 48 | Pending |
| FUSE-01 | Phase 49 | Pending |
| FUSE-02 | Phase 49 | Pending |
| MONO-01 | Phase 49 | Pending |
| MONO-02 | Phase 49 | Pending |
| VRC-01 | Phase 50 | Pending |
| VRC-02 | Phase 50 | Pending |
| ARENA-01 | Phase 50 | Pending |
| ARENA-02 | Phase 50 | Pending |

**Coverage:**
- v1 requirements: 26 total
- Mapped to phases: 26
- Unmapped: 0

---
*Requirements defined: 2026-04-07*
*Last updated: 2026-04-07 after roadmap creation (phases 46-50)*
