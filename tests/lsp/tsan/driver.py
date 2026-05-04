#!/usr/bin/env python3
"""Phase 7 Plan 07-03 Task 01 (HARD-17, D-05) -- Iron LSP TSAN driver.

Python stdlib-only (no pytest-lsp, no yaml, no pandas). Spawns an
`ironls` subprocess built with -fsanitize=thread, runs an LSP
initialize handshake, opens doc_a.iron + doc_b.iron, then fires a
300-event JSONL workload across TWO parallel worker threads so the
writer queue (CORE-04), coalescing mailbox (Plan 02-05), cancel
registry (CORE-14), workspace index (NAV-01), and facade (CORE-22)
are all exercised under concurrency.

Success criterion (HARD-17):
    Zero `WARNING: ThreadSanitizer:` lines in the server stderr.

The driver exits:
    0 - clean run, no TSAN warnings
    1 - at least one `WARNING: ThreadSanitizer:` line detected
    2 - driver error (bad args, missing ironls binary, darwin, etc.)

Usage:
    # Normal run (Linux only, per D-05 v1 scope):
    python3 tests/lsp/tsan/driver.py \\
        --ironls build-tsan/src/lsp/ironls \\
        --workload tests/lsp/tsan/workload.jsonl \\
        --workers 2 \\
        --timeout-sec 300

    # Hermetic self-test of the detection logic (no ironls spawn):
    python3 tests/lsp/tsan/driver.py --self-test

macOS is explicitly refused at v1: Apple clang's TSAN is historically
flakier than upstream clang's; macOS TSAN CI is deferred to v2 per
.planning/phases/07-m6-production-hardening/07-CONTEXT.md §D-05.

This driver follows the same stdlib-only stdio conventions as
tests/lsp/soak/harness.py so both harnesses can be debugged with the
same mental model.
"""
from __future__ import annotations

import argparse
import json
import os
import pathlib
import re
import signal
import subprocess
import sys
import threading
import time
from typing import List, Optional


# ── Detection ─────────────────────────────────────────────────────────

# Canonical ThreadSanitizer warning prefix per clang docs. Matches the
# exact line the runtime prints when a race is detected; we use a
# leading-anchor regex so incidental mentions of the string in log
# payloads (e.g. an editor message quoting the docs) don't false-fail.
TSAN_WARNING_RE = re.compile(
    r"^WARNING: ThreadSanitizer:",
    flags=re.MULTILINE,
)


def detect_tsan_finding(stderr_text: str) -> bool:
    """True iff `stderr_text` contains at least one TSAN warning line."""
    return bool(TSAN_WARNING_RE.search(stderr_text))


# ── LSP framing (stdlib-only) ─────────────────────────────────────────

def _send_message(
    stdin,
    method: str,
    params: dict,
    request_id: Optional[int] = None,
) -> None:
    """Write a single framed LSP message to the server stdin."""
    body = {"jsonrpc": "2.0", "method": method, "params": params}
    if request_id is not None:
        body["id"] = request_id
    payload = json.dumps(body).encode("utf-8")
    header = f"Content-Length: {len(payload)}\r\n\r\n".encode("ascii")
    stdin.write(header)
    stdin.write(payload)
    stdin.flush()


def _drain_stdout(stdout, stop: threading.Event) -> None:
    """Read + discard server responses so the OS pipe never backs up.
    The driver does not parse responses; TSAN findings arrive on
    stderr, which is captured separately.
    """
    try:
        while not stop.is_set():
            chunk = stdout.read(4096)
            if not chunk:
                return
    except Exception:
        return


def _capture_stderr(stderr, buf: List[bytes], stop: threading.Event) -> None:
    """Capture every stderr byte into `buf`. We scan for TSAN findings
    after the soak completes. stderr is also mirrored to the driver's
    own stderr for live debugging (GitHub Actions keeps the log tail)."""
    try:
        while not stop.is_set():
            chunk = stderr.read(4096)
            if not chunk:
                return
            buf.append(chunk)
            # Mirror to driver's stderr so CI logs are readable live.
            try:
                sys.stderr.buffer.write(chunk)
                sys.stderr.buffer.flush()
            except Exception:
                pass
    except Exception:
        return


# ── Workload replay ───────────────────────────────────────────────────

def _load_workload(path: pathlib.Path) -> List[dict]:
    events: List[dict] = []
    with path.open("r", encoding="utf-8") as fh:
        for line_no, line in enumerate(fh, start=1):
            line = line.strip()
            if not line:
                continue
            try:
                events.append(json.loads(line))
            except json.JSONDecodeError as exc:
                raise SystemExit(
                    f"workload {path}:{line_no} is not valid JSON: {exc}")
    return events


