---
phase: 05-codegen-fixes-stdlib-wiring
plan: "05"
subsystem: stdlib
tags: [io, c-ffi, static-inline, result-type, integration-test]

# Dependency graph
requires:
  - phase: 05-03
    provides: auto-static dispatch for stdlib module calls from Iron source
  - phase: 05-04
    provides: integration test infrastructure and run_integration.sh

provides:
  - IO.read_file() returns Iron_String via static inline wrapper in iron_io.h
  - _result variants (Iron_io_read_file_result etc.) preserve full result struct for C unit tests
  - test_io.iron exercises IO.read_file end-to-end (write, read, print, cleanup)
  - SC4 verification gap closed: IO.read_file compiles from Iron source without type mismatch

affects:
  - Any future plan adding IO methods — use _result naming convention for C implementations

# Tech tracking
tech-stack:
  added: []
  patterns: ["Static inline wrapper pattern: _result variant returns full struct, plain name returns .v0 value for auto-static dispatch"]

key-files:
  created:
    - tests/integration/test_io.expected (updated)
    - tests/integration/test_io.iron (updated)
  modified:
    - src/stdlib/iron_io.h
    - src/stdlib/iron_io.c
    - tests/test_stdlib.c

key-decisions:
  - "IO _result naming convention: C implementations renamed to *_result; static inline wrappers at same name return .v0 Iron_String for auto-static dispatch"

patterns-established:
  - "Static inline wrapper at C header level bridges Iron declaration (-> String) and C implementation (-> Result_String_Error) without touching codegen"

requirements-completed: [STD-02]

# Metrics
duration: 1min
completed: 2026-03-26
---

# Phase 05 Plan 05: IO.read_file Return Type Fix Summary

**Static inline wrapper bridges Iron `-> String` declaration and C `Iron_Result_String_Error` implementation, closing the last SC4 gap with a passing read_file integration test.**

## Performance

- **Duration:** ~1 min
- **Started:** 2026-03-26T20:47:35Z
- **Completed:** 2026-03-26T20:49:00Z
- **Tasks:** 2
- **Files modified:** 5

## Accomplishments
- Renamed Iron_io_read_file/read_bytes/list_files C implementations to *_result variants
- Added static inline wrappers at header level returning Iron_String for auto-static dispatch
- Updated test_stdlib.c to use _result variants (21 C unit tests still pass)
- Updated test_io.iron to write a temp file, call IO.read_file, and println the result
- All 11 integration tests pass with no regressions

## Task Commits

Each task was committed atomically:

1. **Task 1: Rename C implementations to _result, add Iron_String wrappers** - `8d9c3d2` (feat)
2. **Task 2: Update test_io.iron to test IO.read_file end-to-end** - `303dbd1` (feat)

**Plan metadata:** (docs commit — see final)

## Files Created/Modified
- `src/stdlib/iron_io.h` - Added _result declarations and static inline wrappers returning Iron_String
- `src/stdlib/iron_io.c` - Renamed implementations to *_result, updated internal call in read_bytes_result
- `tests/test_stdlib.c` - Updated 2 call sites to use Iron_io_read_file_result
- `tests/integration/test_io.iron` - Exercises IO.write_file, IO.read_file, println, IO.delete_file
- `tests/integration/test_io.expected` - Updated to include "io works" and "hello from iron" lines

## Decisions Made
- Static inline wrapper pattern chosen over modifying codegen: the wrapper at header level is simpler and localizes the bridge between Iron's declared return type (String) and the C implementation's Result struct — no codegen changes required
- _result suffix for C implementations preserves explicit error handling path for C callers (test_stdlib.c) while letting Iron callers use the clean wrapper

## Deviations from Plan

None - plan executed exactly as written.

## Issues Encountered
None.

## User Setup Required
None - no external service configuration required.

## Next Phase Readiness
- Phase 5 SC4 verification gap is closed: IO.read_file compiles and runs correctly from Iron source
- All Phase 5 integration tests pass (11/11)
- Future IO methods should follow the _result naming convention: C implementation as *_result, static inline wrapper at plain name returning .v0

---
*Phase: 05-codegen-fixes-stdlib-wiring*
*Completed: 2026-03-26*
