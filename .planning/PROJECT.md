# Iron Compiler — Static Interface Dispatch

## What This Is

The Iron compiler compiles Iron source to C. This milestone implements the static interface dispatch specification: replacing vtable-based polymorphism with compiler-generated tagged unions, automatic collection splitting by concrete type, context-dependent AoS/SoA field-level layout, and a suite of advanced optimizations (loop fusion, dead field elimination, monomorphic specialization, arena allocation, prefetch insertion, auto-parallelism, value range compression).

## Core Value

The programmer writes polymorphic code against interfaces; the compiler emits monomorphic, data-oriented C code with no vtables, no heap indirection, and no pointer chasing.

## Previous Milestone: v0.1-alpha Static Interface Dispatch (Shipped)

Shipped: Core dispatch (tagged unions, tag-based dispatch, dead implementor elimination), collection splitting (per-type sub-arrays, unordered per-type loops, ordered iteration), prefetch insertion, documentation/branding.

## Current Milestone: v0.1.1-alpha Collection Methods, Full Captures & Layout Optimizations

**Goal:** Complete the static dispatch optimization stack by adding collection methods (map/filter/reduce with lambdas), full closure capture support, and the remaining layout optimizations (SoA/AoS, dead field elimination, loop fusion, monomorphic specialization, value range compression, arena allocation).

**Target features:**
- Collection methods on arrays: `.map()`, `.filter()`, `.reduce()`, `.forEach()`, `.sum()` accepting lambdas
- Full closure capture: mutable var by reference, closures returned from functions, closures as fields, nested lambdas, recursive lambdas
- AoS/SoA field-level layout selection based on access pattern analysis
- Dead field elimination in collection storage structs
- Common field factoring across implementors
- Small/large variant split for non-collection interface variables
- Loop fusion: chained map/filter/reduce fuse into single pass per type
- Monomorphic collection specialization: single-type collections collapse to plain arrays
- Value range compression: fields narrowed to smallest sufficient type
- Arena allocation per type for split collections
- Layout annotations: `layout: soa`, `layout: aos`, `unordered`

## Requirements

### Validated

<!-- Shipped and confirmed valuable in previous milestones. -->

- ✓ Match exhaustiveness checking for enum types — v0.0
- ✓ PHI node type consistency verification in LIR — v0.0
- ✓ Call argument type validation in LIR verifier — v0.0
- ✓ Generic type constraint checking at instantiation sites — v0.0
- ✓ Cast safety validation — v0.0
- ✓ Definite assignment analysis — v0.0
- ✓ Static array bounds checking — v0.0
- ✓ Slice bounds validation — v0.0
- ✓ Escape analysis for fields, arrays, and function arguments — v0.0
- ✓ Compound assignment overflow detection — v0.0
- ✓ String interpolation type validation — v0.0
- ✓ Concurrency safety (spawn captures, field/array mutations, read-write races) — v0.0
- ✓ Test cases for every new diagnostic — v0.0

### Active

<!-- Current scope. Building toward these. -->

- [ ] Collection methods on arrays (map, filter, reduce, forEach, sum) with method syntax and lambda arguments
- [ ] Full closure capture: mutable var by reference, closures returned from functions, closures as object fields, nested lambdas, recursive lambdas via var self-reference
- [ ] AoS/SoA field-level layout selection based on semantic access pattern analysis
- [ ] Dead field elimination in collection storage structs
- [ ] Common field factoring across implementors into shared arrays
- [ ] Small/large variant split for non-collection interface variables
- [ ] Loop fusion: chained collection operations fuse into single pass per type
- [ ] Monomorphic collection specialization when single concrete type proven
- [ ] Value range compression for narrowable fields
- [ ] Arena allocation per type for split collection sub-arrays
- [ ] Layout annotations: `layout: soa`, `layout: aos`, `unordered`

### Out of Scope

<!-- Explicit boundaries. Includes reasoning to prevent re-adding. -->

- Runtime plugin loading / dynamic extensibility — closed-world model by design
- Pre-compiled binary library support — source-only distribution required for whole-program analysis
- FFI-based interface extensibility — incompatible with static dispatch guarantees
- Incremental compilation — separate concern, can be added later without changing dispatch model
- Changes to interface declaration syntax — Iron's existing `interface` keyword is kept as-is

## Context

- The Iron compiler is a C codebase that compiles Iron source to C
- Interfaces already exist: `Iron_InterfaceDecl` in AST, `object ... implements ...` syntax
- Dispatch is already static for concrete types (mangled C function names like `Dog_speak()`)
- Vtable infrastructure exists in emit_c.c (struct generation) but is unused — no vtable population or indirect calls
- Collections use a List type with built-in methods (sort, filter, map, reduce) in stdlib
- The spec is documented in `docs/static-interface-dispatch-spec.md`
- Key source files: `ast.h`, `types.h`, `typecheck.c`, `hir_to_lir.c`, `emit_c.c`, `list.iron`
- Previous milestone closed all 12 semantic analysis gaps — the analyzer is now comprehensive

## Constraints

- **Language**: All implementation in C, matching existing codebase style
- **Backend**: Generated output is C code — delegates register allocation, instruction selection, SIMD vectorization to GCC/Clang
- **Memory**: Implementations must be memory-efficient — bounded allocations, no exponential blowup
- **Compatibility**: Must not break existing valid Iron programs
- **Closed world**: All source must be visible at compile time — no separate compilation for interface types

## Key Decisions

| Decision | Rationale | Outcome |
|----------|-----------|---------|
| Full spec in one milestone | User wants complete implementation including all advanced optimizations | — Pending |
| Adapt spec to Iron syntax | Iron uses `object`, spec uses `struct`; change `implements` to `impl` for cleaner syntax | — Pending |
| Replace unused vtable infrastructure | Vtable structs generated but never used — replace with tagged union dispatch | — Pending |
| Static analysis only for layout selection | No profiling-guided optimization — compiler uses semantic context analysis | — Pending |

---
*Last updated: 2026-04-07 after milestone v0.1.1-alpha initialization*
