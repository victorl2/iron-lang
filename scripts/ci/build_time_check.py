#!/usr/bin/env python3
"""Phase 7 Plan 07-04 Task 02 (HARD-23, D-06) -- Iron build-time
regression alert with rolling per-OS baseline.

Python stdlib-only (no requirements, no pip). Measures the
wall-clock of a full ``cmake -B build && cmake --build build -j4``
invocation, compares it against a rolling 20-sample per-OS window
stored in ``build-time-baseline.json`` at the repo root. Fires (exit
non-zero) if the current build exceeds ``1.15 x rolling_average`` for
the CURRENT OS.

On push to main, the workflow (``.github/workflows/build-time.yml``)
passes ``--mode append-and-check`` so the current sample is appended
to the baseline and the oldest per-OS entry is trimmed once the
window fills (FIFO, 20 samples per OS).

Warmup tolerance: per-OS windows with < 5 samples skip the threshold
check and print ``WARMUP: ...``. This keeps the gate safe until the
first 5 push-to-main samples have filled the window on each OS.

Exit codes:
    0 -- build completed; current time within threshold (or warmup)
    1 -- build completed; current time > 1.15 x per-OS rolling avg
    2 -- script error (bad args, cmake invocation failed, I/O error)

Usage:
    # PR-mode (CI; no baseline write):
    python3 scripts/ci/build_time_check.py \\
        --mode check --os ubuntu-latest --sha $GITHUB_SHA

    # Push-to-main mode (CI; append + FIFO-trim + check):
    python3 scripts/ci/build_time_check.py \\
        --mode append-and-check --os ubuntu-latest --sha $GITHUB_SHA

    # Hermetic unit self-tests (no cmake invocation):
    python3 scripts/ci/build_time_check.py --self-test

    # Baseline parse-only sanity (used by planners):
    python3 scripts/ci/build_time_check.py --check-only
"""
from __future__ import annotations

import argparse
import datetime
import json
import os
import pathlib
import subprocess
import sys
import time
from typing import List, Optional, Tuple


# ── D-06 + HARD-23 locked constants ────────────────────────────────────

# Regression threshold: 15% per HARD-23 literal wording.
# Grep-matchable at tests/lsp/invariant/CMakeLists.txt + plan done criterion.
REGRESSION_THRESHOLD = 1.15

# Rolling window size per OS. Per-OS because ubuntu-latest and
# macos-latest have substantially different baselines; averaging
# across them would poison the gate.
WINDOW_SIZE = 20

# Warmup tolerance: skip the threshold check until at least this many
# samples exist for the current OS. Protects against the first few
# push-to-main runs tripping the gate from a noisy seed.
WARMUP_MIN_SAMPLES = 5

# Build parallelism: matches slos.yml + ci.yml convention.
BUILD_JOBS = 4

# Baseline JSON lives at the repo root so the rolling window is
# committed alongside the code it describes.
BASELINE_FILE_DEFAULT = "build-time-baseline.json"


# ── Baseline I/O ───────────────────────────────────────────────────────

def load_baseline(path: pathlib.Path) -> List[dict]:
    if not path.is_file():
        return []
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise SystemExit(f"[build-time] baseline {path} parse error: {exc}")
    if not isinstance(data, list):
        raise SystemExit(f"[build-time] baseline {path} is not a JSON array")
    for i, entry in enumerate(data):
        if not isinstance(entry, dict):
            raise SystemExit(f"[build-time] baseline {path}[{i}] not an object")
        for key in ("sha", "ts", "seconds", "os"):
            if key not in entry:
                raise SystemExit(
                    f"[build-time] baseline {path}[{i}] missing '{key}'")
    return data


def save_baseline(path: pathlib.Path, entries: List[dict]) -> None:
    path.write_text(json.dumps(entries, indent=2) + "\n", encoding="utf-8")


def fifo_trim_per_os(entries: List[dict], window: int) -> List[dict]:
    """Return `entries` with each per-OS slice trimmed FIFO to at
    most `window`. Preserves global insertion order per-OS bucket."""
    by_os: dict[str, List[dict]] = {}
    order: List[str] = []
    for e in entries:
        os_name = e["os"]
        if os_name not in by_os:
            by_os[os_name] = []
            order.append(os_name)
        by_os[os_name].append(e)
    # Trim each slice to the last `window` entries (most recent).
    for os_name in by_os:
        by_os[os_name] = by_os[os_name][-window:]
    # Reconstitute in global time order: sort across all slices by
    # timestamp so the committed file reads naturally left-to-right.
    out: List[dict] = []
    for os_name in order:
        out.extend(by_os[os_name])
    return out


def per_os_entries(entries: List[dict], os_name: str) -> List[dict]:
    return [e for e in entries if e["os"] == os_name]


