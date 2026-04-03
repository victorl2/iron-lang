# Requirements: Iron ADTs

**Defined:** 2026-04-02
**Core Value:** Enums can carry data in their variants, and `match` exhaustively destructures them — the type system guarantees every case is handled.

## v1 Requirements

Requirements for ADT milestone. Each maps to roadmap phases.

### Enum Variant Data

- [ ] **EDATA-01**: Enum variants can hold tuple-style associated data (`Circle(Float)`, `BinOp(Expr, Op, Expr)`)
- [x] **EDATA-02**: Variant construction uses dot syntax (`Shape.Circle(5.0)`)
- [ ] **EDATA-03**: Plain C-style enums (no payloads) continue to compile and work unchanged
- [ ] **EDATA-04**: Compiler detects recursive variant types and auto-boxes them (heap allocation via arena)

### Pattern Matching

- [x] **MATCH-01**: Match arms use `->` syntax for single expressions and `-> { }` for multi-line blocks
- [x] **MATCH-02**: Pattern matching destructures variant payloads into named bindings (`Circle(r) -> use(r)`)
- [x] **MATCH-03**: `_` wildcard ignores individual fields in patterns
- [ ] **MATCH-04**: `else` arm catches all remaining variants
- [ ] **MATCH-05**: Compiler errors on non-exhaustive match (lists missing variants in diagnostic)
- [ ] **MATCH-06**: Nested pattern destructuring works (`BinOp(IntLit(n), _, _)`)
- [x] **MATCH-07**: Existing match statements migrate from `{ }` arm syntax to `->` syntax

### Methods on Enums

- [ ] **EMETH-01**: Methods can be defined on enum types using `func EnumType.method()` syntax
- [ ] **EMETH-02**: `self` in enum methods refers to the enum value, usable in match

### Generic Enums

- [ ] **GENER-01**: Enums support generic type parameters (`Option[T]`, `Result[T, E]`)
- [ ] **GENER-02**: Generic enums are monomorphized (consistent with existing generic functions/objects)
- [ ] **GENER-03**: C emission uses type-argument-aware mangling to avoid typedef collisions

## v2 Requirements

Deferred to future release. Tracked but not in current roadmap.

### Advanced Pattern Matching

- **AMATCH-01**: Pattern guards (`Expr.IntLit(n) if n > 0 -> ...`)
- **AMATCH-02**: Or-patterns (`Expr.IntLit(_) | Expr.BoolLit(_) -> ...`)
- **AMATCH-03**: Match as expression (match returns a value)

### Named Field Variants

- **NFVAR-01**: Variants with named fields (`Circle { radius: Float }`)

## Out of Scope

| Feature | Reason |
|---------|--------|
| Named-field variants | Tuple-style only for v1; keeps implementation focused |
| Pattern guards | Adds complexity to exhaustiveness checking; defer |
| Or-patterns | Requires decision tree code-size explosion handling; defer |
| Match as expression | Requires return-type unification across arms; defer |
| `Option[T]` replacing `T?` | Nullable syntax stays independent; both coexist |
| LSP implementation | This milestone is the compiler foundation for it |

## Traceability

Which phases cover which requirements. Updated during roadmap creation.

| Requirement | Phase | Status |
|-------------|-------|--------|
| EDATA-01 | Phase 32 | Pending |
| EDATA-02 | Phase 32 | Complete |
| EDATA-03 | Phase 32 + Phase 35 | Pending |
| EDATA-04 | Phase 38 | Pending |
| MATCH-01 | Phase 32 + Phase 36 | Complete |
| MATCH-02 | Phase 33 | Complete |
| MATCH-03 | Phase 33 | Complete |
| MATCH-04 | Phase 33 | Pending |
| MATCH-05 | Phase 33 | Pending |
| MATCH-06 | Phase 34 | Pending |
| MATCH-07 | Phase 33 + Phase 36 | Complete |
| EMETH-01 | Phase 36 | Pending |
| EMETH-02 | Phase 36 | Pending |
| GENER-01 | Phase 37 | Pending |
| GENER-02 | Phase 37 | Pending |
| GENER-03 | Phase 37 | Pending |

**Coverage:**
- v1 requirements: 16 total
- Mapped to phases: 16
- Unmapped: 0

---
*Requirements defined: 2026-04-02*
*Last updated: 2026-04-02 after roadmap creation*
