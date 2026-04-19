"""FMT-03 / D-06 smoke -- rangeFormatting across two decls returns
two TextEdits, sorted descending by start offset so the client applies
later edits first.

Plan 05-03. Mirrors the Phase 4 D-11 WorkspaceEdit ordering contract.
"""
from __future__ import annotations

import asyncio
import pathlib

import pytest
from lsprotocol import types


def _open(client, uri: str, src: str) -> None:
    client.text_document_did_open(
        types.DidOpenTextDocumentParams(
            text_document=types.TextDocumentItem(
                uri=uri, language_id="iron", version=1, text=src,
            ),
        ),
    )


async def _settle(client, timeout: float = 5.0) -> None:
    try:
        await asyncio.wait_for(
            client.wait_for_notification(types.TEXT_DOCUMENT_PUBLISH_DIAGNOSTICS),
            timeout=timeout,
        )
    except asyncio.TimeoutError:
        pass


@pytest.mark.asyncio
async def test_range_formatting_multi_decl(client, tmp_path):
    src = (
        "func alpha() {\n"
        "    val x = 1\n"
        "}\n"
        "func beta() {\n"
        "    val y = 2\n"
        "}\n"
    )
    fp: pathlib.Path = tmp_path / "x.iron"
    fp.write_text(src, encoding="utf-8")
    uri = fp.as_uri()
    _open(client, uri, src)
    await _settle(client)

    edits = await asyncio.wait_for(
        client.text_document_range_formatting_async(
            types.DocumentRangeFormattingParams(
                text_document=types.TextDocumentIdentifier(uri=uri),
                range=types.Range(
                    start=types.Position(line=0, character=0),
                    end=types.Position(line=5, character=1),  # through end
                ),
                options=types.FormattingOptions(
                    tab_size=2, insert_spaces=True,
                ),
            ),
        ),
        timeout=5.0,
    )

    assert isinstance(edits, list), f"expected list, got {type(edits).__name__}"
    assert len(edits) == 2, f"expected 2 TextEdits, got {len(edits)}: {edits!r}"

    # D-06: descending sort means edits[0] starts at a later line than edits[1]
    # so the client applies later-in-document edits first, preventing offset
    # invalidation of the earlier edit.
    assert edits[0].range.start.line > edits[1].range.start.line, (
        f"expected descending sort by start line; got edits[0]@{edits[0].range.start.line}, "
        f"edits[1]@{edits[1].range.start.line}"
    )
    for e in edits:
        assert e.new_text, f"every TextEdit must have non-empty newText: {e!r}"