def _replay_slice(
    server_stdin,
    server_stdin_lock: threading.Lock,
    events: List[dict],
    slice_start: int,
    slice_end: int,
    worker_id: int,
) -> None:
    """Replay events[slice_start:slice_end] over the shared stdin.

    The stdin write is serialised by `server_stdin_lock` so frames
    are never interleaved at the byte level (the LSP framing protocol
    assumes atomic frame writes). This does NOT serialise the server-
    side work: once both workers push their frames, the server's
    reader thread drains them concurrently from its side, which is
    precisely the contention pattern we want TSAN to stress.
    """
    for i in range(slice_start, slice_end):
        ev = events[i]
        method = ev.get("method", "")
        params = ev.get("params", {})
        request_id = ev.get("id")
        with server_stdin_lock:
            try:
                _send_message(server_stdin, method, params,
                              request_id=request_id)
            except (BrokenPipeError, ValueError):
                # Server died; stop this worker. Main thread will
                # detect the exit and surface it in the summary.
                return
        # A tiny jitter so the two workers interleave instead of
        # bursting a full slice at once. 1 ms is enough to let the
        # server's reader thread pick up the pending frame.
        time.sleep(0.001)


# ── Process lifecycle ─────────────────────────────────────────────────

def _spawn_ironls(
    ironls_path: pathlib.Path,
    suppressions: pathlib.Path,
    fixtures_root: pathlib.Path,
) -> subprocess.Popen:
    env = os.environ.copy()
    # Compose TSAN_OPTIONS so CI logs are readable AND suppressions
    # (if any) are honoured. halt_on_error=0 means TSAN keeps running
    # after the first race so we can surface ALL findings in one job.
    existing = env.get("TSAN_OPTIONS", "")
    tsan_opts = (
        f"suppressions={suppressions}"
        f" halt_on_error=0"
        f" print_suppressions=1"
        f" second_deadlock_stack=1"
    )
    if existing:
        env["TSAN_OPTIONS"] = f"{existing} {tsan_opts}"
    else:
        env["TSAN_OPTIONS"] = tsan_opts
    # RSS cap disabled: the driver owns its own wall-clock budget and
    # a 5 s sampler _exit(42) race with TSAN shutdown would confuse
    # the "server exited cleanly" check.
    env["IRON_LSP_RSS_CAP_BYTES"] = "0"
    return subprocess.Popen(
        [str(ironls_path)],
        cwd=str(fixtures_root),
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        env=env,
    )


def _send_initialize(server_stdin, workspace_uri: str) -> None:
    _send_message(server_stdin, "initialize", {
        "processId": os.getpid(),
        "rootUri": workspace_uri,
        "capabilities": {
            "workspace": {"workspaceFolders": True, "symbol": {}},
            "textDocument": {
                "hover": {},
                "completion": {},
                "definition": {},
                "synchronization": {"didSave": True},
            },
        },
        "workspaceFolders": [{"uri": workspace_uri, "name": "tsan"}],
    }, request_id=1)
    _send_message(server_stdin, "initialized", {})


def _didopen_both(server_stdin, doc_a_uri: str, doc_b_uri: str,
                  doc_a_text: str, doc_b_text: str) -> None:
    _send_message(server_stdin, "textDocument/didOpen", {
        "textDocument": {
            "uri": doc_a_uri,
            "languageId": "iron",
            "version": 1,
            "text": doc_a_text,
        }
    })
    _send_message(server_stdin, "textDocument/didOpen", {
        "textDocument": {
            "uri": doc_b_uri,
            "languageId": "iron",
            "version": 1,
            "text": doc_b_text,
        }
    })


def _send_shutdown(server_stdin) -> None:
    try:
        _send_message(server_stdin, "shutdown", {}, request_id=9999)
        _send_message(server_stdin, "exit", {})
    except (BrokenPipeError, ValueError):
        pass


# ── Main driver ───────────────────────────────────────────────────────

