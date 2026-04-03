---
phase: 38-concurrency-safety
verified: 2026-04-03T12:00:00Z
status: passed
score: 11/11 must-haves verified
re_verification: false
---

# Phase 38: Concurrency Safety Verification Report

**Phase Goal:** Mutation detection in parallel and spawn blocks covers field access, array index expressions, and read-write race patterns so that concurrent code with data races produces compile-time warnings
**Verified:** 2026-04-03
**Status:** passed
**Re-verification:** No — initial verification

---

## Goal Achievement

### Observable Truths

| # | Truth | Status | Evidence |
|---|-------|--------|----------|
| 1 | parallel for body assigning to obj.field where obj is an outer variable produces E0208 | VERIFIED | `test_parallel_for_field_mutation_error` passes; `check_stmt_for_mutation` calls `expr_ident_name` on assignment target and emits `IRON_ERR_PARALLEL_MUTATION` (208) |
| 2 | parallel for body assigning to arr[i] where arr is an outer variable produces E0208 | VERIFIED | `test_parallel_for_index_mutation_error` passes; `IRON_NODE_INDEX` branch in `expr_ident_name` extracts root name; outer check fires |
| 3 | parallel for body assigning to bare outer identifier still produces E0208 (existing behavior preserved) | VERIFIED | `test_parallel_for_outer_mutation_error` passes (original test, 5 original tests still green) |
| 4 | parallel for body assigning to local.field or local[i] does NOT produce E0208 | VERIFIED | `test_parallel_for_local_field_mutation_ok` and `test_parallel_for_partitioned_local_index_ok` pass; `name_is_local` check prevents false positive |
| 5 | All existing concurrency tests continue to pass | VERIFIED | All 5 original tests (outer mutation error, outer read ok, local mutation ok, sequential for ok, loop var ok) pass |
| 6 | Spawn block that writes to an outer variable produces IRON_WARN_SPAWN_DATA_RACE | VERIFIED | `test_spawn_write_outer_var_race` passes; `collect_spawn_refs` emits W0604 via `emit_warn` for outer writes |
| 7 | Spawn block capture analysis identifies which outer variables are referenced inside the block | VERIFIED | `collect_spawn_refs` in `concurrency.c` walks spawn body, populates `ctx->spawn_writes` and `ctx->spawn_reads` arrays for outer names |
| 8 | Mutable captures in spawn blocks are flagged as potential data races | VERIFIED | `test_spawn_field_write_outer_race` and `test_spawn_index_write_outer_race` pass; field access and index writes through `expr_ident_name` are detected |
| 9 | Spawn block that only reads outer variables does NOT produce a data race warning | VERIFIED | `test_spawn_read_outer_var_ok` passes; `spawn_reads` is populated but no warning emitted for read-only captures |
| 10 | Spawn block that mutates only local variables does NOT produce a warning | VERIFIED | `test_spawn_write_local_var_ok` passes; `name_is_local` check suppresses warning for spawn-local declarations |
| 11 | All Plan 01 field/index tests continue to pass after Plan 02 | VERIFIED | 14/14 tests pass; 18/18 unit tests across full suite pass |

**Score:** 11/11 truths verified

---

### Required Artifacts

| Artifact | Expected | Status | Details |
|----------|----------|--------|---------|
| `src/analyzer/concurrency.c` | Extended mutation detection covering field access and index targets | VERIFIED | 404 lines; contains `expr_ident_name`, `IRON_NODE_FIELD_ACCESS`, `IRON_NODE_INDEX`, `IRON_NODE_SPAWN`, `collect_spawn_refs`, `spawn_writes`, `emit_warn` |
| `src/diagnostics/diagnostics.h` | New warning code IRON_WARN_SPAWN_DATA_RACE | VERIFIED | Line 168: `#define IRON_WARN_SPAWN_DATA_RACE 604` present |
| `tests/unit/test_concurrency.c` | Tests for field/index mutation and spawn capture detection | VERIFIED | Contains `test_parallel_for_field_mutation_error`, `test_parallel_for_index_mutation_error`, `test_parallel_for_local_field_mutation_ok`, `test_parallel_for_partitioned_local_index_ok`, `test_spawn_write_outer_var_race`, `test_spawn_field_write_outer_race`, `test_spawn_index_write_outer_race`, `test_spawn_read_outer_var_ok`, `test_spawn_write_local_var_ok`; all 14 registered with `RUN_TEST()` |

---

### Key Link Verification

