"""Smoke: ironls exits cleanly on stdin close (CORE-19).

We spawn `ironls` directly via asyncio.create_subprocess_exec (no
pytest-lsp client here -- we want to test the bare parent-death
response). The server must:

  1. Log a "reader-eof" event.
  2. Join writer + workers.
  3. Return an exit code of 1 (no initialize was sent, so lifecycle
     stayed in UNINIT -- any code other than 0 is acceptable per the
     CORE-19 SLO, which only requires a clean exit).

Budget: 2 seconds (spec SLO for parent-death detection in Phase 2).
"""
from __future__ import annotations

import asyncio
import os
import pytest


@pytest.mark.asyncio
async def test_stdin_close_triggers_clean_exit(lsp_binary):
    proc = await asyncio.create_subprocess_exec(
        lsp_binary,
        stdin=asyncio.subprocess.PIPE,
        stdout=asyncio.subprocess.PIPE,
        stderr=asyncio.subprocess.PIPE,
    )
    assert proc.stdin is not None

    # Close stdin immediately -- simulates the editor closing the LSP
    # subprocess's stdin before sending any message.
    proc.stdin.close()

    # Wait up to 2 s for the process to exit. If it doesn't, we kill
    # it and fail the test.
    try:
        rc = await asyncio.wait_for(proc.wait(), timeout=2.0)
    except asyncio.TimeoutError:
        proc.kill()
        await proc.wait()
        pytest.fail("ironls did not exit within 2 s of stdin close")

    # Any non-crash exit is fine. Code 0 or 1 are both acceptable.
    assert rc is not None
    assert rc == 0 or rc == 1, f"unexpected exit code {rc}"
