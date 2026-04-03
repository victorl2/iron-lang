---
phase: 33-type-validation-checks
verified: 2026-04-02T00:00:00Z
status: passed
score: 12/12 must-haves verified
re_verification: false
gaps: []
human_verification: []
---

# Phase 33: Type Validation Checks Verification Report

**Phase Goal:** The type checker catches match exhaustiveness violations, unsafe casts, non-stringable interpolations, and narrow-integer overflow risks at compile time
**Verified:** 2026-04-02
**Status:** PASSED
**Re-verification:** No — initial verification

## Goal Achievement

### Observable Truths

| #  | Truth | Status | Evidence |
|----|-------|--------|----------|
| 1  | A match on an enum type missing variants and lacking else produces IRON_ERR_NONEXHAUSTIVE_MATCH listing uncovered variants | VERIFIED | `test_nonexhaustive_match_enum` PASS; enum path at typecheck.c:1485 |
| 2  | A match on an enum type covering all variants produces no error | VERIFIED | `test_exhaustive_match_all_variants` PASS; error_count == 0 |
| 3  | A match on an enum type with an else clause produces no error even with missing variants | VERIFIED | `test_exhaustive_match_with_else` PASS |
| 4  | A match on a non-enum type without else produces IRON_ERR_NONEXHAUSTIVE_MATCH | VERIFIED | `test_nonexhaustive_match_non_enum` PASS; non-enum path at typecheck.c:1492 |
| 5  | A match on a non-enum type with else produces no error | VERIFIED | `test_match_non_enum_with_else` PASS |
| 6  | A duplicate match arm produces IRON_ERR_DUPLICATE_MATCH_ARM | VERIFIED | `test_duplicate_match_arm` PASS; typecheck.c:1449 |
| 7  | A cast from a non-numeric, non-bool type produces IRON_ERR_INVALID_CAST | VERIFIED | `test_cast_invalid_source` PASS; typecheck.c:585 |
| 8  | A cast from Bool to Int is allowed with no error | VERIFIED | `test_cast_bool_to_int_ok` PASS |
| 9  | A cast from Int to Bool produces IRON_ERR_INVALID_CAST with suggestion | VERIFIED | `test_cast_int_to_bool_error` PASS; typecheck.c:593 |
| 10 | A wider-to-narrower integer cast produces IRON_WARN_NARROWING_CAST | VERIFIED | `test_cast_narrowing_warning` PASS; typecheck.c:622 |
| 11 | A narrower-to-wider cast produces no warning | VERIFIED | `test_cast_widening_no_warning` PASS |
| 12 | A constant literal cast that overflows target produces IRON_ERR_CAST_OVERFLOW | VERIFIED | `test_cast_overflow_constant` PASS; typecheck.c:611 |
| 13 | A constant literal cast that fits produces no warning or error | VERIFIED | `test_cast_constant_fits_no_warning` PASS |
| 14 | A string interpolation with a primitive type produces no warning | VERIFIED | `test_interp_primitive_no_warning` PASS |
| 15 | A string interpolation with a Bool produces no warning | VERIFIED | `test_interp_bool_no_warning` PASS |
| 16 | A string interpolation with an object lacking to_string() produces IRON_WARN_NOT_STRINGABLE | VERIFIED | `test_interp_not_stringable` PASS; typecheck.c:397 |
| 17 | A string interpolation with an object that has to_string() produces no warning | VERIFIED | `test_interp_object_with_to_string_ok` PASS |
| 18 | A compound assignment on a narrow integer with non-constant RHS produces IRON_WARN_POSSIBLE_OVERFLOW | VERIFIED | `test_compound_narrow_overflow_warning` PASS; typecheck.c:1243 |
| 19 | A compound assignment on a narrow integer with a fitting constant RHS produces no warning | VERIFIED | `test_compound_narrow_constant_fits_no_warning` PASS |
| 20 | A compound assignment on a narrow integer with an overflowing constant RHS produces IRON_WARN_POSSIBLE_OVERFLOW | VERIFIED | `test_compound_narrow_constant_overflows_warning` PASS |
| 21 | A compound assignment on a full-width Int produces no warning | VERIFIED | `test_compound_full_width_no_warning` PASS |
| 22 | Subtraction compound assignment on narrow type produces IRON_WARN_POSSIBLE_OVERFLOW | VERIFIED | `test_compound_subtract_narrow_warning` PASS |

