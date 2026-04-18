"""workspace/symbol smoke -- Phase 3 Plan 03 Task 03 (NAV-08).

Flipped from the Wave 0 stub. Drives the ironls binary end-to-end:
opens a few files, issues workspace/symbol, verifies the response
shape and that the handler is registered (not MethodNotFound).
"""
from __future__ import annotations

import asyncio
import pathlib
import pytest
from lsprotocol import types


@pytest.mark.asyncio
async def test_workspace_symbol_responds(client, tmp_path):
    """After opening a few .iron files, workspace/symbol with any query
    must respond with an array (possibly empty). The essential invariant
    is that the handler is registered -- Phase 2 would have returned
    MethodNotFound."""
    # Seed a workspace with 3 functions to discover.
    (tmp_path / "iron.toml").write_text(
        '[package]\nname="t"\nversion="0.1.0"\n', encoding="utf-8")
    main_f = tmp_path / "main.iron"
    main_f.write_text(
        "func greeter() {}\n"
        "func reader()  {}\n"
        "func writer()  {}\n",
        encoding="utf-8",
    )

    client.text_document_did_open(
        types.DidOpenTextDocumentParams(
            text_document=types.TextDocumentItem(
                uri=main_f.as_uri(), language_id="iron", version=1,
                text=main_f.read_text(encoding="utf-8"),
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

    # workspace/symbol with query "greeter". Request shape per LSP 3.17.
    try:
        result = await asyncio.wait_for(
            client.workspace_symbol_async(
                types.WorkspaceSymbolParams(query="greeter"),
            ),
            timeout=5.0,
        )
    except asyncio.TimeoutError:
        pytest.fail("workspace/symbol did not respond within 5s")

    # Response shape: SymbolInformation[] | WorkspaceSymbol[] | None.
    assert result is None or isinstance(result, list), (
        f"unexpected workspace/symbol response: {result!r}"
    )


@pytest.mark.asyncio
async def test_workspace_symbol_empty_query_returns_list(client, tmp_path):
    """An empty query must still produce a valid (possibly empty) list
    response; the server must not crash or return MethodNotFound."""
    (tmp_path / "iron.toml").write_text(
        '[package]\nname="t"\nversion="0.1.0"\n', encoding="utf-8")
    mf = tmp_path / "m.iron"
    mf.write_text("func foo() {}\n", encoding="utf-8")
    client.text_document_did_open(
        types.DidOpenTextDocumentParams(
            text_document=types.TextDocumentItem(
                uri=mf.as_uri(), language_id="iron", version=1,
                text=mf.read_text(encoding="utf-8"),
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

    try:
        result = await asyncio.wait_for(
            client.workspace_symbol_async(
                types.WorkspaceSymbolParams(query=""),
            ),
            timeout=5.0,
        )
    except asyncio.TimeoutError:
        pytest.fail("workspace/symbol did not respond within 5s")

    assert result is None or isinstance(result, list), (
        f"unexpected workspace/symbol response: {result!r}"
    )
