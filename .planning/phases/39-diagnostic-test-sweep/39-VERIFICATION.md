---
phase: 39-diagnostic-test-sweep
verified: 2026-04-03T00:00:00Z
status: gaps_found
score: 4/7 roadmap success criteria verified
re_verification: false
gaps:
  - truth: "Every diagnostic has at least one .iron test file that triggers it (SC1)"
    status: partial
    reason: "Diagnostics 306, 307 (LIR verifier) and 604 (concurrency) are tested via hand-built ASTs in C test files, not via Iron source strings. Tests for 308-314, 601-603, 206 do embed Iron source strings in C unit tests, which satisfies the requirement intent. The REQUIREMENTS.md definition says 'test case with Iron source' (satisfied by C-embedded strings), but the ROADMAP success criterion specifically says '.iron test file' -- a stricter reading that hand-built-AST tests do not satisfy for codes 306, 307, 604."
    artifacts:
      - path: "tests/lir/test_lir_verify.c"
        issue: "Tests for IRON_ERR_LIR_PHI_TYPE_MISMATCH (306) and IRON_ERR_LIR_CALL_TYPE_MISMATCH (307) build LIR by hand -- no Iron source strings"
      - path: "tests/unit/test_concurrency.c"
        issue: "Tests for IRON_WARN_SPAWN_DATA_RACE (604) build ASTs by hand -- no Iron source strings"
    missing:
      - "Iron source-string-based tests (or .iron files) for LIR diagnostics 306 and 307"
      - "Iron source-string-based tests for IRON_WARN_SPAWN_DATA_RACE (604)"
  - truth: "Concurrency analysis has edge-case test for spawn inside parallel-for (SC4)"
    status: failed
    reason: "SC4 explicitly requires 'spawn inside parallel-for'. The test added was test_spawn_inside_sequential_for (spawn inside a sequential for loop). A parallel-for body containing a spawn block is a distinct scenario not covered."
    artifacts:
      - path: "tests/unit/test_concurrency.c"
        issue: "test_spawn_inside_sequential_for tests spawn in sequential for; no test for spawn inside parallel-for (is_parallel=true ForStmt)"
    missing:
      - "test_spawn_inside_parallel_for: a test where a parallel-for body contains a spawn block"
  - truth: "Test count grows by at least 30 new diagnostic-focused tests (SC5)"
    status: failed
    reason: "Only 15 new test functions were added across all phase 39 commits. The roadmap success criterion requires a growth of at least 30 new diagnostic-focused tests."
    artifacts:
      - path: "tests/unit/test_typecheck.c"
        issue: "1 new test function added (was 67, now 68)"
      - path: "tests/lir/test_lir_verify.c"
        issue: "0 new test functions added (was 14, still 14; one inline assertion added)"
      - path: "tests/unit/test_init_check.c"
        issue: "8 new test functions added (was 14, now 22)"
      - path: "tests/unit/test_concurrency.c"
        issue: "6 new test functions added (was 14, now 20)"
    missing:
      - "15 additional diagnostic-focused test functions to reach the 30-test growth target"
---

# Phase 39: Diagnostic Test Sweep Verification Report

**Phase Goal:** Every new diagnostic introduced in phases 32-38 has both positive tests (triggers the diagnostic) and negative tests (valid code that must not trigger it), with edge-case coverage for complex analyses
**Verified:** 2026-04-03
**Status:** gaps_found
**Re-verification:** No -- initial verification

## Goal Achievement

### Observable Truths (from Roadmap Success Criteria)

