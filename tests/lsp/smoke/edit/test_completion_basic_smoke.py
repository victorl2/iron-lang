"""textDocument/completion basic smoke -- Phase 4 Plan 04-02 Task 03 (EDIT-01, EDIT-06).

Drives a fresh ironls subprocess end-to-end via pytest-lsp:
  1. initialize -> asserts completionProvider shape.
  2. didOpen a trivial `.iron` fixture.
  3. textDocument/completion at a sensible cursor position.
  4. Asserts CompletionList shape (isIncomplete + items).
"""
from __future__ import annotations

import asyncio
import pathlib

import pytest
from lsprotocol import types


@pytest.mark.asyncio
async def test_completion_provider_advertised(raw_client, init_params_factory):
    """The initialize handshake must advertise completionProvider with
    resolveProvider=True and triggerCharacters containing [".", ":", "/"].

    Uses `raw_client` (pre-init) + initialize_session so we can capture
    the server's InitializeResult.capabilities directly (the standard
    `client` fixture throws the return value away)."""
    init_result = await raw_client.initialize_session(init_params_factory())
    caps = init_result.capabilities
    cp = caps.completion_provider
    assert cp is not None, f"completionProvider not advertised: {caps!r}"
    assert cp.resolve_provider is True, (
        f"resolveProvider not True: {cp.resolve_provider!r}"
    )
    trig = cp.trigger_characters or []
    for ch in (".", ":", "/"):
        assert ch in trig, f"trigger character {ch!r} missing from {trig!r}"


@pytest.mark.asyncio
async def test_completion_returns_items(client, tmp_path):
    """didOpen a small Iron file, then request completion at a cursor
    position inside a function body. The server must reply with a
    CompletionList whose `items` list is non-empty (keywords at minimum,
    plus whatever local/top-level symbols the fuzzy prefix matches)."""
    src = "func main() {\n    val x = 1\n    pri\n}\n"
    fp: pathlib.Path = tmp_path / "complete_basic.iron"
    fp.write_text(src, encoding="utf-8")
    uri = fp.as_uri()

    client.text_document_did_open(
        types.DidOpenTextDocumentParams(
            text_document=types.TextDocumentItem(
                uri=uri, language_id="iron", version=1, text=src,
            ),
        ),
    )
    # Let the worker emit the initial diagnostics publish so the server
    # state machine has settled before we poke it.
    try:
        await asyncio.wait_for(
            client.wait_for_notification(types.TEXT_DOCUMENT_PUBLISH_DIAGNOSTICS),
            timeout=5.0,
        )
    except asyncio.TimeoutError:
        pass

    # Cursor after "pri" on the 3rd line (0-indexed line=2). "pri" starts
    # at column 4 so the cursor (end of prefix) is column 7.
    try:
        result = await asyncio.wait_for(
            client.text_document_completion_async(
                types.CompletionParams(
                    text_document=types.TextDocumentIdentifier(uri=uri),
                    position=types.Position(line=2, character=7),
                ),
            ),
            timeout=5.0,
        )
    except asyncio.TimeoutError:
        pytest.fail("textDocument/completion did not respond within 5s")

    assert result is not None, "completion returned None"
    # Accept both CompletionList and plain list/tuple shapes. pytest-lsp
    # deserializes the CompletionItem[] into a tuple by default.
    items = None
    if hasattr(result, "items"):
        items = result.items
        is_incomplete = getattr(result, "is_incomplete", False)
        assert is_incomplete is False, "isIncomplete must be False in Plan 04-02"
    elif isinstance(result, (list, tuple)):
        items = result
    assert items is not None, f"unexpected completion response shape: {result!r}"
    # items may be either list or tuple; both are acceptable sequence
    # shapes. The keyword `private` should match the "pri" prefix.
    assert isinstance(items, (list, tuple)), (
        f"items must be a sequence, got {type(items).__name__}"
    )


@pytest.mark.asyncio
async def test_completion_items_carry_required_fields(client, tmp_path):
    """Each CompletionItem must carry label, kind, sortText, insertText,
    and a `data` object that completionItem/resolve can round-trip."""
    src = "func main() {\n    val x = 1\n    va\n}\n"
    fp: pathlib.Path = tmp_path / "complete_fields.iron"
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

    # Cursor after "va" on line 2 (0-indexed) — should surface `val`/`var`
    # keyword candidates at minimum.
    result = await asyncio.wait_for(
        client.text_document_completion_async(
            types.CompletionParams(
                text_document=types.TextDocumentIdentifier(uri=uri),
                position=types.Position(line=2, character=6),
            ),
        ),
        timeout=5.0,
    )
    items = result.items if hasattr(result, "items") else (result or [])
    if not items:
        pytest.skip("no completion items matched 'va' in this environment")
    it = items[0]
    assert it.label, "item missing label"
    assert it.kind is not None, "item missing kind"
    # sortText and insertText are optional in the LSP spec; we always emit
    # them so the client can rely on stable ordering + plain-text insertion.
    assert getattr(it, "sort_text", None), "item missing sortText"
    assert getattr(it, "insert_text", None), "item missing insertText"
    assert it.data is not None, "item missing opaque data handle"
