# Benchmark Calibration

Phase 69 Plan 05 (REG-03). Documents the benchmark-threshold drift incident
observed during Phase 59 and establishes a 5-minute re-calibration runbook
for future drifts.

## Motivating Incident (Phase 59)

During Phase 59 (net foundation + Windows CI + URL module), the benchmark
suite began reporting threshold failures on ubuntu-latest runners that did
not reproduce on macos-arm64 developer machines. The four benchmarks that
drifted:

| Benchmark               | macOS-arm64 baseline | Linux-x86_64 observed | Delta |
|-------------------------|---------------------:|----------------------:|------:|
| `sieve_of_eratosthenes` | 0.73x                | 1.70x                 | +0.97 |
| `merge_k_sorted_lists`  | 0.77x                | 1.50x                 | +0.73 |
| `find_first_last`       | 1.10x                | 1.40x                 | +0.30 |
| `matrix_chain_mult`     | 0.77x                | 1.40x                 | +0.63 |

Reference: `.planning/REQUIREMENTS.md` §73 REG-03 text. The CI-runner
observations landed via GitHub Actions runs `24282224300` and `24273837832`
(commit `3b75cfc`) and were re-captured into the per-benchmark
`tests/benchmarks/problems/<name>/config.json` `rationale` strings on
2026-04-11 as part of the cross-platform reconciliation commit.

The drift consumed approximately 30 minutes of triage per incident because
neither the thresholds nor the methodology for re-capturing them was
documented. Phase 69 closes that gap.

## Root Cause

Benchmark ratios (iron_ms / c_ms) vary across hardware for three reasons:

1. **CPU architecture** — Apple Silicon's M-series cores execute Iron's
   RC-heavy paths differently from x86_64 due to branch-predictor behavior
   and cache line size (128 B on M-series vs 64 B on most x86_64 parts).
2. **Memory hierarchy** — L1/L2/L3 sizes differ across runners. A
   benchmark that fits in L2 on one host may spill to L3 on another,
   changing absolute runtime by 2-3x.
3. **Compiler differences** — clang on Linux and clang on macOS use
   different default flags (e.g. `-fomit-frame-pointer` on/off) and
   library implementations (`libc++` vs `libstdc++`).

The ratio metric intentionally normalizes against a C baseline, but the
ratio itself is still platform-sensitive because Iron's RC/arena overhead
scales differently from hand-written C across those three axes. The four
REG-03 benchmarks are particularly sensitive because they sit in the
10-50 ms absolute runtime range where cache topology and scheduler jitter
dominate the measurement noise.

## Re-calibration Methodology

Phase 58 established the 5-round trimmed-mean audit: run the benchmark
suite 5 times, discard the min and max ratio per benchmark, compute mean
and stddev from the middle 3. Implemented in `scripts/bench_audit.sh`.
Phase 69 extends this with a `--platform` flag so the output CSV and the
derived JSON record the runner tag.

**To re-calibrate after a drift:**

1. Identify the runner on which the drift was observed. Most commonly
   `linux-x86_64` (GitHub Actions ubuntu-24.04).
2. From a Release build of the repo:
   ```bash
   cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -G Ninja
   cmake --build build
   bash scripts/bench_audit.sh --platform linux-x86_64 5
   ```
3. Inspect `/tmp/58-audit.csv`. Each row is a benchmark with its trimmed
   mean / stddev / max ratio and a leading `platform` column tagged with
   the runner name.
4. Convert CSV → JSON using the snippet in Plan 05 Task 2 (saved as
   `tools/audit_csv_to_json.py` if that utility is later extracted from
   the plan):
   ```python
   import csv, json, os
   platform = "linux-x86_64"
   rows = {}
   with open("/tmp/58-audit.csv") as f:
       for row in csv.DictReader(f):
           if row.get("platform") != platform:
               continue
           rows[row["problem"]] = {
               "max_ratio":    float(row["ratio_max"]),
               "mean_ratio":   float(row["ratio_mean"]),
               "stddev_ratio": float(row["ratio_stddev"]),
               "variance_pct": float(row["variance_pct"]),
               "runs_n":       int(row["runs_used"]),
           }
   out = f"tests/benchmarks/thresholds.{platform}.json"
   json.dump({"_platform": platform, "_runs_per_benchmark": 5,
              "thresholds": rows}, open(out, "w"), indent=2, sort_keys=True)
   ```
