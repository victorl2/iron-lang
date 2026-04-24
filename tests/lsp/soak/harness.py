#!/usr/bin/env python3
"""Phase 7 Plan 07-02 Task 02 (HARD-16, D-04) -- Iron LSP soak harness.

Python stdlib-only (no numpy, no pandas, no pytest-lsp). Spawns an
`ironls` subprocess, runs an LSP initialize + initialized handshake,
replays a JSONL workload over stdio, samples RSS every 10 seconds
into `result.csv`, and on completion runs a peak + linear-regression
growth analysis against thresholds. Exits non-zero on threshold
breach per D-04.

Usage:
    # Run canonical 8-hour soak:
    python3 harness.py --ironls ./build/ironls \\
        --workload tests/lsp/soak/workload.jsonl \\
        --fixtures-root tests/lsp/soak/fixtures \\
        --output-csv result.csv

    # Run 30-minute PR short-soak:
    python3 harness.py --ironls ./build/ironls \\
        --workload tests/lsp/soak/short.jsonl \\
        --fixtures-root tests/lsp/soak/fixtures \\
        --output-csv short-result.csv \\
        --threshold-peak-mib 400 --threshold-growth-mib-per-hr 5

    # Self-test the analyze() logic without touching ironls:
    python3 harness.py --self-test

Exit codes:
    0 - soak completed; thresholds not breached
    1 - threshold breach OR ironls exited non-zero (e.g. RSS cap)
    2 - harness error (bad args, ironls not executable, etc.)
"""
from __future__ import annotations

import argparse
import csv
import json
import os
import pathlib
import signal
import subprocess
import sys
import threading
import time


# ── D-04 thresholds ────────────────────────────────────────────────────

DEFAULT_PEAK_MIB             = 800     # 838,860,800 bytes (nightly)
DEFAULT_GROWTH_MIB_PER_HR    = 5       #   5,242,880 bytes/hour
WARMUP_EXCLUSION_SEC         = 3600    # 1 hour skipped for growth calc
RSS_SAMPLE_INTERVAL_SEC      = 10


# ── LSP framing ────────────────────────────────────────────────────────

def _send_message(stdin, method: str, params: dict,
                   request_id: int | None = None) -> None:
    """Write a single framed LSP message to the server stdin."""
    body: dict = {"jsonrpc": "2.0", "method": method, "params": params}
    if request_id is not None:
        body["id"] = request_id
    payload = json.dumps(body).encode("utf-8")
    header = f"Content-Length: {len(payload)}\r\n\r\n".encode("ascii")
    stdin.write(header)
    stdin.write(payload)
    stdin.flush()


def _drain_output(stdout, stop: threading.Event) -> None:
    """Read + discard all bytes the server sends until `stop` is set.

    The harness does not parse responses in v1 -- we are measuring
    RSS growth, not response correctness (that is the Phase 2-6
    smoke/parity coverage). Draining prevents the OS pipe from
    filling up and blocking ironls's writer thread.
    """
    try:
        while not stop.is_set():
            chunk = stdout.read(4096)
            if not chunk:
                return
    except Exception:
        return


# ── RSS sampling ───────────────────────────────────────────────────────

def _read_rss_bytes(pid: int) -> int:
    """Return the RSS of `pid` in bytes, or 0 on failure."""
    # Linux: /proc/<pid>/status VmRSS (kB).
    status_path = f"/proc/{pid}/status"
    if os.path.exists(status_path):
        try:
            with open(status_path) as f:
                for line in f:
                    if line.startswith("VmRSS:"):
                        parts = line.split()
                        # parts = ['VmRSS:', '12345', 'kB']
                        return int(parts[1]) * 1024
        except (OSError, ValueError, IndexError):
            return 0
        return 0

    # macOS fallback: ps -o rss= -p <pid>  (kB).
    try:
        out = subprocess.check_output(
            ["ps", "-o", "rss=", "-p", str(pid)],
            text=True, stderr=subprocess.DEVNULL, timeout=5,
        ).strip()
        return int(out) * 1024
    except (subprocess.SubprocessError, ValueError, FileNotFoundError):
        return 0


