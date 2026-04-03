---
phase: 37-generic-constraint-checking
verified: 2026-04-03T11:10:00Z
status: gaps_found
score: 5/6 must-haves verified
re_verification: false
gaps:
  - truth: "IRON_ERR_GENERIC_CONSTRAINT (206) emitted naming the constraint and the failing type"
    status: partial
    reason: "GEN-04 in REQUIREMENTS.md specifies the error code symbolic name as IRON_ERR_CONSTRAINT_NOT_SATISFIED, but the implementation defines and uses IRON_ERR_GENERIC_CONSTRAINT (206) instead. The error code number (206) and behavior (error emitted with constraint name and failing type in message) are correct, but the identifier name diverges from the requirement specification."
    artifacts:
      - path: "src/diagnostics/diagnostics.h"
        issue: "Defines IRON_ERR_GENERIC_CONSTRAINT 206, but GEN-04 requires IRON_ERR_CONSTRAINT_NOT_SATISFIED"
      - path: "src/analyzer/typecheck.c"
        issue: "References IRON_ERR_GENERIC_CONSTRAINT; if requirement name is canonical, this needs updating or REQUIREMENTS.md needs a correction"
      - path: "tests/unit/test_typecheck.c"
        issue: "Tests assert IRON_ERR_GENERIC_CONSTRAINT; would need update if name is corrected"
    missing:
      - "Resolve naming: either rename IRON_ERR_GENERIC_CONSTRAINT -> IRON_ERR_CONSTRAINT_NOT_SATISFIED in diagnostics.h, typecheck.c, and test_typecheck.c, OR update REQUIREMENTS.md GEN-04 to use IRON_ERR_GENERIC_CONSTRAINT"
---

# Phase 37: Generic Constraint Checking Verification Report

**Phase Goal:** Concrete type arguments that violate declared generic constraints are rejected at instantiation sites
**Verified:** 2026-04-03T11:10:00Z
**Status:** gaps_found
**Re-verification:** No — initial verification

## Goal Achievement

### Observable Truths

| # | Truth | Status | Evidence |
|---|-------|--------|----------|
| 1 | Parser accepts `func foo[T: Comparable](x: T)` syntax and stores constraint name on generic param node | VERIFIED | `src/parser/parser.c:281-285` — `iron_match(p, IRON_TOK_COLON)` in `iron_parse_generic_params`, stores via `iron_arena_strdup` into `id->constraint_name` |
| 2 | Parser still accepts unconstrained generic params `func foo[T](x: T)` with NULL constraint | VERIFIED | `src/parser/parser.c:280` — `id->constraint_name = NULL` set unconditionally before colon check; also initialised at lines 677, 689, 700 for all other ident allocations |
| 3 | Type checker rejects concrete type args that do not satisfy declared generic constraints at function call sites | VERIFIED | `src/analyzer/typecheck.c:954` — `check_generic_constraints(ctx, fd->generic_params, ...)` called in CALL handling; test `test_generic_constraint_violated_function_call` PASSES |
| 4 | Type checker rejects concrete type args that do not satisfy declared generic constraints at type construction sites | VERIFIED | `src/analyzer/typecheck.c:818` (call-as-construction) and `:1117` (ConstructExpr) — `check_generic_constraints` called at both; test `test_generic_constraint_violated_construction` PASSES |
| 5 | IRON_ERR_GENERIC_CONSTRAINT (206) emitted naming the constraint and the failing type | PARTIAL | Error code 206 is defined and emitted with constraint name in message (`type '%s' does not satisfy constraint '%s'`). However GEN-04 in REQUIREMENTS.md specifies the name `IRON_ERR_CONSTRAINT_NOT_SATISFIED`, not `IRON_ERR_GENERIC_CONSTRAINT`. Behavior is correct; identifier name diverges from spec. |
| 6 | Concrete types satisfying the constraint compile without error | VERIFIED | Tests `test_generic_constraint_satisfied_no_error` and `test_generic_constraint_satisfied_construction` both PASS with `TEST_ASSERT_FALSE(has_error(IRON_ERR_GENERIC_CONSTRAINT))` |

**Score:** 5/6 truths verified (1 partial — naming discrepancy on error code identifier)

### Required Artifacts

| Artifact | Expected | Status | Details |
|----------|----------|--------|---------|
| `src/parser/ast.h` | `constraint_name` field on `Iron_Ident` | VERIFIED | Line 370: `const char *constraint_name; /* generic param constraint; NULL if unconstrained */` |
| `src/parser/parser.c` | Constraint parsing in `iron_parse_generic_params` | VERIFIED | Lines 280-285: colon-check with `iron_arena_strdup` into `id->constraint_name`; all other ident allocations initialised to NULL (lines 677, 689, 700) |
| `src/analyzer/typecheck.c` | Constraint checking at CALL and CONSTRUCT sites | VERIFIED | `type_satisfies_constraint` (line 267), `check_generic_constraints` (line 326) defined; hooked at line 818 (call-as-construction), line 954 (function call), line 1117 (ConstructExpr) |
| `tests/unit/test_typecheck.c` | Generic constraint checking unit tests | VERIFIED | 6 test functions present (lines 946-1063), all 6 registered in `main()` (lines 1132-1137), all 6 PASS |