| From | To | Via | Status | Details |
|------|----|-----|--------|---------|
| `src/analyzer/concurrency.c` | `expr_ident_name` pattern from `escape.c` | Static helper function for recursive name extraction | WIRED | Independent static copy present at lines 43-55; handles `IRON_NODE_IDENT`, `IRON_NODE_FIELD_ACCESS`, `IRON_NODE_INDEX` |
| `src/analyzer/concurrency.c` | `walk_stmt` switch | `IRON_NODE_SPAWN` case in `walk_stmt` | WIRED | `case IRON_NODE_SPAWN:` present at line 306; saves state, collects locals, calls `collect_spawn_refs`, emits warnings, restores state |
| `src/analyzer/concurrency.c` | `src/diagnostics/diagnostics.h` | `IRON_WARN_SPAWN_DATA_RACE` constant | WIRED | `emit_warn(ctx, IRON_WARN_SPAWN_DATA_RACE, ...)` used at line 336; constant defined at diagnostics.h line 168 |

---

### Requirements Coverage

| Requirement | Source Plan | Description | Status | Evidence |
|-------------|------------|-------------|--------|----------|
| CONC-01 | 38-01 | Mutation detection covers field access expressions (not just bare identifiers) | SATISFIED | `expr_ident_name` recursively extracts root identifier from `IRON_NODE_FIELD_ACCESS`; `check_stmt_for_mutation` uses it; `test_parallel_for_field_mutation_error` passes |
| CONC-02 | 38-01 | Mutation detection covers array index expressions | SATISFIED | `expr_ident_name` handles `IRON_NODE_INDEX`; `test_parallel_for_index_mutation_error` passes |
| CONC-03 | 38-02 | Compiler detects concurrent reads of variables being written in spawn blocks (read-write races) | SATISFIED | `collect_spawn_refs` tracks `spawn_reads`; writes to outer vars detected and warned; `test_spawn_write_outer_var_race` passes |
| CONC-04 | 38-02 | Compiler performs capture analysis for spawn blocks tracking which outer variables are referenced | SATISFIED | `collect_spawn_refs` populates `ctx->spawn_writes` and `ctx->spawn_reads` by walking spawn body and checking `name_is_local`; two-pass approach (collect locals first, then walk for outer refs) |
| CONC-05 | 38-02 | Mutable captures in spawn blocks are flagged as potential data races | SATISFIED | `IRON_WARN_SPAWN_DATA_RACE` (604) emitted for each entry in `spawn_writes`; field and index writes also detected via `expr_ident_name`; three test cases confirm (bare ident, field access, index expression) |

**Orphaned requirements check:** REQUIREMENTS.md maps CONC-01 through CONC-05 all to Phase 38 with status Complete. All five are claimed in plan frontmatter (38-01: CONC-01, CONC-02; 38-02: CONC-03, CONC-04, CONC-05). No orphans.

---

### Anti-Patterns Found

| File | Line | Pattern | Severity | Impact |
|------|------|---------|----------|--------|
| None | — | — | — | — |

No TODOs, FIXMEs, placeholder returns, or stub implementations found in modified files. `collect_spawn_refs` has a bounded-capacity guard (`MAX_SPAWN_CAPTURES=64`) which is a deliberate conservative design choice, not a stub.

---

### Human Verification Required

None. All behavioral assertions are unit-tested and verified programmatically. The feature operates at the AST level with no UI, external services, or visual output requiring human observation.

---

### Test Execution Summary

- `ctest -R test_concurrency`: 14/14 individual tests pass (confirmed via verbose binary output)
- `ctest -L unit`: 18/18 unit test binaries pass — zero regressions across the full suite
- Build: clean, zero warnings (`cmake --build` exits with `[100%] Built target test_hir_to_lir`)

### Commit Verification

All four phase commits exist in `git log`:
- `eb9f9c7` — test(38-01): failing tests for field/index mutation detection
- `4218f9d` — feat(38-01): extend parallel-for mutation detection to field access and index targets
- `fb29514` — feat(38-02): add spawn capture analysis and race detection
- `e32df26` — test(38-02): add 5 spawn capture analysis tests

---

## Summary

Phase 38 goal is fully achieved. Both plans delivered substantive, non-stub implementations:

**Plan 01** extended `check_stmt_for_mutation` in `concurrency.c` with a `expr_ident_name` helper (mirroring the pattern from `escape.c`) that recursively unwraps field-access chains and index expressions to their root identifier. This made E0208 fire for `obj.field = val` and `arr[i] = val` when `obj`/`arr` are outer-scope variables, with correct suppression for spawn-local declarations.

**Plan 02** added a complete spawn-block analysis subsystem: the `ConcurrencyCtx` struct gained `in_spawn`, `spawn_writes`, and `spawn_reads` fields; a new `collect_spawn_refs` recursive walker populates them; a new `case IRON_NODE_SPAWN:` branch in `walk_stmt` orchestrates the two-pass analysis (collect locals first, then walk for outer refs); and `IRON_WARN_SPAWN_DATA_RACE` (604) is emitted via a new `emit_warn` helper for each outer write detected. The `has_warning()` helper in tests correctly checks `level == IRON_DIAG_WARNING` to distinguish warnings from errors.

All five requirement IDs (CONC-01 through CONC-05) are fully satisfied with passing unit tests and no false positives.

---

_Verified: 2026-04-03_
_Verifier: Claude (gsd-verifier)_