| #  | Truth                                                                                | Status      | Evidence                                                                             |
|----|--------------------------------------------------------------------------------------|-------------|--------------------------------------------------------------------------------------|
| 1  | Every IRON_ERR_*/IRON_WARN_* diagnostic has a test that triggers it                  | ✓ VERIFIED  | All 14 codes have pos >= 1 (ASSERT_TRUE); all test suites pass 100%                 |
| 2  | Every diagnostic has a negative test (no false positives)                            | ✓ VERIFIED  | All 14 codes have neg >= 1 (ASSERT_FALSE); confirmed by grep coverage matrix        |
| 3  | Definite assignment has edge-case tests (nested if/else, early returns)              | ✓ VERIFIED  | 8 new tests in test_init_check.c covering nested branching, match variants, returns |
| 4  | Concurrency has edge-case tests for nested spawn and multiple shared vars            | ✓ VERIFIED  | test_multiple_spawns_same_var, test_spawn_read_write_different_vars present          |
| 5  | SC1/SC2: Tests use Iron source (or .iron files) for diagnostics 306, 307, 604       | ✗ PARTIAL   | Hand-built ASTs used for LIR (306, 307) and concurrency (604) diagnostics           |
| 6  | SC4: Concurrency has edge-case test for spawn inside parallel-for                    | ✗ FAILED    | Only test_spawn_inside_sequential_for exists; parallel-for variant is absent        |
| 7  | SC5: Test count grows by at least 30 new diagnostic-focused tests                   | ✗ FAILED    | 15 new test functions added (target: 30)                                            |

**Score:** 4/7 truths verified

### Diagnostic Coverage Matrix

| Code | Name                          | Phase | Positive Tests | Negative Tests | Status    |
|------|-------------------------------|-------|---------------|----------------|-----------|
| 306  | IRON_ERR_LIR_PHI_TYPE_MISMATCH | 32   | 1             | 1              | ✓ Covered |
| 307  | IRON_ERR_LIR_CALL_TYPE_MISMATCH | 32  | 2             | 1              | ✓ Covered |
| 308  | IRON_ERR_NONEXHAUSTIVE_MATCH  | 33    | 2             | 1              | ✓ Covered |
| 309  | IRON_ERR_DUPLICATE_MATCH_ARM  | 33    | 1             | 1              | ✓ Covered |
| 310  | IRON_ERR_INVALID_CAST         | 33    | 2             | 1              | ✓ Covered |
| 311  | IRON_ERR_CAST_OVERFLOW        | 33    | 1             | 1              | ✓ Covered |
| 312  | IRON_ERR_INDEX_OUT_OF_BOUNDS  | 34    | 2             | 3              | ✓ Covered |
| 313  | IRON_ERR_INVALID_SLICE_BOUNDS | 34    | 3             | 3              | ✓ Covered |
| 314  | IRON_ERR_POSSIBLY_UNINITIALIZED | 36  | 9             | 13             | ✓ Covered |
| 206  | IRON_ERR_GENERIC_CONSTRAINT   | 37    | 3             | 3              | ✓ Covered |
| 601  | IRON_WARN_NARROWING_CAST      | 33    | 1             | 2              | ✓ Covered |
| 602  | IRON_WARN_NOT_STRINGABLE      | 33    | 1             | 3              | ✓ Covered |
| 603  | IRON_WARN_POSSIBLE_OVERFLOW   | 33    | 3             | 2              | ✓ Covered |
| 604  | IRON_WARN_SPAWN_DATA_RACE     | 38    | 6             | 3              | ✓ Covered |

All 14 diagnostic codes from phases 32-38 have both positive and negative test assertions.

### Required Artifacts

| Artifact                              | Expected                                           | Status     | Details                                              |
|---------------------------------------|----------------------------------------------------|------------|------------------------------------------------------|
| `tests/unit/test_typecheck.c`         | Contains test_unique_match_arms_no_duplicate_error | ✓ VERIFIED | Function at line 584, RUN_TEST at line 1125          |
| `tests/unit/test_init_check.c` (P01)  | Contains test_match_with_else_all_assign_no_error  | ✗ ORPHANED | Function name absent; test_match_all_arms_assign (pre-existing) covers this scenario |
| `tests/unit/test_concurrency.c` (P01) | Contains test_spawn_read_only_no_race             | ✗ ORPHANED | Function name absent; test_spawn_read_outer_var_ok (pre-existing) covers this scenario |
| `tests/unit/test_init_check.c` (P02)  | Contains test_nested_if_else                      | ✓ VERIFIED | test_nested_if_else_both_assign at line 205          |
| `tests/unit/test_concurrency.c` (P02) | Contains test_spawn_inside_parallel_for           | ✗ FAILED   | test_spawn_inside_sequential_for exists instead; parallel-for variant absent |

**Note on ORPHANED artifacts:** The two Plan 01 artifacts for test_init_check.c and test_concurrency.c specified function names that were never created. The SUMMARY confirms Plan 01 only modified test_typecheck.c and test_lir_verify.c. The underlying negative-test coverage was already present via pre-existing tests, so TEST-01/TEST-02 compliance is not broken. However, the specific artifact contracts from Plan 01 were not fulfilled.

