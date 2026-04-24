# Iron LSP soak harness

Phase 7 Plan 07-02 Task 02 (HARD-16, D-04).

This directory holds the nightly 8-hour soak + per-PR 30-minute short-soak
harness that detects RSS growth / leak regressions in `ironls`. The
driver is Python 3 stdlib-only; no extra pip installs are required.

See `docs/dev/soak-test-runbook.md` for:

- how to run locally (short-soak takes 30 minutes)
- how to interpret the emitted `result.csv`
- how to debug a suspected leak (valgrind / heaptrack recipes)
- how to recalibrate thresholds after sustained green runs

Contents:

| File                 | Purpose                                                    |
|----------------------|------------------------------------------------------------|
| `harness.py`         | Driver + RSS sampler + analyze()                           |
| `gen_workload.py`    | Committed generator for `workload.jsonl` / `short.jsonl`   |
| `gen_fixtures.py`    | Committed generator for `fixtures/module_NN.iron`          |
| `workload.jsonl`     | 28,800 events, 8h canonical nightly workload               |
| `short.jsonl`        | 1,800 events, 30-minute PR short-soak workload             |
| `fixtures/iron.toml` | Canonical workspace manifest                               |
| `fixtures/module_*.iron` | 50 cross-referenced Iron modules (~5000 LoC total)     |
