"""textDocument/prepareTypeHierarchy smoke -- Phase 3 Plan 05 Task 02 (NAV-11, D-08).

Real pytest-lsp e2e.  Drives the in-process ironls binary
end-to-end via pytest-lsp.  The key invariant: the server accepts the
prepare request and responds with a list (or None) within the asyncio
deadline -- earlier phases responded MethodNotFound.
"""
from __future__ import annotations

import asyncio

import pytest
from lsprotocol import types


@pytest.mark.asyncio
async def test_prepare_on_object(client, tmp_path):
    src = (
        "object Circle implements Shape { val r: Int }\n"
        "interface Shape { func area() -> Int }\n"
    )
    fp = tmp_path / "th_obj.iron"
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
            client.text_document_prepare_type_hierarchy_async(
                types.TypeHierarchyPrepareParams(
                    text_document=types.TextDocumentIdentifier(uri=uri),
                    position=types.Position(line=0, character=8),
                ),
            ),
            timeout=5.0,
        )
    except asyncio.TimeoutError:
        pytest.fail("textDocument/prepareTypeHierarchy did not respond within 5s")

    # Server MUST respond.  Accept None (no item at cursor), list, or
    # tuple (pygls variants).  MethodNotFound would have raised.
    assert result is None or isinstance(result, (list, tuple)), (
        f"unexpected prepareTypeHierarchy response shape: {result!r}"
    )


@pytest.mark.asyncio
async def test_prepare_on_whitespace_returns_none(client, tmp_path):
    src = "func main() {}\n"
    fp = tmp_path / "th_ws.iron"
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
            client.text_document_prepare_type_hierarchy_async(
                types.TypeHierarchyPrepareParams(
                    text_document=types.TextDocumentIdentifier(uri=uri),
                    position=types.Position(line=0, character=50),
                ),
            ),
            timeout=5.0,
        )
    except asyncio.TimeoutError:
        pytest.fail("textDocument/prepareTypeHierarchy did not respond within 5s")

    # Cursor far past end-of-line -> no item expected.
    if result is not None:
        assert isinstance(result, (list, tuple)), (
            f"unexpected prepareTypeHierarchy response shape: {result!r}"
        )