class RssSampler(threading.Thread):
    """Daemon thread: samples RSS every `interval_sec` into a CSV file."""

    def __init__(self, pid: int, csv_path: pathlib.Path,
                  interval_sec: float = RSS_SAMPLE_INTERVAL_SEC):
        super().__init__(daemon=True)
        self._pid = pid
        self._csv_path = csv_path
        self._interval = interval_sec
        self._stop = threading.Event()
        self._t0 = time.monotonic()
        self._in_flight = 0
        self._lock = threading.Lock()

    def incr_in_flight(self) -> None:
        with self._lock:
            self._in_flight += 1

    def decr_in_flight(self) -> None:
        with self._lock:
            self._in_flight = max(0, self._in_flight - 1)

    def stop(self) -> None:
        self._stop.set()

    def run(self) -> None:
        with self._csv_path.open("w", newline="") as f:
            writer = csv.writer(f)
            writer.writerow(["t_sec", "rss_bytes", "in_flight_requests"])
            while not self._stop.is_set():
                t_sec = time.monotonic() - self._t0
                rss = _read_rss_bytes(self._pid)
                with self._lock:
                    inflight = self._in_flight
                writer.writerow([f"{t_sec:.3f}", rss, inflight])
                f.flush()
                # Event.wait returns True if set, False on timeout.
                if self._stop.wait(self._interval):
                    break


# ── LSP workload replay ────────────────────────────────────────────────

_LSP_METHOD_MAP = {
    "didOpen":    "textDocument/didOpen",
    "didClose":   "textDocument/didClose",
    "didChange":  "textDocument/didChange",
    "hover":      "textDocument/hover",
    "completion": "textDocument/completion",
    "definition": "textDocument/definition",
    "diagnostic": "textDocument/diagnostic",
}


def _build_lsp_params(op: str, params: dict, version_counter: dict) -> dict:
    """Shape workload params into LSP 3.17 message params."""
    uri = params.get("uri", "file:///workspace/main.iron")
    if op == "didOpen":
        return {"textDocument": {
            "uri": uri, "languageId": "iron",
            "version": 1, "text": "// soak open\n",
        }}
    if op == "didClose":
        return {"textDocument": {"uri": uri}}
    if op == "didChange":
        v = version_counter.get(uri, 1) + 1
        version_counter[uri] = v
        return {
            "textDocument": {"uri": uri, "version": v},
            "contentChanges": [{"text": params.get("text", "// edit\n")}],
        }
    if op in ("hover", "completion", "definition"):
        return {
            "textDocument": {"uri": uri},
            "position": params.get("position", {"line": 0, "character": 0}),
        }
    if op == "diagnostic":
        return {"textDocument": {"uri": uri}}
    raise ValueError(f"unknown op {op}")


def replay_workload(proc: subprocess.Popen, events: list[dict],
                     sampler: RssSampler) -> None:
    """Replay events honoring each entry's `t` (ms since start)."""
    version_counter: dict[str, int] = {}
    request_id = 1
    t0 = time.monotonic()
    for ev in events:
        # Honor the event's scheduled time.
        target = ev["t"] / 1000.0
        now = time.monotonic() - t0
        if target > now:
            # Cap the sleep to avoid huge blocking waits in pathological
            # workloads (idle gaps are already <= 60s by construction).
            time.sleep(min(target - now, 60.0))
        op = ev["op"]
        if op == "sleep":
            time.sleep(ev.get("params", {}).get("duration_ms", 0) / 1000.0)
            continue
        params = ev.get("params", {})
        method = _LSP_METHOD_MAP.get(op)
        if method is None:
            continue  # silently skip unknown ops
        lsp_params = _build_lsp_params(op, params, version_counter)
        # didOpen/didClose/didChange are notifications (no id).
        if op in ("didOpen", "didClose", "didChange"):
            _send_message(proc.stdin, method, lsp_params)
        else:
            _send_message(proc.stdin, method, lsp_params, request_id=request_id)
            request_id += 1
            sampler.incr_in_flight()
        # Check ironls is still alive every 1000 events.
        if proc.poll() is not None:
            break


