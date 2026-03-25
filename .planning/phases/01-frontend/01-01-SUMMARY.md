---
phase: 01-frontend
plan: "01"
subsystem: infra
tags: [cmake, ninja, unity, c11, arena-allocator, stb-ds, diagnostics]

requires: []

provides:
  - CMake+Ninja build system with Unity FetchContent, ASan/UBSan in Debug mode
  - Arena allocator (iron_arena_create/alloc/strdup/free, ARENA_ALLOC macro)
  - String builder (iron_strbuf_create/append/appendf/append_char/get/free/reset)
  - stb_ds.h vendored (dynamic arrays + hash maps, single-TU impl)
  - Diagnostic system (Iron_Span, Iron_DiagList, iron_diag_emit, Rust-style print with color)
  - Error codes E0001-E0106 for lexer and parser errors
  - Unity unit tests for arena and diagnostics (13 tests total)

affects:
  - 01-02-lexer (uses arena, diagnostics, stb_ds)
  - 01-03-parser (uses arena, diagnostics, stb_ds, strbuf)
  - all subsequent phases (depend on arena + diagnostics as load-bearing interfaces)

tech-stack:
  added:
    - CMake 3.25+ with Ninja backend
    - Unity v2.6.1 (C unit test framework via FetchContent)
    - stb_ds.h (nothings/stb, dynamic arrays and hash maps)
    - AddressSanitizer + UndefinedBehaviorSanitizer (Debug builds)
  patterns:
    - Iron_ prefix naming convention for all public types and functions
    - Arena allocator for all heap allocations (no individual frees during compilation)
    - stb_ds STB_DS_IMPLEMENTATION in dedicated stb_ds_impl.c (single TU)
    - Rust-style 3-line source context in diagnostics with isatty ANSI color

key-files:
  created:
    - CMakeLists.txt
    - src/util/arena.h
    - src/util/arena.c
    - src/util/strbuf.h
    - src/util/strbuf.c
    - src/util/stb_ds_impl.c
    - src/vendor/stb_ds.h
    - src/diagnostics/diagnostics.h
    - src/diagnostics/diagnostics.c
    - tests/test_arena.c
    - tests/test_diagnostics.c
    - examples/hello.iron
    - examples/game.iron
  modified: []

key-decisions:
  - "stb_ds STB_DS_IMPLEMENTATION compiled in dedicated src/util/stb_ds_impl.c to avoid multiple-definition linker errors"
  - "Arena allocator uses realloc growth (double capacity) so base pointer can move — callers must not cache raw pointers across alloc calls"
  - "Iron_Span uses 1-indexed lines and byte-based columns to match editor conventions and Rust diagnostic style"
  - "isatty(STDERR_FILENO) gates ANSI color so piped output is clean"
  - "Ninja installed via Homebrew as deviation (Rule 3 — blocking) since it was not present on the system"

patterns-established:
  - "Iron_ prefix: all public types and functions use Iron_ prefix (Iron_Arena, iron_arena_create, etc.)"
  - "Arena-first allocation: all strings and structs go through iron_arena_alloc, not malloc directly"
  - "ARENA_ALLOC(arena, T) macro for typed allocation with correct alignment"
  - "stb_ds dynamic arrays: arrput/arrlen/arrfree pattern for growable collections"
  - "Unity test structure: setUp/tearDown empty stubs, RUN_TEST in main, explicit arena create/free per test"

requirements-completed: [LEX-01, LEX-02, LEX-03, LEX-04, PARSE-01]

duration: 4min
completed: 2026-03-25
---

# Phase 1 Plan 1: Project Scaffolding and Foundational Infrastructure Summary

**CMake+Ninja build system with arena allocator, string builder, stb_ds, and Rust-style diagnostic system — 13 Unity tests passing under ASan/UBSan**

## Performance

- **Duration:** ~4 min
- **Started:** 2026-03-25T21:26:00Z
- **Completed:** 2026-03-25T21:30:05Z
- **Tasks:** 2 of 2
- **Files modified:** 13 created, 0 pre-existing modified

## Accomplishments

