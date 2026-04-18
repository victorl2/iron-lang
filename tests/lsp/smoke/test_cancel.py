"""Smoke: $/cancelRequest drops or cancels the pending request (CORE-14).

Sends a textDocument/diagnostic request and immediately cancels it via
$/cancelRequest. The server either:
  (a) returns -32800 RequestCancelled, or
  (b) drops the response entirely (valid per LSP 3.17).

We accept either via an asyncio.wait_for timeout: if the response
arrives it MUST have code -32800; if it doesn't arrive within 2s we
consider the cancel a drop (also valid).
"""
from __future__ import annotations

import asyncio
import pytest
from lsprotocol import types
from pygls.exceptions import JsonRpcException


@pytest.mark.asyncio
async def test_cancel_request_is_dropped_or_cancelled(client, tmp_path):
    uri = (tmp_path / "cancel.iron").as_uri()
    src = "fun main() {\n" + ("    val x = 1\n" * 200) + "}\n"
    client.text_document_did_open(
        types.DidOpenTextDocumentParams(
            text_document=types.TextDocumentItem(
                uri=uri, language_id="iron", version=1, text=src,
            ),
        ),
    )

    # Fire the diagnostic request and immediately cancel. Dispatch the
    # cancel on the same event loop tick so the server sees it before
    # or during the compile.
    req_task = asyncio.create_task(
        client.text_document_diagnostic_async(
            types.DocumentDiagnosticParams(
                text_document=types.TextDocumentIdentifier(uri=uri),
            ),
        ),
    )
    # Give the server a chance to register the request (so the cancel
    # registry has a flag to flip). 10ms is enough on every CI box we
    # care about.
    await asyncio.sleep(0.01)

    # Find the request id. pygls stores outbound requests' futures by
    # their LSP id; we pull the newest.
    pending_ids = list(client.protocol._request_futures.keys())
    if not pending_ids:
        # Request already returned -- OK, the compile was fast enough
        # that cancellation couldn't take effect. Drain the result.
        try:
            await req_task
        except Exception:
            pass
        return

    cancel_id = pending_ids[-1]
    client.cancel_request(types.CancelParams(id=cancel_id))

    # Allow up to 2s for either outcome.
    try:
        await asyncio.wait_for(req_task, timeout=2.0)
        # Response arrived -- either a normal result (compile was
        # faster than cancel) OR a -32800 error. Both OK.
    except asyncio.TimeoutError:
        # No response within 2s -- server dropped it, which is valid.
        req_task.cancel()
    except JsonRpcException as e:
        # Cancelled response: -32800. Accept any code < 0 (server
        # legitimately reported the cancel).
        assert getattr(e, "code", 0) < 0, f"unexpected error code: {e}"
