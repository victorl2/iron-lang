# Requirements: Iron v0.1.0-alpha Lambda Capture

**Defined:** 2026-04-02
**Core Value:** Every Iron language feature compiles to correct, working C code that produces a native binary

## v0.1.0-alpha Requirements

### Capture Foundation (Phase 32 — COMPLETE)

- [x] **FOUND-01**: Compiler performs free variable analysis on lambda bodies
- [x] **FOUND-02**: Compiler generates typed environment structs in C output
- [x] **FOUND-03**: Closure values use `Iron_Closure {fn, env}` fat pointer representation
- [x] **FOUND-04**: Lifted lambda functions receive an env parameter

### Value & Mutable Capture (Phase 33)

- [x] **CAPT-01**: User can capture an immutable `val` by value in a lambda
- [x] **CAPT-02**: User can capture a mutable `var` by reference, with mutations visible outside
- [x] **CAPT-03**: User can capture multiple variables of different types in one lambda
- [x] **CAPT-04**: User can capture a loop variable by snapshot
- [x] **OPT-01**: DCE does not eliminate MAKE_CLOSURE instructions
- [x] **OPT-02**: Function inliner skips lifted lambda functions
- [x] **OPT-03**: Copy propagation and store-load elimination respect heap-box indirection

### Advanced Captures (Phase 34)

- [ ] **LIFE-01**: Closures returned from functions outlive creating scope
- [ ] **LIFE-02**: Closures stored as object fields retain environment
- [ ] **LIFE-03**: Two closures share same mutable variable
- [ ] **LIFE-04**: Recursive lambda via `var` self-reference
- [ ] **HOF-01**: Capture `self` in method's lambda
- [ ] **HOF-02**: Pass capturing lambda as callback argument
- [ ] **HOF-03**: Capture lambda inside another lambda
- [ ] **HOF-04**: Lambda inside if/else branch captures outer variables
- [ ] **HOF-05**: Compose higher-order functions returning capturing lambdas

### Concurrency Captures (Phase 35)

- [ ] **CONC-01**: Spawn task captures read-only outer variables
- [ ] **CONC-02**: Parallel-for with captured read-only arrays

### Diagnostics & Performance (Phase 36)

- [ ] **DIAG-01**: Clear error messages for capture-related issues
- [ ] **DIAG-02**: Closure call overhead benchmarked
- [ ] **DIAG-03**: All 20 canonical capture examples have integration tests