# ── Analysis ───────────────────────────────────────────────────────────

def linear_regression_slope(xs: list[float], ys: list[float]) -> float:
    """Ordinary-least-squares slope (no numpy). Returns bytes/sec when
    xs is seconds and ys is bytes. Returns 0.0 on degenerate input."""
    n = len(xs)
    if n < 2:
        return 0.0
    x_mean = sum(xs) / n
    y_mean = sum(ys) / n
    num = 0.0
    den = 0.0
    for i in range(n):
        dx = xs[i] - x_mean
        num += dx * (ys[i] - y_mean)
        den += dx * dx
    return (num / den) if den != 0.0 else 0.0


def analyze(csv_path: pathlib.Path, peak_threshold_mib: int,
             growth_threshold_mib_per_hr: int,
             warmup_exclusion_sec: int = WARMUP_EXCLUSION_SEC) -> tuple[int, str]:
    """Return (exit_code, message). 0 = pass, 1 = threshold breach."""
    rows: list[tuple[float, int, int]] = []
    with csv_path.open() as f:
        reader = csv.reader(f)
        header = next(reader, None)
        if not header:
            return 1, f"analyze: {csv_path} is empty"
        for row in reader:
            try:
                rows.append((float(row[0]), int(row[1]), int(row[2])))
            except (ValueError, IndexError):
                continue
    if not rows:
        return 1, f"analyze: {csv_path} has no data rows"

    peak = max(r[1] for r in rows)
    peak_threshold = peak_threshold_mib * 1024 * 1024
    growth_threshold = growth_threshold_mib_per_hr * 1024 * 1024

    # Warm-up exclusion. If the run is shorter than warmup, we skip the
    # growth check entirely and only enforce the peak threshold (the
    # plan explicitly documents this for --self-test and short-soak at
    # 30min < 1hr warmup; short-soak thus only enforces peak).
    post_warmup = [r for r in rows if r[0] >= warmup_exclusion_sec]
    growth_per_hr = 0.0
    if len(post_warmup) >= 2:
        xs = [r[0] for r in post_warmup]
        ys = [float(r[1]) for r in post_warmup]
        slope_bps = linear_regression_slope(xs, ys)
        growth_per_hr = slope_bps * 3600.0

    messages = []
    if peak > peak_threshold:
        return 1, (f"peak RSS {peak} > {peak_threshold} threshold "
                   f"({peak_threshold_mib} MiB)")
    if growth_per_hr > growth_threshold:
        return 1, (f"growth {growth_per_hr:.0f} bytes/hr > "
                   f"{growth_threshold} bytes/hr threshold "
                   f"({growth_threshold_mib_per_hr} MiB/hr)")
    messages.append(f"peak={peak} bytes ({peak / (1024*1024):.1f} MiB)")
    messages.append(f"growth={growth_per_hr:.0f} bytes/hr "
                    f"({growth_per_hr / (1024*1024):.3f} MiB/hr)")
    return 0, "OK: " + "; ".join(messages)


# ── Self-test ──────────────────────────────────────────────────────────

