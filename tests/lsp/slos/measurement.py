#!/usr/bin/env python3
"""Phase 7 Plan 07-04 Task 01 (HARD-18, D-06) -- Iron LSP SLO measurement.

Python stdlib-only (no numpy, no pandas, no pytest-lsp). Spawns an
`ironls` Release-mode subprocess, runs an LSP initialize + didOpen
handshake on each canonical fixture (small / medium / large), warms
the workspace (stdlib pre-parse + one throwaway request), then replays
100 samples per LSP request method and measures start-to-response-flush
wall-clock via ``time.monotonic_ns()``.

For each (fixture x method) pair we compute p50 / p95 / p99 from the
100 samples using pure-Python linear-interpolation percentiles and
write the full per-sample detail plus the summary to
``slo-result.json``. p50 thresholds (per D-06) are enforced ONLY on
the medium (500 LoC) fixture:

    textDocument/hover       < 20 ms
    textDocument/completion  < 100 ms
    textDocument/diagnostic  < 500 ms
    textDocument/definition  < 50 ms

p95 and p99 are recorded to the artifact but NOT enforced (tail-
latency observability per D-06: GitHub Actions runner CPU variance
makes p95/p99 flaky; p50 with ~20% headroom is the stable gate).
Small + large fixtures are measured and recorded but their p50 is
informational only.

Exit codes:
    0 -- all medium-fixture p50 thresholds satisfied
    1 -- at least one medium-fixture p50 threshold exceeded
    2 -- driver error (bad args, ironls not executable, protocol
         failure before measurement could complete)

Usage:
    # Normal per-PR run on CI:
    python3 tests/lsp/slos/measurement.py \\
        --ironls ./build/ironls \\
        --fixtures-root tests/lsp/slos/fixtures \\
        --samples 100 \\
        --output-json slo-result.json

    # Dry-run plumbing check (no ironls spawn, fixtures validated):
    python3 tests/lsp/slos/measurement.py --dry-run

    # Hermetic unit self-tests (percentile + threshold + framing):
    python3 tests/lsp/slos/measurement.py --self-test
"""
from __future__ import annotations

import argparse
import io
import json
import math
import os
import pathlib
import signal
import subprocess
import sys
import threading
import time
from typing import Dict, List, Optional, Tuple


# ── D-06 thresholds ────────────────────────────────────────────────────

# p50 (medium fixture) enforced values. Edit discipline: keep in sync
# with .planning/phases/07-m6-production-hardening/07-CONTEXT.md §D-06
# AND docs/dev/slo-runbook.md. The grep invariant in slos.yml / the
# plan done-criterion also depends on these literals.
THRESHOLDS_MEDIUM_MS: Dict[str, int] = {
    "textDocument/hover":      20,   # D-06 hover
    "textDocument/completion": 100,  # D-06 completion
    "textDocument/diagnostic": 500,  # D-06 diagnostic (full file)
    "textDocument/definition": 50,   # D-06 self-imposed (navigation)
}

# Fixtures ordered small -> large. Enforcement applies only to
# ``FIXTURE_ENFORCED``; the others are informational (recorded to
# the artifact but never fail the job).
FIXTURES: Tuple[str, ...] = ("small_50loc", "medium_500loc", "large_5000loc")
FIXTURE_ENFORCED = "medium_500loc"

# Per-request deadline (T-07-04-02 mitigation). If ironls does not
# respond within this budget we mark the sample as 10s and continue.
# Bounds a hang so the outer CI timeout-minutes: 20 is not the only
# backstop.
PER_REQUEST_DEADLINE_SEC = 10.0


# ── LSP framing (stdlib-only) ──────────────────────────────────────────

def _encode_frame(body: dict) -> bytes:
    payload = json.dumps(body).encode("utf-8")
    header = f"Content-Length: {len(payload)}\r\n\r\n".encode("ascii")
    return header + payload


def _send_notification(stdin, method: str, params: dict) -> None:
    stdin.write(_encode_frame({"jsonrpc": "2.0", "method": method,
                                "params": params}))
    stdin.flush()


def _send_request(stdin, request_id: int, method: str,
                  params: dict) -> None:
    stdin.write(_encode_frame({"jsonrpc": "2.0", "id": request_id,
                                "method": method, "params": params}))
    stdin.flush()


