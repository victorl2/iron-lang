# Phase 39: Diagnostic Test Sweep - Context

**Gathered:** 2026-04-03
**Status:** Ready for planning

<domain>
## Phase Boundary

Comprehensive test coverage audit for all diagnostics added in phases 32-38. Every new `IRON_ERR_*` and `IRON_WARN_*` must have positive tests (triggers the diagnostic) and negative tests (valid code that must NOT trigger it). Complex analyses need edge-case tests.

</domain>

<decisions>
## Implementation Decisions

### Test Coverage Requirements
- Every new error/warning diagnostic needs at least one positive test (triggers it)
- Every new diagnostic needs at least one negative test (valid code, no false positive)
- Complex analyses (definite assignment, escape, concurrency) need edge-case tests for branching, loops, nested structures

### New Diagnostics Added (Phases 32-38)
- Phase 32: IRON_ERR_LIR_PHI_TYPE_MISMATCH (306), IRON_ERR_LIR_CALL_TYPE_MISMATCH (307)
- Phase 33: IRON_ERR_NONEXHAUSTIVE_MATCH (308), IRON_ERR_DUPLICATE_MATCH_ARM (309), IRON_ERR_INVALID_CAST (310), IRON_ERR_CAST_OVERFLOW (311), IRON_WARN_NARROWING_CAST (601), IRON_WARN_NOT_STRINGABLE (602), IRON_WARN_POSSIBLE_OVERFLOW (603)
- Phase 34: IRON_ERR_INDEX_OUT_OF_BOUNDS (312), IRON_ERR_INVALID_SLICE_BOUNDS (313)
- Phase 35: (no new error codes — escape analysis extensions)
- Phase 36: IRON_ERR_POSSIBLY_UNINITIALIZED (314)
- Phase 37: IRON_ERR_GENERIC_CONSTRAINT (206 — pre-existing code, newly emitted)
- Phase 38: IRON_WARN_SPAWN_DATA_RACE (604)

### Approach
- Audit existing tests first — many diagnostics already have tests from TDD during implementation
- Identify gaps where positive or negative tests are missing
- Add .iron test files for integration-level testing where appropriate
- Add edge-case unit tests for definite assignment (nested if/else, match with return, loops with break) and concurrency (nested spawn, spawn inside parallel-for)

### Claude's Discretion
- Whether to add integration tests (.iron files) or unit tests or both
- Which edge cases are most valuable to test
- How to organize test files

</decisions>

<canonical_refs>
## Canonical References

### Existing Test Files
- tests/unit/test_lir_verify.c — LIR verifier tests (Phase 32)
- tests/unit/test_typecheck.c — Type checker tests (Phases 33, 34, 37)
- tests/unit/test_escape.c — Escape analysis tests (Phase 35)
- tests/unit/test_init_check.c — Definite assignment tests (Phase 36)
- tests/unit/test_concurrency.c — Concurrency tests (Phase 38)

### Diagnostics
- src/diagnostics/diagnostics.h — All error/warning codes

</canonical_refs>

<code_context>
## Existing Code Insights

### What Already Has Tests
- Phase 32: 6 LIR tests (PHI mismatch, call mismatch, valid cases)
- Phase 33: 22 typecheck tests (match, cast, string interp, overflow)
- Phase 34: 13 bounds tests (array index, slice bounds)
- Phase 35: 7 escape tests (field, index, call arg)
- Phase 36: 14 init_check tests (basic, control flow, edge cases)
- Phase 37: 6 generic constraint tests
- Phase 38: 14 concurrency tests (parallel-for mutations, spawn captures)

Total: ~82 new tests across phases 32-38. The TDD approach means most diagnostics already have both positive and negative tests.

### What May Need Gaps Filled
- Integration-level .iron test files that exercise the full compilation pipeline
- Edge cases for nested control flow in definite assignment
- Edge cases for spawn inside parallel-for in concurrency

</code_context>

<deferred>
## Deferred Ideas

None

</deferred>

---

*Phase: 39-diagnostic-test-sweep*
*Context gathered: 2026-04-03*
