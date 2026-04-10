---
phase: 58-benchmark-stabilization
plan: 02
subsystem: testing
tags: [benchmarks, nanoseconds, timing, quantization, bulk-rewrite, binary_tree_diameter, parallel_benchmarks]

# Dependency graph
requires:
  - phase: 58-benchmark-stabilization
    provides: "Time.now_ns() Iron stdlib API + ns-preferred extract_time_ms() runner helper"
provides:
  - "139 benchmark main.iron files emit 'Total time: {elapsed_ns} ns' as primary timing + 'Total time: {elapsed_ms} ms' for backwards compatibility"
  - "Zero Time.now_ms() calls remain anywhere under tests/benchmarks/problems/*/main.iron"
  - "5 parallel_* benchmarks additionally emit 'Sequential time' and 'Parallel time' in both ns and ms precision"
  - "Full benchmark suite pre-Plan-03 baseline: 138/139 passed, 0 failed, 0 errors, 1 skipped (nullable_sum_tree config skip), 0 Iron compile failures, 0 correctness failures"
affects:
  - 58-03-per-problem-audit      # relies on sub-ms precision now available end-to-end
  - 58-04-baseline-refresh        # consumes per-problem thresholds from 58-03's audit

# Tech tracking
tech-stack:
  added: []  # Pure mechanical rewrite — no new libraries, no new patterns beyond what Plan 01 already established
  patterns:
    - "Scripted bulk rewrite for uniform text transformations across a benchmark corpus: regex-per-declaration + index-shift on multi-line insert, warning list for anything that doesn't match, hand-fix second pass for the warned files"
    - "Dual-timing-pair handling in parallel benchmarks: run the same declaration-renaming transform twice (start_seq/elapsed_seq + start_par/elapsed_par) and convert every matching 'Total time:' / 'Sequential time:' / 'Parallel time:' print line to an ns+ms pair"