5. Commit the updated `tests/benchmarks/thresholds.<platform>.json`.

Expected wall-clock: ~5 minutes on a local dev box, ~15 minutes on a
GitHub Actions ubuntu-24.04 runner (Release build + 5 audit rounds).

The next time a drift incident happens, this runbook replaces the
30-minute manual-debug session with a ~5-minute re-run.

## Observed Thresholds

The canonical observed data for each platform lives at
`tests/benchmarks/thresholds.<platform>.json`. Phase 69 captured the
initial `linux-x86_64` entry from the Phase 59 motivating-incident values
documented in REG-03 and the per-benchmark `config.json` `rationale`
strings (provenance: GitHub Actions runs `24282224300` and `24273837832`,
commit `3b75cfc`). The first four benchmarks listed below are the only
ones present in the initial `linux-x86_64.json` because they were the only
ones with documented Linux observations at Plan 05 landing time; a
follow-up commit can re-run the audit on an ubuntu-24.04 runner to fill
in the remaining ~135 benchmarks.

Future runs overwrite these files in place — no historical archive is
maintained. The git history of the JSON file is the archive.

Platforms currently calibrated:

| Platform       | File                                            | Captured                              |
|----------------|-------------------------------------------------|---------------------------------------|
| `linux-x86_64` | `tests/benchmarks/thresholds.linux-x86_64.json` | 2026-04-14 from Phase 59 CI run data  |
| `macos-arm64`  | (pre-existing hardcoded thresholds in `tests/benchmarks/problems/*/config.json`) | Phase 58 audit (2026-04-10) |

macOS-arm64 thresholds remain in the per-benchmark `config.json` files
alongside their `rationale` strings (Phase 58 artifacts). A future phase
can lift those into `thresholds.macos-arm64.json` for symmetry with the
`linux-x86_64` file.

### Phase 59 drift data (in the initial `thresholds.linux-x86_64.json`)

| Benchmark               | `max_ratio` | `mean_ratio` | `stddev_ratio` | `variance_pct` | `runs_n` |
|-------------------------|------------:|-------------:|---------------:|---------------:|---------:|
| `sieve_of_eratosthenes` |        1.70 |         1.50 |           0.12 |            7.9 |        5 |
| `merge_k_sorted_lists`  |        1.50 |         1.32 |           0.10 |            7.5 |        5 |
| `find_first_last`       |        1.40 |         1.25 |           0.06 |            4.8 |        5 |
| `matrix_chain_mult`     |        1.40 |         1.15 |           0.23 |           19.9 |        5 |

The `mean_ratio` and `stddev_ratio` values are conservative estimates
derived from the `variance_pct` observations documented in the original
`config.json` rationale strings; the `max_ratio` values are the observed
peaks from the Phase 59 CI runs. A fresh 5-round audit on an ubuntu-24.04
runner will replace the estimated columns with exact trimmed-mean data.

## Soft-Warning Policy

Phase 69 does NOT convert these thresholds into CI-blocking checks. The
existing `run_benchmarks.sh --check-regression` step in
`.github/workflows/benchmark.yml` is still `continue-on-error: true`.
`.github/workflows/benchmark.yml` loads `thresholds.<platform>.json` into
the `IRON_BENCH_THRESHOLDS_FILE` env var on matching runners (with a soft
fallback to the per-benchmark `config.json` hardcoded thresholds when the
file is absent), but the current `run_benchmarks.sh` driver does not yet
consume the env var. Once the team has lived with the calibrated
thresholds for a milestone, a follow-up phase can wire
`run_benchmarks.sh` to read the platform JSON and flip the regression
check from informational to hard enforcement.
