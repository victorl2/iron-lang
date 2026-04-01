---
phase: 31-spawn-await-correctness
plan: "02"
subsystem: benchmarks
tags: [spawn, await, benchmarks, performance, thresholds]
dependency_graph:
  requires: ["31-01"]
  provides: ["await-based benchmark timing", "updated thresholds", "new baseline"]
  affects: ["tests/benchmarks/problems/spawn_*/", "tests/benchmarks/problems/concurrency_spawn_*/", "tests/benchmarks/problems/concurrency_pipeline/"]
tech_stack:
  added: []
  patterns: ["val h = spawn(...) + await h", "per-iteration await inside loop"]
key_files:
  created:
    - tests/benchmarks/results/post-optimization.json
  modified:
    - tests/benchmarks/problems/spawn_independent_work/main.iron
    - tests/benchmarks/problems/concurrency_spawn_captured/main.iron
    - tests/benchmarks/problems/concurrency_spawn_independence/main.iron
    - tests/benchmarks/problems/concurrency_spawn_result/main.iron
    - tests/benchmarks/problems/concurrency_pipeline/main.iron
    - tests/benchmarks/problems/spawn_pipeline_stages/main.iron
    - tests/benchmarks/problems/spawn_independent_work/config.json
    - tests/benchmarks/problems/spawn_pipeline_stages/config.json
    - tests/benchmarks/problems/concurrency_spawn_captured/config.json
    - tests/benchmarks/problems/concurrency_spawn_independence/config.json
    - tests/benchmarks/problems/concurrency_spawn_result/config.json
    - tests/benchmarks/problems/concurrency_pipeline/config.json
    - tests/benchmarks/baselines/latest.json
decisions:
  - "All spawn benchmark thresholds updated via update_thresholds.py from measured ratios with built-in headroom formula"
  - "Spawn benchmark configs annotated with Phase 31 await-based timing notes for traceability"
  - "spawn_pipeline_stages threshold raised from 6.5 to 7.2 (measured ratio 5.7x; 25% headroom applied)"
  - "Concurrency spawn benchmarks thresholds tightened from 5.0-6.2 to 1.5 (all now sub-0.3x ratio)"
metrics:
  duration_minutes: 31
  completed_date: "2026-04-01"
  tasks_total: 3
  tasks_completed: 3
  files_modified: 13
  files_created: 1
---

# Phase 31 Plan 02: Spawn Benchmark Await Migration Summary

**One-liner:** All 6 spawn benchmarks migrated from fire-and-forget to `val h = spawn(...) + await h` pattern for fair timing vs C pthread_join; thresholds updated and baseline saved.

## Tasks Completed

### Task 1: Update ALL spawn-based benchmarks to use await

Updated 6 benchmark files to capture handles and await them:

1. **spawn_independent_work**: 8 spawns assigned to h0-h7; awaits placed after main thread work and before `val elapsed`. Fire-and-forget removed entirely.

2. **concurrency_spawn_captured**: 3 spawns per iteration now captured as h0/h1/h2; awaits at end of each loop iteration.

3. **concurrency_spawn_independence**: 4 spawns per iteration now captured as h0/h1/h2/h3; awaits at end of each loop iteration.

4. **concurrency_spawn_result**: 4 spawns per iteration now captured as h0/h1/h2/h3; awaits at end of each loop iteration.

5. **concurrency_pipeline**: 3 spawns per iteration now captured as h0/h1/h2; awaits at end of each loop iteration.

6. **spawn_pipeline_stages**: 2 bg_worker spawns now captured as hbg1/hbg2; awaits placed after all pipeline computation and before `val elapsed`.

All 6 compile successfully and produce correct output. No fire-and-forget spawns remain.

**Commit:** `61f60f4` — feat(31-02): update all spawn benchmarks to use await for fair timing

### Task 2: Run full benchmark suite, save baseline, update thresholds

- Full benchmark suite run: 137/138 pass (`nullable_sum_tree` pre-existing compilation failure, unrelated to spawn changes)
- All 6 spawn benchmarks PASS with await-based timing:
  - `spawn_independent_work`: 0.8x (threshold: 1.5x)
  - `spawn_pipeline_stages`: 4.9x (threshold: 7.2x)
  - `concurrency_spawn_captured`: 0.1x (threshold: 1.5x)
  - `concurrency_spawn_independence`: 0.0x (threshold: 1.5x)
  - `concurrency_spawn_result`: 0.0x (threshold: 1.5x)
  - `concurrency_pipeline`: 0.0x (threshold: 1.5x)
- Thresholds updated via `update_thresholds.py` from measured ratios
- Baseline saved to `tests/benchmarks/baselines/latest.json`
- Integration tests: 173/173 pass
- Algorithm tests: 13/13 pass

**Commit:** `5333a38` — feat(31-02): run benchmark suite, update thresholds and save baseline

## Task 3: Human Verification — APPROVED

Task 3 was a `checkpoint:human-verify` gate. Execution paused at checkpoint; user verified and approved the implementation.

**Verification confirmed:**
- Integration tests: 173/173 pass (including spawn_await, spawn_await_return, spawn_await_multiple)
- Algorithm tests: 13/13 pass
- All 6 spawn benchmarks contain `await` — no fire-and-forget spawns remain
- Benchmark suite: 137/138 pass (nullable_sum_tree pre-existing failure, unrelated to these changes)
- spawn/await end-to-end behavior verified correct

## Deviations from Plan

### Auto-fixed Issues

None — plan executed exactly as written.

**Observation:** The `nullable_sum_tree` benchmark has a pre-existing Iron compilation failure unrelated to spawn/await changes. This is an out-of-scope pre-existing issue logged in deferred-items.md.

## Self-Check: PASSED

All files verified present:
- tests/benchmarks/problems/spawn_independent_work/main.iron — FOUND
- tests/benchmarks/problems/concurrency_spawn_captured/main.iron — FOUND
- tests/benchmarks/problems/concurrency_spawn_independence/main.iron — FOUND
- tests/benchmarks/problems/concurrency_spawn_result/main.iron — FOUND
- tests/benchmarks/problems/concurrency_pipeline/main.iron — FOUND
- tests/benchmarks/problems/spawn_pipeline_stages/main.iron — FOUND
- tests/benchmarks/baselines/latest.json — FOUND
- tests/benchmarks/results/post-optimization.json — FOUND

All commits verified present:
- 61f60f4 feat(31-02): update all spawn benchmarks to use await for fair timing
- 5333a38 feat(31-02): run benchmark suite, update thresholds and save baseline
