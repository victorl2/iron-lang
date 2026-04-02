# Requirements: Iron v0.1.0-alpha Lambda Capture

**Defined:** 2026-04-02
**Core Value:** Every Iron language feature compiles to correct, working C code that produces a native binary

## v0.1.0-alpha Requirements

Requirements for lambda capture milestone. Each maps to roadmap phases.

### Capture Foundation

- [x] **FOUND-01**: Compiler performs free variable analysis on lambda bodies, identifying all outer-scope references
- [ ] **FOUND-02**: Compiler generates typed environment structs in C output for each capturing lambda
- [ ] **FOUND-03**: Closure values use `Iron_Closure {fn, env}` fat pointer representation instead of bare `void*`
- [ ] **FOUND-04**: Lifted lambda functions receive an env parameter and dereference captured variables from it

### Value & Mutable Capture

- [ ] **CAPT-01**: User can capture an immutable `val` by value in a lambda (examples 1, 3)
- [ ] **CAPT-02**: User can capture a mutable `var` by reference, with mutations visible outside (examples 2, 7, 13, 14)
- [ ] **CAPT-03**: User can capture multiple variables of different types (Int, Float, String) in one lambda (example 3)
- [ ] **CAPT-04**: User can capture a loop variable by snapshot, with each iteration getting its own copy (example 4)

### Escaping & Lifetime

- [ ] **LIFE-01**: User can return a capturing lambda from a function, with the environment outliving the creating scope (examples 5, 10)
- [ ] **LIFE-02**: User can store a capturing lambda as an object field, retaining its environment for the object's lifetime (example 17)
- [ ] **LIFE-03**: Two closures in the same scope can share the same mutable variable, with mutations visible to both (examples 11, 20)
- [ ] **LIFE-04**: User can define a recursive lambda via a `var` that captures itself by reference (example 19)

### Nested & Higher-Order

- [ ] **HOF-01**: User can capture `self` in a method's lambda, with mutations to object fields visible on the original (example 6)
- [ ] **HOF-02**: User can pass a capturing lambda as a callback argument to another function (example 7)
- [ ] **HOF-03**: User can capture a lambda inside another lambda (example 9)
- [ ] **HOF-04**: User can create a lambda inside an if/else branch that captures outer variables (example 12)
- [ ] **HOF-05**: User can compose higher-order functions that return capturing lambdas (example 18)

### Concurrency Capture

- [ ] **CONC-01**: User can spawn a task that captures read-only outer variables (example 15)
- [ ] **CONC-02**: User can use parallel-for with captured read-only arrays (example 16)

### Optimizer Integration

- [ ] **OPT-01**: DCE does not eliminate MAKE_CLOSURE instructions (closures stored in collections or passed as callbacks survive)
- [ ] **OPT-02**: Function inliner skips lifted lambda functions
- [ ] **OPT-03**: Copy propagation and store-load elimination respect heap-box indirection for mutable captures

### Diagnostics & Performance

- [ ] **DIAG-01**: Compiler produces clear error messages for capture-related issues (uncaptured variable, type mismatch in environment)
- [ ] **DIAG-02**: Closure call overhead is benchmarked against equivalent direct function calls with measurable results
- [ ] **DIAG-03**: Correctness test suite includes integration tests for all 20 canonical lambda capture patterns

## Future Requirements

Deferred to future release. Tracked but not in current roadmap.

### Capture Optimization

- **COPT-01**: Stack-allocate environment structs for non-escaping closures (escape analysis already exists)
- **COPT-02**: Inline closure calls when the callee is statically known
- **COPT-03**: Reference-count environment structs for deterministic cleanup (Iron_Rc integration)

### Advanced Capture

- **ACAP-01**: Explicit capture syntax (e.g., `func [x, &y]() { ... }`) for user-controlled capture mode
- **ACAP-02**: Move capture semantics for transferring ownership into closures

## Out of Scope

Explicitly excluded. Documented to prevent scope creep.

| Feature | Reason |
|---------|--------|
| Garbage collection for environments | Iron uses ref counting / manual memory; GC is a different memory model |
| Boxing all values | Only mutable captures need heap cells; value captures stay unboxed |
| Closure serialization | No use case in game dev target |
| LLVM-specific closure optimizations | C backend milestone; LLVM backend is a future milestone |
| Self-hosting compiler changes | Compiler remains in C11 |
| Generic closures / closure traits | Requires generics system which doesn't exist yet |

## Traceability

Which phases cover which requirements. Updated during roadmap creation.

| Requirement | Phase | Status |
|-------------|-------|--------|
| FOUND-01 | Phase 32 | Complete |
| FOUND-02 | Phase 32 | Pending |
| FOUND-03 | Phase 32 | Pending |
| FOUND-04 | Phase 32 | Pending |
| CAPT-01 | Phase 33 | Pending |
| CAPT-02 | Phase 33 | Pending |
| CAPT-03 | Phase 33 | Pending |
| CAPT-04 | Phase 33 | Pending |
| OPT-01 | Phase 33 | Pending |
| OPT-02 | Phase 33 | Pending |
| OPT-03 | Phase 33 | Pending |
| LIFE-01 | Phase 34 | Pending |
| LIFE-02 | Phase 34 | Pending |
| LIFE-03 | Phase 34 | Pending |
| LIFE-04 | Phase 34 | Pending |
| HOF-01 | Phase 34 | Pending |
| HOF-02 | Phase 34 | Pending |
| HOF-03 | Phase 34 | Pending |
| HOF-04 | Phase 34 | Pending |
| HOF-05 | Phase 34 | Pending |
| CONC-01 | Phase 35 | Pending |
| CONC-02 | Phase 35 | Pending |
| DIAG-01 | Phase 36 | Pending |
| DIAG-02 | Phase 36 | Pending |
| DIAG-03 | Phase 36 | Pending |

**Coverage:**
- v0.1.0-alpha requirements: 25 total
- Mapped to phases: 25
- Unmapped: 0

---
*Requirements defined: 2026-04-02*
*Last updated: 2026-04-02 — traceability filled after roadmap creation*
