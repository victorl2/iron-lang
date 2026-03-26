---
phase: 04-comptime-game-dev-and-cross-platform
plan: 04
subsystem: comptime
tags: [comptime, read_file, cache, fnv1a, codegen, global-constants]

# Dependency graph
requires:
  - phase: 04-02
    provides: comptime interpreter with iron_comptime_apply, Iron_ComptimeCtx, iron_analyze

provides:
  - read_file(String)->String builtin in comptime evaluator with source-relative path resolution
  - FNV-1a cache for comptime results in .iron-build/comptime/ keyed by source hash
  - iron_analyze() extended with source_file_dir, source_text, source_len, force_comptime params
  - Top-level val/var global constant emission in codegen (string inits deferred to Iron_main preamble)
  - Integration test: comptime_basic uses comptime string val emitted as global Iron_String

affects: [codegen, analyzer, cli/build, tests]

# Tech tracking
tech-stack:
  added: []
  patterns:
    - FNV-1a hash of full source text as conservative cache key (whole-file granularity)
    - Cache format: kind integer on line 1, value on remaining lines (.iron-build/comptime/<hex>.cache)
    - Top-level val/var emitted as static C globals; string types init in C main() preamble after iron_runtime_init()
    - read_file builtin dispatched by name before global scope lookup in IRON_NODE_CALL handler

key-files:
  created:
    - tests/integration/comptime_basic.iron
    - tests/integration/comptime_basic.expected
  modified:
    - src/comptime/comptime.h - added source_text/source_len fields; extended iron_comptime_apply signature
    - src/comptime/comptime.c - added fnv1a_hash, comptime_cache_read/write, read_file builtin in CALL handler
    - src/analyzer/analyzer.h - extended iron_analyze() with source_file_dir/source_text/source_len/force_comptime
    - src/analyzer/analyzer.c - passes new params through to iron_comptime_apply
    - src/analyzer/resolve.c - registered read_file(String)->String builtin in global scope
    - src/cli/build.c - passes dirname(source_path) and source text+size to iron_analyze
    - src/cli/check.c - updated to new iron_analyze() signature (passes NULL/0/false)
    - src/codegen/codegen.h - added global_consts Iron_StrBuf field
    - src/codegen/codegen.c - added 4b pass for top-level val/var globals; string inits in main() preamble
    - tests/test_comptime.c - added test_comptime_read_file, test_comptime_read_file_not_found

key-decisions:
  - "Cache keyed by FNV-1a hash of full source text (conservative, correct per-file granularity)"
  - "Global string constants initialized in C main() preamble after iron_runtime_init() — iron_string_from_literal() is a runtime call, not a C static initializer"
  - "read_file builtin dispatched by name check in IRON_NODE_CALL before global scope lookup — avoids needing a new AST node kind"
  - "Cache write happens per-COMPTIME node on successful evaluation in replace_in_node()"

patterns-established:
  - "Comptime cache: .iron-build/comptime/<16-hex-digit-fnv1a-hash>.cache per source file"
  - "Top-level val/var in Iron programs: emit as C static globals; strings use preamble init pattern"

requirements-completed: [CT-02]

# Metrics
duration: 15min
completed: 2026-03-26
---

# Phase 4 Plan 4: comptime read_file builtin and FNV-1a cache Summary

**comptime read_file() builtin with source-relative path resolution, FNV-1a content-hash cache in .iron-build/comptime/, and top-level val/var global constant codegen**

## Performance

- **Duration:** ~15 min
- **Started:** 2026-03-26T15:55:00Z
- **Completed:** 2026-03-26T16:05:47Z
- **Tasks:** 2
- **Files modified:** 11

## Accomplishments

