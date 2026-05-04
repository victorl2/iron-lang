# Iron LSP SLO measurement harness

Per-PR p50 SLO gate for the four latency-sensitive LSP methods
(`textDocument/hover`, `textDocument/completion`,
`textDocument/diagnostic`, `textDocument/definition`).

See [`docs/dev/slo-runbook.md`](../../../docs/dev/slo-runbook.md) for
the full runbook (local reproduction, re-calibration procedure,
threshold-tuning policy, CI noise guidance).

## Quick reference

```bash
# Self-test (hermetic; percentile + threshold + framing + literals):
python3 tests/lsp/slos/measurement.py --self-test

# Dry-run (validate fixtures + plumbing, no ironls spawn):
python3 tests/lsp/slos/measurement.py --dry-run

# Full run against a Release-mode ironls:
python3 tests/lsp/slos/measurement.py \
    --ironls ./build/ironls \
    --fixtures-root tests/lsp/slos/fixtures \
    --samples 100 \
    --output-json slo-result.json
```

## D-06 locked thresholds (p50, medium fixture only)

| Method                     | p50 threshold |
|----------------------------|---------------|
| `textDocument/hover`       | < 20 ms       |
| `textDocument/completion`  | < 100 ms      |
| `textDocument/diagnostic`  | < 500 ms      |
| `textDocument/definition`  | < 50 ms       |

p95 and p99 are recorded to `slo-result.json` as observability but
not enforced (tail-latency flakes on GitHub Actions runner CPU
variance).

## Fixture inventory

| File                        | LoC  | Role           |
|-----------------------------|------|----------------|
| `fixtures/small_50loc.iron` | 46   | informational  |
| `fixtures/medium_500loc.iron` | 501 | **ENFORCED** (p50 gate applies here) |
| `fixtures/large_5000loc.iron` | 5000 | informational |
| `fixtures/iron.toml`        |   -  | workspace manifest |

`large_5000loc.iron` is the deterministic output of `gen_large.py`
at `SEED=20260423`; regenerating MUST produce byte-identical bytes.
