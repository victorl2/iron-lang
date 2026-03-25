---
phase: 01-frontend
plan: 02
subsystem: compiler-lexer
tags: [c11, lexer, tokenizer, arena, diagnostics, unity, cmake, stb_ds]

# Dependency graph
requires:
  - phase: 01-frontend plan 01
    provides: arena allocator, diagnostics system, stb_ds, Iron_ naming convention, CMake/Ninja build

provides:
  - Iron_TokenKind enum with all 37 keywords plus operators, literals, and delimiters (76 token kinds total)
  - Iron_Token struct with arena-copied value and 1-indexed line/col span
  - Iron_Lexer struct with iron_lexer_create / iron_lex_all API
  - 25-test suite covering keywords, operators, spans, errors, comments, interpolation

affects:
  - 01-frontend plan 03 (parser depends entirely on iron_lex_all token stream)
  - 01-frontend plan 04 (diagnostics error codes E0001/E0002 consumed by parser diagnostics)

# Tech tracking
tech-stack:
  added: []
  patterns:
    - "bsearch keyword lookup: sorted KeywordEntry[] + bsearch for O(log n) keyword resolution"
    - "stb_ds arrput: token array grown dynamically, caller calls arrfree"
    - "Arena-buffered string lexing: 4 KB pre-alloc for string content, then iron_arena_strdup for final value"
    - "Error recovery: emit diagnostic, return IRON_TOK_ERROR, continue lexing"

key-files:
  created:
    - src/lexer/lexer.h
    - src/lexer/lexer.c
    - tests/test_lexer.c
  modified:
    - CMakeLists.txt

key-decisions:
  - "Keywords stored in alphabetically sorted array; bsearch gives O(log n) lookup with zero hash overhead"
  - "Comment (--) consumed in iron_lex_punctuation, returns IRON_TOK_NEWLINE covering the whole comment+newline"
  - "Multiline strings (triple-quote) handled in iron_lex_string: newlines inside allowed, no unterminated error"
  - "String interpolation detection: any { inside string content sets has_interp flag → IRON_TOK_INTERP_STRING"
  - "4 KB arena pre-alloc for string content avoids per-character allocs; final value iron_arena_strdup'd compactly"

patterns-established:
  - "Token value is NULL for pure punctuation (no string copy needed); non-NULL for literals/identifiers/keywords"
  - "All error diagnostics emitted via iron_diag_emit before returning IRON_TOK_ERROR — no silent failures"

requirements-completed: [LEX-01, LEX-02, LEX-03, LEX-04]

# Metrics
duration: 4min
completed: 2026-03-25
---

# Phase 1 Plan 2: Lexer Summary

**Hand-written Iron lexer with 37-keyword bsearch table, span-accurate tokens, error recovery, and 25 Unity tests all passing**

## Performance

- **Duration:** ~4 min
- **Started:** 2026-03-25T21:33:20Z
- **Completed:** 2026-03-25T21:37:03Z
- **Tasks:** 2
- **Files modified:** 4

## Accomplishments

- Complete Iron lexer tokenizing all 37 keywords, 22 operators, 7 delimiters, 4 literal kinds, and comments
- Every token carries 1-indexed line/col span sourced from the lexer cursor at token start
- Lexer error recovery: unterminated strings emit E0001, invalid characters emit E0002, and lexing continues — N independent errors produce exactly N diagnostics
- 25 Unity tests passing: keyword coverage, span accuracy, error invariants, comment suppression, interpolation detection

## Task Commits

1. **Task 1: Implement lexer header and complete implementation** - `b9f3622` (feat)
2. **Task 2: Write comprehensive lexer tests** - `d4df428` (feat)

**Plan metadata:** (see final commit below)

## Files Created/Modified

- `src/lexer/lexer.h` — Iron_TokenKind enum (76 values), Iron_Token struct, Iron_Lexer struct, public API
- `src/lexer/lexer.c` — 657 lines: keyword bsearch table, string/number/identifier/punctuation scanners, main lex loop
- `tests/test_lexer.c` — 431 lines: 25 test functions covering all requirements
- `CMakeLists.txt` — added src/lexer/lexer.c to iron_compiler, added test_lexer target and ctest entry

## Decisions Made

- Keywords stored in alphabetically sorted `KeywordEntry[]`; `bsearch` resolves in O(log n) — no hash table needed for 37 entries
- Comment `--` consumed inside `iron_lex_punctuation` by checking `peek_next == '-'`; the entire comment+trailing newline becomes a single `IRON_TOK_NEWLINE` token
- String interpolation detection uses a simple flag (`has_interp`): any unescaped `{` inside string content sets it; no parsing needed at lex time
- Token `value` field is `NULL` for punctuation tokens (no arena copy wasted on single-char tokens)

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 3 - Blocking] Added missing `#include <stdio.h>` for `snprintf`**
- **Found during:** Task 1 (first build attempt)
- **Issue:** `snprintf` used in error message formatting but `<stdio.h>` not included; `-Wimplicit-function-declaration` is an error under `-Werror`
- **Fix:** Added `#include <stdio.h>` to `src/lexer/lexer.c`
- **Files modified:** src/lexer/lexer.c
- **Verification:** Build passed with zero warnings after fix
- **Committed in:** b9f3622 (Task 1 commit)

---

**Total deviations:** 1 auto-fixed (1 blocking)
**Impact on plan:** Single missing include; no scope creep.

## Issues Encountered

None beyond the missing include above.

## User Setup Required

None - no external service configuration required.

## Next Phase Readiness

- Lexer fully complete; `iron_lex_all` returns a stb_ds token array the parser can consume directly
- Token spans are 1-indexed and accurate — parser can propagate them to AST nodes without retrofitting
- Error codes E0001/E0002/E0003 established; parser should use E01xx range (101+) per diagnostics.h
- All existing tests (arena, diagnostics, lexer) continue to pass

---
*Phase: 01-frontend*
*Completed: 2026-03-25*
