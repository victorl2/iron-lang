# Requirements: Iron

**Defined:** 2026-03-29
**Core Value:** Every Iron language feature compiles to correct, working C code that produces a native binary

## v0.0.5-alpha Requirements

Requirements for IR optimization and High IR architecture milestone. Each maps to roadmap phases.

### IR Optimization

- [x] **IROPT-01**: Copy propagation eliminates trivial LOAD copies by replacing all uses of copied values with their originals
- [x] **IROPT-02**: Expression inlining reconstructs compound C expressions for single-use pure values during emission
- [x] **IROPT-03**: Dead code elimination removes instructions whose results are never referenced
- [x] **IROPT-04**: Constant folding evaluates compile-time constant arithmetic and propagates through store/load chains
- [x] **IROPT-05**: Strength reduction replaces `i * cols + j` loop patterns with induction variables
- [x] **IROPT-06**: Redundant STORE/LOAD elimination removes pairs where no intervening instruction modifies the stored value

### Benchmark Parity

- [ ] **BENCH-01**: median_two_sorted_arrays runs at ≤1.2x C parity (currently 4.5x)
- [ ] **BENCH-02**: count_paths_with_obstacles, min_path_sum run at ≤1.2x (currently 1.4-1.6x)
- [ ] **BENCH-03**: largest_rect_histogram, max_depth_binary_tree run at ≤1.2x (currently 1.4-1.5x)
- [ ] **BENCH-04**: target_sum, three_sum, num_islands run at ≤1.2x (currently 1.2-1.6x)
- [x] **BENCH-05**: All 127 benchmarks pass at their configured parity threshold (100% pass rate)

### High IR Architecture

- [ ] **HIR-01**: HIR data structures represent structured control flow (if/else, for, while, match as tree nodes, not CFG)
- [ ] **HIR-02**: HIR preserves named variables with let/mut binding semantics and lexical scopes
- [ ] **HIR-03**: HIR represents method calls, field access, and array indexing at language level
- [ ] **HIR-04**: HIR represents closures, spawn, parallel-for, and defer as first-class constructs
- [ ] **HIR-05**: AST-to-HIR lowering pass covers all Iron language features and produces a valid HIR module
- [ ] **HIR-06**: HIR-to-LIR lowering pass produces equivalent SSA-form output to the current AST-to-LIR path
- [ ] **HIR-07**: HIR printer outputs human-readable representation that resembles Iron source
- [ ] **HIR-08**: HIR verifier validates structural invariants (scope nesting, type consistency, completeness)

### Infrastructure

- [ ] **INFRA-01**: Current IR renamed to Lower IR (LIR) throughout codebase — types, files, functions, comments
- [x] **INFRA-02**: ir_optimize.c module houses all optimization passes, called between phi elimination and C emission
- [ ] **INFRA-03**: All 127 benchmarks and 42+ integration tests pass through the new AST→HIR→LIR→C pipeline
- [ ] **INFRA-04**: Old AST→LIR direct lowering path removed after HIR pipeline achieves parity

## Future Requirements

Deferred to future milestones. Tracked but not in current roadmap.

### HIR-Level Optimizations

- **HIROPT-01**: Tail call detection and optimization at HIR level
- **HIROPT-02**: Loop fusion for adjacent loops over the same range
- **HIROPT-03**: Devirtualization of interface calls when concrete type is known
- **HIROPT-04**: Inlining decisions based on HIR-level function complexity

### Networking Standard Library

- **NET-01**: TCP and UDP socket client/server
- **NET-02**: TLS/SSL support via OpenSSL
- **NET-03**: HTTP client and server
- **NET-04**: URL parsing and JSON serialization

## Out of Scope

| Feature | Reason |
|---------|--------|
| LLVM backend | Future milestone — HIR/LIR architecture enables this but not building it now |
| Interprocedural optimization | Complex, diminishing returns for C backend |
| Vectorization / register allocation | Clang handles these automatically |
| Self-hosting | Stretch goal for future milestone |
| Networking | Deferred — IR architecture work is higher priority |

## Traceability

Which phases cover which requirements. Updated during roadmap creation.

| Requirement | Phase | Status |
|-------------|-------|--------|
| IROPT-01 | Phase 15 | Complete |
| IROPT-02 | Phase 16 | Complete |
| IROPT-03 | Phase 15 | Complete |
| IROPT-04 | Phase 15 | Complete |
| IROPT-05 | Phase 17 | Complete |
| IROPT-06 | Phase 17 | Complete |
| BENCH-01 | Phase 18 | Pending |
| BENCH-02 | Phase 18 | Pending |
| BENCH-03 | Phase 18 | Pending |
| BENCH-04 | Phase 18 | Pending |
| BENCH-05 | Phase 18 | Complete |
| HIR-01 | Phase 19 | Pending |
| HIR-02 | Phase 19 | Pending |
| HIR-03 | Phase 19 | Pending |
| HIR-04 | Phase 19 | Pending |
| HIR-05 | Phase 20 | Pending |
| HIR-06 | Phase 20 | Pending |
| HIR-07 | Phase 19 | Pending |
| HIR-08 | Phase 19 | Pending |
| INFRA-01 | Phase 19 | Pending |
| INFRA-02 | Phase 15 | Complete |
| INFRA-03 | Phase 20 | Pending |
| INFRA-04 | Phase 20 | Pending |

**Coverage:**
- v0.0.5-alpha requirements: 23 total
- Mapped to phases: 23
- Unmapped: 0

---
*Requirements defined: 2026-03-29*
*Last updated: 2026-03-29 after roadmap creation*
