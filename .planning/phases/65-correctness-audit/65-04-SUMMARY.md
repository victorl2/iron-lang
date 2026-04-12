---
phase: 65-correctness-audit
plan: 04
subsystem: compiler-lir
tags: [audit, lir, emit, codegen, correctness, blind-cast, null-safety, allocation]

requires:
  - phase: 65-correctness-audit (plans 01-03)
    provides: audit methodology established for parser, analyzer, hir-comptime directories
provides:
  - Per-file 7-dimension audit of all 11 .c files in src/lir/ (15,818 lines)
  - 130 findings with file:line, severity, suggested fix, regression fixture name
  - Summary severity table ready for Plan 06 consolidation
affects: [65-06-PLAN consolidation, phase 66+ remediation]

tech-stack:
  added: []
  patterns: [7-dimension audit framework applied to LIR emitter, optimizer, analysis passes]

key-files:
  created:
    - .planning/phases/65-correctness-audit/audit-lir.md
  modified: []

key-decisions:
  - "emit_c.c emit_instr main switch is fully exhaustive over all 44 IronLIR_OpKind values"
  - "2 unique H-severity findings: generated HEAP_ALLOC and RC_ALLOC C code does malloc without NULL check, immediate dereference on next line"
  - "stb_ds OOM paths are the dominant M-severity pattern (13 of 42 medium findings), consistent with audit-parser-lexer.md and audit-analyzer.md"
  - "value_range.c has best integer safety discipline (__builtin_*_overflow), one edge case in narrow_from_comparison at INT64 boundaries"

patterns-established:
  - "void* cast pattern: Iron_Field, Iron_TypeAnnotation, Iron_EnumVariant cast from void* arrays using only loop index bounds as guard -- fragile but correct given current AST layout"

requirements-completed: [AUDIT-01, AUDIT-02, AUDIT-03, AUDIT-04, AUDIT-05, AUDIT-06, AUDIT-08]

duration: 7min
completed: 2026-04-12
---

# Phase 65 Plan 04: LIR Directory Audit Summary

**130 findings across 7 dimensions for all 11 src/lir/ files (15,818 lines): 4 H-severity (malloc without NULL check in generated code), 42 M-severity (stb_ds OOM, void* casts, __builtin_overflow portability), 84 L-severity**

## Performance

- **Duration:** 7 min
- **Started:** 2026-04-12T14:54:44Z
- **Completed:** 2026-04-12T15:01:57Z
- **Tasks:** 2
- **Files modified:** 1

## Accomplishments
- Complete audit of emit subsystem (emit_c.c + 4 sub-modules) across all 7 correctness dimensions
- Complete audit of LIR core (lir.c, lir_optimize.c, layout_analysis.c, value_range.c, verify.c, print.c)
- Summary severity table with counts per dimension ready for Plan 06 consolidation
- All 11 .c files in src/lir/ referenced at least once in audit-lir.md

## Task Commits

Each task was committed atomically:

1. **Task 1: Audit emit_c.c and emit sub-modules** - `4f4c4fc` (feat)
2. **Task 2: Audit LIR core and write summary** - `b1d64cd` (feat)

## Files Created/Modified
- `.planning/phases/65-correctness-audit/audit-lir.md` - Per-file audit findings for all 11 LIR source files

## Decisions Made
- emit_c.c emit_instr main switch is fully exhaustive over all 44 IronLIR_OpKind values -- no missing cases
- 2 unique H-severity findings: generated HEAP_ALLOC and RC_ALLOC C code does malloc without NULL check, immediate dereference on next line
- stb_ds OOM paths are the dominant M-severity pattern (13 of 42 medium findings), consistent with findings in audit-parser-lexer.md and audit-analyzer.md
- value_range.c has best integer safety discipline (__builtin_*_overflow), one edge case in narrow_from_comparison at INT64 boundaries

## Deviations from Plan

None - plan executed exactly as written.

## Issues Encountered
None.

## User Setup Required
None - no external service configuration required.

## Next Phase Readiness
- audit-lir.md complete and ready for Plan 06 consolidation
- All H-severity findings documented with exact file:line for remediation in Phase 66+

---
*Phase: 65-correctness-audit*
*Completed: 2026-04-12*
