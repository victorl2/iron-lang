---
phase: 02-semantics-and-codegen
plan: 08
subsystem: codegen
tags: [codegen, c11, clang, field-access, compound-literal, printf, auto_free, escape-analysis]

# Dependency graph
requires:
  - phase: 02-05
    provides: gen_exprs.c emit_expr with field access and CALL handler
  - phase: 02-06
    provides: Iron_Codegen context with global_scope for symbol lookup
  - phase: 02-07
    provides: Full pipeline (iron_analyze + iron_codegen) and test_pipeline fixture
  - phase: 02-04
    provides: iron_escape_analyze sets auto_free=true on non-escaping heap nodes
provides:
  - "gen_exprs.c: IRON_NODE_FIELD_ACCESS emits -> for self, . for struct values"
  - "gen_exprs.c: IRON_NODE_CALL redirects IRON_SYM_TYPE callees to compound-literal emission"
  - "gen_exprs.c: println/print emit printf(\"%s\\n\", arg) with newline in format string"
  - "gen_stmts.c: emit_block scans auto_free heap nodes and emits free(varname) at exit"
  - "tests/test_codegen.c: 5 new regression tests, clang -std=c11 -Wall -Werror -fsyntax-only verification"
  - "tests/integration/run_integration.sh: clang -fsyntax-only step activates when compiler binary available"
affects:
  - 03-runtime-stdlib-cli

# Tech tracking
tech-stack:
  added: []
  patterns:
    - "IRON_SYM_TYPE check in CALL handler before regular function call emission"
    - "use_arrow heuristic: IRON_NODE_IDENT name == 'self' implies pointer receiver"
    - "emit_block exit scan: auto_free heap nodes emit free(var) before closing brace"
    - "clang -fsyntax-only as compilability oracle in unit tests"

key-files:
  created:
    - tests/test_codegen.c (new tests: lines 635-790)
  modified:
    - src/codegen/gen_exprs.c
    - src/codegen/gen_stmts.c
    - tests/test_codegen.c
    - tests/integration/run_integration.sh

key-decisions:
  - "Self-pointer heuristic: check id->name == 'self' for -> vs . in FIELD_ACCESS; no resolved_type pointer-kind check needed because methods always declare self as T* and non-self field access is always struct-value in Phase 2"
  - "println format: printf(\"%s\\n\", arg) — newline is part of format string, arg is second positional; avoids clang -Wformat-extra-args error from two-argument printf(str, \"\\n\") form"
  - "CALL->CONSTRUCT redirect uses IRON_SYM_TYPE check on resolved_sym; falls through to regular function call path for non-type callees; compound literal uses same field-lookup logic as IRON_NODE_CONSTRUCT"
  - "auto_free free() emitted at emit_block exit (after defers), not at statement emission, so free happens at block scope exit matching Iron semantics"
  - "clang compilation test uses -Wno-unused-variable -Wno-unused-function because Iron val/var declarations may be unused in test programs; -Werror still catches format errors and pointer-dereference errors"

patterns-established:
  - "Codegen correctness gated by clang -fsyntax-only: any future codegen addition that produces invalid C will be caught by test_codegen_output_compiles_with_clang before merging"

requirements-completed: [GEN-01, GEN-02, SEM-09, TEST-02]

# Metrics
duration: 4min
completed: 2026-03-25
---

# Phase 2 Plan 8: Codegen Bug Fixes and Clang Verification Summary

**Three clang-blocking codegen bugs fixed (self->, compound literal, printf format) plus auto_free emission and clang -fsyntax-only test gate**

## Performance

- **Duration:** ~4 min
- **Started:** 2026-03-25T20:27:09Z
- **Completed:** 2026-03-25T20:30:40Z
- **Tasks:** 2
- **Files modified:** 4

## Accomplishments

- Fixed IRON_NODE_FIELD_ACCESS to emit `self->x` (pointer dereference) instead of `self.x` (invalid C on pointer receiver) — all method bodies now produce valid C
- Fixed IRON_NODE_CALL to redirect type-constructor calls (callee resolves to IRON_SYM_TYPE) to compound-literal emission `(Iron_Vec2){.x=1.0, .y=2.0}` instead of invalid `Iron_Vec2(1.0, 2.0)` function call
- Fixed println/print codegen to emit `printf("%s\n", arg)` — newline in format string — instead of `printf(arg, "\n")` which clang rejects with -Wformat-extra-args
- Added auto_free consumption in emit_block: scans all val/var decls at block exit for heap nodes with auto_free=true and emits `free(varname);` — closes the gap where escape analysis set the flag but codegen never read it
- Added `test_codegen_output_compiles_with_clang` which writes generated C to `/tmp/iron_codegen_test.c` and invokes `clang -std=c11 -Wall -Werror -fsyntax-only` — any future codegen regression produces a hard test failure
- Updated `run_integration.sh` to add clang -fsyntax-only verification step after pattern checks (activates when iron_integrate/iron binary is available in Phase 3)

## Task Commits

Each task was committed atomically:

1. **Task 1 (TDD RED): Failing regression tests** - `fa50986` (test)
2. **Task 1 (TDD GREEN): Fix three codegen bugs + auto_free** - `bc3942a` (feat)
3. **Task 2: Clang compilation test + integration runner** - `caab222` (feat)

## Files Created/Modified

- `src/codegen/gen_exprs.c` - FIELD_ACCESS arrow logic, CALL->CONSTRUCT redirect, println format fix
- `src/codegen/gen_stmts.c` - emit_block exit scan for auto_free heap nodes, free() emission
- `tests/test_codegen.c` - 5 new tests (4 regression + 1 clang compilation), stdlib.h include
- `tests/integration/run_integration.sh` - clang -fsyntax-only step added to pattern verification loop

## Decisions Made

- Self-pointer heuristic: check `id->name == "self"` for `->` vs `.` in FIELD_ACCESS. No resolved_type pointer-kind check needed because methods always declare `self` as `T*` and non-self field access is always struct-value in Phase 2.
- println format uses `printf("%s\n", arg)` — newline baked into format string. Avoids the two-argument form that clang rejects with -Wformat-extra-args.
- CALL->CONSTRUCT redirect uses `IRON_SYM_TYPE` check on `resolved_sym`; the compound literal uses same field-lookup logic as IRON_NODE_CONSTRUCT for named fields.
- auto_free `free()` emitted at `emit_block` exit (after defers drain), not at statement emission, so free happens at block scope exit matching Iron semantics.

## Deviations from Plan

None - plan executed exactly as written.

## Issues Encountered

None.

## User Setup Required

None - no external service configuration required.

## Next Phase Readiness

- Generated C now passes `clang -std=c11 -Wall -Werror -fsyntax-only` for programs with methods, object construction, and println
- Phase 2 verification gap GEN-01 is definitively closed
- SEM-09 (escape analysis) is now fully satisfied at both analysis and codegen levels
- Phase 3 (runtime/stdlib/CLI) can proceed with confidence that the code generator produces valid C

## Self-Check: PASSED

- src/codegen/gen_exprs.c: FOUND
- src/codegen/gen_stmts.c: FOUND
- tests/test_codegen.c: FOUND
- tests/integration/run_integration.sh: FOUND
- fa50986 (test: failing tests): FOUND
- bc3942a (feat: bug fixes): FOUND
- caab222 (feat: clang test): FOUND
- All 14 test executables: 100% pass (29 codegen tests including 5 new)

---
*Phase: 02-semantics-and-codegen*
*Completed: 2026-03-25*