def rolling_average_seconds(entries: List[dict], window: int) -> float:
    """Arithmetic mean of the last `window` entries' `seconds` fields."""
    window_entries = entries[-window:]
    if not window_entries:
        return 0.0
    return sum(float(e["seconds"]) for e in window_entries) / len(window_entries)


# ── Build invocation ───────────────────────────────────────────────────

def _iso_utc_now() -> str:
    return datetime.datetime.now(datetime.timezone.utc).strftime(
        "%Y-%m-%dT%H:%M:%SZ")


def measure_build_seconds(build_dir: str) -> float:
    """Configure + build. Returns wall-clock seconds. Raises on
    non-zero exit from either cmake step (we want a hard fail at the
    CI step rather than a misleading 'regression')."""
    t0 = time.monotonic()
    # Configure. Use Ninja matching slos.yml + ci.yml convention.
    subprocess.check_call([
        "cmake", "-B", build_dir, "-G", "Ninja",
        "-DCMAKE_BUILD_TYPE=Release",
        "-DIRON_BUILD_LSP=ON",
    ])
    subprocess.check_call([
        "cmake", "--build", build_dir, f"-j{BUILD_JOBS}",
    ])
    return time.monotonic() - t0


# ── Threshold decision ─────────────────────────────────────────────────

def compare_against_baseline(
    current_seconds: float,
    baseline: List[dict],
    os_name: str,
) -> Tuple[str, str]:
    """Return (status, message) where status is one of
    'PASS' / 'FAIL' / 'WARMUP'. PASS means current <= 1.15 x avg."""
    os_entries = per_os_entries(baseline, os_name)
    if len(os_entries) < WARMUP_MIN_SAMPLES:
        return "WARMUP", (
            f"WARMUP: only {len(os_entries)} baseline samples for "
            f"{os_name}; regression check skipped (minimum "
            f"{WARMUP_MIN_SAMPLES}; window fills on push-to-main)")
    avg = rolling_average_seconds(os_entries, WINDOW_SIZE)
    ratio = current_seconds / avg if avg > 0 else 0.0
    msg = (f"{os_name}: current={current_seconds:.1f}s "
           f"avg{WINDOW_SIZE}={avg:.1f}s ratio={ratio:.3f} "
           f"threshold={REGRESSION_THRESHOLD}")
    if ratio > REGRESSION_THRESHOLD:
        return "FAIL", msg
    return "PASS", msg


# ── Self-test (hermetic; no cmake invocation) ──────────────────────────

def _self_tests() -> int:
    # Test 1 -- baseline of 20 ubuntu-latest entries @ 100s; current
    # = 110s -> ratio 1.10 <= 1.15 PASS.
    b1 = [{"sha": f"s{i}", "ts": _iso_utc_now(), "seconds": 100,
           "os": "ubuntu-latest"} for i in range(20)]
    status, msg = compare_against_baseline(110, b1, "ubuntu-latest")
    assert status == "PASS", f"expected PASS, got {status}: {msg}"
    print(f"[self-test 1] 110/100=1.10 -> PASS: {msg}", file=sys.stderr)

    # Test 2 -- same baseline, current = 120s -> ratio 1.20 > 1.15 FAIL.
    status, msg = compare_against_baseline(120, b1, "ubuntu-latest")
    assert status == "FAIL", f"expected FAIL, got {status}: {msg}"
    print(f"[self-test 2] 120/100=1.20 -> FAIL: {msg}", file=sys.stderr)

    # Test 3 -- baseline has 0 entries for current OS (cold start) ->
    # WARMUP (skip threshold).
    status, msg = compare_against_baseline(999, [], "ubuntu-latest")
    assert status == "WARMUP", f"expected WARMUP, got {status}: {msg}"
    print(f"[self-test 3] cold start -> {msg}", file=sys.stderr)

    # Test 4 -- 21 existing ubuntu entries, FIFO-trim after append
    # leaves exactly 20 most-recent entries for ubuntu-latest and does
    # not touch macos-latest.
    pre_entries = (
        [{"sha": f"ub{i}", "ts": _iso_utc_now(), "seconds": 100,
          "os": "ubuntu-latest"} for i in range(21)]
        + [{"sha": f"mac{i}", "ts": _iso_utc_now(), "seconds": 150,
            "os": "macos-latest"} for i in range(3)]
    )
    appended = pre_entries + [{"sha": "new", "ts": _iso_utc_now(),
                                "seconds": 105, "os": "ubuntu-latest"}]
    trimmed = fifo_trim_per_os(appended, WINDOW_SIZE)
    ubuntu = per_os_entries(trimmed, "ubuntu-latest")
    macos = per_os_entries(trimmed, "macos-latest")
    assert len(ubuntu) == 20, (
        f"expected 20 ubuntu after trim, got {len(ubuntu)}")
    # Newest 20: ub2..ub20 (19 of the original 21) plus new = 20.
    # Original indices 0, 1 should have been dropped.
    ubuntu_shas = {e["sha"] for e in ubuntu}
    assert "ub0" not in ubuntu_shas, "FIFO should have dropped ub0"
    assert "ub1" not in ubuntu_shas, "FIFO should have dropped ub1"
    assert "new" in ubuntu_shas, "new sample must be in window"
    assert len(macos) == 3, f"macos untouched, got {len(macos)}"
    print(f"[self-test 4] 22 ubuntu -> FIFO-trim 20; macos untouched "
          f"(3 entries) -- OK", file=sys.stderr)

    # Test 5 -- macos-latest and ubuntu-latest tracked independently.
    # Append 1 to ubuntu; macos window (3 samples, all < WARMUP) must
    # still be WARMUP after the ubuntu append.
    baseline_mixed = trimmed
    status, msg = compare_against_baseline(160, baseline_mixed, "macos-latest")
    assert status == "WARMUP", (
        f"macos window still in warmup; got {status}: {msg}")
    print(f"[self-test 5] per-OS isolation: macos still WARMUP after "
          f"ubuntu append -- OK", file=sys.stderr)

    # Test 6 -- literal 1.15 threshold locked (guards accidental edit).
    assert REGRESSION_THRESHOLD == 1.15, (
        f"REGRESSION_THRESHOLD drifted from 1.15 to {REGRESSION_THRESHOLD}")
    assert WINDOW_SIZE == 20, f"WINDOW_SIZE drifted from 20 to {WINDOW_SIZE}"
    print("[self-test 6] 1.15 threshold + 20 window literals locked -- OK",
          file=sys.stderr)

    print("[self-test] PASS (6 cases)", file=sys.stderr)
    return 0


