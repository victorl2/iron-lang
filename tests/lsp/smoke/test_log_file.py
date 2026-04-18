"""Smoke: JSON-line log lands under $XDG_STATE_HOME (CORE-21).

Spawn ironls directly with asyncio.create_subprocess_exec (no
pytest-lsp fixture -- we only need the binary to run long enough to
open its log file, and we don't want pytest-lsp's teardown paths to
interfere with the explicit subprocess lifecycle). Verify the log
file exists under $XDG_STATE_HOME/iron-lsp/server-<pid>.log and that
at least the "startup" event is present.
"""
from __future__ import annotations

import asyncio
import json
import os
import pathlib
import pytest


@pytest.mark.asyncio
async def test_log_file_lands_under_xdg(lsp_binary, tmp_path):
    xdg = tmp_path / "state"
    env = {
        **os.environ,
        "XDG_STATE_HOME": str(xdg),
        "IRONLS_LOG": "INFO",
    }

    proc = await asyncio.create_subprocess_exec(
        lsp_binary, "--log-level=INFO",
        stdin=asyncio.subprocess.PIPE,
        stdout=asyncio.subprocess.PIPE,
        stderr=asyncio.subprocess.PIPE,
        env=env,
    )
    server_pid = proc.pid
    assert server_pid is not None

    # Give the server a moment to open the log, emit the startup line,
    # and flush.
    await asyncio.sleep(0.25)

    # Close stdin -> reader hits EOF -> main.c drains + closes log
    # sink. wait() completes within 2s.
    proc.stdin.close()
    try:
        await asyncio.wait_for(proc.wait(), timeout=3.0)
    except asyncio.TimeoutError:
        proc.kill()
        await proc.wait()
        pytest.fail("ironls did not exit within 3 s")

    # The file must exist under $XDG_STATE_HOME/iron-lsp/server-<pid>.log.
    log_path = xdg / "iron-lsp" / f"server-{server_pid}.log"
    assert log_path.exists(), f"expected log at {log_path}"
    assert log_path.stat().st_size > 0

    lines = log_path.read_text(encoding="utf-8").splitlines()
    assert len(lines) >= 1, "log file was empty"

    # Every line is a JSON object with {ts,pid,lvl,event,msg}.
    events = []
    for line in lines:
        obj = json.loads(line)
        for key in ("ts", "pid", "lvl", "event", "msg"):
            assert key in obj, f"missing key {key!r} in log line {line!r}"
        events.append(obj["event"])

    # The "startup" event must appear (emitted by main.c).
    assert "startup" in events, f"no 'startup' event; saw {events!r}"
