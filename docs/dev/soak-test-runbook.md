# Soak-test runbook

Phase 7 Plan 07-02 Task 02 (HARD-16, D-04).

The `ironls` soak harness is the leak detector we depend on to ship
alpha releases. This runbook explains how to run it locally, interpret
results, debug suspected leaks, and rotate thresholds when the server
outgrows them.

## Quick start — 30-minute short soak (local)

```bash
# Build a Release-mode ironls with the RSS cap code compiled in.
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DIRON_BUILD_LSP=ON
cmake --build build -j4 --target ironls

# Drive the 1800-event 30-minute short-soak workload.
python3 tests/lsp/soak/harness.py \
    --ironls ./build/ironls \
    --workload tests/lsp/soak/short.jsonl \
    --fixtures-root tests/lsp/soak/fixtures \
    --output-csv /tmp/short-result.csv \
    --threshold-peak-mib 400 \
    --threshold-growth-mib-per-hr 5
```

On a developer laptop expect the run to take ~30 minutes (the harness
honors the `t` field in workload.jsonl, which is the point — the
thresholds are calibrated against real-time event pacing). To iterate
faster on harness correctness without spending 30 minutes, run
`python3 tests/lsp/soak/harness.py --self-test` which exercises
`analyze()` with synthetic RSS curves in under a second.

## Interpreting `result.csv`

The CSV has three columns sampled every 10 seconds:

| Column              | Meaning                                           |
|---------------------|---------------------------------------------------|
| `t_sec`             | Seconds since harness start (wall clock)          |
| `rss_bytes`         | Process RSS from /proc/self/status (Linux) or ps (macOS) |
| `in_flight_requests`| Harness-tracked count of outstanding LSP requests |

The harness's `analyze()` step computes:

- **peak** — `max(rss_bytes)` across the whole run
- **growth** — linear-regression slope over samples where
  `t_sec >= 3600` (warm-up exclusion), multiplied by 3600 to give
  bytes/hour

Both are checked against thresholds. Nightly thresholds:

- peak ≤ 800 MiB = 838,860,800 bytes
- growth ≤ 5 MiB/hr = 5,242,880 bytes/hr

Per-PR short-soak thresholds (scaled because 30min < 1hr warmup):

- peak ≤ 400 MiB  (the short-soak only enforces peak; growth is not
  meaningful in a sub-warmup-length run)

## Debugging a leak pattern

When the harness fails with `growth ... bytes/hr > 5242880 bytes/hr
threshold`, your `result.csv` shows a suspicious positive slope. First
inspect the curve:

```bash
# Quick ASCII plot (requires gnuplot).
gnuplot -p -e "set datafile separator ','; set key off; \
    plot '/tmp/short-result.csv' using 1:2 with lines"
```

If the slope is visible from eye, rerun the same short soak under
`valgrind --tool=massif` or `heaptrack`. `ironls` is single-binary so
this is straightforward:

```bash
# Massif (stdlib-only, always available on Linux).
valgrind --tool=massif --massif-out-file=massif.out ./build/ironls \
    < /dev/null &
# ... drive the workload against the valgrind PID, or simply run
# the harness with --ironls $(which valgrind) and --workload short.jsonl
# then let valgrind exit at graceful shutdown.
ms_print massif.out > massif.txt
```

Massif's allocation tree points at the arena / stb_ds pool that
leaked. The top suspects (based on Phase 1-6 architecture) are:

- `Iron_Arena` in `src/lsp/facade/compile.c` — per-request arena is
  freed at the end of each dispatch; a leak here implies a broken
  `iron_arena_free(&arena)` call site.
- stb_ds maps in `src/lsp/store/workspace_index.c` — IndexEntry map
  should be re-derived per compile; a leak here implies a missing
  `shfree`.
- The yyjson allocator pool in `src/lsp/transport/json.c` — arena-
  backed per call; a leak here points at the `ilsp_json_alc` glue.

For each suspect, add an `ilsp_log` INFO pair around
`arena_create`/`arena_free` and re-run. Leaks show as unbalanced
entry/exit counts in the log.

## Recalibrating thresholds

Thresholds should only loosen after five consecutive green nightly
runs show a new baseline peak/growth:

1. Pull the last 5 nightly `result.csv` artifacts from GitHub
   Actions (soak-nightly job → upload-artifact).
2. Compute peak and growth for each run:

   ```bash
   python3 -c "
   import csv, sys
   from tests.lsp.soak.harness import analyze
   from pathlib import Path
   for p in sys.argv[1:]:
       code, msg = analyze(Path(p), 1000, 100)  # no threshold enforcement
       print(p, msg)
   " result-*.csv
   ```

3. Set the new threshold at 1.2 × the 90th percentile of the 5 values.
   For peak: `p90(peaks) * 1.2`; for growth: `p90(growths) * 1.2`.
4. Update `.github/workflows/soak.yml` and this runbook atomically;
   commit message MUST include the 5 measured baselines and the
   chosen multiplier for future spelunking.

## Known limitations

- GitHub-hosted runner CPU variance introduces ~10% noise into
  growth measurements. The 1.2× multiplier when recalibrating is
  intentional headroom.
- The short-soak PR job cannot enforce a meaningful growth threshold
  (30 minutes < 1 hour warmup-exclusion window). The 5 MiB/hr flag
  is advisory in that job; peak is the real per-PR gate.
- `IRON_LSP_RSS_CAP_BYTES=0` (the escape hatch documented in D-03)
  is what the harness sets to prevent `_exit(42)` during soak; do
  NOT set this in production — see `src/lsp/obs/rss.c` comments
  and T-07-02-06.
