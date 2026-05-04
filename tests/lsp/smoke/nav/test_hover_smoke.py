"""textDocument/hover smoke -- Phase 3 Plan 04 Task 02 (NAV-09, D-04).

Real pytest-lsp e2e.  Drives the in-process ironls binary
end-to-end via pytest-lsp.  Key invariant: the server accepts hover
requests and responds with either None or a Hover object within the
asyncio deadline (earlier phases would have responded MethodNotFound).
"""
from __future__ import annotations

import asyncio

import pytest
from lsprotocol import types


@pytest.mark.asyncio
async def test_hover_on_function(client, tmp_path):
    src = "/// Greet someone.\nfunc greet(name: String) -> String { return name }\n"
    fp = tmp_path / "hover_func.iron"
    fp.write_text(src, encoding="utf-8")
    uri = fp.as_uri()

    client.text_document_did_open(
        types.DidOpenTextDocumentParams(
            text_document=types.TextDocumentItem(
                uri=uri, language_id="iron", version=1, text=src,
            ),
        ),
    )
    try:
        await asyncio.wait_for(
            client.wait_for_notification(types.TEXT_DOCUMENT_PUBLISH_DIAGNOSTICS),
            timeout=5.0,
        )
    except asyncio.TimeoutError:
        pass

    try:
        result = await asyncio.wait_for(
            client.text_document_hover_async(
                types.HoverParams(
                    text_document=types.TextDocumentIdentifier(uri=uri),
                    position=types.Position(line=1, character=5),
                ),
            ),
            timeout=5.0,
        )
    except asyncio.TimeoutError:
        pytest.fail("textDocument/hover did not respond within 5s")

    # Accept None (no-hover) or a Hover object.  The server MUST not
    # return MethodNotFound — it's registered.
    assert result is None or hasattr(result, "contents"), (
        f"unexpected hover response shape: {result!r}"
    )


@pytest.mark.asyncio
async def test_hover_on_whitespace_returns_null(client, tmp_path):
    src = "func main() {}\n"
    fp = tmp_path / "hover_ws.iron"
    fp.write_text(src, encoding="utf-8")
    uri = fp.as_uri()

    client.text_document_did_open(
        types.DidOpenTextDocumentParams(
            text_document=types.TextDocumentItem(
                uri=uri, language_id="iron", version=1, text=src,
            ),
        ),
    )
    try:
        await asyncio.wait_for(
            client.wait_for_notification(types.TEXT_DOCUMENT_PUBLISH_DIAGNOSTICS),
            timeout=5.0,
        )
    except asyncio.TimeoutError:
        pass

    try:
        result = await asyncio.wait_for(
            client.text_document_hover_async(
                types.HoverParams(
                    text_document=types.TextDocumentIdentifier(uri=uri),
                    position=types.Position(line=0, character=50),
                ),
            ),
            timeout=5.0,
        )
    except asyncio.TimeoutError:
        pytest.fail("textDocument/hover did not respond within 5s")

    # Whitespace cursor must return None.
    assert result is None, f"expected None on whitespace cursor, got {result!r}"
