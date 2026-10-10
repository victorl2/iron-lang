"""Smoke: a document's diagnostics reach the client one way, never both (#360).

ironls answers textDocument/diagnostic (pull) and can publish
textDocument/publishDiagnostics (push). A client that supports both
(Neovim 0.11+, VS Code, Zed) shows every diagnostic twice when the
server uses both, so the server pushes only to clients that do not
advertise `textDocument.diagnostic` in their capabilities.

- A pull client gets its diagnostics from textDocument/diagnostic and no
  publishDiagnostics, on open and on change.
- A push-only client (the default smoke `client` fixture) keeps getting
  publishDiagnostics, and may still pull.
"""
from __future__ import annotations

import asyncio
import os

import pytest
from lsprotocol import types


_BAD = "func main() {\n    val bad: Int = undefined_thing\n}\n"
_BAD2 = "func main() {\n    val bad: Int = other_undefined\n}\n"

# Comfortably longer than the server's compile debounce (150 ms): a push
# would have arrived by then.
_QUIET_S = 1.0


def _pull_init_params(with_pull: bool) -> types.InitializeParams:
    td = types.TextDocumentClientCapabilities(
        publish_diagnostics=types.PublishDiagnosticsClientCapabilities(),
        synchronization=types.TextDocumentSyncClientCapabilities(
            dynamic_registration=False, did_save=True,
        ),
    )
    if with_pull:
        td.diagnostic = types.DiagnosticClientCapabilities(
            dynamic_registration=False, related_document_support=False,
        )
    return types.InitializeParams(
        process_id=os.getpid(),
        capabilities=types.ClientCapabilities(
            general=types.GeneralClientCapabilities(
                position_encodings=[types.PositionEncodingKind.Utf8],
            ),
            text_document=td,
        ),
        client_info=types.ClientInfo(name="pytest-lsp-pull", version="1.0"),
    )


def _open(client, uri: str, text: str) -> None:
    client.text_document_did_open(
        types.DidOpenTextDocumentParams(
            text_document=types.TextDocumentItem(
                uri=uri, language_id="iron", version=1, text=text,
            ),
        ),
    )


async def _pull(client, uri: str):
    report = await client.text_document_diagnostic_async(
        types.DocumentDiagnosticParams(
            text_document=types.TextDocumentIdentifier(uri=uri),
        ),
    )
    items = getattr(report, "items", None)
    assert items is not None, f"unexpected report shape: {report!r}"
    return items


@pytest.mark.asyncio
async def test_pull_client_gets_no_push(raw_client, tmp_path):
    await raw_client.initialize_session(_pull_init_params(with_pull=True))
    uri = (tmp_path / "pull_only.iron").as_uri()

    _open(raw_client, uri, _BAD)
    items = await _pull(raw_client, uri)
    assert len(items) == 1, f"expected one diagnostic, got {items!r}"
    assert "undefined_thing" in items[0].message

    await asyncio.sleep(_QUIET_S)
    assert uri not in raw_client.diagnostics, (
        "server pushed publishDiagnostics to a client that pulls: "
        f"{raw_client.diagnostics.get(uri)!r}")

    # An edit: the client pulls again and still gets no push.
    raw_client.text_document_did_change(
        types.DidChangeTextDocumentParams(
            text_document=types.VersionedTextDocumentIdentifier(uri=uri, version=2),
            content_changes=[types.TextDocumentContentChangeWholeDocument(text=_BAD2)],
        ),
    )
    items = await _pull(raw_client, uri)
    assert len(items) == 1 and "other_undefined" in items[0].message, items
    await asyncio.sleep(_QUIET_S)
    assert uri not in raw_client.diagnostics, (
        "server pushed publishDiagnostics after didChange to a client that pulls")


@pytest.mark.asyncio
async def test_push_only_client_gets_push(raw_client, tmp_path):
    await raw_client.initialize_session(_pull_init_params(with_pull=False))
    uri = (tmp_path / "push_only.iron").as_uri()

    _open(raw_client, uri, _BAD)
    await asyncio.wait_for(
        raw_client.wait_for_notification(types.TEXT_DOCUMENT_PUBLISH_DIAGNOSTICS),
        timeout=5.0,
    )
    pushed = raw_client.diagnostics.get(uri, [])
    assert len(pushed) == 1, f"expected one pushed diagnostic, got {pushed!r}"

    # The server still answers a pull from a push client (it advertises
    # diagnosticProvider to everyone).
    items = await _pull(raw_client, uri)
    assert len(items) == 1
