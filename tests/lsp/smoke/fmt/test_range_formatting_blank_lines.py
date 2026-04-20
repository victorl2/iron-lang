"""FMT-03 / D-04 smoke -- rangeFormatting covering only blank lines
between decls returns an empty TextEdit[].

Plan 05-03. No top-level Iron_Program decl intersects the requested
Range (the blank-line region lies strictly between the decls), so the
facade emits an empty list. Mirrors D-04 "skip when no decl intersects".
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
async def test_range_formatting_blank_lines(client, tmp_path):
    src = (
        "func alpha() {\n"     # 0
        "    val x = 1\n"      # 1
        "}\n"                  # 2
        "\n"                   # 3 -- blank
        "\n"                   # 4 -- blank
        "func beta() {\n"      # 5
        "    val y = 2\n"      # 6
        "}\n"                  # 7
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
                    # Covers only blank lines 3-4 (half-open end at line 5 col 0
                    # -> does NOT include line 5 per RESEARCH Pitfall 4).
                    start=types.Position(line=3, character=0),
                    end=types.Position(line=5, character=0),
                ),
                options=types.FormattingOptions(
                    tab_size=2, insert_spaces=True,
                ),
            ),
        ),
        timeout=5.0,
    )

    assert isinstance(edits, (list, tuple)), f"expected list, got {type(edits).__name__}"
    assert len(edits) == 0, (
        f"expected empty TextEdit[] for blank-line-only range, got {len(edits)}: {edits!r}"
    )
