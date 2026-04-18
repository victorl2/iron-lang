"""Smoke: lifecycle FSM rejects out-of-order requests (CORE-05).

Per lifecycle.c:
  - UNINIT: any request except `initialize` -> -32002 ServerNotInitialized.
  - INITIALIZING: `initialize` -> -32600 InvalidRequest (duplicate).
  - SHUTDOWN: any non-`exit` -> -32600 InvalidRequest.
"""
from __future__ import annotations

import os
import pytest
from lsprotocol import types
from pygls.exceptions import JsonRpcException


@pytest.mark.asyncio
async def test_request_before_initialize_yields_32002(raw_client):
    """A non-initialize request before `initialize` must be rejected
    with -32002 ServerNotInitialized."""
    uri = "file:///tmp/not-opened.iron"
    # pytest-lsp's `check_params_against_client_capabilities` refuses
    # to send any request before capabilities is set. We want to probe
    # the -32002 path so we stub capabilities BEFORE the call; this is
    # purely a client-side guard -- the server still sees a raw wire
    # request with no prior `initialize`.
    raw_client.capabilities = types.ClientCapabilities()
    params = types.DocumentDiagnosticParams(
        text_document=types.TextDocumentIdentifier(uri=uri),
    )
    try:
        await raw_client.text_document_diagnostic_async(params)
        pytest.fail("expected -32002 ServerNotInitialized")
    except JsonRpcException as e:
        assert getattr(e, "code", None) == -32002, (
            f"expected -32002, got {e}")


@pytest.mark.asyncio
async def test_duplicate_initialize_yields_32600(raw_client):
    """Two consecutive initialize requests: the second must be rejected
    with -32600 InvalidRequest."""
    init = types.InitializeParams(
        process_id=os.getpid(),
        capabilities=types.ClientCapabilities(),
    )
    # initialize_session sets raw_client.capabilities + drives the
    # initialized notification automatically.
    result = await raw_client.initialize_session(init)
    assert result is not None

    try:
        await raw_client.initialize_async(init)
        pytest.fail("expected -32600 InvalidRequest on duplicate initialize")
    except JsonRpcException as e:
        assert getattr(e, "code", None) == -32600, (
            f"expected -32600, got {e}")
