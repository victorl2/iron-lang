---
phase: 32-lir-verifier-hardening
verified: 2026-04-02T00:00:00Z
status: passed
score: 9/9 must-haves verified
re_verification: false
---

# Phase 32: LIR Verifier Hardening Verification Report

**Phase Goal:** The LIR verifier catches type mismatches in PHI nodes and call instructions before they reach the C emitter, preventing invalid C output from well-typed Iron programs
**Verified:** 2026-04-02
**Status:** PASSED
**Re-verification:** No — initial verification

## Goal Achievement

### Observable Truths

| #  | Truth                                                                                          | Status     | Evidence                                                                 |
|----|-----------------------------------------------------------------------------------------------|------------|--------------------------------------------------------------------------|
| 1  | LIR verifier rejects PHI nodes whose incoming values have types differing from the PHI result type | VERIFIED | Invariant 7 loop at verify.c:480-509; test_verify_phi_type_mismatch PASS |
| 2  | LIR verifier emits IRON_ERR_LIR_PHI_TYPE_MISMATCH (code 306) for PHI type inconsistencies    | VERIFIED   | diagnostics.h:133; verify.c:504 emits code 306; test asserts has_error 306 |
| 3  | LIR verifier accepts PHI nodes where all incoming value types match the PHI result type       | VERIFIED   | test_verify_phi_well_formed returns true with 0 errors: PASS            |
| 4  | PHI check skips synthetic param ValueIds (1..param_count) that are NULL in value_table        | VERIFIED   | verify.c:490: `if ((int)val_id >= 1 && (int)val_id <= fn->param_count) continue;` |
| 5  | LIR verifier rejects direct calls where argument count differs from callee parameter count    | VERIFIED   | Invariant 8 loop at verify.c:511-561; test_verify_call_arg_count_mismatch PASS |
| 6  | LIR verifier rejects direct calls where argument type differs from corresponding parameter type | VERIFIED | verify.c:548-558 iron_type_equals check; test_verify_call_arg_type_mismatch PASS |
| 7  | LIR verifier emits IRON_ERR_LIR_CALL_TYPE_MISMATCH (code 307) for count and type mismatches  | VERIFIED   | diagnostics.h:134; verify.c:528,556 emit code 307; tests assert has_error 307 |
| 8  | LIR verifier accepts direct calls with correct argument count and types                        | VERIFIED   | test_verify_call_well_formed returns true with 0 errors: PASS           |
| 9  | LIR verifier skips indirect calls (func_decl == NULL) without error                           | VERIFIED   | verify.c:515: `if (instr->call.func_decl == NULL) continue;`; test_verify_call_indirect_skipped PASS |

**Score:** 9/9 truths verified

### Required Artifacts

| Artifact                             | Expected                                    | Status     | Details                                                        |
|--------------------------------------|---------------------------------------------|------------|----------------------------------------------------------------|
| `src/diagnostics/diagnostics.h`      | IRON_ERR_LIR_PHI_TYPE_MISMATCH (306)        | VERIFIED   | Line 133: `#define IRON_ERR_LIR_PHI_TYPE_MISMATCH      306`   |
| `src/diagnostics/diagnostics.h`      | IRON_ERR_LIR_CALL_TYPE_MISMATCH (307)       | VERIFIED   | Line 134: `#define IRON_ERR_LIR_CALL_TYPE_MISMATCH     307`   |
| `src/lir/verify.c`                   | Invariant 7 PHI type consistency check      | VERIFIED   | Lines 479-509, 580 lines total, substantive implementation    |
| `src/lir/verify.c`                   | Invariant 8 call argument validation        | VERIFIED   | Lines 511-561, find_func_by_name helper at lines 212-220      |
| `tests/lir/test_lir_verify.c`        | PHI type mismatch tests                     | VERIFIED   | test_verify_phi_type_mismatch and test_verify_phi_well_formed present and passing |
| `tests/lir/test_lir_verify.c`        | Call argument validation tests              | VERIFIED   | All 4 call tests present (count_mismatch, type_mismatch, well_formed, indirect_skipped) and passing |

### Key Link Verification

