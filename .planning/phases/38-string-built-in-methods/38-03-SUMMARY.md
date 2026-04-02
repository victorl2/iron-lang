---
phase: 38-string-built-in-methods
plan: 03
subsystem: testing
tags: [iron-lang, integration-tests, string-methods, typecheck, runtime]

# Dependency graph
requires:
  - phase: 38-02
    provides: "All 19 Iron_string_* method bodies in iron_string.c"
provides:
  - "24 integration test files (12 .iron + 12 .expected) covering all 19 string methods"
  - "Forward declarations for all 19 Iron_string_* methods in iron_runtime.h"
  - "Typecheck fix: method call return type resolution for non-ident String receivers"
affects: [future-string-tests, regression-suite]

# Tech tracking
tech-stack:
  added: []
  patterns:
    - "Integration test iron files use val-binding for bool results to avoid nested quote ambiguity"
    - "String method tests verified by compiling with ironc and comparing stdout to .expected"

key-files:
  created:
    - tests/integration/str_upper_lower.iron
    - tests/integration/str_upper_lower.expected
    - tests/integration/str_trim.iron
    - tests/integration/str_trim.expected
    - tests/integration/str_contains.iron
    - tests/integration/str_contains.expected
    - tests/integration/str_starts_ends.iron
    - tests/integration/str_starts_ends.expected
    - tests/integration/str_split_join.iron
    - tests/integration/str_split_join.expected
    - tests/integration/str_replace.iron
    - tests/integration/str_replace.expected
    - tests/integration/str_substring_indexof.iron
    - tests/integration/str_substring_indexof.expected
    - tests/integration/str_char_at.iron
    - tests/integration/str_char_at.expected
    - tests/integration/str_parse.iron
    - tests/integration/str_parse.expected
    - tests/integration/str_len_repeat.iron
    - tests/integration/str_len_repeat.expected
    - tests/integration/str_pad.iron
    - tests/integration/str_pad.expected
    - tests/integration/str_count.iron
    - tests/integration/str_count.expected
  modified:
    - src/runtime/iron_runtime.h
    - src/analyzer/typecheck.c

key-decisions:
  - "iron_runtime.h required explicit forward declarations for all 19 Iron_string_* methods — generated C called these without forward decls causing ISO C99 errors"
  - "typecheck.c method call handler only covered IRON_NODE_IDENT receivers; string literal and chained call receivers defaulted to Void — fixed by capturing obj_type_mc from check_expr and handling IRON_TYPE_STRING for non-ident objects"
  - "Iron prints booleans as true/false in string interpolation; nested double quotes inside interpolation cause parser hang — use val-binding pattern for bool results"
  - "Float output trims trailing zeros: 0.0 prints as 0, 3.14 prints as 3.14"

patterns-established:
  - "Bool integration tests: assign to val first, then println with interpolation"
  - "String method integration tests: verify actual output before writing .expected"

requirements-completed:
  - STR-01
  - STR-02
  - STR-03
  - STR-04
  - STR-05
  - STR-06
  - STR-07
  - STR-08
  - STR-09
  - STR-10
  - STR-11
  - STR-12
  - STR-13
  - STR-14
  - STR-15
  - STR-16
  - STR-17
  - STR-18
  - STR-19
  - ITEST-01

# Metrics
duration: 21min
completed: 2026-04-02
---

# Phase 38 Plan 03: String Integration Tests Summary

**24 integration test files (12 pairs) covering all 19 Iron string methods, with two compiler bug fixes required to make them compile**

## Performance

- **Duration:** 21 min
- **Started:** 2026-04-02T23:16:58Z
- **Completed:** 2026-04-02T23:38:01Z
- **Tasks:** 2
- **Files modified:** 26 (24 test files + iron_runtime.h + typecheck.c)

## Accomplishments
- Created all 12 `.iron` + `.expected` file pairs in `tests/integration/` covering STR-01 through STR-19 and ITEST-01
- Fixed missing forward declarations in `iron_runtime.h` for all 19 `Iron_string_*` methods (clang ISO C99 error)
- Fixed `typecheck.c` to resolve return types for method calls on non-ident String receivers (string literals, chained calls)
- All 173 integration tests pass with zero failures (no regressions)

## Task Commits

Each task was committed atomically:

1. **Task 1: STR-01 through STR-11 (8 test pairs + compiler fixes)** - `5fdfe0a` (feat)
2. **Task 2: STR-12 through STR-19 (4 test pairs) + full suite run** - `69c41f5` (feat)