def run_soak(args: argparse.Namespace) -> int:
    ironls = pathlib.Path(args.ironls).resolve()
    if not ironls.is_file() or not os.access(ironls, os.X_OK):
        print(f"[tsan-driver] ironls not executable: {ironls}",
              file=sys.stderr)
        return 2

    workload_path = pathlib.Path(args.workload).resolve()
    events = _load_workload(workload_path)
    if len(events) == 0:
        print(f"[tsan-driver] workload {workload_path} is empty",
              file=sys.stderr)
        return 2

    fixtures_root = pathlib.Path(args.fixtures_root).resolve()
    doc_a_path = fixtures_root / "doc_a.iron"
    doc_b_path = fixtures_root / "doc_b.iron"
    if not doc_a_path.is_file() or not doc_b_path.is_file():
        print(f"[tsan-driver] fixtures missing under {fixtures_root}",
              file=sys.stderr)
        return 2
    doc_a_text = doc_a_path.read_text(encoding="utf-8")
    doc_b_text = doc_b_path.read_text(encoding="utf-8")
    doc_a_uri = doc_a_path.as_uri()
    doc_b_uri = doc_b_path.as_uri()
    workspace_uri = fixtures_root.as_uri()

    suppressions = pathlib.Path(args.suppressions).resolve()
    if not suppressions.is_file():
        print(f"[tsan-driver] suppressions file missing: {suppressions}",
              file=sys.stderr)
        return 2

    print(f"[tsan-driver] spawning {ironls} with {len(events)} events "
          f"across {args.workers} workers (timeout {args.timeout_sec}s)",
          file=sys.stderr)
    proc = _spawn_ironls(ironls, suppressions, fixtures_root)

    stderr_buf: List[bytes] = []
    stop = threading.Event()
    stdout_thread = threading.Thread(
        target=_drain_stdout, args=(proc.stdout, stop), daemon=True)
    stderr_thread = threading.Thread(
        target=_capture_stderr, args=(proc.stderr, stderr_buf, stop),
        daemon=True)
    stdout_thread.start()
    stderr_thread.start()

    stdin_lock = threading.Lock()
    deadline = time.monotonic() + args.timeout_sec
    exit_code = 0

    try:
        with stdin_lock:
            _send_initialize(proc.stdin, workspace_uri)
            _didopen_both(proc.stdin, doc_a_uri, doc_b_uri,
                          doc_a_text, doc_b_text)

        # Split events into <workers> contiguous slices. Each worker
        # owns its slice and pushes frames as fast as it can; the
        # shared stdin_lock serialises byte-level writes, but the
        # server's reader + AST worker pipeline drains them in true
        # parallel (which is what TSAN needs to see).
        n = len(events)
        workers = max(1, min(args.workers, 8))
        slice_size = n // workers
        threads: List[threading.Thread] = []
        for w in range(workers):
            s = w * slice_size
            e = n if w == workers - 1 else (w + 1) * slice_size
            t = threading.Thread(
                target=_replay_slice,
                args=(proc.stdin, stdin_lock, events, s, e, w),
                daemon=True,
                name=f"tsan-worker-{w}",
            )
            threads.append(t)

        for t in threads:
            t.start()
        for t in threads:
            remaining = max(0.0, deadline - time.monotonic())
            t.join(timeout=remaining)
            if t.is_alive():
                print(f"[tsan-driver] worker {t.name} hung past deadline",
                      file=sys.stderr)
                exit_code = max(exit_code, 2)

        with stdin_lock:
            _send_shutdown(proc.stdin)
            try:
                proc.stdin.close()
            except Exception:
                pass

    finally:
        # Give the server a brief window to flush pending TSAN output
        # and exit cleanly. If it doesn't, kill it and capture what we
        # have.
        grace = 10.0
        try:
            proc.wait(timeout=grace)
        except subprocess.TimeoutExpired:
            print(f"[tsan-driver] ironls did not exit within {grace}s; "
                  "sending SIGTERM", file=sys.stderr)
            proc.terminate()
            try:
                proc.wait(timeout=5.0)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait(timeout=5.0)
        stop.set()
        stdout_thread.join(timeout=2.0)
        stderr_thread.join(timeout=2.0)

    stderr_text = b"".join(stderr_buf).decode("utf-8", errors="replace")

    if detect_tsan_finding(stderr_text):
        print("[tsan-driver] FAIL: ThreadSanitizer warning detected",
              file=sys.stderr)
        # Re-emit each WARNING line so CI tooling can parse them.
        for match in TSAN_WARNING_RE.finditer(stderr_text):
            start = match.start()
            end = stderr_text.find("\n", start)
            if end == -1:
                end = len(stderr_text)
            print(stderr_text[start:end], file=sys.stderr)
        return 1

    if proc.returncode not in (0, -signal.SIGTERM, -signal.SIGPIPE):
        # We do not fail the TSAN run on a non-zero exit code by itself:
        # an intentional shutdown races with in-flight requests and can
        # produce -SIGPIPE or abnormal exits. The only hard-fail gate
        # is the WARNING regex. Log so maintainers notice if it becomes
        # a consistent pattern.
        print(f"[tsan-driver] note: ironls exited with code "
              f"{proc.returncode}", file=sys.stderr)

    print("[tsan-driver] TSAN clean: zero WARNING lines in "
          f"{len(stderr_text)} bytes of stderr", file=sys.stderr)
    return exit_code


# ── Self-tests (no ironls spawn) ──────────────────────────────────────

