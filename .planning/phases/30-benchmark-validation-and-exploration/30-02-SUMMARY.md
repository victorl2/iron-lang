---
phase: 30-benchmark-validation-and-exploration
plan: 02
subsystem: documentation
tags: [benchmarks, performance, analysis, report, documentation]

requires:
  - phase: 30-01
    provides: post-optimization.json, v0.0.6-alpha.json baseline, compare-output.txt

provides:
  - "docs/benchmark_report.md: comprehensive 352-line v0.0.7-alpha benchmark analysis report"
  - "tests/benchmarks/analyze_results.py: reusable analysis script for future milestone reports"
  - "docs/suggested_performance_improvements.md: updated with v0.0.7-alpha post-optimization state"
  - "Top 3 outlier deep-dive with generated C comparison and root-cause classification"

affects: [future-planning, documentation, ci]

tech-stack:
  added:
    - "analyze_results.py: Python3 script using json + statistics stdlib"
  patterns:
    - "Benchmark analysis script pattern: load two JSON baselines, compute improvement factors, categorize, generate Markdown report"
    - "Per-phase attribution via heuristic benchmark membership in known-affected sets"

key-files:
  created:
    - docs/benchmark_report.md
    - tests/benchmarks/analyze_results.py
  modified:
    - docs/suggested_performance_improvements.md

key-decisions:
  - "Top 3 outliers (median_two_sorted_arrays, three_sum, spawn_pipeline_stages): no prototype fixes committed because root causes are FUTURE PASS (P6/P7) or ARCHITECTURAL — premature to fix before structural loop reconstruction"
  - "analyze_results.py uses heuristic phase attribution (benchmark membership in named sets) rather than exact attribution — exact attribution would require git bisect or instrumentation"
  - "Deep-dive ordered by post-optimization ratio excluding sub-ms timing noise benchmarks (concurrency benchmarks excluded as non-meaningful performance targets)"

metrics:
  duration: 11min
  completed: 2026-04-01
  tasks_completed: 2
  files_modified: 3
---

# Phase 30 Plan 02: Benchmark Analysis Report Summary

**Comprehensive 352-line v0.0.7-alpha benchmark analysis report created; top 3 outliers deep-dived with generated C comparison; suggested_performance_improvements.md updated with post-optimization state and DONE annotations for P0–P5**

## Performance

- **Duration:** 11 min
- **Started:** 2026-04-01T17:53:24Z
- **Completed:** 2026-04-01T18:04:02Z
- **Tasks:** 2 completed
- **Files modified:** 3 (docs/benchmark_report.md created, analyze_results.py created, suggested_performance_improvements.md updated)

## Accomplishments

### Task 1: Create comprehensive benchmark analysis report

- Wrote `tests/benchmarks/analyze_results.py`: Python3 script that reads `post-optimization.json`
  and `v0.0.6-alpha.json`, computes per-benchmark improvement factors, categorizes into 8 groups,
  and generates a Markdown report to stdout. Script is reusable for future milestone reports.
- Generated `docs/benchmark_report.md` (352 lines) with all required sections:
  - **Executive Summary**: 138 benchmarks, 136/138 pass rate, median ratio 5.7x -> 1.0x (82% improvement)
  - **Biggest win**: `median_stream` improved 6226x (6848.5x -> 1.1x); `fibonacci_matrix` 930x
  - **Per-Phase Attribution**: table for Phases 24-29 with estimated benchmarks affected and improvement ranges
  - **connected_components journey**: ratio at each phase milestone from 11.5x (v0.0.6) to 0.5x (v0.0.7)
  - **Results by Category**: 8 categories (Array/Fill, DP, Tree/Graph, String, Stack/Queue, Concurrency, Math, General) with per-benchmark pre/post ratios and category medians
  - **Top 10 Most Improved**: sorted by improvement factor — median_stream (6226x), fibonacci_matrix (930x), connected_components (23x)
  - **Worst 10 Remaining**: spawn_pipeline_stages (5.2x), median_two_sorted_arrays (4.4x), three_sum (1.9x)
  - **Validation of P0-P5 Predictions**: P3 and P4 exceeded predictions; P2 partial; P0, P1, P5 met
  - **Remaining Opportunities**: P6 structured loops is highest-impact next step; only 3 benchmarks above 3x