- CMake 3.25 project with Ninja backend, Unity FetchContent, and ASan+UBSan in Debug mode — `cmake -G Ninja -B build -DCMAKE_BUILD_TYPE=Debug . && cmake --build build && ctest --test-dir build --output-on-failure` passes cleanly
- Arena allocator with bump-pointer allocation, alignment-up via `(used + align - 1) & ~(align - 1)`, realloc growth (doubles capacity), and `iron_arena_strdup` for string interning — 5 tests including alignment, grow-on-overflow, and `ARENA_ALLOC` macro
- Diagnostic system with `Iron_Span` (filename, 1-indexed line/col/end), `Iron_DiagList` backed by stb_ds dynamic array, Rust-style 3-line context window, ANSI color via `isatty`, and error codes E0001–E0106 — 8 tests covering all emit paths

## Task Commits

1. **Task 1: Project structure, CMakeLists.txt, arena, strbuf, stb_ds** - `c6e49b4` (feat)
2. **Task 2: Diagnostics system and unit tests** - `50d6f8f` (feat)

## Files Created/Modified

- `CMakeLists.txt` — cmake_minimum_required 3.25, Unity FetchContent v2.6.1, iron_compiler static lib, ASan/UBSan Debug, test targets
- `src/util/arena.h` — Iron_Arena struct, iron_arena_create/alloc/strdup/free, ARENA_ALLOC macro
- `src/util/arena.c` — bump-pointer allocator with alignment and realloc growth
- `src/util/strbuf.h` — Iron_StrBuf struct, full append/appendf/append_char/get/free/reset API
- `src/util/strbuf.c` — dynamic string builder with vsnprintf two-pass for appendf
- `src/util/stb_ds_impl.c` — single-TU STB_DS_IMPLEMENTATION file
- `src/vendor/stb_ds.h` — vendored stb_ds (1895 lines)
- `src/diagnostics/diagnostics.h` — Iron_Span, Iron_DiagLevel, Iron_Diagnostic, Iron_DiagList, error code constants
- `src/diagnostics/diagnostics.c` — iron_span_make/merge, iron_diaglist_create, iron_diag_emit, iron_diag_print (Rust-style), iron_diaglist_free
- `tests/test_arena.c` — 5 Unity tests for arena allocator
- `tests/test_diagnostics.c` — 8 Unity tests for diagnostic system
- `examples/hello.iron` — minimal hello world
- `examples/game.iron` — full game loop example from language spec

## Decisions Made

- stb_ds STB_DS_IMPLEMENTATION placed in `src/util/stb_ds_impl.c` (not in diagnostics.c) so the implementation is isolated and any future .c file can safely `#include "stb_ds.h"` without re-defining the implementation
- Arena allocator doubles capacity on overflow (vs fixed growth) to amortize realloc cost for large token streams
- `Iron_Span` uses 1-indexed lines and byte-based columns — consistent with Rust/clang diagnostics and editor line numbers
- `isatty(STDERR_FILENO)` gates ANSI color so output is clean when piped to files or other tools

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 3 - Blocking] Installed Ninja build tool**
- **Found during:** Task 1 verification (cmake configure)
- **Issue:** `cmake -G Ninja` failed with "unable to find a build program corresponding to Ninja" — Ninja not installed on the system
- **Fix:** `brew install ninja` (v1.13.2 installed)
- **Files modified:** none (system-level install)
- **Verification:** `ninja --version` returns 1.13.2; cmake configure succeeds
- **Committed in:** N/A (system dependency)

---

**Total deviations:** 1 auto-fixed (1 blocking system dependency)
**Impact on plan:** Ninja install required for CMake build as specified. No scope changes.

## Issues Encountered

- CMakeLists.txt was initially drafted with diagnostics.c already included (Task 2 content in Task 1). Corrected to match plan's task boundary: Task 1 CMakeLists.txt contains only arena/strbuf/stb_ds, Task 2 adds diagnostics.c and test_diagnostics target.

## User Setup Required

None — no external service configuration required.

## Next Phase Readiness

- Arena allocator, string builder, stb_ds, and diagnostics are tested and committed — all load-bearing interfaces established
- Plan 02 (lexer) can immediately use Iron_Arena, Iron_DiagList, and stb_ds dynamic arrays
- No blockers for Plan 02

---
*Phase: 01-frontend*
*Completed: 2026-03-25*
