---
phase: 67-correctness-fixes-+-crash-canaries
plan: 03
subsystem: compiler-correctness
tags: [integer-safety, comptime, parser, overflow, __builtin_overflow, strtol, undefined-behavior, FIX-01, FIX-04]

# Dependency graph
requires:
  - phase: 67-02
    provides: iron_oom_abort helper (src/runtime/iron_oom.c + diagnostics.h decl); pattern for FIX-01 rank guards
  - phase: 66-02
    provides: -Werror=switch-enum enforcement; 82 opt-out comments across analyzer + lowering
  - phase: 65
    provides: CORRECTNESS-AUDIT.md top-20 row inventory (ranks 14, 15, 19 were the remaining integer-safety OPEN rows)
provides:
  - Closed top-20 audit ranks 14, 15, 19 (final integer-safety OPEN rows)
  - Overflow-safe comptime arithmetic (+, -, *) via __builtin_*_overflow with comptime diagnostic emission
  - INT64_MIN / -1 guards on comptime integer division and modulo
  - INT64_MIN negation guard on comptime unary minus
  - Parser enum-variant explicit-value path rewritten from atoi to strtol with full ERANGE + INT_MIN/INT_MAX bounds validation
  - typecheck.c plain-enum exhaustiveness buffer converted from fixed-size bool[256] with silent truncation to dynamically-sized calloc + iron_oom_abort guard + free (FIX-04 row 13)
  - 3 new regression fixtures with 4-section doc-comment headers
  - 67-AUDIT-STATUS.md updated to 20 DONE / 0 OPEN (was 17/3)
