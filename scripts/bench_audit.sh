#!/usr/bin/env bash
# Phase 58 Plan 03: 5-round benchmark audit helper.
#
# Runs tests/benchmarks/run_benchmarks.sh five times back-to-back, captures
# each run's --json output, and emits a CSV of per-problem statistics at
# /tmp/58-audit.csv.
#
# STATISTICAL AGGREGATION: trimmed-mean over 5 runs.
# For each benchmark, the min and max ratio across the 5 runs are discarded
# and mean + stddev are computed from the middle 3 (also known as a
# "winsorized 20% trimmed mean" when done symmetrically). This is robust to
# single-run outliers caused by cold-cache effects, scheduler jitter, or
# short benchmarks sitting below the machine's noise floor — all of which
# were observed in the pre-trim Phase 58 audit on binary_tree_diameter and
# similar ~15ms-runtime problems. Same treatment applied to iron_ms_mean
# and c_ms_mean columns.
#
# The raw 5-run ratio values are also preserved in dedicated columns so
# Plan 04's VERIFICATION narrative can cite both raw and trimmed stats.
#
# The CSV is consumed by Plan 03 Task 2 to write 'rationale' strings into
# every benchmark config.json.
#
# Usage: bash scripts/bench_audit.sh [runs]
#   runs: number of audit rounds (default 5)
#
# Outputs:
#   /tmp/58-audit-run-{1..5}.json  per-run JSON results
#   /tmp/58-audit.csv              aggregated CSV with columns:
#     problem, runs_total, runs_used, ratio_min, ratio_max,
#     ratio_mean, ratio_stddev, variance_pct, iron_ms_mean, c_ms_mean,
#     pass_count, current_max_ratio,
#     ratio_r1, ratio_r2, ratio_r3, ratio_r4, ratio_r5  (raw per-run ratios)

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
RUNNER="$REPO_ROOT/tests/benchmarks/run_benchmarks.sh"
RUNS="${1:-5}"

if [ ! -x "$RUNNER" ]; then
    echo "ERROR: $RUNNER not found or not executable"
    exit 1
fi

if ! command -v jq &>/dev/null; then
    echo "ERROR: jq is required for audit aggregation"
    exit 1
fi

echo "=== Phase 58 Plan 03: benchmark audit, $RUNS rounds ==="
echo ""

# 1) Run the benchmark suite $RUNS times, capture each run's JSON output.
# Note: the runner exits 1 if any benchmark fails its threshold, which is
# expected during the audit — we only care about the JSON output, not the
# pass/fail summary. Only abort on rc >= 100 (infra failures).
for i in $(seq 1 "$RUNS"); do
    out="/tmp/58-audit-run-$i.json"
    echo "[run $i/$RUNS] writing $out ..."
    bash "$RUNNER" --json "$out" >"/tmp/58-audit-run-$i.log" 2>"/tmp/58-audit-run-$i.err"
    rc=$?
    if [ "$rc" -ge 100 ]; then
        echo "  ABORT: runner exited with rc=$rc (infrastructure failure)"
        head -20 "/tmp/58-audit-run-$i.err"
        exit 1
    fi
    if [ ! -f "$out" ]; then
        echo "ABORT: $out was not produced (rc=$rc)"
        head -20 "/tmp/58-audit-run-$i.err"
        exit 1
    fi
    count=$(jq '.benchmarks | length' "$out" 2>/dev/null || echo "?")
    echo "  OK (rc=$rc, $count benchmarks recorded)"
done

echo ""
echo "=== Aggregating $RUNS runs into /tmp/58-audit.csv ==="

# 2) Aggregate with trimmed-mean robustness.
#    For each benchmark, drop the min and max across the N runs, then compute
#    mean + stddev from the remaining middle (N-2) values. Bessel's correction
#    (n-1 divisor) on the trimmed subset. Raw per-run ratios are kept in
#    dedicated columns so downstream tooling can cite both raw and trimmed.
#    Rationale: machine-level noise on ~15ms-runtime benchmarks produces
#    single-run outliers that distort a naive 5-sample mean; trimming min+max
#    removes those outliers while keeping enough samples for a useful stddev.
python3 - "$RUNS" <<'PYEOF'
import json
import math
import sys

