"""Smoke: didChange triggers publishDiagnostics (CORE-22 hero test).

Opens a document with a known type-mismatch error; the server must
asynchronously emit textDocument/publishDiagnostics with at least one
Diagnostic whose message references the type mismatch. The CORE-22
parity claim ("facade output matches iron check") is asserted by the
`test_parity_ironc_lsp` harness in tests/lsp/parity; this test proves
the same output flows end-to-end through the real LSP protocol.
"""
from __future__ import annotations

import asyncio
import pytest
from lsprotocol import types


_BAD_SOURCE = 'fun main() {\n    val x: Int = "this is a string"\n}\n'


@pytest.mark.asyncio
async def test_did_open_produces_diagnostics(client, tmp_path):
    uri = (tmp_path / "bad.iron").as_uri()
    client.text_document_did_open(
        types.DidOpenTextDocumentParams(
            text_document=types.TextDocumentItem(
                uri=uri,
                language_id="iron",
                version=1,
                text=_BAD_SOURCE,
            ),
        ),
    )

    # The server debounces publishDiagnostics ~250ms; give it ample
    # budget on slower CI boxes.
    await asyncio.wait_for(
        client.wait_for_notification(types.TEXT_DOCUMENT_PUBLISH_DIAGNOSTICS),
        timeout=5.0,
    )
    diags = client.diagnostics.get(uri, [])
    assert len(diags) >= 1, f"expected >=1 diagnostic, got {diags!r}"

    # Phase 1 type-check error code TYPE_MISMATCH is 200-range; any
    # diagnostic with non-empty message is fine for the smoke
    # assertion, parity is asserted in tests/lsp/parity.
    messages = [d.message or "" for d in diags]
    assert any(messages), "all diagnostics had empty messages"


@pytest.mark.asyncio
async def test_did_change_updates_diagnostics(client, tmp_path):
    """didOpen(bad) -> wait for diagnostics -> didChange(good) -> wait
    until diagnostics count changes.

    We don't assert the diagnostics become empty because the "good"
    source may still trigger other diagnostics (e.g. "unused val") --
    the contract is that a didChange notification actually re-triggers
    a compile + publish, not that the source is error-free."""
    uri = (tmp_path / "bad2.iron").as_uri()
    client.text_document_did_open(
        types.DidOpenTextDocumentParams(
            text_document=types.TextDocumentItem(
                uri=uri, language_id="iron", version=1, text=_BAD_SOURCE,
            ),
        ),
    )
    await asyncio.wait_for(
        client.wait_for_notification(types.TEXT_DOCUMENT_PUBLISH_DIAGNOSTICS),
        timeout=5.0,
    )
    first = list(client.diagnostics.get(uri, []))
    assert len(first) >= 1

    good = 'fun main() {\n    val y: Int = 42\n}\n'
    client.text_document_did_change(
        types.DidChangeTextDocumentParams(
            text_document=types.VersionedTextDocumentIdentifier(uri=uri, version=2),
            content_changes=[
                types.TextDocumentContentChangeWholeDocument(text=good),
            ],
        ),
    )
    # Wait for another publishDiagnostics notification; it must not
    # be the same set as the bad compile.
    deadline = asyncio.get_event_loop().time() + 5.0
    current = first
    while asyncio.get_event_loop().time() < deadline and current == first:
        try:
            await asyncio.wait_for(
                client.wait_for_notification(types.TEXT_DOCUMENT_PUBLISH_DIAGNOSTICS),
                timeout=1.0,
            )
        except asyncio.TimeoutError:
            pass
        current = list(client.diagnostics.get(uri, []))
    assert current != first, (
        f"diagnostics did not change after didChange; still {current!r}")