affects:
  - 67-04 (parser arena walkthrough — shares parser.c; clean baseline for parser-wide audit)
  - 67-05 (analyzer + comptime arena walkthrough — shares comptime.c, typecheck.c; this plan's edits are upstream of that sweep)
  - 67-06 (hir + lir + runtime/stdlib arena walkthrough — no direct overlap)
  - 67-08 (REG-02 canaries — can now assume all top-20 H rows are closed)

# Tech tracking
tech-stack:
  added: []
  patterns:
    - "GCC/Clang __builtin_*_overflow for signed-integer overflow detection in compile-time evaluators"
    - "strtol + errno/ERANGE + explicit bounds + endptr-consumed as the canonical replacement for atoi in parser-visible integer literals"
    - "calloc + iron_oom_abort + matched free() for dynamically-sized scratch buffers in analyzer exhaustiveness checks (vs fixed-size bool[256] with silent truncation)"
    - "safe-path fixture + source-level grep acceptance criteria split: integer-safety canaries exercise the non-overflow code path on every build while the diagnostic path is verified by grep on the PLAN.md acceptance criteria, because run_integration.sh has no .expected-error mode"

key-files:
  created:
    - tests/integration/int_comptime_arith_overflow.iron
    - tests/integration/int_comptime_arith_overflow.expected
    - tests/integration/int_comptime_neg_min.iron
    - tests/integration/int_comptime_neg_min.expected
    - tests/integration/int_enum_value_overflow.iron
    - tests/integration/int_enum_value_overflow.expected
  modified:
    - src/comptime/comptime.c
    - src/parser/parser.c
    - src/analyzer/typecheck.c
    - docs/regression-fixtures.md
    - .planning/phases/67-correctness-fixes-+-crash-canaries/67-AUDIT-STATUS.md

key-decisions:
  - "Use __builtin_add/sub/mul_overflow (GCC/Clang) directly in comptime.c rather than wrapping behind a portable helper — the lir/value_range.c precedent (Phase 54) already uses these builtins unwrapped; Windows is out of scope per REQUIREMENTS.md; a portable helper is premature abstraction."
  - "Also guard INT64_MIN / -1 in the IRON_TOK_SLASH and IRON_TOK_PERCENT cases even though the audit only listed +, -, * — C11 6.5.5p6 makes division and modulo UB in that exact boundary case, and the cost is two conditions for a complete fix."
  - "Use IRON_ERR_UNEXPECTED_TOKEN (101) for the enum-value-out-of-range parser diagnostic rather than adding a new error code — the existing IRON_ERR_INVALID_NUMBER is lexer-level and misleading here; 101 is the parser-generic bucket and the 'enum variant value ... out of range' message carries the semantics."
  - "Integration fixtures exercise the safe (non-overflow) path rather than attempting to assert on the error diagnostic, because run_integration.sh has no .expected-error mode (checked by reading the full script). The diagnostic paths are verified by (a) direct source-level grep in the acceptance criteria and (b) a manual reproducer under /tmp/ confirming 'enum variant value 99999999999999 out of range' fires on the OOB case."
  - "Use calloc + iron_oom_abort + free for the typecheck.c row 13 dynamic sizing rather than ctx->arena, because (a) the PLAN literally specified calloc + iron_oom_abort + free in its acceptance criteria, (b) this makes the OOM abort site individually bisectable via stderr grep, and (c) the arena used by the adjacent has_payloads branch currently has a latent NULL-return bug that is properly addressed by 67-05's analyzer-arena walkthrough — mixing the fix here would blur plan boundaries."
  - "FIX-04 walkthrough confirmed 16 of the 17 AUDIT-02 M rows are already covered by Phase 66-02 / 66-03 (explicit cases + opt-out comments + -Werror=switch-enum enforcement). Only row 13 (typecheck.c:2927 covered[256] silent truncation) needed an edit. Walkthrough evidence preserved in the Task 3 commit message for future traceability."

patterns-established:
  - "Integer-safety regression canary: safe-path fixture with 4-section doc-comment header + acceptance-criteria grep on the error message literal"
  - "Audit row closure workflow: per-row walkthrough documents current head state in the commit message even for rows that need no edit, so downstream plans can grep commit history for coverage evidence"

requirements-completed: [FIX-01, FIX-04]

# Metrics
duration: 45min
completed: 2026-04-13
---

# Phase 67 Plan 03: Integer-safety tail + FIX-04 enum-switch walkthrough Summary

**Closed the final 3 top-20 OPEN integer-safety rows (ranks 14, 15, 19) via __builtin_*_overflow in comptime arithmetic, INT64_MIN negation guard, and strtol-with-bounds in the enum-variant parser; walked the AUDIT-02 M-severity tail and fixed the one remaining row (typecheck.c covered[256] silent truncation).**

## Performance

- **Duration:** 45min
- **Started:** 2026-04-13T17:40:24Z
- **Completed:** 2026-04-13T18:25:40Z
- **Tasks:** 3
- **Files modified:** 5 (+ 6 new fixtures)

## Accomplishments

- **Ranks 14 + 15 (comptime.c):** IRON_CVAL_INT binop switch now uses `__builtin_add_overflow`, `__builtin_sub_overflow`, `__builtin_mul_overflow` with diagnostic emission on overflow; SLASH and PERCENT gained explicit `INT64_MIN / -1` guards (C11 6.5.5p6); unary minus guards `INT64_MIN` before negating (C11 6.5.3.3p3). All four overflow classes now produce a comptime diagnostic instead of optimizer-dependent behavior.
- **Rank 19 (parser.c):** Enum-variant explicit-value path rewritten from `atoi(num->value)` to `strtol` with `errno == ERANGE`, `INT_MIN / INT_MAX` bounds, and endptr-consumed check. Out-of-range values now produce a parser diagnostic with the offending literal; defensive fallback of `0` keeps the parse tree well-formed for downstream checks.
- **FIX-04 row 13 (typecheck.c):** `bool covered[256]` with silent `if (vc > 256) vc = 256;` truncation replaced by dynamic `calloc(vc, sizeof(bool))` guarded by `iron_oom_abort` and released with `free(covered)`. Plain enums with more than 256 variants are now checked correctly.
- **FIX-04 walkthrough evidence:** 16 of 17 AUDIT-02 M rows verified already-covered at current head via per-row grep in the Task 3 commit message; no edits needed.
- **Top-20 audit status:** 17 DONE / 3 OPEN → **20 DONE / 0 OPEN**. All top-20 H-severity rows from Phase 65 CORRECTNESS-AUDIT.md are now closed.
- **Regression fixtures:** 3 new integration fixtures (6 files: `.iron` + `.expected` each) with the full 4-section doc-comment template exercising the non-overflow code path so the rewritten branches run on every compiler build.
- **Integration suite:** 354 → 357 (+3 fixtures), all green. ctest green. No regression.

## Task Commits

Each task committed atomically:

1. **Task 1: Overflow-safe comptime arithmetic + INT64_MIN negation guards (ranks 14 + 15) + 2 fixtures** — `1220839` (feat)
2. **Task 2: atoi -> strtol for enum variant values (rank 19) + 1 fixture + docs update** — `1372702` (feat)
3. **Task 3: FIX-04 enum-switch tail walkthrough + typecheck covered[] dynamic sizing (row 13)** — `e84497c` (fix)

**Plan metadata commit:** pending (this SUMMARY.md + STATE.md + ROADMAP.md + REQUIREMENTS.md)

## Files Created/Modified

### Created

- `tests/integration/int_comptime_arith_overflow.iron` / `.expected` — Safe-path canary for the IRON_CVAL_INT `+`, `-`, `*` overflow rewrite (rank 14).
- `tests/integration/int_comptime_neg_min.iron` / `.expected` — Safe-path canary for the IRON_TOK_MINUS unary INT64_MIN guard (rank 15).
- `tests/integration/int_enum_value_overflow.iron` / `.expected` — In-range canary (at exactly INT_MAX = 2147483647) for the parser strtol rewrite (rank 19).

### Modified

- `src/comptime/comptime.c` — IRON_CVAL_INT binop switch rewritten to use `__builtin_add/sub/mul_overflow` + `INT64_MIN / -1` guards + INT64_MIN unary-minus guard. 3 new error message literals (addition, subtraction, multiplication, negation).
- `src/parser/parser.c` — Enum-variant explicit-value branch rewritten from atoi to strtol with full bounds validation; added `<errno.h>`, `<limits.h>`, `<stdio.h>` to the include block.
- `src/analyzer/typecheck.c` — Plain-enum exhaustiveness `bool covered[256]` replaced with `calloc + iron_oom_abort + free`. Eliminates silent truncation at 256 variants.
- `docs/regression-fixtures.md` — Added "Phase 67 Plan 03 (FIX-01 ranks 14 + 15 + 19 — integer safety)" section documenting all three fixtures and the safe-path + grep verification split.
- `.planning/phases/67-correctness-fixes-+-crash-canaries/67-AUDIT-STATUS.md` — Ranks 14, 15, 19 moved from OPEN to DONE with commit hash citations; Summary line updated from "17 DONE / 3 OPEN" to "20 DONE / 0 OPEN"; Plan Assignment table updated.

## Decisions Made

1. **Use `__builtin_*_overflow` directly, not behind a portable helper.** The `lir/value_range.c` precedent from Phase 54 already uses these builtins unwrapped; Windows is explicitly out of scope per REQUIREMENTS.md; a portable helper would be premature abstraction. Keeps the fix minimal and matches existing code style.
2. **Extend rank 14 to cover division and modulo.** The audit only flagged `+`, `-`, `*` but `INT64_MIN / -1` and `INT64_MIN % -1` are the companion C11 6.5.5p6 UB cases. Two extra conditions + two extra error-message literals makes the comptime integer evaluator comprehensively overflow-safe rather than "mostly safe on three operators".
3. **Reuse `IRON_ERR_UNEXPECTED_TOKEN` (101) for the enum-value parser error.** `IRON_ERR_INVALID_NUMBER` (3) is a lexer-level code and would misrepresent the stage. 101 is the parser-generic bucket and the message carries the semantics ("enum variant value '%s' out of range").
4. **Safe-path fixtures + grep on source literals.** `run_integration.sh` has no `.expected-error` mode (verified by reading the full 138-line script). The fixtures exercise values deliberately close to the limits so the rewritten branches run on every build, and the acceptance criteria grep for the exact error-message literals to prove the diagnostic paths are present. A manual reproducer under `/tmp/` confirmed "enum variant value 99999999999999 out of range" fires on the OOB case before the commit.
5. **Use calloc + iron_oom_abort + free for typecheck row 13, not the adjacent arena path.** The plan's acceptance criteria literally asked for `iron_oom_abort` coverage; `ctx->arena` in the has_payloads branch above has a latent NULL-return bug that is properly fixed by 67-05's analyzer-arena walkthrough; mixing the concerns would blur plan boundaries. The calloc path is individually bisectable via stderr grep.
6. **Preserve FIX-04 walkthrough evidence in the Task 3 commit message rather than editing 16 files that need no changes.** The row-by-row checklist with current-head evidence (line numbers, marker comments, explicit cases) lives in the commit body so any downstream reviewer can `git show e84497c` to see the full walkthrough without touching source.

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 3 — Blocking] Fixture syntax mismatch with plan example**
- **Found during:** Task 1 (creating the comptime overflow fixture)
- **Issue:** The plan used `comptime { expr }` block syntax and `const X = ...` declarations, but Iron uses `val X = comptime expr` (no block braces, `val` keyword, not `const`). Grep on existing integration fixtures (`hir_comptime_basic.iron`, `comptime_basic.iron`, `hir_edge_comptime_in_expr.iron`) confirmed the correct form before writing the new fixtures.
- **Fix:** Wrote fixtures using `val MAX_SAFE = comptime 4611686018427387900 + 7` etc. Same semantics, correct syntax.
- **Files modified:** `tests/integration/int_comptime_arith_overflow.iron`, `tests/integration/int_comptime_neg_min.iron`, `tests/integration/int_enum_value_overflow.iron`
- **Verification:** All 3 fixtures compile + run + match expected output; integration suite 354 -> 357.
- **Committed in:** `1220839`, `1372702`.