def _read_one_frame(stdout) -> Optional[dict]:
    """Read exactly one LSP frame from `stdout`. Return the decoded
    JSON object or None on EOF / malformed headers.

    Blocking read; callers time out via an outer threading.Event."""
    header = b""
    while True:
        byte = stdout.read(1)
        if not byte:
            return None
        header += byte
        if header.endswith(b"\r\n\r\n"):
            break
        if len(header) > 8192:
            return None  # runaway header
    content_length = None
    for line in header.split(b"\r\n"):
        if line.lower().startswith(b"content-length:"):
            try:
                content_length = int(line.split(b":", 1)[1].strip())
            except (ValueError, IndexError):
                return None
    if content_length is None or content_length < 0:
        return None
    body = b""
    while len(body) < content_length:
        chunk = stdout.read(content_length - len(body))
        if not chunk:
            return None
        body += chunk
    try:
        return json.loads(body.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError):
        return None


# ── Percentile + threshold (pure Python) ───────────────────────────────

def percentile(values: List[float], p: float) -> float:
    """Linear-interpolation percentile. Matches NumPy's default
    ``method='linear'`` behaviour for a single list of samples."""
    if not values:
        return 0.0
    sorted_vals = sorted(values)
    if len(sorted_vals) == 1:
        return sorted_vals[0]
    k = (len(sorted_vals) - 1) * (p / 100.0)
    lo = int(math.floor(k))
    hi = int(math.ceil(k))
    if lo == hi:
        return sorted_vals[lo]
    return sorted_vals[lo] + (k - lo) * (sorted_vals[hi] - sorted_vals[lo])


def enforce_threshold(samples_ms: List[float], threshold_ms: float) -> bool:
    """True iff p50 of `samples_ms` is <= threshold_ms. Boundary-
    inclusive (exactly-at-threshold is a pass) so the test is
    reproducible when runners are right at the edge."""
    return percentile(samples_ms, 50) <= threshold_ms


# ── Self-test (hermetic; no ironls spawn) ──────────────────────────────

def _self_tests() -> int:
    # Test 1 -- percentile on [1..100] yields p50=50, p95=95, p99=99.
    sample = list(range(1, 101))
    p50 = percentile(sample, 50)
    p95 = percentile(sample, 95)
    p99 = percentile(sample, 99)
    assert abs(p50 - 50.5) < 1e-9, f"p50={p50}"
    assert abs(p95 - 95.05) < 1e-9, f"p95={p95}"
    assert abs(p99 - 99.01) < 1e-9, f"p99={p99}"
    print(f"[self-test 1] percentile [1..100]: "
          f"p50={p50:.3f} p95={p95:.3f} p99={p99:.3f} -- OK", file=sys.stderr)

    # Test 2 -- enforce_threshold at the boundary (p50 == threshold).
    #   [15,18,22,25] sorted -> p50 = (18+22)/2 = 20.0, threshold=20
    assert enforce_threshold([15, 18, 22, 25], 20) is True
    print("[self-test 2] enforce_threshold at boundary (p50==20) -- OK",
          file=sys.stderr)

    # Test 3 -- enforce_threshold above the boundary (should fail).
    #   [22,25,30,35] sorted -> p50 = (25+30)/2 = 27.5, threshold=20
    assert enforce_threshold([22, 25, 30, 35], 20) is False
    print("[self-test 3] enforce_threshold above threshold -- OK",
          file=sys.stderr)

    # Test 4 -- synthetic JSON-RPC frame encode/decode round-trip via
    # in-memory pipe. Asserts the framing decode path handles a well-
    # formed Content-Length header and arbitrary JSON body.
    body = {"jsonrpc": "2.0", "id": 42, "result": {"hello": "world"}}
    frame = _encode_frame(body)
    buf = io.BytesIO(frame)
    decoded = _read_one_frame(buf)
    assert decoded == body, f"round-trip mismatch: {decoded!r} != {body!r}"
    print("[self-test 4] framing encode/decode round-trip -- OK",
          file=sys.stderr)

    # Test 5 -- medium_500loc.iron is 500 ± 5 LoC. Guards the plan done
    # criterion at unit-test time so drift is caught fast.
    here = pathlib.Path(__file__).resolve().parent
    medium = here / "fixtures" / "medium_500loc.iron"
    assert medium.is_file(), f"missing fixture: {medium}"
    line_count = sum(1 for _ in medium.open("rb"))
    assert 495 <= line_count <= 505, (
        f"medium_500loc.iron LoC {line_count} outside 495..505 window")
    print(f"[self-test 5] medium_500loc.iron line count = {line_count} "
          "(in 495..505) -- OK", file=sys.stderr)

    # Test 6 -- literal thresholds match D-06. Guards accidental edits
    # to THRESHOLDS_MEDIUM_MS.
    assert THRESHOLDS_MEDIUM_MS["textDocument/hover"] == 20
    assert THRESHOLDS_MEDIUM_MS["textDocument/completion"] == 100
    assert THRESHOLDS_MEDIUM_MS["textDocument/diagnostic"] == 500
    assert THRESHOLDS_MEDIUM_MS["textDocument/definition"] == 50
    print("[self-test 6] D-06 thresholds 20/100/500/50 locked -- OK",
          file=sys.stderr)

    print("[self-test] PASS (6 cases)", file=sys.stderr)
    return 0


