# Requirements: Iron

**Defined:** 2026-03-31
**Core Value:** Every Iron language feature compiles to correct, working C code that produces a native binary

## v0.0.7-alpha Requirements

Requirements for Performance Optimization milestone. Each maps to roadmap phases.

### Loop Optimization

- [x] **LOOP-01**: Range bound hoisting evaluates `Iron_range()` once in the loop pre-header instead of every iteration
- [x] **LOOP-02**: All for-range benchmarks show measurable improvement from bound hoisting

### Memory Optimization

- [ ] **MEM-01**: `fill(CONST, val)` with constant size <= 1024 and non-escaping result is stack-allocated via alloca
- [ ] **MEM-02**: Stack-promoted arrays emit declarations at function entry to avoid VLA+goto bypass

### Expression Optimization

- [ ] **EXPR-01**: LOAD instructions are eligible for expression inlining when use site is in the same block as the LOAD
- [ ] **EXPR-02**: Cross-block LOADs remain excluded to prevent undeclared variable errors

### Function Inlining

- [ ] **INLINE-01**: Small (<= 20 instructions), non-recursive, pure functions are inlined at LIR level
- [ ] **INLINE-02**: Inlining pass runs before the copy-prop/DCE fixpoint loop so inlined code gets optimized
- [ ] **INLINE-03**: Value IDs are correctly remapped during instruction cloning to prevent table corruption

### Phi Elimination

- [ ] **PHI-01**: SSA phi elimination produces fewer temporary variables through copy coalescing
- [ ] **PHI-02**: Complex control flow benchmarks (connected_components) show measurable reduction in generated temporaries

### Sized Integers

- [ ] **INT-01**: Iron supports explicit `Int32` type annotations that emit `int32_t` in generated C
- [ ] **INT-02**: Array operations with `Int32` elements use 32-bit memory bandwidth

### Benchmark Validation

- [ ] **BENCH-01**: Benchmark suite is run after all optimizations and results are compared to pre-optimization baseline
- [ ] **BENCH-02**: Exploration pass identifies any remaining optimization opportunities beyond P0-P5

## Future Requirements

Deferred to future milestones. Tracked but not in current roadmap.

### Advanced Optimizations

- **P6-01**: Structured loop reconstruction emits for/while instead of goto-based control flow
- **P6-02**: Clang loop optimizations (vectorization, unrolling) enabled by structured emission

### Auto-Narrowing

- **NARROW-01**: Compiler automatically narrows Int to Int32 when range analysis proves values fit
- **NARROW-02**: Range analysis propagates through arithmetic operations and array indexing

## Out of Scope

| Feature | Reason |
|---------|--------|
| Structured loop reconstruction (P6) | High effort, deferred — goto-based emission is correct, optimization benefit is secondary |
| Auto-narrowing integer types | High complexity — requires range analysis; explicit Int32 annotations are sufficient for v0.0.7 |
| LLVM backend | Future milestone — HIR/LIR architecture enables this but not building it now |
| Interprocedural optimization beyond inlining | Complex, diminishing returns for C backend |

## Traceability

Which phases cover which requirements. Updated during roadmap creation.

| Requirement | Phase | Status |
|-------------|-------|--------|
| LOOP-01 | Phase 24 | Complete |
| LOOP-02 | Phase 24 | Complete |
| MEM-01 | Phase 25 | Pending |
| MEM-02 | Phase 25 | Pending |
| EXPR-01 | Phase 26 | Pending |
| EXPR-02 | Phase 26 | Pending |
| INLINE-01 | Phase 27 | Pending |
| INLINE-02 | Phase 27 | Pending |
| INLINE-03 | Phase 27 | Pending |
| PHI-01 | Phase 28 | Pending |
| PHI-02 | Phase 28 | Pending |
| INT-01 | Phase 29 | Pending |
| INT-02 | Phase 29 | Pending |
| BENCH-01 | Phase 30 | Pending |
| BENCH-02 | Phase 30 | Pending |

**Coverage:**
- v0.0.7-alpha requirements: 15 total
- Mapped to phases: 15
- Unmapped: 0

---
*Requirements defined: 2026-03-31*
*Last updated: 2026-03-31 after roadmap creation (Phases 24-30)*