def _self_test() -> int:
    """Validate the analyze() logic with synthetic RSS curves."""
    import tempfile

    print("self-test: linear_regression_slope()", file=sys.stderr)

    # Synthetic: 8h of 10s samples with exactly 5 MiB/hr growth.
    # 5 MiB/hr = 5242880 / 3600 = 1456 bytes/sec.
    with tempfile.TemporaryDirectory() as td:
        csv_path = pathlib.Path(td) / "synthetic.csv"
        with csv_path.open("w", newline="") as f:
            w = csv.writer(f)
            w.writerow(["t_sec", "rss_bytes", "in_flight_requests"])
            for i in range(8 * 60 * 6 + 1):   # 8 hours @ 10s cadence
                t = i * 10.0
                rss = int(100 * 1024 * 1024 + 1456 * t)   # 100 MiB + growth
                w.writerow([f"{t:.3f}", rss, 0])
        code, msg = analyze(csv_path, peak_threshold_mib=800,
                             growth_threshold_mib_per_hr=5)
        # At exactly 5 MiB/hr, growth is at threshold -- our threshold
        # check is strictly >, so this should PASS.
        assert code == 0, f"expected pass, got {code}: {msg}"
        print(f"  at threshold: {msg}", file=sys.stderr)

    # Synthetic 2: growth of 7 MiB/hr (should FAIL).
    with tempfile.TemporaryDirectory() as td:
        csv_path = pathlib.Path(td) / "synthetic2.csv"
        with csv_path.open("w", newline="") as f:
            w = csv.writer(f)
            w.writerow(["t_sec", "rss_bytes", "in_flight_requests"])
            slope_bps = (7 * 1024 * 1024) / 3600.0
            for i in range(8 * 60 * 6 + 1):
                t = i * 10.0
                rss = int(100 * 1024 * 1024 + slope_bps * t)
                w.writerow([f"{t:.3f}", rss, 0])
        code, msg = analyze(csv_path, peak_threshold_mib=800,
                             growth_threshold_mib_per_hr=5)
        assert code == 1, f"expected fail, got {code}: {msg}"
        assert "growth" in msg, msg
        print(f"  above growth threshold: {msg}", file=sys.stderr)

    # Synthetic 3: flat 850 MiB peak (should FAIL peak threshold).
    with tempfile.TemporaryDirectory() as td:
        csv_path = pathlib.Path(td) / "synthetic3.csv"
        with csv_path.open("w", newline="") as f:
            w = csv.writer(f)
            w.writerow(["t_sec", "rss_bytes", "in_flight_requests"])
            for i in range(10):
                t = i * 10.0
                w.writerow([f"{t:.3f}", 850 * 1024 * 1024, 0])
        code, msg = analyze(csv_path, peak_threshold_mib=800,
                             growth_threshold_mib_per_hr=5)
        assert code == 1, f"expected fail, got {code}: {msg}"
        assert msg.startswith("peak RSS"), msg
        # Numeric exactness matches D-04: 850 MiB > 800 MiB.
        assert "891289600" in msg, msg
        print(f"  above peak threshold: {msg}", file=sys.stderr)

    # Synthetic 4: well under both thresholds (should PASS).
    with tempfile.TemporaryDirectory() as td:
        csv_path = pathlib.Path(td) / "synthetic4.csv"
        with csv_path.open("w", newline="") as f:
            w = csv.writer(f)
            w.writerow(["t_sec", "rss_bytes", "in_flight_requests"])
            for i in range(8 * 60 * 6 + 1):
                t = i * 10.0
                rss = int(200 * 1024 * 1024 + 500 * t)  # ~1.7 MiB/hr growth
                w.writerow([f"{t:.3f}", rss, 0])
        code, msg = analyze(csv_path, peak_threshold_mib=800,
                             growth_threshold_mib_per_hr=5)
        assert code == 0, f"expected pass, got {code}: {msg}"
        print(f"  green run: {msg}", file=sys.stderr)

    print("self-test: PASS", file=sys.stderr)
    return 0


# ── Main ───────────────────────────────────────────────────────────────

