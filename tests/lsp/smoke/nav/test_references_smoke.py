"""textDocument/references smoke -- Phase 3 Plan 04 Task 01 (NAV-06).

Real pytest-lsp e2e.  Drives the in-process ironls binary
end-to-end via pytest-lsp.  The key invariant is the server accepts
the request and responds with a list (or None) within the asyncio
deadline -- earlier phases would have responded MethodNotFound.
"""
from __future__ import annotations

import asyncio
import pathlib

import pytest
from lsprotocol import types


@pytest.mark.asyncio
async def test_same_file_references(client, tmp_path):
    src = "func foo() {}\nfunc main() { foo() }\n"
    fp = tmp_path / "refs.iron"
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
            client.text_document_references_async(
                types.ReferenceParams(
                    text_document=types.TextDocumentIdentifier(uri=uri),
                    position=types.Position(line=1, character=14),
                    context=types.ReferenceContext(include_declaration=True),
                ),
            ),
            timeout=5.0,
        )
    except asyncio.TimeoutError:
        pytest.fail("textDocument/references did not respond within 5s")

    # Server must respond; result is None or a Location[] list.
    # pygls maps JSON null / empty array to various Python values; accept
    # None, list, or tuple (pygls sometimes uses empty tuples for null).
    assert result is None or isinstance(result, (list, tuple)), (
        f"unexpected references response shape: {result!r}"
    )


@pytest.mark.asyncio
async def test_references_without_include_declaration(client, tmp_path):
    src = "func foo() {}\nfunc main() { foo() }\n"
    fp = tmp_path / "refs_nodecl.iron"
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
            client.text_document_references_async(
                types.ReferenceParams(
                    text_document=types.TextDocumentIdentifier(uri=uri),
                    position=types.Position(line=1, character=14),
                    context=types.ReferenceContext(include_declaration=False),
                ),
            ),
            timeout=5.0,
        )
    except asyncio.TimeoutError:
        pytest.fail("textDocument/references did not respond within 5s")

    # pygls maps JSON null / empty array to various Python values; accept
    # None, list, or tuple (pygls sometimes uses empty tuples for null).
    assert result is None or isinstance(result, (list, tuple)), (
        f"unexpected references response shape: {result!r}"
    )