def run_self_tests() -> int:
    """Validate detect_tsan_finding() + argument parsing without a
    server spawn. CI wires this as the `test_tsan_driver_self_test`
    CTest invariant under `phase-m6-invariant`.
    """
    failures: List[str] = []

    # Test 1: synthetic TSAN warning -> detected.
    sample_race = (
        "Some server log line\n"
        "WARNING: ThreadSanitizer: data race (pid=1234)\n"
        "  Write of size 8 at 0xdeadbeef by thread T1:\n"
    )
    if not detect_tsan_finding(sample_race):
        failures.append("sample WARNING line did not fire detector")

    # Test 2: clean output -> no detection.
    clean = ("everything is fine\n"
             "LSP initialize reply sent\n"
             "exit code 0\n")
    if detect_tsan_finding(clean):
        failures.append("clean stderr produced false positive")

    # Test 3: multi-warning run (halt_on_error=0 path) -> detected.
    multi = (
        "line 1\n"
        "WARNING: ThreadSanitizer: data race in writer_queue_push\n"
        "line 2\n"
        "WARNING: ThreadSanitizer: data race in cancel_flag_load\n"
    )
    if not detect_tsan_finding(multi):
        failures.append("multi-warning log did not fire detector")

    # Test 4: the literal text appearing quoted inside a log payload
    # but NOT at line start should not false-fire. This guards against
    # a log line like `msg: "see WARNING: ThreadSanitizer: for triage"`.
    quoted = 'log: "for triage see WARNING: ThreadSanitizer: link above"\n'
    if detect_tsan_finding(quoted):
        failures.append("embedded (non-line-start) WARNING false-fired")

    # Test 5: workload count + methods -- cheap sanity on the
    # committed JSONL. If this test lives next to the committed file
    # the value of a standalone `test_tsan_workload_counts` CMake test
    # diminishes; we keep it here to localise the constant.
    here = pathlib.Path(__file__).resolve().parent
    workload = here / "workload.jsonl"
    if workload.is_file():
        events = _load_workload(workload)
        if len(events) != 300:
            failures.append(
                f"workload.jsonl has {len(events)} events; expected 300")
        # Every event must have a `method` string.
        bad = [i for i, ev in enumerate(events) if not isinstance(
            ev.get("method"), str) or not ev["method"]]
        if bad:
            failures.append(
                f"workload events without method: {bad[:5]}")

    # Test 6: darwin refusal is a runtime check, not a unit test, so
    # we just assert the platform check exists by reading our own
    # source. This stops a future refactor from silently dropping it.
    self_src = pathlib.Path(__file__).read_text(encoding="utf-8")
    if "sys.platform == 'darwin'" not in self_src \
            and 'sys.platform == "darwin"' not in self_src:
        failures.append(
            "darwin refusal guard missing from driver source")

    if failures:
        print("[tsan-driver] self-test FAILED:", file=sys.stderr)
        for f in failures:
            print(f"  - {f}", file=sys.stderr)
        return 1

    print("[tsan-driver] self-tests pass (5 cases)")
    return 0


# ── CLI ───────────────────────────────────────────────────────────────

def _parse_args(argv: List[str]) -> argparse.Namespace:
    here = pathlib.Path(__file__).resolve().parent
    parser = argparse.ArgumentParser(
        description="Iron LSP ThreadSanitizer driver (HARD-17, D-05).")
    parser.add_argument(
        "--ironls", default="build-tsan/src/lsp/ironls",
        help="Path to the TSAN-instrumented ironls binary.")
    parser.add_argument(
        "--workload", default=str(here / "workload.jsonl"),
        help="Path to the 300-event JSONL workload file.")
    parser.add_argument(
        "--fixtures-root", default=str(here / "fixtures"),
        help="Directory containing doc_a.iron + doc_b.iron + iron.toml.")
    parser.add_argument(
        "--suppressions", default=str(here / "suppressions.txt"),
        help="TSAN suppressions.txt (empty at v1 land per D-05).")
    parser.add_argument(
        "--workers", type=int, default=2,
        help="Number of parallel replay workers (default 2).")
    parser.add_argument(
        "--timeout-sec", type=int, default=300,
        help="Wall-clock budget for the whole soak (default 300 = 5 min).")
    parser.add_argument(
        "--self-test", action="store_true",
        help="Run detection-logic self-tests only; no ironls spawn.")
    return parser.parse_args(argv)


def main(argv: Optional[List[str]] = None) -> int:
    args = _parse_args(argv if argv is not None else sys.argv[1:])
    if args.self_test:
        return run_self_tests()
    # macOS TSAN deferred to v2 per .planning/phases/07-m6-production-hardening
    # /07-CONTEXT.md §D-05 (Apple clang TSAN is flakier than upstream clang).
    if sys.platform == "darwin":
        print("[tsan-driver] refusing to run on darwin: macOS TSAN is "
              "deferred to v2 per Phase 7 D-05. Use a Linux runner.",
              file=sys.stderr)
        return 2
    return run_soak(args)


if __name__ == "__main__":
    sys.exit(main())
