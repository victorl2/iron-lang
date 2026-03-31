# Requirements: Iron

**Defined:** 2026-03-31
**Core Value:** Every Iron language feature compiles to correct, working C code that produces a native binary

## v0.0.6-alpha Requirements

Requirements for HIR Pipeline Correctness milestone. Each maps to roadmap phases.

### Parallel-For Codegen

- [x] **PFOR-01**: `__pfor_` lifted functions are recognized by `is_lifted_func()` and emitted in the correct output section
- [x] **PFOR-02**: All pfor integration tests pass (test_parallel, parallel_for_capture, hir_parallel_for)
- [x] **PFOR-03**: All pfor-dependent algorithm tests compile and pass (concurrent_hash_map, graph_bfs_dfs, parallel_merge_sort, work_stealing)

### Struct Codegen

- [x] **STRUCT-01**: Functions returning object types are only lowered as constructors when function name matches type name
- [x] **STRUCT-02**: CONSTRUCT emission emits exactly the number of fields in the struct definition (no excess elements)
- [x] **STRUCT-03**: game_loop_headless composite test passes with correct struct value passing

### Algorithm Correctness

- [x] **ALG-01**: quicksort algorithm test produces correctly sorted output
- [x] **ALG-02**: hash_map algorithm test runs without crash and produces correct output
- [x] **ALG-03**: All 13 algorithm tests pass (8 already passing + 5 fixed)

### Correctness Audit

- [x] **AUDIT-01**: Systematic investigation of HIR→LIR→C pipeline identifies any additional correctness bugs
- [x] **AUDIT-02**: All identified correctness issues are fixed with regression tests
- [ ] **AUDIT-03**: New correctness test cases cover pfor, struct passing, and any other identified problem areas

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

### Performance Tuning

- **PERF-01**: Benchmark threshold tuning for production-ready targets
- **PERF-02**: Profile-guided optimization decisions

## Out of Scope

| Feature | Reason |
|---------|--------|
| LLVM backend | Future milestone — HIR/LIR architecture enables this but not building it now |
| Interprocedural optimization | Complex, diminishing returns for C backend |
| Vectorization / register allocation | Clang handles these automatically |
| Self-hosting | Stretch goal for future milestone |
| Networking | Deferred — correctness is higher priority |
| Benchmark threshold tuning | Performance must be reasonable but production-ready tuning is deferred |

## Traceability

Which phases cover which requirements. Updated during roadmap creation.

| Requirement | Phase | Status |
|-------------|-------|--------|
| PFOR-01 | Phase 21 | Complete |
| PFOR-02 | Phase 21 | Complete |
| PFOR-03 | Phase 21 | Complete |
| STRUCT-01 | Phase 22 | Complete |
| STRUCT-02 | Phase 22 | Complete |
| STRUCT-03 | Phase 22 | Complete |
| ALG-01 | Phase 22 | Complete |
| ALG-02 | Phase 22 | Complete |
| ALG-03 | Phase 22 | Complete |
| AUDIT-01 | Phase 23 | Complete |
| AUDIT-02 | Phase 23 | Complete |
| AUDIT-03 | Phase 23 | Pending |

**Coverage:**
- v0.0.6-alpha requirements: 12 total
- Mapped to phases: 12
- Unmapped: 0

---
*Requirements defined: 2026-03-31*
*Last updated: 2026-03-31 after roadmap creation (phases 21-23)*