### Key Link Verification

| From | To | Via | Status | Details |
|------|----|-----|--------|---------|
| `src/parser/parser.c` | `src/parser/ast.h` | `id->constraint_name` stored in `iron_parse_generic_params` | WIRED | `parser.c:284` stores `iron_arena_strdup` result into `id->constraint_name`; field declared in `ast.h:370` |
| `src/analyzer/typecheck.c` | `src/parser/ast.h` | reads `generic_params[i]->constraint_name` from FuncDecl/ObjectDecl | WIRED | `typecheck.c:337`: `gp->constraint_name` read in `check_generic_constraints`; `gp` cast from `generic_params[i]` which are `Iron_Ident*` from the AST |
| `src/analyzer/typecheck.c` | `src/diagnostics/diagnostics.h` | emits `IRON_ERR_GENERIC_CONSTRAINT` on constraint violation | WIRED | `typecheck.c:348`: `emit_error(ctx, IRON_ERR_GENERIC_CONSTRAINT, span, msg, NULL)` |
| `tests/unit/test_typecheck.c` | `src/analyzer/typecheck.c` | `parse_and_resolve` drives full pipeline | WIRED | `test_typecheck.c:964,980,998,1022,1035,1051` — all 6 tests call `parse_and_resolve()` which exercises lexer → parser → resolver → typecheck |

### Requirements Coverage

| Requirement | Source Plan | Description | Status | Evidence |
|-------------|------------|-------------|--------|----------|
| GEN-01 | 37-01, 37-02 | Compiler validates concrete type args satisfy declared generic constraints at instantiation sites | SATISFIED | Both function call and construction sites validate; `test_generic_constraint_satisfied_no_error`, `test_generic_unconstrained_no_error` pass |
| GEN-02 | 37-01, 37-02 | Compiler checks constraint satisfaction for generic function calls | SATISFIED | `check_generic_constraints` called in CALL path (typecheck.c:954); `test_generic_constraint_violated_function_call` PASSES |
| GEN-03 | 37-01, 37-02 | Compiler checks constraint satisfaction for generic type construction | SATISFIED | `check_generic_constraints` called in call-as-construction (typecheck.c:818) and ConstructExpr (typecheck.c:1117); `test_generic_constraint_violated_construction` PASSES |
| GEN-04 | 37-01, 37-02 | Compiler emits `IRON_ERR_CONSTRAINT_NOT_SATISFIED` when concrete type does not meet constraint | PARTIAL | Error is emitted (code 206, message names constraint and type). However the identifier is `IRON_ERR_GENERIC_CONSTRAINT`, not `IRON_ERR_CONSTRAINT_NOT_SATISFIED` as REQUIREMENTS.md specifies. Functional behavior matches; symbolic name does not. |

### Anti-Patterns Found

| File | Line | Pattern | Severity | Impact |
|------|------|---------|----------|--------|
| None found | — | — | — | — |

No TODO/FIXME/placeholder comments, no empty implementations, no stub handlers found in any modified file.

### Build and Test Results

- **Build:** Clean (0 warnings, 0 errors) — `cmake --build build` shows all targets built successfully
- **Unit tests:** 18/18 pass (`ctest -L unit` — 100% pass rate, 67 individual test cases in `test_typecheck`, 0 failures)
- **New tests:** All 6 generic constraint tests PASS:
  - `test_generic_constraint_satisfied_no_error` — PASS
  - `test_generic_constraint_violated_function_call` — PASS
  - `test_generic_constraint_violated_construction` — PASS
  - `test_generic_constraint_satisfied_construction` — PASS
  - `test_generic_unconstrained_no_error` — PASS
  - `test_generic_constraint_error_message` — PASS
- **Commits verified:** `25e89ed`, `35a7451`, `982e7be` all present in git log

### Gaps Summary

There is one gap, which is a naming discrepancy rather than a functional failure.

**GEN-04 error code name mismatch:** REQUIREMENTS.md GEN-04 states the compiler should emit `IRON_ERR_CONSTRAINT_NOT_SATISFIED`. The implementation defines and uses `IRON_ERR_GENERIC_CONSTRAINT` (value 206) in `src/diagnostics/diagnostics.h`. The actual behavior — emitting error code 206 with a message that names both the constraint and the failing type — fully satisfies the observable intent of GEN-04. The gap is purely at the symbolic identifier level.

Resolution requires one of:
1. Rename `IRON_ERR_GENERIC_CONSTRAINT` to `IRON_ERR_CONSTRAINT_NOT_SATISFIED` in `diagnostics.h`, `typecheck.c`, and `test_typecheck.c` (aligns code to spec), OR
2. Update REQUIREMENTS.md GEN-04 to use `IRON_ERR_GENERIC_CONSTRAINT` (aligns spec to code, treating the plan/summary as the canonical decision record).

Given that the PLAN frontmatter explicitly specifies `IRON_ERR_GENERIC_CONSTRAINT` (206) as the error code and all tests are written against it, option 2 (update REQUIREMENTS.md) is the lower-risk fix.

---

_Verified: 2026-04-03T11:10:00Z_
_Verifier: Claude (gsd-verifier)_
