---
phase: 05-codegen-fixes-stdlib-wiring
plan: 01
subsystem: codegen
tags: [codegen, string-interpolation, snprintf, iron_string, c-codegen]

requires:
  - phase: 03-runtime-stdlib-and-cli
    provides: iron_string_from_literal, iron_string_cstr runtime API
  - phase: 02-semantics-and-codegen
    provides: gen_exprs.c codegen framework, IRON_NODE_INTERP_STRING AST node

provides:
  - IRON_NODE_INTERP_STRING emits two-pass snprintf code via GNU statement expression
  - Int uses %lld, Float uses %g, Bool uses ternary "true"/"false", String uses iron_string_cstr
  - C unit test suite verifying interpolation codegen emits correct patterns

affects:
  - 05-codegen-fixes-stdlib-wiring (subsequent plans building on working interpolation)

tech-stack:
  added: []
  patterns:
    - GNU statement expression ({...}) for multi-statement expression emission
    - lambda_counter used as unique index for interpolation temp variable names
    - Iron_IntLit* cast to access resolved_type from generic Iron_Node* at codegen time

key-files:
  created:
    - tests/test_interp_codegen.c
  modified:
    - src/codegen/gen_exprs.c
    - CMakeLists.txt

key-decisions:
  - "Macro-defined INTERP_APPEND_CHAR/STR for building format string at codegen time (grows buffer dynamically)"
  - "iron_string_from_literal() used for interpolation result per locked user decision (not iron_string_from_cstr)"
  - "lambda_counter reused as unique interpolation index to prevent temp var collisions in nested interpolations"
  - "((Iron_IntLit *)part)->resolved_type cast pattern to access resolved_type via shared struct layout"

requirements-completed: [GEN-01]

duration: 10min
completed: 2026-03-26
---

# Phase 5 Plan 01: String Interpolation Codegen Summary

**snprintf two-pass GNU statement expression emission for all 4 primitive types (Int/Float/Bool/String) replacing empty-string stub, with C unit tests verifying codegen patterns**

## Performance

- **Duration:** ~10 min
- **Started:** 2026-03-26T19:40:56Z
- **Completed:** 2026-03-26T19:51:11Z
- **Tasks:** 2
- **Files modified:** 3

## Accomplishments
- Replaced IRON_NODE_INTERP_STRING stub (`""`) with full snprintf two-pass emission using GNU statement expressions
- Handles Int (`%lld`), Float (`%g`), Bool (`%s` with ternary), and String (`%s` with `iron_string_cstr`)
- Stack buffer (1024 bytes) with heap fallback path via malloc/free for long strings
- Uses `iron_string_from_literal()` per locked user decision throughout
- Created `tests/test_interp_codegen.c` with 6 Unity tests verifying codegen patterns
- End-to-end: `"value is 42"`, `"pi is 3.14"`, `"flag is true"`, `"hello world"` all correct

## Task Commits

1. **Task 1: Implement snprintf two-pass string interpolation in gen_exprs.c** - `bcccd11` (feat)
2. **Task 2: Create C unit tests for interpolation codegen and verify end-to-end** - `6ca9fc2` (feat)

## Files Created/Modified
- `src/codegen/gen_exprs.c` - IRON_NODE_INTERP_STRING case replaced with full snprintf implementation
- `tests/test_interp_codegen.c` - 6 Unity tests: int/float/bool/string/from_literal/no_exprs
- `CMakeLists.txt` - Registered test_interp_codegen executable and test

## Decisions Made
- `INTERP_APPEND_CHAR`/`INTERP_APPEND_STR` macros build format string at Iron compile time with dynamic buffer growth
- `lambda_counter` reused as unique interp index (avoids adding a new field to Iron_Codegen)
- `((Iron_IntLit *)part)->resolved_type` cast pattern to access resolved_type from generic Iron_Node* — all expression nodes share the same first-three-field layout
- Added `#include <stdlib.h>` to gen_exprs.c for malloc/realloc/free (was missing for heap fallback path)

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 2 - Missing Critical] Added #include <stdlib.h> to gen_exprs.c**
- **Found during:** Task 1 (build verification)
- **Issue:** malloc/realloc/free needed for heap fallback path and format string buffer but stdlib.h was not included
- **Fix:** Added `#include <stdlib.h>` alongside existing stdio/string includes
- **Files modified:** src/codegen/gen_exprs.c
- **Verification:** Compiler built without warnings
- **Committed in:** bcccd11 (Task 1 commit)

---

**Total deviations:** 1 auto-fixed (missing include)
**Impact on plan:** Necessary for correctness. No scope creep.

## Issues Encountered
- `Iron_Node` base struct has no `resolved_type` field — required casting each expression part to `(Iron_IntLit *)` to access the common layout field. The linter auto-converted `part->resolved_type` to `((Iron_IntLit *)part)->resolved_type` during edit, confirming correct pattern.
- A pre-existing `test_parallel_codegen` test times out in the full test suite (~97s before killed) — unrelated to this plan's changes.

## Next Phase Readiness
- String interpolation fully working for all 4 primitive types
- Ready for Phase 5 Plan 02 (subsequent codegen/stdlib fixes)

---
*Phase: 05-codegen-fixes-stdlib-wiring*
*Completed: 2026-03-26*

## Self-Check: PASSED
- src/codegen/gen_exprs.c: FOUND
- tests/test_interp_codegen.c: FOUND
- 05-01-SUMMARY.md: FOUND
- Commit bcccd11: FOUND
- Commit 6ca9fc2: FOUND
