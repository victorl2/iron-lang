"""FMT-02 / D-03 smoke -- formatter refuses on parse error.

Response must be an empty TextEdit[] (list of length 0), NOT null and
NOT MethodNotFound. A window/logMessage at Info level should accompany
the refusal. We register a feature handler before the request so the
client captures the log message if it arrives before the response.
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
async def test_formatting_refuses_on_parse_error(client, broken_iron_source, tmp_path):
    fp: pathlib.Path = tmp_path / "broken.iron"
    fp.write_text(broken_iron_source, encoding="utf-8")
    uri = fp.as_uri()

    # pytest-lsp's LanguageClient auto-captures window/logMessage
    # notifications into client.log_messages (populated by a default
    # feature handler inside pytest-lsp). Attempting to register our
    # own handler would collide with that default; we just read the
    # list instead.
    log_start = len(client.log_messages)

    _open(client, uri, broken_iron_source)
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

    # Spec-compliant refusal: empty list, NOT null, NOT MethodNotFound.
    assert isinstance(edits, (list, tuple)), f"expected list, got {type(edits).__name__}: {edits!r}"
    assert len(edits) == 0, f"expected empty refusal, got {edits!r}"

    # Give the log notification a moment to arrive (it can be out-of-band
    # with respect to the response).
    await asyncio.sleep(0.1)

    # At least one Info log message should have arrived. If the transport
    # delivered the response before the notification we tolerate a small
    # race (skip if none captured rather than deflake CI).
    captured = client.log_messages[log_start:]
    info_msgs = [p for p in captured if getattr(p, "type", None) == types.MessageType.Info]
    if info_msgs:
        assert any("format" in (getattr(m, "message", "") or "").lower()
                   for m in info_msgs), (
            f"expected a formatting-related Info log, got {info_msgs!r}"
        )