- `comptime read_file("path")` reads files relative to the source .iron file directory and embeds contents as STRING_LIT before codegen runs
- FNV-1a hash of full source text caches comptime results in `.iron-build/comptime/<hash>.cache`; `--force-comptime` bypasses cache
- `iron_analyze()` extended with `source_file_dir`, `source_text`, `source_len`, `force_comptime` — build.c wires these from the real source file path
- Top-level `val`/`var` declarations now generate C global variables; string types use a preamble init in `main()` after `iron_runtime_init()`
- Integration test `comptime_basic` passes end-to-end: Iron `val GREETING = comptime "hello comptime"` produces `hello comptime` at runtime

## Task Commits

Each task was committed atomically:

1. **Task 1: Add read_file builtin and FNV-1a cache to comptime interpreter** - `812cfdb` (feat)
2. **Task 2: Add tests for read_file and comptime integration test** - `d7dcc6f` (feat)

**Plan metadata:** (docs commit below)

## Files Created/Modified

- `src/comptime/comptime.h` - added source_text/source_len fields; extended iron_comptime_apply signature
- `src/comptime/comptime.c` - FNV-1a hash, cache read/write, read_file builtin in CALL handler
- `src/analyzer/analyzer.h` - extended iron_analyze() with source_file_dir/source_text/source_len/force_comptime
- `src/analyzer/analyzer.c` - passes new params through to iron_comptime_apply
- `src/analyzer/resolve.c` - registered read_file(String)->String builtin in global scope
- `src/cli/build.c` - passes dirname(source_path) and src_size to iron_analyze
- `src/cli/check.c` - updated to new iron_analyze() signature
- `src/codegen/codegen.h` - added global_consts Iron_StrBuf field
- `src/codegen/codegen.c` - top-level val/var global emission; string inits in main() preamble
- `tests/test_comptime.c` - test_comptime_read_file and test_comptime_read_file_not_found
- `tests/integration/comptime_basic.iron` - comptime string integration test
- `tests/integration/comptime_basic.expected` - expected output

## Decisions Made

- Cache keyed by FNV-1a of full source text: conservative (whole-file granularity) but correct. Avoids partial-match false positives.
- Global string constants use preamble init in `main()` (after `iron_runtime_init()`) because `iron_string_from_literal()` is a runtime function, not a C constant expression.
- `read_file` dispatched by name check in `IRON_NODE_CALL` handler before global scope lookup — avoids needing a new AST node kind.

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 2 - Missing Critical] Added top-level val/var global constant emission to codegen**
- **Found during:** Task 2 (comptime integration test)
- **Issue:** The codegen had no support for top-level `val`/`var` declarations — they were simply ignored. The `comptime_basic.iron` integration test failed because `GREETING` was undeclared in the generated C.
- **Fix:** Added a new pass (section 4b) in `iron_codegen()` that iterates over top-level `VAL_DECL`/`VAR_DECL` nodes, emits them as `static` C globals, and initializes string types in the `main()` preamble after `iron_runtime_init()`.
- **Files modified:** `src/codegen/codegen.h`, `src/codegen/codegen.c`
- **Verification:** All 21 tests pass including comptime_basic integration test
- **Committed in:** `d7dcc6f` (Task 2 commit)

---

**Total deviations:** 1 auto-fixed (Rule 2 — missing critical functionality)
**Impact on plan:** Necessary for correctness — top-level comptime vals would be unusable without global emission. No scope creep.

## Issues Encountered

- `iron_string_from_literal()` cannot be used as a C static initializer (it's a runtime call). Solved by emitting global declarations without initializer and adding initialization calls in the `main()` preamble after `iron_runtime_init()`.

## Next Phase Readiness

- CT-02 requirement met: `comptime read_file("shaders/main.glsl")` can embed shader files at compile time
- Cache mechanism ready for plan 04-05 (game dev integration)
- Top-level val/var global emission enables comptime results to flow into full programs

## Self-Check: PASSED

All key files verified present and all commits confirmed in git log.

---
*Phase: 04-comptime-game-dev-and-cross-platform*
*Completed: 2026-03-26*
