---
phase: 65-correctness-audit
plan: 03
subsystem: compiler
tags: [hir, comptime, blind-cast, enum-switch, null-safety, arena-lifetime, cross-platform, audit]

requires:
  - phase: 65-correctness-audit P01
    provides: parser/lexer audit baseline
  - phase: 65-correctness-audit P02
    provides: typecheck/resolve/scope audit baseline
provides:
  - Per-file audit findings for HIR and comptime directories across 7 dimensions
  - 369 total findings (4 H / 304 M / 61 L) documented with file:line and severity
  - Local struct typedef shadow analysis (0 remaining after Phase 59 fix)
affects: [65-correctness-audit P06 consolidation, future remediation phases]

tech-stack:
  added: []
  patterns:
    - "7-dimension audit format: blind casts, enum switch, null safety, arena lifetimes, integer safety, allocation errors, cross-platform"

key-files:
  created:
    - .planning/phases/65-correctness-audit/audit-hir-comptime.md
  modified: []

key-decisions:
  - "Iron_ExprNode common layout assumption in hir_lower.c:109 classified H -- locally-defined struct bypasses real type definitions"
  - "comptime.c sys/stat.h and mkdir() classified H for cross-platform -- POSIX-only APIs break Windows compilation"
  - "Allocation error handling counted per call-site (99 M-severity) for accurate remediation scope"
  - "collect_mono_enums_node incomplete switch classified M -- 8 missing node kinds may silently miss monomorphized enums in arrays/lambdas"

patterns-established:
  - "Blind cast severity: H for layout-assumption casts, M for switch-guarded casts, L for const-stripping casts"

requirements-completed: [AUDIT-01, AUDIT-02, AUDIT-03, AUDIT-04, AUDIT-05, AUDIT-06, AUDIT-08]

duration: 7min
completed: 2026-04-12
---

# Phase 65 Plan 03: HIR + Comptime Audit Summary

**369 correctness findings across src/hir/ and src/comptime/ -- 174 blind casts, 27 enum switch gaps, 100 unchecked allocations, 3 POSIX-only cross-platform blockers**

## Performance

- **Duration:** 7 min
- **Started:** 2026-04-12T14:45:54Z
- **Completed:** 2026-04-12T14:53:10Z
- **Tasks:** 2
- **Files modified:** 1

## Accomplishments
- Audited 8 HIR source files (hir.c/h, hir_lower.c/h, hir_to_lir.c/h, hir_print.c, hir_verify.c) and 2 comptime files across all 7 correctness dimensions
- Special focus on hir_to_lir.c which was the site of the Phase 59 SIGSEGV -- confirmed local struct typedef shadow pattern is fully eliminated
- Identified 1 H-severity blind cast (Iron_ExprNode common layout assumption) and 3 H-severity cross-platform findings (POSIX mkdir in comptime cache)
- Discovered collect_mono_enums_node is incomplete (missing 8 node kinds including FOR, LAMBDA, ARRAY_LIT)

## Task Commits

Each task was committed atomically:

1. **Task 1: Audit src/hir/** - `ea022a0` (feat)
2. **Task 2: Audit src/comptime/ and write summary** - `647f8db` (feat)

## Files Created/Modified
- `.planning/phases/65-correctness-audit/audit-hir-comptime.md` - Per-file audit findings with 7 dimension tables and summary counts

## Decisions Made
- Iron_ExprNode common layout assumption in hir_lower.c:109 classified H -- locally-defined struct assumes `{span, kind, resolved_type}` prefix for all AST nodes without compile-time verification
- comptime.c `<sys/stat.h>` + `mkdir()` classified H for cross-platform -- POSIX-only APIs will fail on Windows MSVC
- Allocation error handling counted per call-site (99 M) to enable precise remediation
- collect_mono_enums_node incomplete switch classified M -- silently misses monomorphized enums in array literals, for-loops, and lambda bodies

## Deviations from Plan

None - plan executed exactly as written.

## Issues Encountered

None.

## User Setup Required

None - no external service configuration required.

## Next Phase Readiness
- audit-hir-comptime.md is ready for Plan 06 consolidation into the unified audit report
- Summary severity counts are tabulated for automatic rollup

---
*Phase: 65-correctness-audit*
*Completed: 2026-04-12*
