"""Parameter-name inlay hints at call sites, constructions and interface
calls included, and the settings that turn each kind of hint off:
`initializationOptions.inlayHints` at startup and
`workspace/didChangeConfiguration` (settings.iron.inlayHints) later,
after which the server asks the editor to refresh its hints."""
from __future__ import annotations

import asyncio
import os

import pytest
from lsprotocol import types
from pytest_lsp import ClientServerConfig

SOURCE = """object P {
    val x: Int
    val y: Int
}

interface Shape {
    func scaled(factor: Int, offset: Int) -> Int
}

func area(w: Int, h: Int) -> Int {
    return w * h
}

func use(s: Shape) -> Int {
    return s.scaled(2, 3)
}

func main() {
    val p = P(1, 2)
    val a = area(3, 4)
    println("{p.x} {a}")
}
"""
LINES = SOURCE.split("\n")


async def _open(client, tmp_path):
    fp = tmp_path / "hints.iron"
    fp.write_text(SOURCE, encoding="utf-8")
    uri = fp.as_uri()
    client.text_document_did_open(types.DidOpenTextDocumentParams(
        text_document=types.TextDocumentItem(uri=uri, language_id="iron", version=1, text=SOURCE)))
    return uri


async def _hints(client, uri):
    hints = await asyncio.wait_for(client.text_document_inlay_hint_async(types.InlayHintParams(
        text_document=types.TextDocumentIdentifier(uri=uri),
        range=types.Range(start=types.Position(line=0, character=0),
                          end=types.Position(line=len(LINES), character=0)))), timeout=10.0)
    params = {(h.position.line + 1, h.label) for h in hints or []
              if h.kind == types.InlayHintKind.Parameter}
    typed = {(h.position.line + 1, h.label) for h in hints or []
             if h.kind == types.InlayHintKind.Type}
    return params, typed


@pytest.mark.asyncio
async def test_parameter_hints_at_call_sites(client, tmp_path):
    params, typed = await _hints(client, await _open(client, tmp_path))
    # A construction names the object's fields.
    assert {(19, "x:"), (19, "y:")} <= params, sorted(params)
    assert {(20, "w:"), (20, "h:")} <= params, sorted(params)
    # A call through an interface names the signature's parameters.
    assert {(15, "factor:"), (15, "offset:")} <= params, sorted(params)
    # `println` is a builtin with one parameter and a non-literal argument.
    assert not any(line == 21 for line, _ in params)
    assert (19, ": P") in typed and (20, ": Int") in typed


async def _start(lsp_binary, init_options, refresh_support):
    config = ClientServerConfig(server_command=[lsp_binary])
    client = await config.start()
    refreshes = []

    def on_refresh(*_args):
        refreshes.append(True)
        return None

    for name, handler in [("workspace/inlayHint/refresh", on_refresh),
                          ("client/registerCapability", lambda *_a: None),
                          ("client/unregisterCapability", lambda *_a: None),
                          ("workspace/diagnostic/refresh", lambda *_a: None)]:
        try:
            client.feature(name)(handler)
        except Exception:
            pass
    await client.initialize_session(types.InitializeParams(
        process_id=os.getpid(),
        capabilities=types.ClientCapabilities(
            general=types.GeneralClientCapabilities(
                position_encodings=[types.PositionEncodingKind.Utf8]),
            workspace=types.WorkspaceClientCapabilities(
                inlay_hint=types.InlayHintWorkspaceClientCapabilities(
                    refresh_support=refresh_support))),
        initialization_options=init_options,
    ))
    return client, refreshes


async def _stop(client):
    try:
        await asyncio.wait_for(client.shutdown_session(), timeout=2.0)
    except Exception:
        pass
    try:
        await asyncio.wait_for(client.stop(), timeout=3.0)
    except Exception:
        server = getattr(client, "_server", None)
        if server is not None and server.returncode is None:
            server.kill()


@pytest.mark.asyncio
async def test_initialization_options_turn_parameter_hints_off(lsp_binary, tmp_path):
    client, _ = await _start(lsp_binary, {"inlayHints": {"parameterNames": False}}, False)
    try:
        params, typed = await _hints(client, await _open(client, tmp_path))
        assert params == set()
        assert (20, ": Int") in typed
    finally:
        await _stop(client)


@pytest.mark.asyncio
async def test_configuration_change_turns_hints_off_and_refreshes(lsp_binary, tmp_path):
    client, refreshes = await _start(lsp_binary, None, True)
    try:
        uri = await _open(client, tmp_path)
        params, typed = await _hints(client, uri)
        assert params and typed

        client.workspace_did_change_configuration(types.DidChangeConfigurationParams(
            settings={"iron": {"inlayHints": {"bindingTypes": False}}}))
        for _ in range(50):
            if refreshes:
                break
            await asyncio.sleep(0.05)
        assert refreshes, "no workspace/inlayHint/refresh after a settings change"
        params, typed = await _hints(client, uri)
        assert typed == set() and (20, "w:") in params

        client.workspace_did_change_configuration(types.DidChangeConfigurationParams(
            settings={"iron": {"inlayHints": {"bindingTypes": True, "parameterNames": False}}}))
        await asyncio.sleep(0.2)
        params, typed = await _hints(client, uri)
        assert params == set() and (20, ": Int") in typed

        # The same settings again change nothing and ask for no refresh.
        count = len(refreshes)
        client.workspace_did_change_configuration(types.DidChangeConfigurationParams(
            settings={"iron": {"inlayHints": {"bindingTypes": True, "parameterNames": False}}}))
        await asyncio.sleep(0.2)
        assert len(refreshes) == count
    finally:
        await _stop(client)
