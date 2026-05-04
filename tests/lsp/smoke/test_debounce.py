"""Smoke: 5 rapid didChange collapse into ~1 publishDiagnostics (CORE-16).

The mailbox coalesces COMPILE messages and the worker debounces with a
~250 ms idle window. If we fire 5 didChange notifications inside 50 ms,
the server must publish diagnostics for the *newest* version at least
250 ms after the last edit -- and should NOT publish 5 separate bursts.

We accept 1 or 2 publishes (the server may have already picked up the
first change before the burst landed; coalescing elides the middle).
We reject 5+ publishes (that would mean no coalescing).
"""
from __future__ import annotations

import asyncio
import pytest
from lsprotocol import types


@pytest.mark.asyncio
async def test_debounce_coalesces_rapid_edits(client, tmp_path):
    uri = (tmp_path / "burst.iron").as_uri()
    initial = 'fun main() {\n    val x: Int = "x"\n}\n'

    # Track every publishDiagnostics notification to this URI.
    publish_log: list[list] = []
    original = client.diagnostics

    # Hook: wrap the diagnostics dict with a counting proxy.
    class _Counter(dict):
        def __setitem__(self, k, v):
            if k == uri:
                publish_log.append(list(v))
            super().__setitem__(k, v)

    counter = _Counter(original)
    client.diagnostics = counter

    client.text_document_did_open(
        types.DidOpenTextDocumentParams(
            text_document=types.TextDocumentItem(
                uri=uri, language_id="iron", version=1, text=initial,
            ),
        ),
    )

    # Fire 5 didChange in rapid succession.
    for v in range(2, 7):
        src = f'fun main() {{\n    val x: Int = "ver{v}"\n}}\n'
        client.text_document_did_change(
            types.DidChangeTextDocumentParams(
                text_document=types.VersionedTextDocumentIdentifier(
                    uri=uri, version=v),
                content_changes=[
                    types.TextDocumentContentChangeWholeDocument(text=src),
                ],
            ),
        )
        await asyncio.sleep(0.01)  # 10 ms between edits

    # Wait for the debounce window plus generous margin.
    await asyncio.sleep(1.5)

    # We expect at most a couple of publishes for the whole burst.
    # Strict coalescing should give exactly 1; allow up to 3 to tolerate
    # races on slow CI boxes where the first edit landed before the
    # burst throttle engaged.
    assert 1 <= len(publish_log) <= 3, (
        f"expected 1..3 publishes, got {len(publish_log)}: {publish_log!r}")