**2. [Rule 3 — Blocking] Parser diagnostic helper name mismatch**
- **Found during:** Task 2 (parser.c strtol rewrite)
- **Issue:** The plan's example used `iron_parser_emit_error(p, code, span, msg)`. parser.c actually uses a static helper `iron_emit_diag(p, code, span, msg)` which wraps `iron_diag_emit` with error-recovery suppression. Grep confirmed this is the canonical helper for all ~20 in-file error sites.
- **Fix:** Used `iron_emit_diag` instead.
- **Files modified:** `src/parser/parser.c`
- **Verification:** Build clean; manual reproducer on OOB enum value emits "error[E0101]: enum variant value '99999999999999' out of range ..." with correct source-location pointer.
- **Committed in:** `1372702`.

**3. [Rule 2 — Missing Critical] Division and modulo INT64_MIN guards**
- **Found during:** Task 1 (comptime binop rewrite)
- **Issue:** The plan only listed rank 14 as `+`, `-`, `*` but the same C11 boundary-UB class applies to `INT64_MIN / -1` and `INT64_MIN % -1` (C11 6.5.5p6). Leaving those two arms unsafe would close ~75% of the UB surface, not 100%.
- **Fix:** Added explicit `INT64_MIN` checks to the SLASH and PERCENT cases with dedicated error messages.
- **Files modified:** `src/comptime/comptime.c`
- **Verification:** grep on "INT64_MIN" in comptime.c returns 3 guard sites (div, mod, negation); build clean; acceptance criteria met.
- **Committed in:** `1220839`.

