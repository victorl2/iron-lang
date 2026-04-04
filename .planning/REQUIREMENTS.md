# Requirements: Iron Compiler -- Semantic Analysis Gaps

**Defined:** 2026-04-02
**Core Value:** Every invalid Iron program must produce a clear diagnostic at compile time -- no silent pass-through to the C backend.

## v1 Requirements

Requirements for closing all 12 semantic analysis gaps. Each maps to roadmap phases.

### Match Analysis

- [x] **MATCH-01**: Compiler checks that match statements on enum types cover all variants or have an else clause
- [x] **MATCH-02**: Compiler emits `IRON_ERR_NONEXHAUSTIVE_MATCH` listing uncovered variants when coverage is incomplete
- [x] **MATCH-03**: Compiler requires else clause when match subject is not an enum type

### LIR Verification

- [x] **LIR-01**: LIR verifier checks that all PHI incoming values have types matching the PHI result type
- [x] **LIR-02**: LIR verifier emits `IRON_ERR_LIR_PHI_TYPE_MISMATCH` on PHI type inconsistency
- [x] **LIR-03**: LIR verifier checks call argument types against callee's parameter types for direct calls
- [x] **LIR-04**: LIR verifier checks argument count matches parameter count for direct calls
- [x] **LIR-05**: LIR verifier emits `IRON_ERR_LIR_CALL_TYPE_MISMATCH` on argument type mismatch

### Generic Constraints

- [x] **GEN-01**: Compiler validates that concrete type arguments satisfy declared generic constraints at instantiation sites
- [x] **GEN-02**: Compiler checks constraint satisfaction for generic function calls
- [x] **GEN-03**: Compiler checks constraint satisfaction for generic type construction
- [x] **GEN-04**: Compiler emits `IRON_ERR_GENERIC_CONSTRAINT` (206) when a concrete type does not meet the constraint

### Cast Safety

- [x] **CAST-01**: Compiler validates that the source expression type is numeric or bool before allowing primitive cast
- [x] **CAST-02**: Compiler emits `IRON_ERR_INVALID_CAST` when source type is not castable
- [x] **CAST-03**: Compiler emits `IRON_WARN_NARROWING_CAST` for wider-to-narrower integer casts
- [x] **CAST-04**: Compiler validates compile-time constant values fit in the target narrow type (literal range check)

### Definite Assignment

- [x] **INIT-01**: Compiler performs definite assignment analysis tracking initialization state across control flow paths
- [x] **INIT-02**: Compiler detects variables that may be read before being assigned on all paths
- [x] **INIT-03**: Compiler emits `IRON_ERR_POSSIBLY_UNINITIALIZED` when a variable may be used uninitialized
- [x] **INIT-04**: Analysis handles if/else, match, loops, and early returns correctly

### Array Bounds

- [x] **BOUNDS-01**: Compiler validates constant array indices against known array sizes (`0 <= index < size`)
- [x] **BOUNDS-02**: Compiler emits `IRON_ERR_INDEX_OUT_OF_BOUNDS` for provably out-of-bounds constant indices
- [x] **BOUNDS-03**: Compiler validates that array index expressions resolve to integer types

### Slice Bounds

- [x] **SLICE-01**: Compiler validates that slice start and end expressions resolve to integer types
- [x] **SLICE-02**: Compiler validates `start <= end` when both are compile-time constants
- [x] **SLICE-03**: Compiler validates slice bounds are within array size when all values are compile-time constants
- [x] **SLICE-04**: Compiler emits `IRON_ERR_INVALID_SLICE_BOUNDS` for invalid constant slice bounds

### Escape Analysis

- [x] **ESC-01**: Escape analysis tracks heap values assigned through field access (`obj.field = heap_val`)
- [x] **ESC-02**: Escape analysis tracks heap values assigned through array index (`arr[i] = heap_val`)
- [x] **ESC-03**: Escape analysis tracks heap values passed as function arguments
- [x] **ESC-04**: Extended `expr_ident_name()` or equivalent recognizes field-access and index-access targets

### Compound Overflow

- [x] **OVFL-01**: Compiler detects compound assignments (`+=`, `-=`, `*=`, `/=`) on narrow integer types (Int8, Int16, UInt8, etc.)
- [x] **OVFL-02**: Compiler emits `IRON_WARN_POSSIBLE_OVERFLOW` when target type is narrower than platform int and RHS is not a fitting constant
- [x] **OVFL-03**: Compiler validates compile-time constant RHS values fit in the narrow target type

### String Interpolation

- [x] **STRN-01**: Compiler validates that interpolated expression types are stringifiable (primitives, String, Bool, or types with `to_string`)
- [x] **STRN-02**: Compiler emits `IRON_ERR_NOT_STRINGABLE` for types without string conversion capability