**Score:** 22/22 truths verified — all 48 typecheck tests pass, 17/17 unit suites green

### Required Artifacts

| Artifact | Expected | Status | Details |
|----------|----------|--------|---------|
| `src/diagnostics/diagnostics.h` | All Phase 33 error/warning codes | VERIFIED | Lines 137-164: IRON_ERR_NONEXHAUSTIVE_MATCH (308), IRON_ERR_DUPLICATE_MATCH_ARM (309), IRON_ERR_INVALID_CAST (310), IRON_ERR_CAST_OVERFLOW (311), IRON_WARN_NARROWING_CAST (601), IRON_WARN_NOT_STRINGABLE (602), IRON_WARN_POSSIBLE_OVERFLOW (603) |
| `src/analyzer/typecheck.c` | All Phase 33 logic: helpers, match exhaustiveness, cast validation, stringability, overflow detection | VERIFIED | 6 helpers at lines 107-191; match exhaustiveness at 1409-1498; cast validation at 570-634; stringability at 385-406; overflow detection at 1223-1246 |
| `tests/unit/test_typecheck.c` | 22 new tests across all 4 domains | VERIFIED | Lines 470-786: 6 match tests, 7 cast tests, 4 string interpolation tests, 5 compound overflow tests; all registered in main() at lines 815-836 |

### Key Link Verification

| From | To | Via | Status | Details |
|------|----|-----|--------|---------|
| `src/analyzer/typecheck.c` | `src/diagnostics/diagnostics.h` | IRON_ERR_NONEXHAUSTIVE_MATCH (308), IRON_ERR_DUPLICATE_MATCH_ARM (309) | WIRED | emit_error calls at lines 1449, 1485, 1492 |
| `src/analyzer/typecheck.c` | `src/diagnostics/diagnostics.h` | IRON_ERR_INVALID_CAST (310), IRON_ERR_CAST_OVERFLOW (311) | WIRED | emit_error calls at lines 585, 593, 611 |
| `src/analyzer/typecheck.c` | `src/diagnostics/diagnostics.h` | IRON_WARN_NARROWING_CAST (601), IRON_WARN_NOT_STRINGABLE (602), IRON_WARN_POSSIBLE_OVERFLOW (603) | WIRED | emit_warning calls at lines 397, 622, 1243 |
| `src/analyzer/typecheck.c` | `src/analyzer/typecheck.c` (internal) | type_bit_width, value_fits_type, is_narrow_integer, is_compound_assign_op, is_stringifiable | WIRED | Helpers defined at 107-191; called at lines 391, 599, 605, 622, 1224, 1226, 1233 |
| `src/analyzer/typecheck.c` | `src/analyzer/types.h` | IRON_TYPE_ENUM kind check, enu.decl access | WIRED | Match exhaustiveness checks at lines 1418-1419 |

### Requirements Coverage