# ── Dry-run (plumbing check without ironls spawn) ──────────────────────

def _dry_run(fixtures_root: pathlib.Path) -> int:
    missing: List[pathlib.Path] = []
    for f in FIXTURES:
        p = fixtures_root / (f + ".iron")
        if not p.is_file():
            missing.append(p)
        else:
            loc = sum(1 for _ in p.open("rb"))
            print(f"[dry-run] {p.name}: {loc} LoC", file=sys.stderr)
    toml = fixtures_root / "iron.toml"
    if not toml.is_file():
        missing.append(toml)
    else:
        print(f"[dry-run] {toml.name}: present", file=sys.stderr)
    if missing:
        for m in missing:
            print(f"[dry-run] MISSING: {m}", file=sys.stderr)
        return 2
    print("[dry-run] fixture set complete; plumbing OK", file=sys.stderr)
    return 0


# ── Measurement harness ────────────────────────────────────────────────

class _FrameReader(threading.Thread):
    """Daemon reader: pulls LSP frames off `stdout`, dispatches them
    to either the response map (by id) or a best-effort discard for
    notifications. Other threads wait on per-request threading.Events
    to observe response arrival.
    """

    def __init__(self, stdout, responses: Dict[int, dict],
                 events: Dict[int, threading.Event],
                 stop: threading.Event):
        super().__init__(daemon=True, name="slo-frame-reader")
        self._stdout = stdout
        self._responses = responses
        self._events = events
        self._stop = stop

    def run(self) -> None:
        while not self._stop.is_set():
            frame = _read_one_frame(self._stdout)
            if frame is None:
                # Server closed stdout (EOF) or a malformed header --
                # either way, wake any waiters and bail.
                for ev in list(self._events.values()):
                    ev.set()
                return
            rid = frame.get("id")
            if rid is None:
                # Notification; ignored for timing purposes.
                continue
            try:
                rid_int = int(rid)
            except (TypeError, ValueError):
                continue
            self._responses[rid_int] = frame
            ev = self._events.get(rid_int)
            if ev is not None:
                ev.set()


class _Session:
    """Wrap one ironls subprocess + its reader thread + an id
    counter. Not thread-safe across calls -- the harness is a single-
    threaded request/response loop (100 samples is sequential)."""

    def __init__(self, ironls: pathlib.Path, workspace_root: pathlib.Path):
        env = os.environ.copy()
        env["IRON_LSP_RSS_CAP_BYTES"] = "0"  # soak cap out of scope here
        self._proc = subprocess.Popen(
            [str(ironls)],
            cwd=str(workspace_root),
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            env=env,
            bufsize=0,
        )
        self._responses: Dict[int, dict] = {}
        self._events: Dict[int, threading.Event] = {}
        self._stop = threading.Event()
        self._reader = _FrameReader(self._proc.stdout, self._responses,
                                     self._events, self._stop)
        self._reader.start()
        # Drain stderr to avoid the OS pipe backing up.
        self._stderr_stop = threading.Event()
        self._stderr_thread = threading.Thread(
            target=self._drain_stderr, daemon=True,
            name="slo-stderr-drain")
        self._stderr_thread.start()
        self._next_id = 1

    def _drain_stderr(self) -> None:
        try:
            while not self._stderr_stop.is_set():
                chunk = self._proc.stderr.read(4096)
                if not chunk:
                    return
        except Exception:
            return

    def next_id(self) -> int:
        rid = self._next_id
        self._next_id += 1
        return rid

    def request(self, method: str, params: dict,
                deadline_sec: float = PER_REQUEST_DEADLINE_SEC
                ) -> Tuple[float, Optional[dict]]:
        """Send a request and block (up to deadline) for its response.
        Returns (elapsed_ms, response_or_None). elapsed_ms is measured
        start-of-send to completion-of-recv via time.monotonic_ns()."""
        rid = self.next_id()
        ev = threading.Event()
        self._events[rid] = ev
        t0 = time.monotonic_ns()
        _send_request(self._proc.stdin, rid, method, params)
        if not ev.wait(timeout=deadline_sec):
            elapsed_ns = time.monotonic_ns() - t0
            return elapsed_ns / 1_000_000, None
        elapsed_ns = time.monotonic_ns() - t0
        resp = self._responses.pop(rid, None)
        self._events.pop(rid, None)
        return elapsed_ns / 1_000_000, resp

    def notify(self, method: str, params: dict) -> None:
        _send_notification(self._proc.stdin, method, params)

    def shutdown(self) -> None:
        try:
            _send_request(self._proc.stdin, self.next_id(), "shutdown", {})
            _send_notification(self._proc.stdin, "exit", {})
            self._proc.stdin.close()
        except (BrokenPipeError, ValueError):
            pass
        try:
            self._proc.wait(timeout=5.0)
        except subprocess.TimeoutExpired:
            self._proc.terminate()
            try:
                self._proc.wait(timeout=3.0)
            except subprocess.TimeoutExpired:
                self._proc.kill()
        self._stop.set()
        self._stderr_stop.set()


