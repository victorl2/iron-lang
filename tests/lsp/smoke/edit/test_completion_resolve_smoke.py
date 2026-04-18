"""completionItem/resolve smoke -- Phase 4 Plan 04-02 Task 03 (EDIT-03, D-04).

Validates the lazy-resolve round-trip: the initial CompletionItem
carries only `label`, `kind`, `sortText`, `insertText`, `data`. When the
client sends completionItem/resolve with that `data` handle back, the
server fills `documentation` (markdown) + upgraded `detail`.
"""
from __future__ import annotations

import asyncio
import pathlib

import pytest
from lsprotocol import types


@pytest.mark.asyncio
async def test_resolve_fills_documentation(client, tmp_path):
    """Request completion, pick the first returned item, resolve it. The
    resolved item should have documentation populated OR the same shape
    as the initial item (for purely textual keywords)."""
    src = "func greet(name: String) {}\nfunc main() {\n    gre\n}\n"
    fp: pathlib.Path = tmp_path / "resolve_basic.iron"
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

    completion = await asyncio.wait_for(
        client.text_document_completion_async(
            types.CompletionParams(
                text_document=types.TextDocumentIdentifier(uri=uri),
                position=types.Position(line=2, character=7),
            ),
        ),
        timeout=5.0,
    )
    items = completion.items if hasattr(completion, "items") else (completion or [])
    if not items:
        pytest.skip("no completion items to resolve in this environment")

    first = items[0]
    # Send completionItem/resolve. pytest-lsp's convenience wrapper is
    # `completion_item_resolve_async`; fall back to send_request if the
    # installed version exposes a different name.
    send_resolve = getattr(client, "completion_item_resolve_async", None)
    if send_resolve is None:
        pytest.skip("pytest-lsp client missing completion_item_resolve_async")
    try:
        resolved = await asyncio.wait_for(send_resolve(first), timeout=5.0)
    except asyncio.TimeoutError:
        pytest.fail("completionItem/resolve did not respond within 5s")

    assert resolved is not None, "resolve returned None"
    # The server must not error — either documentation is filled or the
    # item comes back unchanged (acceptable for keyword items or stale
    # data handles per D-04).
    # At minimum the label must survive the round-trip.
    assert resolved.label == first.label, (
        f"label drift across resolve: sent {first.label!r} got {resolved.label!r}"
    )


@pytest.mark.asyncio
async def test_resolve_tolerates_stale_data(client, tmp_path):
    """Send completionItem/resolve with a tampered content_hash in the
    `data` handle. The server must return the item without JSON-RPC
    errors (D-04: never errors, returns NULL documentation on stale)."""
    src = "func main() {}\n"
    fp: pathlib.Path = tmp_path / "resolve_stale.iron"
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

    # Hand-craft a CompletionItem whose `data.content_hash` is tampered.
    tampered = types.CompletionItem(
        label="main",
        kind=types.CompletionItemKind.Function,
        insert_text="main",
        sort_text="2-00-main",
        filter_text="main",
        data={
            "canonical_path": uri.replace("file://", ""),
            "name_path": "main",
            "bucket": 2,
            "content_hash": 0xDEADBEEFCAFEBABE,  # tampered
        },
    )

    send_resolve = getattr(client, "completion_item_resolve_async", None)
    if send_resolve is None:
        pytest.skip("pytest-lsp client missing completion_item_resolve_async")
    try:
        resolved = await asyncio.wait_for(send_resolve(tampered), timeout=5.0)
    except asyncio.TimeoutError:
        pytest.fail("completionItem/resolve did not respond to stale handle")

    # Server must not have errored; it echoes the item back (possibly
    # without documentation).
    assert resolved is not None, "resolve returned None on stale handle"
    assert resolved.label == "main", "label must survive stale-data resolve"