# ── check-only (parse-only sanity for planners) ────────────────────────

def _check_only(baseline_path: pathlib.Path) -> int:
    entries = load_baseline(baseline_path)
    if not entries:
        print(f"[check-only] {baseline_path}: empty baseline (acceptable at "
              f"first boot; push-to-main appends will fill it)",
              file=sys.stderr)
        return 0
    by_os: dict[str, int] = {}
    for e in entries:
        by_os[e["os"]] = by_os.get(e["os"], 0) + 1
    print(f"[check-only] {baseline_path}: {len(entries)} total entries",
          file=sys.stderr)
    for os_name, count in sorted(by_os.items()):
        print(f"[check-only]   {os_name}: {count} samples",
              file=sys.stderr)
    return 0


# ── Main ───────────────────────────────────────────────────────────────

def main(argv: Optional[List[str]] = None) -> int:
    p = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    p.add_argument("--mode", choices=("check", "append-and-check"),
                   default="check",
                   help="check: PR gate. append-and-check: push-to-main "
                        "gate that also appends the new sample.")
    p.add_argument("--os", default=None,
                   help="OS label (ubuntu-latest / macos-latest) for "
                        "the baseline bucket.")
    p.add_argument("--sha", default="unknown",
                   help="Commit SHA recorded alongside the sample.")
    p.add_argument("--build-dir", default="build",
                   help="CMake build directory used for the measurement.")
    p.add_argument("--baseline", type=pathlib.Path,
                   default=pathlib.Path(BASELINE_FILE_DEFAULT),
                   help="Path to build-time-baseline.json at the repo root.")
    p.add_argument("--self-test", action="store_true",
                   help="Run hermetic unit tests and exit.")
    p.add_argument("--check-only", action="store_true",
                   help="Parse baseline JSON and exit (no build).")
    args = p.parse_args(argv)

    if args.self_test:
        return _self_tests()
    if args.check_only:
        return _check_only(args.baseline)

    if not args.os:
        print("[build-time] --os is required outside --self-test / "
              "--check-only modes", file=sys.stderr)
        return 2

    # Measure.
    try:
        current_seconds = measure_build_seconds(args.build_dir)
    except subprocess.CalledProcessError as exc:
        print(f"[build-time] cmake invocation failed: {exc}",
              file=sys.stderr)
        return 2

    # Load baseline + compute decision.
    baseline = load_baseline(args.baseline)
    status, msg = compare_against_baseline(current_seconds, baseline, args.os)
    print(msg)

    # Append-mode: record sample, FIFO-trim, save.
    if args.mode == "append-and-check":
        new_entry = {
            "sha": args.sha,
            "ts": _iso_utc_now(),
            "seconds": round(current_seconds, 2),
            "os": args.os,
        }
        baseline.append(new_entry)
        baseline = fifo_trim_per_os(baseline, WINDOW_SIZE)
        save_baseline(args.baseline, baseline)
        print(f"[build-time] appended sample for {args.os}; baseline "
              f"now {len(baseline)} total entries", file=sys.stderr)

    if status == "FAIL":
        print(f"FAIL: build time grew > {REGRESSION_THRESHOLD}x baseline "
              f"for {args.os}")
        return 1
    print("PASS" if status == "PASS" else status)
    return 0


if __name__ == "__main__":
    sys.exit(main())