def _measure_fixture(ironls: pathlib.Path, fixtures_root: pathlib.Path,
                     fixture_stem: str, samples: int) -> Dict[str, dict]:
    """Run one ironls session against one fixture. Return
    {method: {p50, p95, p99, samples}}."""
    fixture_path = fixtures_root / (fixture_stem + ".iron")
    fixture_uri = fixture_path.as_uri()
    fixture_text = fixture_path.read_text(encoding="utf-8")
    workspace_uri = fixtures_root.as_uri()

    session = _Session(ironls, fixtures_root)
    try:
        # initialize (request 1).
        elapsed, resp = session.request("initialize", {
            "processId": os.getpid(),
            "rootUri": workspace_uri,
            "capabilities": {
                "workspace": {"workspaceFolders": True, "symbol": {}},
                "textDocument": {
                    "hover": {},
                    "completion": {},
                    "definition": {},
                    "synchronization": {"didSave": True},
                    "diagnostic": {"dynamicRegistration": False},
                },
            },
            "workspaceFolders": [
                {"uri": workspace_uri, "name": "slo"},
            ],
        })
        if resp is None:
            print(f"[measurement] {fixture_stem}: initialize timeout",
                  file=sys.stderr)
            raise RuntimeError("initialize-timeout")
        session.notify("initialized", {})

        # didOpen the fixture.
        session.notify("textDocument/didOpen", {
            "textDocument": {
                "uri": fixture_uri,
                "languageId": "iron",
                "version": 1,
                "text": fixture_text,
            }
        })

        # Warm the workspace: one throwaway diagnostic pull so the
        # stdlib is pre-parsed before we start measuring. Also gives
        # the analyzer a chance to build its scope tree once so the
        # per-method samples measure hot-path latency, not cold-start.
        warm_elapsed, warm_resp = session.request(
            "textDocument/diagnostic",
            {"textDocument": {"uri": fixture_uri}})
        print(f"[measurement] {fixture_stem}: warm-up diagnostic = "
              f"{warm_elapsed:.1f} ms (resp={'ok' if warm_resp else 'TIMEOUT'})",
              file=sys.stderr)

        # Cursor positions used for position-sensitive requests.
        # Picked deterministically inside the fixture so hover /
        # completion / definition have something resolvable to do.
        # For all three fixtures, position (line=20, col=5) is always
        # inside a function body (small has 40+ lines; medium + large
        # have 500+).
        anchor_position = {"line": 20, "character": 5}

        results: Dict[str, dict] = {}
        for method in THRESHOLDS_MEDIUM_MS.keys():
            samples_ms: List[float] = []
            timeouts = 0
            for i in range(samples):
                if method == "textDocument/diagnostic":
                    params = {"textDocument": {"uri": fixture_uri}}
                else:
                    params = {
                        "textDocument": {"uri": fixture_uri},
                        "position": anchor_position,
                    }
                    if method == "textDocument/completion":
                        params["context"] = {"triggerKind": 1}
                elapsed_ms, resp = session.request(method, params)
                if resp is None:
                    timeouts += 1
                    elapsed_ms = PER_REQUEST_DEADLINE_SEC * 1000.0
                samples_ms.append(elapsed_ms)
            p50 = percentile(samples_ms, 50)
            p95 = percentile(samples_ms, 95)
            p99 = percentile(samples_ms, 99)
            results[method] = {
                "p50": p50, "p95": p95, "p99": p99,
                "samples": samples_ms,
                "timeouts": timeouts,
            }
            print(f"[measurement] {fixture_stem} {method}: "
                  f"p50={p50:.1f}ms p95={p95:.1f}ms p99={p99:.1f}ms "
                  f"timeouts={timeouts}/{samples}",
                  file=sys.stderr)
        return results
    finally:
        session.shutdown()