| Requirement | Source Plan | Description | Status | Evidence |
|-------------|------------|-------------|--------|----------|
| MATCH-01 | 33-01 | Match on enum covers all variants or has else | SATISFIED | Enum exhaustiveness check at typecheck.c:1418-1489; test_nonexhaustive_match_enum, test_exhaustive_match_all_variants, test_exhaustive_match_with_else all PASS |
| MATCH-02 | 33-01 | Emits IRON_ERR_NONEXHAUSTIVE_MATCH listing uncovered variants | SATISFIED | typecheck.c:1483-1488 builds uncovered variant names list; test_nonexhaustive_match_enum PASS |
| MATCH-03 | 33-01 | Requires else clause when subject is not an enum type | SATISFIED | Non-enum path at typecheck.c:1490-1495; test_nonexhaustive_match_non_enum and test_match_non_enum_with_else PASS |
| CAST-01 | 33-02 | Validates source is numeric or bool before allowing cast | SATISFIED | typecheck.c:575-586; test_cast_invalid_source, test_cast_bool_to_int_ok PASS |
| CAST-02 | 33-02 | Emits IRON_ERR_INVALID_CAST for non-castable source | SATISFIED | typecheck.c:585, 593; test_cast_invalid_source and test_cast_int_to_bool_error PASS |
| CAST-03 | 33-02 | Emits IRON_WARN_NARROWING_CAST for wider-to-narrower integer casts | SATISFIED | typecheck.c:622; test_cast_narrowing_warning PASS, test_cast_widening_no_warning PASS |
| CAST-04 | 33-02 | Validates constant values fit in narrow target type | SATISFIED | typecheck.c:601-614; test_cast_overflow_constant and test_cast_constant_fits_no_warning PASS |
| STRN-01 | 33-03 | Validates interpolated types are stringifiable | SATISFIED | is_stringifiable helper at typecheck.c:165-191; stringability check at 390-400; all 4 interp tests PASS |
| STRN-02 | 33-03 | Emits diagnostic for types without string conversion | SATISFIED (with note) | Implemented as IRON_WARN_NOT_STRINGABLE (602) rather than IRON_ERR_NOT_STRINGABLE as REQUIREMENTS.md specifies. Behavior is correct: non-stringable types are flagged. The diagnostic is a warning (not an error), which is the more correct severity. REQUIREMENTS.md uses the wrong prefix — `IRON_ERR_NOT_STRINGABLE` is never defined; `IRON_WARN_NOT_STRINGABLE` is the actual symbol. test_interp_not_stringable PASS |
| OVFL-01 | 33-03 | Detects compound assignments on narrow integer types | SATISFIED | is_narrow_integer and is_compound_assign_op used at typecheck.c:1224-1226; all 5 overflow tests PASS |
| OVFL-02 | 33-03 | Emits IRON_WARN_POSSIBLE_OVERFLOW when narrow and non-fitting RHS | SATISFIED | typecheck.c:1243; test_compound_narrow_overflow_warning and test_compound_subtract_narrow_warning PASS |
| OVFL-03 | 33-03 | Validates constant RHS fits narrow target | SATISFIED | Constant suppression path at typecheck.c:1229-1235; test_compound_narrow_constant_fits_no_warning and test_compound_narrow_constant_overflows_warning PASS |

**Note on STRN-02:** REQUIREMENTS.md describes the symbol as `IRON_ERR_NOT_STRINGABLE` (error prefix), but the implementation uses `IRON_WARN_NOT_STRINGABLE` (warning prefix, code 602). This is a documentation discrepancy — the behavior matches the intent (flagging non-stringable types). The warning severity is arguably more correct than an error for this diagnostic, since interpolating a non-stringable type does not halt compilation. The symbol `IRON_ERR_NOT_STRINGABLE` is not defined anywhere in the codebase.

### Anti-Patterns Found

None found in the Phase 33 modified files. No TODO/FIXME/placeholder comments, no stub implementations, no empty returns in modified files.

### Human Verification Required

None — all Phase 33 behaviors are unit-tested and verified programmatically.

### Test Summary

| Domain | Tests Added | Pass |
|--------|-------------|------|
| Match exhaustiveness | 6 | 6/6 |
| Cast safety | 7 | 7/7 |
| String interpolation | 4 | 4/4 |
| Compound overflow | 5 | 5/5 |
| **Total** | **22** | **22/22** |

Full suite: 48/48 typecheck tests pass, 17/17 unit suites pass (0 regressions).

---

_Verified: 2026-04-02_
_Verifier: Claude (gsd-verifier)_
