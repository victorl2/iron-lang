# Iron Compiler — Semantic Analysis Gaps

## What This Is

A comprehensive effort to close all 12 known semantic analysis gaps in the Iron compiler. These gaps allow invalid or dangerous Iron code to pass through the analyzer, HIR, and LIR without producing diagnostics — resulting in C code that fails to compile, has undefined behavior, or silently produces wrong results. This work lives in a dedicated PR against the iron-lang repo.

## Core Value

Every invalid Iron program must produce a clear diagnostic at compile time — no silent pass-through to the C backend.

## Requirements

### Validated

(None yet — ship to validate)

### Active

- [ ] Match exhaustiveness checking for enum types
- [ ] PHI node type consistency verification in LIR
- [ ] Call argument type validation in LIR verifier
- [ ] Generic type constraint checking at instantiation sites
- [ ] Cast safety validation (source type, narrowing, literal range)
- [ ] Definite assignment analysis (uninitialized variable detection)
- [ ] Static array bounds checking for constant indices
- [ ] Slice bounds validation (type and range)
- [ ] Escape analysis for fields, arrays, and function arguments
- [ ] Compound assignment overflow detection for narrow integer types
- [ ] String interpolation type validation (stringability check)
- [ ] Concurrency safety beyond parallel-for (spawn captures, field/array mutations, read-write races)
- [ ] Test cases for every new diagnostic

### Out of Scope

- Runtime bounds checking insertion (only static/compile-time checks) — runtime checks are a separate feature
- Full deadlock detection — requires effect system or ownership model, longer-term project
- Task lifetime analysis for spawn — requires borrow-checker-level analysis
- Changes to the C code emitter — this is analyzer/verifier work only

## Context

- The Iron compiler is a C codebase that compiles Iron source to C
- All 12 gaps are documented in `docs/semantic_analysis_gaps.md` with exact file/line references
- Key source files: `typecheck.c`, `verify.c`, `escape.c`, `concurrency.c`, `hir_lower.c`, `types.c`
- AST types in `ast.h`, type system in `types.h`/`types.c`, LIR in `lir.h`
- The compiler already has working type checking, nullable access checks, name resolution, immutability enforcement, and interface conformance — this work extends coverage to unchecked areas
- Existing test infrastructure in `tests/` directory
- Memory sensitivity: avoid approaches that could cause excessive memory consumption during compilation (e.g., exponential path enumeration in dataflow analysis)

## Constraints

- **Language**: All implementation in C, matching existing codebase style
- **Memory**: Implementations must be memory-efficient — no unbounded allocations, use worklist algorithms with bounded state
- **Compatibility**: New diagnostics must not break valid Iron programs — only flag genuinely invalid code
- **PR scope**: Single dedicated PR with all 12 fixes, logically organized commits
- **Error codes**: Follow existing `IRON_ERR_*` and `IRON_WARN_*` naming conventions

## Key Decisions

| Decision | Rationale | Outcome |
|----------|-----------|---------|
| Full implementation for all gaps including complex ones (generics, definite assignment, concurrency) | User wants comprehensive coverage, not partial | — Pending |
| Work in worker-1/iron-lang copy | Keep main repo clean until PR is ready | — Pending |
| Test cases for every new diagnostic | Each gap's examples from the doc become test cases | — Pending |
| Static analysis only, no runtime checks | Scope control — runtime bounds checks are a separate feature | — Pending |

---
*Last updated: 2026-04-02 after initialization*
