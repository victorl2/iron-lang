"""`ironls --supervised` restarts its worker (POSIX fork, Win32 CreateProcess).

Drives the supervisor over raw stdio rather than pytest-lsp: initialize,
kill the worker from outside, expect the synthetic window/showMessage
frame, and expect a fresh worker to answer the next initialize. Closing
stdin ends the supervisor.
"""
import json
import os
import subprocess
import sys
import time

import pytest


def _send(proc, obj):
    body = json.dumps(obj).encode()
    proc.stdin.write(b"Content-Length: %d\r\n\r\n" % len(body) + body)
    proc.stdin.flush()


def _read_frame(proc):
    hdr = b""
    while not hdr.endswith(b"\r\n\r\n"):
        c = proc.stdout.read(1)
        if not c:
            return None
        hdr += c
    n = int(hdr.split(b"Content-Length:")[1].split(b"\r\n")[0])
    return json.loads(proc.stdout.read(n))


def _worker_pids(parent_pid):
    if sys.platform == "win32":
        out = subprocess.run(
            ["powershell", "-NoProfile", "-Command",
             "(Get-CimInstance Win32_Process -Filter 'ParentProcessId=%d').ProcessId" % parent_pid],
            capture_output=True, text=True).stdout
    else:
        out = subprocess.run(["pgrep", "-P", str(parent_pid)], capture_output=True, text=True).stdout
    return [int(x) for x in out.split() if x.isdigit()]


def _kill(pid):
    if sys.platform == "win32":
        subprocess.run(["taskkill", "/f", "/pid", str(pid)], capture_output=True)
    else:
        os.kill(pid, 9)


def test_supervisor_restarts_a_killed_worker(lsp_binary):
    proc = subprocess.Popen([lsp_binary, "--supervised"], stdin=subprocess.PIPE,
                            stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    try:
        init = {"jsonrpc": "2.0", "id": 1, "method": "initialize",
                "params": {"capabilities": {}, "rootUri": None}}
        _send(proc, init)
        first = _read_frame(proc)
        assert first and first.get("id") == 1 and "result" in first
        _send(proc, {"jsonrpc": "2.0", "method": "initialized", "params": {}})

        workers = []
        for _ in range(50):
            workers = _worker_pids(proc.pid)
            if workers:
                break
            time.sleep(0.1)
        assert workers, "the supervisor should have a worker child"
        for pid in workers:
            _kill(pid)

        # The showMessage frame comes once the supervisor notices the death;
        # frames the first worker had in flight may precede it.
        seen = []
        got_message = False
        for _ in range(10):
            frame = _read_frame(proc)
            assert frame is not None, "supervisor closed its output after the worker died: %r" % seen
            seen.append(frame.get("method"))
            if frame.get("method") == "window/showMessage":
                got_message = True
                break
        assert got_message, seen

        time.sleep(1.5)  # first backoff step
        _send(proc, {**init, "id": 2})
        answered = False
        for _ in range(10):
            frame = _read_frame(proc)
            assert frame is not None
            if frame.get("id") == 2:
                answered = "result" in frame
                break
        assert answered, "the respawned worker should answer initialize"
    finally:
        proc.stdin.close()
        try:
            rc = proc.wait(timeout=15)
        except subprocess.TimeoutExpired:
            proc.kill()
            pytest.fail("the supervisor did not exit after stdin closed")
        assert rc == 0