---

**Total deviations:** 3 auto-fixed (1 blocking syntax, 1 blocking helper rename, 1 correctness expansion)
**Impact on plan:** All three were necessary for the plan's stated intent (overflow-safe comptime arithmetic + working parser diagnostic + working integration fixtures). No scope creep.

## Issues Encountered

- **Integration runner has no `.expected-error` mode.** Verified by reading all 138 lines of `tests/integration/run_integration.sh`. This ruled out the Option B fixture style discussed in the plan and forced Option A (safe-path canary + source-grep verification). Documented in `docs/regression-fixtures.md` and in each fixture's Fix Summary section so future integer-safety canaries inherit the pattern.
- **Pre-existing Clang `-Wswitch` warnings on generated C for enum with explicit values.** When compiling a fixture that uses an enum with `= N` variant values inside a `match` expression, the generated C triggers 3 `case value not in enumerated type` warnings. Verified via `git stash` that these exist on the Phase 67-02 baseline (commit `48cb384`) and are NOT introduced by this plan's parser changes — the warnings come from the downstream match-to-C lowering using sequential internal ordinals rather than the declared variant values. Out of scope for 67-03 (logged here for future cleanup in a later plan or a dedicated AUDIT-01 rerun on match codegen).

## User Setup Required

None — no external service configuration required.

## Next Phase Readiness

- **67-04 (parser arena walkthrough)** can begin immediately; the only overlap was parser.c:2585 and that site is now clean. No conflicting edits pending.
- **67-05 (analyzer + comptime arena walkthrough)** will revisit comptime.c and typecheck.c for arena-NULL-check coverage. This plan's edits are structurally upstream and do not conflict — the new calloc path in typecheck.c is stdlib, not arena, and the comptime.c overflow rewrites don't touch arena allocation.
- **67-06, 67-07, 67-08** have no direct overlap with this plan.
- **Top-20 audit closure milestone reached:** 20 of 20 H-severity rows from Phase 65 CORRECTNESS-AUDIT.md are now DONE. The remaining 67-04..67-08 plans address non-top-20 M-severity rows and REG-02 canaries only.

## Self-Check

Verifying claims before proceeding:

**Files created:**
- FOUND: tests/integration/int_comptime_arith_overflow.iron
- FOUND: tests/integration/int_comptime_arith_overflow.expected
- FOUND: tests/integration/int_comptime_neg_min.iron
- FOUND: tests/integration/int_comptime_neg_min.expected
- FOUND: tests/integration/int_enum_value_overflow.iron
- FOUND: tests/integration/int_enum_value_overflow.expected

**Commits exist:**
- FOUND: 1220839 (Task 1 — comptime overflow + neg-min)
- FOUND: 1372702 (Task 2 — parser strtol + fixture + docs)
- FOUND: e84497c (Task 3 — FIX-04 walkthrough + typecheck covered[])

## Self-Check: PASSED

---
*Phase: 67-correctness-fixes-+-crash-canaries*
*Completed: 2026-04-13*