### Key Link Verification

| From                              | To                          | Via                                          | Status     | Details                                            |
|-----------------------------------|-----------------------------|----------------------------------------------|------------|----------------------------------------------------|
| `tests/unit/test_typecheck.c`     | `diagnostics/diagnostics.h` | has_error.*IRON_ERR_DUPLICATE_MATCH_ARM      | ✓ WIRED    | Assertion at line 579 (positive) and 600 (negative) |
| `tests/unit/test_init_check.c`    | `src/analyzer/init_check.c` | iron_init_check call in run_init_check helper | ✓ WIRED    | iron_init_check called at line 56 of the helper    |
| `tests/unit/test_concurrency.c`   | `src/analyzer/concurrency.c` | iron_concurrency_check call                 | ✓ WIRED    | iron_concurrency_check called at lines 270, 306, 336, 366, 392, ... |

### Requirements Coverage

| Requirement | Source Plan     | Description                                                                                     | Status     | Evidence                                               |
|-------------|-----------------|------------------------------------------------------------------------------------------------|------------|--------------------------------------------------------|
| TEST-01     | 39-01 (Plan 01) | Each new error/warning diagnostic has at least one test case with Iron source that triggers it  | ✓ SATISFIED | All 14 diagnostic codes have pos >= 1 assertions       |
| TEST-02     | 39-01 (Plan 01) | Each diagnostic has at least one test case with valid Iron source that should NOT trigger it    | ✓ SATISFIED | All 14 diagnostic codes have neg >= 1 assertions       |
| TEST-03     | 39-02 (Plan 02) | Complex analyses have edge-case tests for branching, loops, and nested structures               | ✓ SATISFIED | 8 init_check + 6 concurrency edge-case tests added     |

All three requirement IDs from the PLAN frontmatter are satisfied. No orphaned requirements in REQUIREMENTS.md for Phase 39.

### Anti-Patterns Found

| File                             | Line | Pattern                                  | Severity | Impact                         |
|----------------------------------|------|------------------------------------------|----------|--------------------------------|
| `tests/unit/test_concurrency.c`  | 178  | "placeholder ident" in code comment     | Info     | Inline comment explaining test AST construction; not an implementation stub |

No blockers or warnings found. The "placeholder" occurrence is a developer comment explaining why a simple ident is used in place of a range() call expression -- this is appropriate for a hand-built AST test.

### Human Verification Required

No items require human verification. All assertions and test results are verifiable programmatically.

### Gaps Summary

Three gaps block full goal achievement against the roadmap success criteria:

**Gap 1 (SC1/SC2 partial -- low severity):** Diagnostics 306, 307, and 604 are tested via hand-built ASTs rather than Iron source strings. The REQUIREMENTS.md definition of TEST-01/TEST-02 says "test case with Iron source" which is satisfied by the C unit tests embedding Iron source strings for the other 11 diagnostics. However, the ROADMAP success criteria say ".iron test file" which is stricter. For LIR and AST-builder tests this was the pre-established pattern (not introduced in phase 39), but the strict SC1/SC2 wording is not met.

**Gap 2 (SC4 failed -- medium severity):** The roadmap explicitly lists "spawn inside parallel-for" as a required concurrency edge-case. The test `test_spawn_inside_sequential_for` covers spawn inside a sequential for loop, but the parallel-for variant (ForStmt.is_parallel = true) is a distinct execution context and is absent. Iron does support parallel-for (is_parallel flag on Iron_ForStmt).

**Gap 3 (SC5 failed -- medium severity):** Roadmap requires test count to grow by at least 30 new diagnostic-focused tests. The phase added 15 new test functions (1 in test_typecheck.c, 8 in test_init_check.c, 6 in test_concurrency.c) plus 3 inline assertions -- 15 short of the 30-test target.

The core goal -- every diagnostic has positive and negative coverage -- is fully achieved. The gaps are in two specific success criteria (test count and spawn-in-parallel-for) plus a strict reading of the .iron file criterion.

---

_Verified: 2026-04-03_
_Verifier: Claude (gsd-verifier)_
