---
phase: 38-string-built-in-methods
plan: 01
subsystem: runtime
tags: [c, iron_string, string-methods, ctype, strstr, memcmp]

# Dependency graph
requires:
  - phase: 37-string-dispatch-wiring
    provides: "full dispatch chain from Iron source to Iron_string_* C call sites"
provides:
  - "10 Iron_string_* C function bodies in iron_string.c: upper, lower, trim, contains, starts_with, ends_with, index_of, char_at, len, count"
affects:
  - 38-string-built-in-methods plan 02 (complex methods)
  - integration test writing for STR-01 through STR-06, STR-10, STR-11, STR-15, STR-19

# Tech tracking
tech-stack:
  added: ["<ctype.h> (toupper/tolower)"]
  patterns:
    - "Iron_String by-value receiver pattern for all method bodies"
    - "malloc+iron_string_from_cstr+free idiom for building result strings"
    - "iron_string_cstr / iron_string_byte_len accessor idiom (no direct union access)"
    - "strstr-based substring search (contains, index_of, count)"
    - "memcmp-based prefix/suffix comparison"

key-files:
  created: []
  modified:
    - src/runtime/iron_string.c

key-decisions:
  - "All 10 method bodies appended to iron_string.c in a single Phase 38 section comment block"
  - "iron_string_len returns byte count (not codepoint count) — consistent with STR-ADV-01 deferral"
  - "iron_string_char_at returns empty string for out-of-range index (no crash)"
  - "iron_string_count returns 0 for empty sub (avoids infinite strstr loop)"

patterns-established:
  - "Phase 38 method block: wrapped in /* ── String built-in methods (Phase 38) ─── */ comment"
  - "by-value Iron_String self: all methods match emit_c.c call pattern (no address-of)"

requirements-completed: [STR-01, STR-02, STR-03, STR-04, STR-05, STR-06, STR-10, STR-11, STR-15, STR-19]

# Metrics
duration: 1min
completed: 2026-04-02
---

# Phase 38 Plan 01: String Built-In Methods (Simple 10) Summary

**10 single-argument string method bodies added to iron_string.c: upper, lower, trim, contains, starts_with, ends_with, index_of, char_at, len, count — build clean with -Wall -Werror**

## Performance

- **Duration:** 1 min
- **Started:** 2026-04-02T23:08:48Z
- **Completed:** 2026-04-02T23:09:57Z
- **Tasks:** 1
- **Files modified:** 1

## Accomplishments
- Added `#include <ctype.h>` for toupper/tolower support
- Implemented all 10 method bodies in a single appended block (no existing code modified)
- All functions receive `Iron_String self` by value, matching the hir_to_lir.c emit pattern
- Build exits 0 with no warnings and no errors under `-std=c11 -Wall -Werror`

## Task Commits

Each task was committed atomically:

1. **Task 1: Add 10 simple Iron_string_* method bodies to iron_string.c** - `c50d6ed` (feat)

## Files Created/Modified
- `src/runtime/iron_string.c` - Added `#include <ctype.h>` and 10 Iron_string_* method bodies in Phase 38 section block

## Decisions Made
- `iron_string_len` returns byte count via `iron_string_byte_len` for O(1) performance; codepoint count deferred per STR-ADV-01
- `iron_string_char_at` returns empty string for out-of-range index (clamped, no crash, no abort)
- `iron_string_count` returns 0 for empty sub to avoid the `strstr("", "")` infinite-loop pitfall
- Memory ownership follows existing tolerated-leak policy: `malloc` temp buf, call `iron_string_from_cstr`, then `free(buf)`

## Deviations from Plan

None - plan executed exactly as written.

## Issues Encountered

None.

## User Setup Required

None - no external service configuration required.

## Next Phase Readiness

- Plan 01 complete: 10 simple method bodies present and building cleanly
- Plan 02 can now add the 9 remaining complex methods (split, replace, substring, to_int, to_float, join, repeat, pad_left, pad_right)
- Integration tests for the 10 methods can be written against the current build

---
*Phase: 38-string-built-in-methods*
*Completed: 2026-04-02*