key-files:
  created: []
  modified:
    - tests/benchmarks/problems/binary_tree_diameter/main.iron   # Task 1 canonical reference
    - tests/benchmarks/problems/*/main.iron                       # Task 2 bulk (138 other files)

key-decisions:
  - "Two-script bulk rewrite (main script for 133 single-pair files + dedicated script for 5 dual-pair parallel benchmarks) instead of extending the main regex with dual-pair logic — keeps the primary script simple and auditable, and the 5 warned files are hand-inspected before any transformation is applied"
  - "Keep Iron: 0.00ms display in runner output for sub-ms benchmarks — the two-decimal format in the runner's display path (v < 1 ? %.2f : %d) rounds sub-5μs values to 0.00 but the underlying extract_time_ms() returns 6-decimal precision, so the ratio calculation uses the true sub-ms value; changing the display format is Plan 03's scope, not this plan's"
  - "Preserve 'Total time: {elapsed_ms} ms' fallback print on every benchmark alongside the ns line — Plan 01 documented this as backwards compatibility for CI dashboards/manual scripts that haven't been updated to prefer ns; the runner's ns-first grep ensures it's ignored when both are present"
  - "Use integer division elapsed_ns / 1000000 for the ms fallback line (no float formatting) — Iron has no float format specifier support, and the fallback ms line only exists for grep-based backwards compatibility, not precision; the ns line carries the full precision"

patterns-established:
  - "Ns+ms dual-print convention: every benchmark's timing output carries BOTH a 'Total time: X ns' (precise, integer ns) and 'Total time: Y ms' (coarse, integer ms) line — the runner prefers ns, external tools can fall back to ms"
  - "Variable naming with unit suffix: start_ns, elapsed_ns, elapsed_ms — forces a compile error on any mis-substitution and lets a future reader (or LLM agent) see the unit in the variable name rather than inferring it from context"
  - "Mechanical bulk rewrite: audit pattern uniformity first (grep both the declaration and print sites), run the script, triage warnings with a second targeted pass, never leave warned files untouched"

requirements-completed: [BENCH-01, BENCH-02]

# Metrics
duration: 10 min
completed: 2026-04-10
---

# Phase 58 Plan 02: Benchmark Ns-Timing Rewrite Summary

**Rewrote all 139 benchmark main.iron files to use Time.now_ns() as the primary timer with an integer-ms fallback print — eliminating the ms-integer quantization that left 20+ sub-ms benchmarks at iron_ms: 0 in the Phase 54 baseline, and unblocking Plan 03's 5-round per-problem audit.**

## Performance

- **Duration:** 10 min
- **Started:** 2026-04-10T11:12:47Z
- **Completed:** 2026-04-10T11:23:29Z
- **Tasks:** 2
- **Files modified:** 139 (1 Task 1 reference + 138 Task 2 bulk)

## Accomplishments

- **binary_tree_diameter reference** rewritten end-to-end and smoke-tested through `run_benchmarks.sh`: prints `Total time: 13840000 ns` + `Total time: 13 ms`, runner normalizes ns to `13.840000` ms and reports `[PASS] 1.3x speed, 1.3x memory` — sub-ms precision confirmed, no `ratio: 0.0`, no `timing extraction failed`, no `CORRECTNESS FAILURE`.
- **138 other benchmarks bulk-rewritten** with a regex-driven Python script (`/tmp/58-02-rewrite.py`). Script initially reported 133 rewritten + 5 warnings (the parallel_* benchmarks carry a dual timing pair the single-pair regex doesn't match). A targeted second-pass script (`/tmp/58-02-rewrite-parallel.py`) handled those 5 files by walking declarations and prints once, renaming every `val X = Time.now_ms()...` and every `println("...: {X} ms")` that referenced a renamed var. Both scratch scripts deleted after the rewrite.
- **Zero Time.now_ms() calls remain** across all 139 benchmark main.iron files (`grep -l "Time.now_ms()" tests/benchmarks/problems/*/main.iron` returns empty).
- **Full suite smoke-run: 138/139 passed, 0 failed, 0 errors, 1 skipped** (the skip is `nullable_sum_tree`, marked `skip: true` in its config — pre-existing, unrelated to this plan). Zero Iron compile failures, zero correctness failures.
- **Every benchmark now prints both timing lines:** 139 files have `Total time: {*_ns} ns`, 139 files have `Total time: {*_ms} ms`. The 5 parallel benchmarks additionally emit `Sequential time: {*_ns} ns` + ms and `Parallel time: {*_ns} ns` + ms label lines.

## Task Commits

Each task was committed atomically:

1. **Task 1: Rewrite binary_tree_diameter as canonical reference** — `75a120e` (feat)
2. **Task 2: Bulk-rewrite 138 benchmarks to Time.now_ns() timing** — `2d60c16` (feat)

**Plan metadata:** _(final docs commit appended after this summary)_

## Files Created/Modified

### Modified

- `tests/benchmarks/problems/binary_tree_diameter/main.iron` — canonical reference, replaced lines 60-72 with the ns+ms dual-print pattern; diameter() function body (lines 6-42) and all four correctness tests (lines 49-58) untouched
- `tests/benchmarks/problems/*/main.iron` — 138 other benchmark sources, mechanically rewritten to match the Task 1 reference pattern
  - 133 single-pair files rewritten by the main regex script
  - 5 dual-pair parallel_* files (`parallel_compute_intensive`, `parallel_fibonacci`, `parallel_mandelbrot`, `parallel_matrix_multiply`, `parallel_prime_sieve`) rewritten by the targeted secondary script

## Bulk Rewrite Statistics

| Measure                                        |  Count |
| ----------------------------------------------- | ------:|
| Total benchmark main.iron files                 |    139 |
| Rewritten in Task 1 (manual, canonical ref)     |      1 |
| Rewritten in Task 2 (single-pair, main script)  |    133 |
| Rewritten in Task 2 (dual-pair, parallel_*)     |      5 |
| Unmodified                                      |      0 |
| Files still containing `Time.now_ms()`          |      0 |
| Files containing `Time.now_ns()`                |    139 |
| Files printing `Total time: {*_ns} ns`          |    139 |
| Files printing `Total time: {*_ms} ms`          |    139 |

## Full Suite Run Output (Pre-Plan-03 Baseline)

```
Results: 138/139 passed (0 failed, 0 errors, 1 skipped)
```

| Outcome    | Count | Notes                                                                                          |
| ---------- | -----:| ---------------------------------------------------------------------------------------------- |
| Passed     |   138 | All passed against their per-problem `max_ratio` threshold                                     |
| Failed     |     0 | Zero regressions from Phase 54 baseline introduced by the rewrite                              |
| Errors     |     0 | Zero Iron compilation failures, zero correctness failures                                      |
| Skipped    |     1 | `nullable_sum_tree` — `skip: true` in its config.json, pre-existing, unrelated                 |
| **Total**  | **139** | Full corpus                                                                                |

**Delta vs Phase 54 baseline (from 54-02-SUMMARY and the ROADMAP progress table):**

- Phase 54 baseline: ~113/139 passing under the uniform 2.5x threshold
- Phase 58 Plan 02 baseline: 138/139 passing under a mix of 1.7x/1.8x/2.3x/2.5x/2.8x/5.0x/7.2x per-problem thresholds

**The jump from ~113 to 138 is not a Plan 02 change — it's the per-problem threshold adjustments that landed between Phase 54 and Phase 58 as individual hotfixes. Plan 02 is functionally change-neutral on pass/fail counts: it only converts the timing source, not the thresholds.** Plan 03 will formally re-audit every threshold under 5 rounds of the ns-precision timing.

## Sub-ms Quantization Observations for Plan 03

44 benchmarks display `Iron: 0.00ms` in the runner output (2-decimal format rounds values < 5μs to `0.00`). These are the exact benchmarks that were previously `iron_ms: 0` under the ms-integer timing. Under Plan 02, extract_time_ms() is correctly pulling the ns line and computing a 6-decimal-precision ms value, so the ratio calculation (done in awk against the full-precision value, not the display string) is meaningful. **Plan 03 should consider widening the runner display format to 3+ decimals** (e.g. `if (v < 1) printf "%.3f"` → `0.003`) so that fast benchmarks no longer visually round to zero.

Sub-ms benchmarks identified (sample — full list derivable from `grep "Iron: 0.00ms" /tmp/58-02-t2-bench.log`):

- `buy_sell_stock`, `climbing_stairs`, `container_most_water`, `course_schedule`, `pascal_triangle`, `power_of_two`, `product_except_self`, `rabin_karp`, `reverse_bits`, `run_length_encoding`, `search_rotated_array`, `sieve_of_eratosthenes`, `single_number`, `spiral_matrix`, `sqrt_integer`, `subsets_bitmask`, `task_scheduler`, `trapping_rain_water`, `two_sum`, `wildcard_matching`, and ~24 more

None of these are Plan 02 regressions — they all pass their max_ratio threshold because the runner's awk division on the 6-decimal ns-derived value correctly computes a near-zero ratio (Iron is dramatically faster than C on these pure-integer loops, which tracks with the Phase 56 monomorphic-collapse + Phase 49 fusion engine work).

## Decisions Made

- **Canonical-reference-first strategy:** Task 1 rewrote binary_tree_diameter manually and smoke-tested it end-to-end BEFORE running the Task 2 bulk script. This validated (a) the exact pattern we want to propagate, (b) that Plan 01's runner ns-preference works against real benchmark output (not just the shell smoke tests Plan 01 ran in isolation), and (c) that the benchmark still passes correctness against expected_output.txt. Running the bulk script first would have risked propagating an unverified pattern to 138 files.
- **Split the bulk script into single-pair + dual-pair passes** rather than extending the main regex to handle both. The 5 parallel_* files are a tiny minority of the corpus and hand-triaging them in a dedicated pass is auditable; making the main script polymorphic on pair count would have added state machines that are harder to read.
- **Include the `Total time: {elapsed_ms} ms` fallback print line on every benchmark**, not just the binary_tree_diameter reference. Plan 01 already documented this as belt-and-suspenders for pre-Phase-58 grep-based tooling; the runner's ns-first preference means the ms line is harmless when both are present, and omitting it on the bulk 138 would have been an inconsistency with the canonical reference.
- **Use integer division `elapsed_ns / 1000000`** for the ms fallback value (not any kind of rounding). Iron has no float format specifier; the fallback line exists only for backwards compatibility, not precision; the ns line carries the real precision. Integer truncation is the simplest correct behavior.
- **Leave the 5 warnings from the main bulk script as a targeted hand-pass, not a script extension.** Hand-inspecting the 5 parallel_* files revealed they also carry `Sequential time:` and `Parallel time:` label lines that still needed ns conversion (otherwise the acceptance criterion "zero Time.now_ms() calls" would have failed). Extending the main script to handle these would have added label-regex complexity that isn't needed for the other 133 files.

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 3 — Blocking] 5 parallel benchmarks have a dual-pair timing structure the main regex script cannot handle**

- **Found during:** Task 2 (first run of the main bulk rewrite script)
- **Issue:** The plan's main regex script (from the plan action text) matches a single `val START = Time.now_ms()` / `val ELAPSED = Time.now_ms() - START` / `println("Total time: {ELAPSED} ms")` triplet. The 5 parallel_* benchmarks (`parallel_compute_intensive`, `parallel_fibonacci`, `parallel_mandelbrot`, `parallel_matrix_multiply`, `parallel_prime_sieve`) carry TWO timing pairs — `start_seq/elapsed_seq` for sequential baseline and `start_par/elapsed_par` for the parallel version — plus three print lines (`"Sequential time: {elapsed_seq} ms"`, `"Parallel time: {elapsed_par} ms"`, `"Total time: {elapsed_par} ms"`). The main script found `start_seq` first, then searched for `"Total time: {elapsed_seq}"` which doesn't exist (the `Total time` line references `elapsed_par`), reported `print=None`, and left the file untouched. Script output: `Rewritten: 133 / Skipped: 1 / Warnings: 5`.
- **Fix:** Wrote a dedicated secondary script (`/tmp/58-02-rewrite-parallel.py`) that walks the file linearly, transforms BOTH `val ... = Time.now_ms()` declarations, transforms BOTH `val ... = Time.now_ms() - ...` elapsed lines into ns+ms pairs, and transforms EVERY print line matching `"<LABEL>: {<var>} ms"` whose var is in the set of renamed elapsed vars. Ran it on the 5 files, each reported `rewrote 2 elapsed var pair(s)`, leaving zero warnings. The plan's action text explicitly authorized this ("If the script reports any warnings (pattern not matched for a file), inspect each warned file by hand and apply the transformation manually using the Task 1 pattern") — so a scripted hand-fix still counts as following the plan.
- **Files modified:** 5 files — `tests/benchmarks/problems/parallel_{compute_intensive,fibonacci,mandelbrot,matrix_multiply,prime_sieve}/main.iron`
- **Verification:**
  - `grep -l "Time.now_ms()"` across all 139 benchmarks returns empty (zero remaining now_ms calls)
  - `parallel_fibonacci` direct-run output shows both ns and ms lines for all three timing sites: `Sequential time: 651331000 ns / Sequential time: 651 ms / Parallel time: 88353000 ns / Parallel time: 88 ms / Total time: 88353000 ns / Total time: 88 ms`
  - Full suite passes all 5 parallel benchmarks under their max_ratio thresholds
- **Committed in:** `2d60c16` (Task 2 commit — the secondary script's output is folded into the Task 2 bulk commit because it's part of the same mechanical rewrite, just handled in a two-pass manner)

---

**Total deviations:** 1 auto-fixed (1 blocking; Rule 3)
**Impact on plan:** Zero scope creep. The plan's action text explicitly anticipated and authorized the hand-fix pathway for any files the main script warned on; the only novel aspect was automating the hand-fix with a second targeted script. No other deviations.

## Issues Encountered

- **Main bulk script's regex-PRINT_RE first-match behavior didn't handle dual-pair parallel benchmarks.** Detected, fixed via a targeted second-pass script, no rework required. Documented above under Deviations so future plans know the 5 parallel_* benchmarks have a non-standard timing shape.
- **No other issues.** Build was clean on every incremental cmake call. Python 3 was available. All 139 benchmarks compiled on first post-rewrite build. Full suite ran end-to-end on first post-rewrite invocation.

## User Setup Required

None — no external service configuration, no environment variables, no secrets. Pure text rewrite of Iron benchmark source files.

## Next Phase Readiness

- **Plan 03 (per-problem audit) has a meaningful signal source.** Every benchmark emits ns-precision timing; extract_time_ms() consumes the ns line; ratios are computed against 6-decimal-precision values. Plan 03 can now run its 5-round audit and actually distinguish fast benchmarks from each other rather than seeing 44 of them collapsed at `iron_ms: 0`.
- **Plan 03 display format consideration:** Plan 03 should widen the runner's display path (line 362 of `run_benchmarks.sh`, `if (v < 1) printf "%.2f"` → `"%.3f"` or `"%.4f"`) so that sub-ms benchmarks no longer visually round to `0.00ms`. The ratio calculation already uses full precision; only the display needs updating. This is noted as a Plan 03 consideration, not a Plan 02 deviation.
- **Pre-Plan-03 baseline locked:** 138 passing / 0 failing / 0 errors / 1 skipped (config-skip nullable_sum_tree). Plan 03's audit should hold this line modulo per-problem threshold adjustments.
- **No blockers.** Plan 03 can start immediately against a clean post-rewrite benchmark suite.

---

## Self-Check: PASSED

Verified commits and files on disk:

- `75a120e` Task 1 commit — FOUND in `git log --oneline`
- `2d60c16` Task 2 commit — FOUND in `git log --oneline`
- `tests/benchmarks/problems/binary_tree_diameter/main.iron` contains `Time.now_ns` and `elapsed_ns` — VERIFIED
- `grep -l "Time.now_ms()" tests/benchmarks/problems/*/main.iron` → empty — VERIFIED
- `grep -l "Time.now_ns()" tests/benchmarks/problems/*/main.iron | wc -l` → 139 — VERIFIED
- `grep -l 'Total time: {.*_ns} ns' tests/benchmarks/problems/*/main.iron | wc -l` → 139 — VERIFIED
- `grep -l 'Total time: {.*_ms} ms' tests/benchmarks/problems/*/main.iron | wc -l` → 139 — VERIFIED
- `cmake --build build` → exit 0 with no work — VERIFIED
- Full suite: `Results: 138/139 passed (0 failed, 0 errors, 1 skipped)` in `/tmp/58-02-t2-bench.log` — VERIFIED
- Zero `Iron compilation failed` in full suite log — VERIFIED
- Zero `CORRECTNESS FAILURE` in full suite log — VERIFIED
- `/tmp/58-02-rewrite.py` and `/tmp/58-02-rewrite-parallel.py` deleted — VERIFIED

---
*Phase: 58-benchmark-stabilization*
*Completed: 2026-04-10*
