---
phase: 38-string-built-in-methods
plan: 02
subsystem: runtime
tags: [iron_string, c, string-methods, split, join, replace, substring, to_int, to_float, repeat, pad]

# Dependency graph
requires:
  - phase: 38-01
    provides: "10 simple Iron_string_* method bodies already in iron_string.c"
provides:
  - "Iron_string_split: empty sep splits by char; non-empty sep uses strstr walk"
  - "Iron_string_join: two-pass size calculation then single memcpy into malloc buf"
  - "Iron_string_replace: source-walk strstr to avoid infinite loop when new_s contains old_s"
  - "Iron_string_substring: clamped [start, end_idx) byte-index slice"
  - "Iron_string_to_int: strtoll with end==s guard; returns 0 when nothing consumed"
  - "Iron_string_to_float: strtod with end==s guard; returns 0.0 when nothing consumed"
  - "Iron_string_repeat: malloc n*len, memcpy n times; n<=0 returns empty"
  - "Iron_string_pad_left: prepend pad char until byte length >= width"
  - "Iron_string_pad_right: append pad char until byte length >= width"
  - "All 19 Iron_string_* method bodies complete in src/runtime/iron_string.c"
affects: [phase-39, integration-tests, string-stdlib]

# Tech tracking
tech-stack:
  added: []
  patterns:
    - "Source-walk pattern for replace: iterate cur pointer in original string, never in output buffer"
    - "Two-pass buffer sizing: count occurrences first, malloc exact size, then fill"
    - "strtoll/strtod end-pointer guard: return 0/0.0 only when end==s (nothing consumed)"
    - "Pad functions return self unchanged when slen >= width (no-op fast path)"

key-files:
  created: []
  modified:
    - src/runtime/iron_string.c

key-decisions:
  - "Iron_string_replace walks source string (not output buffer) to avoid infinite loop when new_s contains old_s"
  - "Iron_string_to_int/to_float return 0/0.0 only when end==s (nothing consumed); partial numeric prefix like '42abc' returns 42"
  - "Iron_string_pad_left/pad_right use only first byte of ch parameter as the pad character"
  - "Iron_string_substring treats indices as byte offsets, consistent with char_at and len from Plan 01"

patterns-established:
  - "Source-walk replace: track cur in original string; strstr finds next hit in source; copy segment+replacement to separate output buffer"
  - "Two-pass allocation: first pass counts replacements, malloc exact total, second pass fills"

requirements-completed: [STR-07, STR-08, STR-09, STR-12, STR-13, STR-14, STR-16, STR-17, STR-18]

# Metrics
duration: 5min
completed: 2026-04-02
---

# Phase 38 Plan 02: String Built-In Methods (Wave 2) Summary

**9 remaining Iron_string_* method bodies added to iron_string.c: split/join using Iron_List_Iron_String API, source-walk replace, clamped substring, strtoll/strtod parse, repeat, and pad_left/pad_right**

## Performance

- **Duration:** ~5 min
- **Started:** 2026-04-02T23:09:57Z
- **Completed:** 2026-04-02T23:14:07Z
- **Tasks:** 2
- **Files modified:** 1

## Accomplishments

- Appended 4 function bodies (split, join, replace, substring) after the Plan 01 Phase 38 block
- Appended 5 more function bodies (to_int, to_float, repeat, pad_left, pad_right) completing all 19 Iron_string_* methods
- Build remains clean with zero warnings or errors after both additions

## Task Commits

Each task was committed atomically:

1. **Task 1: split, join, replace, substring** - `ce1cc9f` (feat)
2. **Task 2: to_int, to_float, repeat, pad_left, pad_right** - `300b138` (feat)

**Plan metadata:** (docs commit — see below)

## Files Created/Modified

- `src/runtime/iron_string.c` - Added 9 Iron_string_* function bodies (wave 2), completing all 19 declared in string.iron

## Decisions Made

- Iron_string_replace walks the source string (not the output buffer) — avoids infinite loop when `new_s` contains `old_s` as a substring
- to_int/to_float return 0/0.0 only when `end == s` (nothing consumed); partial prefix `"42abc"` returns 42, matching strtoll C standard behavior and plan spec
- pad_left/pad_right use only the first byte of the `ch` parameter as the pad character (ASCII single-char string convention)
- substring treats start/end_idx as byte offsets, consistent with char_at byte indexing established in Plan 01

## Deviations from Plan

None - plan executed exactly as written.

## Issues Encountered

None.

## User Setup Required

None - no external service configuration required.

## Next Phase Readiness

- All 19 Iron_string_* C function bodies are present and the build is clean
- The dispatch chain (string.iron → typecheck → hir_to_lir → emit_c → iron_string.c) is fully wired from Phase 37
- Integration tests for all 19 string methods still need to be created (deferred to a later plan per Phase 38 wave structure)

---
*Phase: 38-string-built-in-methods*
*Completed: 2026-04-02*