RUNS = int(sys.argv[1])
runs_data = []
for i in range(1, RUNS + 1):
    path = f"/tmp/58-audit-run-{i}.json"
    with open(path) as f:
        runs_data.append(json.load(f))

# Index by problem name
by_problem = {}
for run in runs_data:
    for b in run.get("benchmarks", []):
        name = b["name"]
        entry = by_problem.setdefault(name, [])
        entry.append({
            "ratio": float(b["ratio"]),
            "iron_ms": float(b["iron_ms"]),
            "c_ms": float(b["c_ms"]),
            "status": b.get("status", "unknown"),
            "max_ratio_at_run": float(b.get("max_ratio", 0)),
        })

def trimmed_mean_std(values):
    """Discard single min and single max, compute mean + sample stddev.
    Bessel's correction (n-1 divisor) on the middle subset. If the input has
    < 3 values, fall back to plain mean and stddev on the full set (can't trim
    meaningfully). All values are treated as floats; equal-valued entries
    count toward distinct min/max slots (so a constant 5-sample set trims to 3)."""
    values = sorted(values)
    n = len(values)
    if n >= 3:
        trimmed = values[1:-1]   # drop one min and one max
    else:
        trimmed = values[:]
    m = len(trimmed)
    mean = sum(trimmed) / m
    if m > 1:
        var = sum((v - mean) ** 2 for v in trimmed) / (m - 1)
        stddev = math.sqrt(var)
    else:
        stddev = 0.0
    return mean, stddev, m

with open("/tmp/58-audit.csv", "w") as out:
    # Header: trimmed-aware statistics + raw per-run ratios for auditability.
    header_cols = [
        "problem", "runs_total", "runs_used",
        "ratio_min", "ratio_max",
        "ratio_mean", "ratio_stddev", "variance_pct",
        "iron_ms_mean", "c_ms_mean",
        "pass_count", "current_max_ratio",
    ]
    for i in range(1, RUNS + 1):
        header_cols.append(f"ratio_r{i}")
    out.write(",".join(header_cols) + "\n")

    for name in sorted(by_problem):
        entries = by_problem[name]
        ratios = [e["ratio"] for e in entries]
        iron_mss = [e["iron_ms"] for e in entries]
        c_mss = [e["c_ms"] for e in entries]
        statuses = [e["status"] for e in entries]
        current_thresh = entries[-1]["max_ratio_at_run"]

        n_total = len(ratios)
        r_min = min(ratios)
        r_max = max(ratios)

        # Trimmed-mean aggregation: drop single min + single max per metric.
        r_mean, r_stddev, n_used = trimmed_mean_std(ratios)
        iron_mean, _iron_std, _n_iron = trimmed_mean_std(iron_mss)
        c_mean, _c_std, _n_c = trimmed_mean_std(c_mss)

        variance_pct = (r_stddev / r_mean * 100) if r_mean > 0 else 0.0
        pass_count = sum(1 for s in statuses if s == "pass")

        row = [
            name, str(n_total), str(n_used),
            f"{r_min:.3f}", f"{r_max:.3f}",
            f"{r_mean:.3f}", f"{r_stddev:.3f}", f"{variance_pct:.2f}",
            f"{iron_mean:.3f}", f"{c_mean:.3f}",
            str(pass_count), str(current_thresh),
        ]
        # Append raw per-run ratios (pad with empty if a run missed this benchmark).
        for i in range(RUNS):
            if i < len(ratios):
                row.append(f"{ratios[i]:.3f}")
            else:
                row.append("")
        out.write(",".join(row) + "\n")

print(f"Wrote /tmp/58-audit.csv ({len(by_problem)} problems, {RUNS} runs each, trimmed-mean over middle {RUNS - 2})")
PYEOF

echo ""
echo "=== Audit complete ==="
echo "Raw per-run data:  /tmp/58-audit-run-{1..$RUNS}.json"
echo "Aggregated CSV:    /tmp/58-audit.csv"
echo ""
echo "binary_tree_diameter row:"
head -1 /tmp/58-audit.csv
grep "^binary_tree_diameter," /tmp/58-audit.csv || echo "  (not found — audit is broken)"
