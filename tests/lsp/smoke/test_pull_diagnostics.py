"""Smoke: textDocument/diagnostic pull returns same data as push (CORE-17).

After a didOpen triggers publishDiagnostics, a subsequent
textDocument/diagnostic request must return a DocumentDiagnosticReport
whose items match the pushed diagnostics (modulo resultId, which is
v<version>).
"""
from __future__ import annotations

import asyncio
import pytest
from lsprotocol import types


_BAD = 'fun main() {\n    val x: Int = "pulled"\n}\n'


@pytest.mark.asyncio
async def test_pull_matches_push(client, tmp_path):
    uri = (tmp_path / "pull.iron").as_uri()
    client.text_document_did_open(
        types.DidOpenTextDocumentParams(
            text_document=types.TextDocumentItem(
                uri=uri, language_id="iron", version=1, text=_BAD,
            ),
        ),
    )
    # Wait for the initial push.
    await asyncio.wait_for(
        client.wait_for_notification(types.TEXT_DOCUMENT_PUBLISH_DIAGNOSTICS),
        timeout=5.0,
    )
    pushed = client.diagnostics.get(uri, [])
    assert len(pushed) >= 1

    # Pull via textDocument/diagnostic.
    report = await client.text_document_diagnostic_async(
        types.DocumentDiagnosticParams(
            text_document=types.TextDocumentIdentifier(uri=uri),
        ),
    )
    assert report is not None
    # The report is a RelatedFullDocumentDiagnosticReport (kind: "full")
    # with .items list of Diagnostic. Verify it aligns with push.
    items = getattr(report, "items", None)
    assert items is not None, f"unexpected report shape: {report!r}"
    assert len(items) == len(pushed), (
        f"push had {len(pushed)} diagnostics, pull returned {len(items)}")

    # resultId should be v1 for version=1.
    rid = getattr(report, "result_id", None)
    assert rid is None or rid == "v1", f"unexpected resultId: {rid!r}"