### Concurrency

- [x] **CONC-01**: Mutation detection in parallel/spawn blocks covers field access expressions (not just bare identifiers)
- [x] **CONC-02**: Mutation detection in parallel/spawn blocks covers array index expressions
- [x] **CONC-03**: Compiler detects concurrent reads of variables being written in spawn blocks (read-write races)
- [x] **CONC-04**: Compiler performs capture analysis for spawn blocks tracking which outer variables are referenced
- [x] **CONC-05**: Compiler validates that mutable captures in spawn blocks are flagged as potential data races

### Testing

- [x] **TEST-01**: Each new error/warning diagnostic has at least one test case with Iron source that triggers it
- [x] **TEST-02**: Each diagnostic has at least one test case with valid Iron source that should NOT trigger it (no false positives)
- [ ] **TEST-03**: Complex analyses (definite assignment, escape, concurrency) have edge-case tests for branching, loops, and nested structures

## v2 Requirements

Deferred to future release. Tracked but not in current roadmap.

### Runtime Safety

- **RTSAFE-01**: Runtime bounds-check code insertion in generated C (flag-controlled)
- **RTSAFE-02**: Runtime overflow checks for integer arithmetic

### Advanced Concurrency

- **ACONC-01**: Deadlock detection via lock ordering analysis
- **ACONC-02**: Task lifetime analysis (spawned tasks don't outlive referenced data)
- **ACONC-03**: Full effect-based or ownership-based race detection

### Advanced Generics

- **AGEN-01**: Indirect calls in LIR carry function type signatures for verification
- **AGEN-02**: Higher-kinded type constraint checking

## Out of Scope

| Feature | Reason |
|---------|--------|
| Runtime bounds checking insertion | Only static/compile-time checks in this scope |
| Full deadlock detection | Requires effect system or ownership model |
| Task lifetime borrow analysis | Requires borrow-checker-level analysis |
| C emitter changes | This is analyzer/verifier work only |
| New language features | Only adding checks for existing syntax |

## Traceability

| Requirement | Phase | Status |
|-------------|-------|--------|
| LIR-01 | Phase 32 | Complete |
| LIR-02 | Phase 32 | Complete |
| LIR-03 | Phase 32 | Complete |
| LIR-04 | Phase 32 | Complete |
| LIR-05 | Phase 32 | Complete |
| MATCH-01 | Phase 33 | Complete |
| MATCH-02 | Phase 33 | Complete |
| MATCH-03 | Phase 33 | Complete |
| CAST-01 | Phase 33 | Complete |
| CAST-02 | Phase 33 | Complete |
| CAST-03 | Phase 33 | Complete |
| CAST-04 | Phase 33 | Complete |
| STRN-01 | Phase 33 | Complete |
| STRN-02 | Phase 33 | Complete |
| OVFL-01 | Phase 33 | Complete |
| OVFL-02 | Phase 33 | Complete |
| OVFL-03 | Phase 33 | Complete |
| BOUNDS-01 | Phase 34 | Complete |
| BOUNDS-02 | Phase 34 | Complete |
| BOUNDS-03 | Phase 34 | Complete |
| SLICE-01 | Phase 34 | Complete |
| SLICE-02 | Phase 34 | Complete |
| SLICE-03 | Phase 34 | Complete |
| SLICE-04 | Phase 34 | Complete |
| ESC-01 | Phase 35 | Complete |
| ESC-02 | Phase 35 | Complete |
| ESC-03 | Phase 35 | Complete |
| ESC-04 | Phase 35 | Complete |
| INIT-01 | Phase 36 | Complete |
| INIT-02 | Phase 36 | Complete |
| INIT-03 | Phase 36 | Complete |
| INIT-04 | Phase 36 | Complete |
| GEN-01 | Phase 37 | Complete |
| GEN-02 | Phase 37 | Complete |
| GEN-03 | Phase 37 | Complete |
| GEN-04 | Phase 37 | Complete |
| CONC-01 | Phase 38 | Complete |
| CONC-02 | Phase 38 | Complete |
| CONC-03 | Phase 38 | Complete |
| CONC-04 | Phase 38 | Complete |
| CONC-05 | Phase 38 | Complete |
| TEST-01 | Phase 39 | Complete |
| TEST-02 | Phase 39 | Complete |
| TEST-03 | Phase 39 | Pending |

**Coverage:**
- v1 requirements: 44 total
- Mapped to phases: 44
- Unmapped: 0

---
*Requirements defined: 2026-04-02*
*Last updated: 2026-04-02 after roadmap creation*