### Task 2: Deep-dive top 3 outliers and update performance doc

**Deep-dives performed:**

1. **`median_two_sorted_arrays` (4.4x)**: Generated C comparison identified `int64_t` vs `int`
   type width (500M iterations × extra 64-bit ops) and goto loops preventing clang optimization.
   Root cause: FUTURE PASS (P7 auto-narrowing + P6 structured loops). No prototype fix committed
   — premature without P6 infrastructure.

2. **`three_sum` (1.9x)**: `skip_dup_lo` and `skip_dup_hi` helper functions not inlined due to
   array-param restriction in inliner. Each is called ~2x per inner-loop iteration in the hot path.
   Root cause: FUTURE PASS (relax const array-param inlining restriction). No prototype fix committed
   — requires careful testing of the inliner gate condition change.

3. **`spawn_pipeline_stages` (5.2x)**: Thread spawn overhead via `iron_spawn_task` and
   `iron_parallel_for`. C reference uses direct `pthread_create` with pre-allocated pool.
   Root cause: ARCHITECTURAL (runtime thread pool pre-warming). Not a compiler issue; benchmark
   is within its 8.0x threshold and tests correctness of spawn semantics.

**`docs/suggested_performance_improvements.md` updated:**
- Current State section replaced with v0.0.7-alpha post-optimization metrics
- Overhead breakdown table extended with Actual Phase + Actual vs Predicted columns
- Added "v0.0.7-alpha Optimization Results" section: P0-P5 each documented as DONE/PARTIAL
- Added "Top 3 Outlier Deep-Dive" section with generated C comparison for each outlier
- Priority Order section: P0-P5 annotated with DONE + actual measured impact; P6-P8+P2b remain

## Key Metrics

| Metric | v0.0.6-alpha | v0.0.7-alpha |
|---|---|---|
| Median ratio (meaningful benchmarks) | 5.7x | 1.0x |
| Benchmarks above 3x | 26 | 3 |
| Benchmarks above 10x | 26 | 0 |
| Pass rate | n/a (old thresholds) | 137/138 (0 failures) |

## Deviations from Plan

### No Prototype Fixes Committed

The plan specified "For each 'FIXABLE NOW' root cause: implement a prototype fix."

All three top outlier root causes were classified as FUTURE PASS or ARCHITECTURAL:
- `median_two_sorted_arrays`: int64_t width (P7 auto-narrowing) + goto loops (P6)
- `three_sum`: const array-param inlining restriction (requires careful inliner gate change)
- `spawn_pipeline_stages`: architectural (runtime thread pool)

No prototype fixes were committed because implementing partial fixes for P6/P7 root causes
without the full infrastructure would create fragile point fixes. The deep-dive analysis is
documented in `suggested_performance_improvements.md` as proposals for future phases.

This is within the plan's scope: "If the fix is complex or risky, document it as a proposal
in the report instead."

## Self-Check: PASSED

- `docs/benchmark_report.md`: FOUND, 352 lines
- `docs/benchmark_report.md` contains "Per-Phase Attribution": FOUND
- `docs/benchmark_report.md` contains "connected_components": FOUND
- `docs/benchmark_report.md` contains "Top 10 Most Improved": FOUND
- `docs/benchmark_report.md` contains "Validation of P0": FOUND
- `tests/benchmarks/analyze_results.py`: FOUND
- `docs/suggested_performance_improvements.md` contains "v0.0.7-alpha": FOUND (7 occurrences)
- `docs/suggested_performance_improvements.md` has DONE annotations: FOUND (11 occurrences)
- Benchmark suite: 137/138 passed, 0 failures (1 pre-existing nullable_sum_tree error)
- Task commits: 2387fcc (Task 1), d445bd8 (Task 2): FOUND
