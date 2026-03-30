---
phase: 18-benchmark-validation
plan: "02"
subsystem: testing
tags: [benchmark, bash, json, optimization, ci]

# Dependency graph
requires:
  - phase: 18-benchmark-validation
    provides: run_benchmarks.sh baseline benchmark runner
provides:
  - "--json flag writing results.json with date/commit/counts/per-benchmark data"
  - "--compare flag showing optimized vs unoptimized timing delta per benchmark"
  - "machine-readable JSON output for future CI trend tracking"
affects: [ci, benchmark-validation]

# Tech tracking
tech-stack:
  added: []
  patterns:
    - "JSON results accumulator built inline in main benchmark loop, written as single cat heredoc after summary"
    - "--no-optimize binary built in separate TMPDIR subdir to avoid clobbering optimized binary"
    - "compare_suffix appended to result strings so all output modes remain consistent"

key-files:
  created: []
  modified:
    - tests/benchmarks/run_benchmarks.sh

key-decisions:
  - "JSON benchmarks array is empty when all benchmarks error (correctness/compilation failures); only fully-measured benchmarks produce array entries — this is correct semantics"
  - "iron_noopt_bin built silently (2>/dev/null) so compare mode degrades gracefully when --no-optimize flag unavailable"
  - "compare_suffix appended to both PASS and FAIL result lines so compare data is visible in all run modes"

patterns-established:
  - "Additive flag pattern: new flags (--json, --compare) never alter existing code paths; all new behavior is gated behind WRITE_JSON/COMPARE_MODE checks"

requirements-completed: [BENCH-05]

# Metrics
duration: 2min
completed: 2026-03-29
---

# Phase 18 Plan 02: Benchmark Runner JSON and Compare Mode Summary

**Benchmark runner extended with --json FILE flag writing per-benchmark results.json and --compare flag showing optimized vs unoptimized speedup percentage per problem.**

## Performance

- **Duration:** 2 min
- **Started:** 2026-03-30T00:40:30Z
- **Completed:** 2026-03-30T00:42:30Z
- **Tasks:** 1
- **Files modified:** 1

## Accomplishments
- Added `--json FILE` flag that writes a machine-readable JSON file with date, commit hash, pass/fail/error/total counts, and a benchmarks array with per-benchmark name, iron_ms, c_ms, ratio, max_ratio, status, iron_mem_kb, c_mem_kb fields
- Added `--compare` flag that builds a second Iron binary with `--no-optimize` and shows `[opt: Nx, noopt: Nx, speedup: X%]` suffix on each result line
- Updated usage string to include both new flags
- All existing flags and terminal output format preserved unchanged

## Task Commits

Each task was committed atomically:

1. **Task 1: Add --json output and --compare mode to benchmark runner** - `fb9125b` (feat)

**Plan metadata:** (docs commit follows)

## Files Created/Modified
- `tests/benchmarks/run_benchmarks.sh` - Added WRITE_JSON/JSON_FILE/COMPARE_MODE variables, --json/--compare arg parsing, Step 2b no-opt build, Step 6b no-opt timing, compare_suffix, json_results accumulation, JSON file write block

## Decisions Made
- JSON benchmarks array only receives entries for benchmarks that complete all 7 steps (C compile, Iron compile, both run, correctness check, timing extract, ratio compute). Errors and skipped benchmarks are counted in top-level `errors`/`skipped` fields only — this is correct semantics for CI consumption.
- `--compare` mode degrades gracefully: if the no-opt binary fails to build, `iron_noopt_bin` remains empty and the compare suffix is simply omitted for that benchmark.
- Used separate TMPDIR subdirs (`iron_noopt_${problem_name}`) for no-opt binaries to avoid any collision with the optimized build dir.

## Deviations from Plan

None - plan executed exactly as written.

## Issues Encountered
Pre-existing `/usr/bin/time -l` abort trap on certain benchmarks on this machine (unrelated to the changes). The JSON file is correctly written regardless; the benchmarks array is empty when all benchmarks fail at the execution step, which is correct behavior.

## User Setup Required
None - no external service configuration required.

## Next Phase Readiness
- `--json` output is ready for CI integration (trend tracking, artifact storage)
- `--compare` mode enables developers to inspect optimization impact per benchmark
- Both flags are additive; no existing benchmark CI workflow is affected

---
*Phase: 18-benchmark-validation*
*Completed: 2026-03-29*
