#!/usr/bin/env python3
"""
Update benchmark config.json max_ratio thresholds from measured post-optimization results.

Strategy:
- measured ratio <= 1.0: max_ratio = 1.5  (generous floor for near-parity)
- measured ratio <= 2.0: max_ratio = round_up(ratio * 1.5, 1 decimal) — 50% headroom
- measured ratio <= 5.0: max_ratio = round_up(ratio * 1.3, 1 decimal) — 30% headroom
- measured ratio <= 20.0: max_ratio = round_up(ratio * 1.25, 1 decimal) — 25% headroom
- measured ratio > 20.0: max_ratio = round_up(ratio * 1.2, 1 decimal) — 20% headroom
"""

import json
import math
import os
import sys
from pathlib import Path

SCRIPT_DIR = Path(__file__).parent
RESULTS_FILE = SCRIPT_DIR / "results" / "post-optimization.json"
PROBLEMS_DIR = SCRIPT_DIR / "problems"


def compute_new_max_ratio(ratio: float) -> float:
    """Compute new max_ratio with headroom based on measured ratio."""
    if ratio <= 1.0:
        return 1.5
    elif ratio <= 2.0:
        # 50% headroom, rounded up to 1 decimal
        raw = ratio * 1.5
    elif ratio <= 5.0:
        # 30% headroom, rounded up to 1 decimal
        raw = ratio * 1.3
    elif ratio <= 20.0:
        # 25% headroom, rounded up to 1 decimal
        raw = ratio * 1.25
    else:
        # 20% headroom, rounded up to 1 decimal
        raw = ratio * 1.2

    # Round up to 1 decimal place
    return math.ceil(raw * 10) / 10


def main():
    if not RESULTS_FILE.exists():
        print(f"ERROR: Results file not found: {RESULTS_FILE}", file=sys.stderr)
        sys.exit(1)

    with open(RESULTS_FILE) as f:
        results = json.load(f)

    benchmarks = results.get("benchmarks", [])
    if not benchmarks:
        print("ERROR: No benchmarks found in results file", file=sys.stderr)
        sys.exit(1)

    print(f"Updating thresholds for {len(benchmarks)} benchmarks...")
    print(f"{'Name':<45} {'Ratio':>7} {'Old max':>9} {'New max':>9} {'Change':>8}")
    print("-" * 82)

    updated = 0
    skipped = 0
    errors = 0

    for bench in benchmarks:
        name = bench["name"]
        ratio = float(bench["ratio"])
        measured_max = float(bench["max_ratio"])

        config_path = PROBLEMS_DIR / name / "config.json"
        if not config_path.exists():
            print(f"  WARNING: config.json not found for {name}", file=sys.stderr)
            errors += 1
            continue

        with open(config_path) as f:
            config = json.load(f)

        old_max = config.get("max_ratio", measured_max)
        new_max = compute_new_max_ratio(ratio)

        # Preserve all existing fields, just update max_ratio
        config["max_ratio"] = new_max

        with open(config_path, "w") as f:
            json.dump(config, f, separators=(", ", ": "))
            # No trailing newline — match existing format

        change = "no change" if abs(new_max - old_max) < 0.001 else f"{old_max:.1f} -> {new_max:.1f}"
        print(f"  {name:<43} {ratio:>7.1f} {old_max:>9.1f} {new_max:>9.1f}  {change}")
        updated += 1

    print()
    print(f"Summary: {updated} updated, {skipped} skipped, {errors} errors")


if __name__ == "__main__":
    main()