**Plan metadata:** (created after this commit)

## Files Created/Modified
- `tests/integration/str_upper_lower.iron/.expected` - Tests STR-01 (upper) and STR-02 (lower)
- `tests/integration/str_trim.iron/.expected` - Tests STR-03 (trim)
- `tests/integration/str_contains.iron/.expected` - Tests STR-04 (contains)
- `tests/integration/str_starts_ends.iron/.expected` - Tests STR-05 (starts_with) and STR-06 (ends_with)
- `tests/integration/str_split_join.iron/.expected` - Tests STR-07 (split) and STR-14 (join)
- `tests/integration/str_replace.iron/.expected` - Tests STR-08 (replace)
- `tests/integration/str_substring_indexof.iron/.expected` - Tests STR-09 (substring) and STR-10 (index_of)
- `tests/integration/str_char_at.iron/.expected` - Tests STR-11 (char_at)
- `tests/integration/str_parse.iron/.expected` - Tests STR-12 (to_int) and STR-13 (to_float)
- `tests/integration/str_len_repeat.iron/.expected` - Tests STR-15 (len) and STR-16 (repeat)
- `tests/integration/str_pad.iron/.expected` - Tests STR-17 (pad_left) and STR-18 (pad_right)
- `tests/integration/str_count.iron/.expected` - Tests STR-19 (count)
- `src/runtime/iron_runtime.h` - Added forward declarations for all 19 Iron_string_* methods
- `src/analyzer/typecheck.c` - Fixed method return type resolution for non-ident String receivers

## Decisions Made
- Iron booleans in string interpolation print as "true"/"false" — test files use `val r = s.method()` then `println("{r}")` to avoid nested-quote ambiguity
- Float `0.0` prints as `0` (trailing zero trimmed) — str_parse.expected uses `0` not `0.0`
- Integer `to_int()` on non-numeric input returns `0` (nothing consumed) — consistent with plan spec

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 1 - Bug] Missing Iron_string_* forward declarations in iron_runtime.h**
- **Found during:** Task 1 (first compilation attempt)
- **Issue:** Generated C code calls `Iron_string_contains(...)` etc. but these functions were not declared in iron_runtime.h — caused "call to undeclared function" ISO C99 error
- **Fix:** Added 19 forward declarations for all Iron_string_* methods at end of iron_runtime.h, after `Iron_List_Iron_String` typedef (split/join need it)
- **Files modified:** `src/runtime/iron_runtime.h`
- **Verification:** str_upper_lower compiled and ran correctly after fix
- **Committed in:** 5fdfe0a (Task 1 commit)

**2. [Rule 1 - Bug] typecheck.c method return type resolution skipped non-ident receivers**
- **Found during:** Task 1 (testing split/join)
- **Issue:** `typecheck.c` IRON_NODE_METHOD_CALL handler only resolved return type when receiver was `IRON_NODE_IDENT`. String literals, interp strings, and chained calls as receivers fell through to the default `IRON_TYPE_VOID` — causing `"a,b,c".split(",")` to type as Void, making the variable unusable and array indexing on split results failing with "undeclared identifier" errors in generated C
- **Fix:** Captured return value of `check_expr(ctx, mc->object)` into `obj_type_mc`, added `else if (obj_type_mc && obj_type_mc->kind == IRON_TYPE_STRING)` branch that performs the same string.iron method decl scan as the ident path
- **Files modified:** `src/analyzer/typecheck.c`
- **Verification:** `"a,b,c".split(",")` now types correctly as `[String]`; `",".join(parts)` types as `String`; all 12 str_* tests pass
- **Committed in:** 5fdfe0a (Task 1 commit)

---

**Total deviations:** 2 auto-fixed (both Rule 1 - Bug)
**Impact on plan:** Both fixes required for integration tests to compile and run. No scope creep — fixes confined to missing declarations and typecheck coverage gap introduced by Phase 37's method dispatch wiring.

## Issues Encountered
- `iron` binary (package manager) delegates to `ironc` (compiler) as subprocess — ninja didn't detect the `ironc` rebuild need after source timestamp matched prior build. Resolved by running `touch` on modified files to force rebuild.

## Next Phase Readiness
- All 19 string method requirements (STR-01 through STR-19) and ITEST-01 have passing integration tests
- Phase 38 is fully complete — runtime implementation (Plans 01-02) and integration tests (Plan 03) all done
- No blockers or open issues

---
*Phase: 38-string-built-in-methods*
*Completed: 2026-04-02*
