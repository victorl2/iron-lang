"""FMT-03 smoke -- textDocument/rangeFormatting covering one top-level
decl returns exactly one TextEdit (D-04 intersection, D-06 shape).

Plan 05-03. For a cleanly-parseable Iron source with two top-level
functions, a Range that covers exactly the first function must yield
one TextEdit whose range starts at line 0 character 0.
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
async def test_range_formatting_single_decl(client, tmp_path):
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
                    end=types.Position(line=2, character=1),  # mid '}' line
                ),
                options=types.FormattingOptions(
                    tab_size=2, insert_spaces=True,
                ),
            ),
        ),
        timeout=5.0,
    )

    assert isinstance(edits, (list, tuple)), f"expected list, got {type(edits).__name__}"
    assert len(edits) == 1, f"expected 1 TextEdit, got {len(edits)}: {edits!r}"
    edit = edits[0]
    # The intersecting decl is func alpha -- its range starts at line 0.
    assert edit.range.start.line == 0
    assert edit.range.start.character == 0
    assert edit.new_text, f"newText must be non-empty, got {edit.new_text!r}"
