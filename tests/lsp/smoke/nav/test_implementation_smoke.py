"""textDocument/implementation smoke -- Phase 3 Plan 05 Task 01 (NAV-05, D-06).

Real pytest-lsp e2e.  Drives the in-process ironls binary
end-to-end via pytest-lsp.  The key invariant: the server accepts the
request and responds with either a list or None within the asyncio
deadline -- earlier phases responded MethodNotFound.
"""
from __future__ import annotations

import asyncio

import pytest
from lsprotocol import types


@pytest.mark.asyncio
async def test_implementation_on_iface_name(client, tmp_path):
    src = (
        "interface Shape { func area() -> Int }\n"
        "object Circle implements Shape { val r: Int }\n"
        "object Square implements Shape { val s: Int }\n"
        "func Circle.area() -> Int { return 0 }\n"
        "func Square.area() -> Int { return 0 }\n"
    )
    fp = tmp_path / "impl_iface.iron"
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
            client.text_document_implementation_async(
                types.ImplementationParams(
                    text_document=types.TextDocumentIdentifier(uri=uri),
                    position=types.Position(line=0, character=13),
                ),
            ),
            timeout=5.0,
        )
    except asyncio.TimeoutError:
        pytest.fail("textDocument/implementation did not respond within 5s")

    # Server MUST respond. Accept None, list, or tuple -- pygls normalises
    # JSON null to different Python values across versions.  Must NOT be
    # MethodNotFound (the request type would raise then).
    assert result is None or isinstance(result, (list, tuple)), (
        f"unexpected implementation response shape: {result!r}"
    )


@pytest.mark.asyncio
async def test_implementation_on_object_returns_empty(client, tmp_path):
    src = (
        "interface Shape { func area() -> Int }\n"
        "object Circle implements Shape { val r: Int }\n"
    )
    fp = tmp_path / "impl_obj.iron"
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
            client.text_document_implementation_async(
                types.ImplementationParams(
                    text_document=types.TextDocumentIdentifier(uri=uri),
                    position=types.Position(line=1, character=8),
                ),
            ),
            timeout=5.0,
        )
    except asyncio.TimeoutError:
        pytest.fail("textDocument/implementation did not respond within 5s")

    # D-06 Case C: object IS its own implementation -> empty.  Accept
    # None, empty list/tuple.
    if result is not None:
        assert isinstance(result, (list, tuple)), (
            f"unexpected implementation response shape: {result!r}"
        )
        assert len(result) == 0, (
            f"expected empty list for object cursor, got {result!r}"
        )
