# Iron LSP SLO Runbook

Phase 7 Plan 07-04 (HARD-18, HARD-23, D-06) establishes a per-PR p50
SLO gate on four LSP methods and a per-PR build-time regression alert
with a rolling per-OS baseline. This runbook captures the local
reproduction path, the re-calibration procedure, and the threshold-
tuning policy that MUST be followed when an SLO regresses.

## D-06 locked thresholds

All four thresholds are enforced on the medium (500 LoC) fixture
ONLY. Small (50 LoC) and large (5000 LoC) fixtures are measured and
recorded into the artifact but never fail CI.

| Method                     | p50 threshold | Rationale (D-06)                 |
|----------------------------|---------------|-----------------------------------|
| `textDocument/hover`       | < 20 ms       | HARD-18 requirement              |
| `textDocument/completion`  | < 100 ms      | HARD-18 requirement              |
| `textDocument/diagnostic`  | < 500 ms      | HARD-18 requirement              |
| `textDocument/definition`  | < 50 ms       | self-imposed; navigation gate    |

p95 and p99 are **recorded** to `slo-result.json` as observability
but never enforced -- GitHub Actions runner CPU variance produces
tail-latency flakes that would chase real regressions into the
noise floor. The 100 samples/method spec is locked at D-06.

## Why the medium fixture is the gate

Per D-06 rationale: 500 LoC is the "typical Iron file size for game
development + stdlib authoring." Small fixture (~50 LoC) is too cold
to measure the hot-path steady state; large fixture (~5000 LoC)
approaches the stdlib.iron upper bound and is kept as a tail-
observability sample. Enforcement on medium is the stable middle
point where dev-machine and CI runner measurements converge within
the ~20% headroom built into the thresholds.

## Local reproduction

```bash
# Build ironls in Release mode (exact build the CI job uses).
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DIRON_BUILD_LSP=ON
cmake --build build -j4 --target ironls

# Run the full 100-sample sweep across all three fixtures.
python3 tests/lsp/slos/measurement.py \
    --ironls ./build/ironls \
    --fixtures-root tests/lsp/slos/fixtures \
    --samples 100 \
    --output-json /tmp/slo-result.json

# Inspect the per-fixture / per-method numbers.
python3 -m json.tool /tmp/slo-result.json | less
```

Exit code 0 means every medium-fixture p50 passed; 1 means at least
one threshold was exceeded; 2 means the harness itself errored
(missing fixture, ironls not executable, initialize timeout).

## Plumbing-only validation (CI fast-fail gate)

The CTest invariant `test_slo_measurement_self_test` runs
`measurement.py --self-test` in under a second without spawning
ironls. It validates the pure-Python percentile function, the
threshold enforcement decision, the LSP framing round-trip, the
medium-fixture LoC window, and the locked D-06 literals. Regressions
in any of those are caught pre-build on every ctest run.

Local dry-run (no ironls spawn; validates fixtures + plumbing):

```bash
python3 tests/lsp/slos/measurement.py --dry-run
```

## Re-calibration procedure

SLO thresholds are DELIBERATELY sticky; D-06 locks them. Raise a
threshold only after this procedure. Lower them only after a
multi-PR stable p50 sits ~30% below the current gate.

1. Run five consecutive green PRs that touch `src/lsp/**`. Collect
   each run's `slo-result` artifact from the GitHub Actions UI.
2. For each method, compute the p90 of the 5 measured p50 values.
   (In practice: `python3 -c "import json, glob; ps=[json.load(open(f))
   ['results']['medium_500loc'][m]['p50'] for f in glob.glob('slo-result*.json')];
   print(sorted(ps)[int(len(ps)*0.9)])"` for each method m.)
3. Proposed new threshold = `1.2 * p90`. The 1.2 multiplier keeps
   ~20% runner-variance headroom (CONTEXT §specifics CI noise
   guidance) and prevents threshold-chasing.
4. Open a PR touching THIS runbook + `measurement.py` (the literal
   constants) + `07-CONTEXT.md §D-06` with the new threshold and
   the measured p90 data as evidence. CODEOWNERS review required.

## Threshold-tuning policy

- **Raise a threshold only after the 5-PR p90 procedure above.**
- **Never** raise a threshold mid-regression to get CI green. That
  is chasing flakiness and masks real regressions.
- If a threshold is at the boundary (p50 within 5 ms of the
  threshold) on more than two green PRs in a row, schedule a
  re-calibration conversation.

## Why GitHub Actions runner CPU varies (~20% headroom)

GitHub-hosted `ubuntu-latest` runners share hypervisor CPU with
other tenants. Measured variance on a fixed-workload SLO run
across 5 sequential identical-PRs in an alpha project of this
scale is 10-18%. D-06 baked ~20% headroom into each threshold
over the empirically-measured dev-machine numbers so that single-
run variance does not flake the gate.

## Build-time regression alert (HARD-23)

A separate `.github/workflows/build-time.yml` job measures
`cmake -B build && cmake --build build -j4` wall-clock and
compares it against a per-OS rolling 20-sample window stored in
`build-time-baseline.json` at the repo root. Fires (CI fails) if
the current build time exceeds `1.15 x rolling_average` for the
current OS.

### Baseline seeding procedure

The initial `build-time-baseline.json` ships with 5 seed samples
per OS. These are calibration numbers based on observed cold-cache
GitHub Actions runner times; the first 20 per-OS PRs will fill the
rolling window and replace the seeds one-by-one via the push-to-
main append path.

Warmup tolerance: until the window for a given OS has >= 5
samples, `scripts/ci/build_time_check.py` skips the threshold
comparison and prints `WARMUP: ...`. This keeps the gate safe
while the window fills.

### Local reproduction

```bash
# Self-test the build-time script (synthetic baselines, no build):
python3 scripts/ci/build_time_check.py --self-test

# Full run against the current tree (spawns cmake + ninja):
python3 scripts/ci/build_time_check.py \
    --mode check \
    --os ubuntu-latest \
    --sha $(git rev-parse HEAD)
```

### Threshold-tuning policy (build-time)

Identical shape to the SLO procedure: 5 PRs, p90, 1.2x for headroom.
Current 1.15x is the HARD-23-locked threshold.

## Appendix: measurement boundary

Per CONTEXT §specifics and RESEARCH §"Per-request SLO measurement":
`measurement.py` brackets the latency measurement from
`time.monotonic_ns()` immediately before the LSP framed request
write to `time.monotonic_ns()` immediately after the response
arrives on stdout. This is a REQUEST-SPECIFIC boundary, not the
whole-process wall-clock boundary (which hyperfine and similar
tools measure and which D-06 explicitly rejects).
