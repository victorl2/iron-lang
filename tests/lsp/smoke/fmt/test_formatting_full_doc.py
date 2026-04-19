"""FMT-02 smoke -- textDocument/formatting returns a full-document edit.

Plan 05-02. For a cleanly-parseable Iron source:
- Response is a list of exactly one TextEdit.
- The TextEdit's range starts at (0, 0).
- newText is non-empty.
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
async def test_formatting_full_doc(client, clean_iron_source, tmp_path):
    fp: pathlib.Path = tmp_path / "x.iron"
    fp.write_text(clean_iron_source, encoding="utf-8")
    uri = fp.as_uri()
    _open(client, uri, clean_iron_source)
    await _settle(client)

    edits = await asyncio.wait_for(
        client.text_document_formatting_async(
            types.DocumentFormattingParams(
                text_document=types.TextDocumentIdentifier(uri=uri),
                options=types.FormattingOptions(
                    tab_size=2, insert_spaces=True,
                ),
            ),
        ),
        timeout=5.0,
    )

    assert isinstance(edits, list), f"expected list, got {type(edits).__name__}"
    assert len(edits) == 1, f"expected 1 TextEdit, got {len(edits)}: {edits!r}"
    edit = edits[0]
    assert edit.range.start.line == 0
    assert edit.range.start.character == 0
    assert edit.new_text, f"newText must be non-empty, got {edit.new_text!r}"


@pytest.mark.asyncio
async def test_formatting_capability_advertised(raw_client, init_params_factory):
    """initialize must advertise documentFormattingProvider."""
    result = await raw_client.initialize_session(init_params_factory())
    caps = result.capabilities
    dfp = getattr(caps, "document_formatting_provider", None)
    # Auto-derived from the handler table: boolean True.
    assert dfp is True or (dfp is not None and dfp is not False), (
        f"documentFormattingProvider must be advertised: {dfp!r}"
    )