| From                      | To                                   | Via                                  | Status   | Details                                             |
|---------------------------|--------------------------------------|--------------------------------------|----------|-----------------------------------------------------|
| `src/lir/verify.c`        | `src/diagnostics/diagnostics.h`      | IRON_ERR_LIR_PHI_TYPE_MISMATCH       | WIRED    | verify.c:504 uses constant from diagnostics.h       |
| `src/lir/verify.c`        | `src/diagnostics/diagnostics.h`      | IRON_ERR_LIR_CALL_TYPE_MISMATCH      | WIRED    | verify.c:528,556 use constant from diagnostics.h   |
| `src/lir/verify.c`        | `src/analyzer/types.h`               | iron_type_equals() for PHI check     | WIRED    | verify.c:497 calls iron_type_equals in Invariant 7  |
| `src/lir/verify.c`        | `src/analyzer/types.h`               | iron_type_equals() for call check    | WIRED    | verify.c:549 calls iron_type_equals in Invariant 8  |
| `src/lir/verify.c`        | IronLIR_Module                       | Lookup callee via module->funcs      | WIRED    | find_func_by_name at lines 213-220 uses module->funcs; (void)module removed |

### Requirements Coverage

| Requirement | Source Plan | Description                                                                | Status    | Evidence                                                      |
|-------------|-------------|----------------------------------------------------------------------------|-----------|---------------------------------------------------------------|
| LIR-01      | 32-01       | LIR verifier checks all PHI incoming values have types matching PHI result | SATISFIED | Invariant 7 loop iterates phi.values, compares via iron_type_equals |
| LIR-02      | 32-01       | LIR verifier emits IRON_ERR_LIR_PHI_TYPE_MISMATCH on PHI type inconsistency | SATISFIED | diagnostics.h:133 defines code 306; verify.c:504 emits it   |
| LIR-03      | 32-02       | LIR verifier checks call argument types against callee parameter types     | SATISFIED | Invariant 8 per-arg loop at verify.c:535-559                 |
| LIR-04      | 32-02       | LIR verifier checks argument count matches parameter count for direct calls | SATISFIED | verify.c:522 checks arg_count != callee->param_count         |
| LIR-05      | 32-02       | LIR verifier emits IRON_ERR_LIR_CALL_TYPE_MISMATCH on argument type mismatch | SATISFIED | diagnostics.h:134 defines code 307; verify.c:528,556 emit it |

All 5 requirements from REQUIREMENTS.md (LIR-01 through LIR-05) are marked Complete in the traceability table and verified in the codebase.

No orphaned requirements: REQUIREMENTS.md maps LIR-01..LIR-05 exclusively to Phase 32, and both plans together claim all five IDs.

### Anti-Patterns Found

| File                      | Line    | Pattern                                    | Severity | Impact |
|---------------------------|---------|--------------------------------------------|----------|--------|
| `src/lir/verify.c`        | 202,397 | "placeholder" in comments about POISON instr | Info    | Legitimate use — describes the POISON instruction kind, not a stub |

No blocker or warning anti-patterns. The two "placeholder" matches are comments accurately describing `IRON_LIR_POISON`, a first-class instruction kind representing a lowering error. They are not stub indicators.

### Human Verification Required

None. All observable behaviors are verifiable programmatically via the test suite.

### Test Results

Build: `cmake --build build --target test_lir_verify` — exit 0

Runtime: `./build/tests/lir/test_lir_verify` — 14 tests, 0 failures, 0 ignored

```
test_verify_well_formed:PASS
test_verify_missing_terminator:PASS
test_verify_use_before_def:PASS
test_verify_invalid_branch_target:PASS
test_verify_instr_after_terminator:PASS
test_verify_no_entry_block:PASS
test_verify_multiple_errors:PASS
test_verify_return_type_mismatch:PASS
test_verify_phi_type_mismatch:PASS
test_verify_phi_well_formed:PASS
test_verify_call_arg_count_mismatch:PASS
test_verify_call_arg_type_mismatch:PASS
test_verify_call_well_formed:PASS
test_verify_call_indirect_skipped:PASS
```

Note: ctest -L lir reports 4 other LIR test targets (test_lir_data, test_lir_print, test_lir_emit, test_lir_optimize) as "Not Run" because their binaries have not been built yet. This is a pre-existing state unrelated to Phase 32; only test_lir_verify is in scope for this phase and it passes completely.

### Gaps Summary

No gaps. All 9 observable truths are verified, all 5 requirements are satisfied, all 6 artifacts are substantive and wired, all key links are confirmed present in the actual source code.

---

_Verified: 2026-04-02_
_Verifier: Claude (gsd-verifier)_