def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(description=__doc__,
                                  formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--ironls", default="ironls",
                   help="Path to the ironls binary.")
    p.add_argument("--workload", type=pathlib.Path,
                   help="Path to the JSONL workload file.")
    p.add_argument("--fixtures-root", type=pathlib.Path,
                   help="Workspace root fed to initialize.")
    p.add_argument("--output-csv", type=pathlib.Path,
                   default=pathlib.Path("result.csv"))
    p.add_argument("--threshold-peak-mib", type=int,
                   default=DEFAULT_PEAK_MIB)
    p.add_argument("--threshold-growth-mib-per-hr", type=int,
                   default=DEFAULT_GROWTH_MIB_PER_HR)
    p.add_argument("--warmup-exclusion-sec", type=int,
                   default=WARMUP_EXCLUSION_SEC)
    p.add_argument("--self-test", action="store_true",
                   help="Run inline unit tests against analyze() and exit.")
    args = p.parse_args(argv)

    if args.self_test:
        return _self_test()

    if not args.workload or not args.workload.exists():
        print(f"harness: --workload file missing: {args.workload}",
              file=sys.stderr)
        return 2

    ironls = pathlib.Path(args.ironls)
    if not ironls.exists() or not os.access(ironls, os.X_OK):
        print(f"harness: ironls not executable: {ironls}", file=sys.stderr)
        return 2

    # Load the workload.
    events: list[dict] = []
    with args.workload.open() as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            events.append(json.loads(line))

    # Spawn ironls.
    env = os.environ.copy()
    # Disable the RSS cap so the soak runs to completion even if the
    # server grows over cap briefly -- the analyze step asserts on its
    # own thresholds (800 MiB / 5 MiB/hr), not on the runtime cap.
    env.setdefault("IRON_LSP_RSS_CAP_BYTES", "0")
    proc = subprocess.Popen(
        [str(ironls)],
        stdin=subprocess.PIPE, stdout=subprocess.PIPE,
        stderr=subprocess.PIPE, env=env, bufsize=0,
    )

    sampler = RssSampler(proc.pid, args.output_csv)
    sampler.start()

    # stdout/stderr drain threads.
    stop_drain = threading.Event()
    t_out = threading.Thread(target=_drain_output,
                              args=(proc.stdout, stop_drain), daemon=True)
    t_err = threading.Thread(target=_drain_output,
                              args=(proc.stderr, stop_drain), daemon=True)
    t_out.start()
    t_err.start()

    root_uri = (f"file://{args.fixtures_root.resolve()}"
                if args.fixtures_root else None)
    # Initialize.
    _send_message(proc.stdin, "initialize",
                   {"processId": os.getpid(),
                    "capabilities": {},
                    "rootUri": root_uri,
                    "workspaceFolders": ([{"uri": root_uri, "name": "soak"}]
                                         if root_uri else None)},
                   request_id=0)
    # initialized notification.
    _send_message(proc.stdin, "initialized", {})

    # Give the server a moment to respond to initialize before
    # starting the replay.
    time.sleep(1.0)

    exit_code = 0
    try:
        replay_workload(proc, events, sampler)
    except BrokenPipeError:
        print("harness: ironls closed stdin mid-replay", file=sys.stderr)
        exit_code = 1

    # shutdown + exit.
    try:
        _send_message(proc.stdin, "shutdown", {}, request_id=999999)
        _send_message(proc.stdin, "exit", {})
        try:
            proc.stdin.close()
        except Exception:
            pass
    except BrokenPipeError:
        pass

    # Let the server exit gracefully; SIGTERM after 10s if it doesn't.
    try:
        proc.wait(timeout=10)
    except subprocess.TimeoutExpired:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()

    sampler.stop()
    sampler.join(timeout=5)
    stop_drain.set()

    # ironls exit code 42 is the RSS-cap-restart sentinel; non-zero here
    # indicates the server hit its own cap during a cap-disabled run
    # (should not happen since we pass IRON_LSP_RSS_CAP_BYTES=0) OR
    # crashed. Either way it's a failure for the soak.
    if proc.returncode not in (0, None):
        print(f"harness: ironls exited with code {proc.returncode}",
              file=sys.stderr)
        exit_code = 1

    # Analyze.
    ac, amsg = analyze(args.output_csv,
                        peak_threshold_mib=args.threshold_peak_mib,
                        growth_threshold_mib_per_hr=args.threshold_growth_mib_per_hr,
                        warmup_exclusion_sec=args.warmup_exclusion_sec)
    print(amsg, file=sys.stderr)
    return exit_code or ac


if __name__ == "__main__":
    sys.exit(main())