def _enforce_medium(results: Dict[str, Dict[str, dict]]) -> int:
    """Print per-method PASS/FAIL for the medium fixture; return 0
    iff every enforced p50 is under threshold."""
    failed = 0
    medium = results.get(FIXTURE_ENFORCED, {})
    for method, threshold_ms in THRESHOLDS_MEDIUM_MS.items():
        detail = medium.get(method)
        if detail is None:
            print(f"FAIL {FIXTURE_ENFORCED} {method}: no data", file=sys.stderr)
            failed += 1
            continue
        p50 = detail["p50"]
        status = "PASS" if p50 <= threshold_ms else "FAIL"
        print(f"{status} {FIXTURE_ENFORCED} {method}: "
              f"p50={p50:.1f}ms threshold={threshold_ms}ms",
              file=sys.stderr)
        if p50 > threshold_ms:
            failed += 1
    # Record-only for the other fixtures (informational).
    for fixture in FIXTURES:
        if fixture == FIXTURE_ENFORCED:
            continue
        for method in THRESHOLDS_MEDIUM_MS.keys():
            detail = results.get(fixture, {}).get(method)
            if detail is None:
                continue
            print(f"RECORD {fixture} {method}: "
                  f"p50={detail['p50']:.1f}ms p95={detail['p95']:.1f}ms "
                  f"p99={detail['p99']:.1f}ms (informational, not enforced)",
                  file=sys.stderr)
    return 1 if failed else 0


# ── Main ───────────────────────────────────────────────────────────────

def main(argv: Optional[List[str]] = None) -> int:
    p = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    p.add_argument("--ironls", default="ironls",
                   help="Path to the ironls binary.")
    p.add_argument("--fixtures-root", type=pathlib.Path,
                   default=pathlib.Path(__file__).resolve().parent / "fixtures",
                   help="Workspace root containing small/medium/large fixtures.")
    p.add_argument("--samples", type=int, default=100,
                   help="Samples per method per fixture. D-06 locks 100.")
    p.add_argument("--output-json", type=pathlib.Path,
                   default=pathlib.Path("slo-result.json"))
    p.add_argument("--self-test", action="store_true",
                   help="Run hermetic unit tests and exit.")
    p.add_argument("--dry-run", action="store_true",
                   help="Validate fixtures + plumbing; do not spawn ironls.")
    args = p.parse_args(argv)

    if args.self_test:
        return _self_tests()

    fixtures_root = args.fixtures_root.resolve()
    if args.dry_run:
        return _dry_run(fixtures_root)

    ironls = pathlib.Path(args.ironls).resolve()
    if not ironls.is_file() or not os.access(ironls, os.X_OK):
        print(f"[measurement] ironls not executable: {ironls}",
              file=sys.stderr)
        return 2

    missing_fixtures = [
        fixtures_root / (name + ".iron")
        for name in FIXTURES
        if not (fixtures_root / (name + ".iron")).is_file()
    ]
    if missing_fixtures:
        for m in missing_fixtures:
            print(f"[measurement] fixture missing: {m}", file=sys.stderr)
        return 2

    results: Dict[str, Dict[str, dict]] = {}
    for fixture in FIXTURES:
        try:
            results[fixture] = _measure_fixture(
                ironls, fixtures_root, fixture, args.samples)
        except Exception as exc:
            print(f"[measurement] {fixture}: harness error {exc!r}",
                  file=sys.stderr)
            return 2

    # Write the JSON artifact before enforcing so CI always has a
    # measurement to upload, even when the job fails on threshold.
    artifact = {
        "schema_version": 1,
        "thresholds_medium_ms": THRESHOLDS_MEDIUM_MS,
        "fixture_enforced": FIXTURE_ENFORCED,
        "samples_per_method": args.samples,
        "results": results,
    }
    args.output_json.write_text(
        json.dumps(artifact, indent=2, sort_keys=True) + "\n",
        encoding="utf-8")
    print(f"[measurement] artifact written: {args.output_json}",
          file=sys.stderr)

    return _enforce_medium(results)


if __name__ == "__main__":
    sys.exit(main())
