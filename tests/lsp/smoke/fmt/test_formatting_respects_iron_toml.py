"""FMT-05 smoke -- iron.toml [fmt] indent_width is honored by the LSP.

A workspace whose iron.toml sets indent_width=4 should produce a
4-space indent in the formatted output, overriding the default (2).
Drives a fresh LanguageClient rooted at the tmp workspace so the
initialize handshake picks up the iron.toml via workspace_root.
"""
from __future__ import annotations

import asyncio
import os
import pathlib

import pytest
from lsprotocol import types
from pytest_lsp import ClientServerConfig


def _install_feature_stubs(lsp_client) -> None:
    @lsp_client.feature("client/registerCapability")
    def _on_register_capability(_c, _params):  # noqa: ARG001
        return None

    @lsp_client.feature("client/unregisterCapability")
    def _on_unregister_capability(_c, _params):  # noqa: ARG001
        return None

    @lsp_client.feature("workspace/diagnostic/refresh")
    def _on_ws_diag_refresh(_c, _params):  # noqa: ARG001
        return None


async def _force_stop(lsp_client) -> None:
    server = getattr(lsp_client, "_server", None)
    if server is not None and server.returncode is None:
        try:
            server.terminate()
        except ProcessLookupError:
            pass
        try:
            await asyncio.wait_for(server.wait(), timeout=2.0)
        except (asyncio.TimeoutError, Exception):
            try:
                server.kill()
                await server.wait()
            except Exception:  # noqa: BLE001
                pass
    try:
        await asyncio.wait_for(lsp_client.stop(), timeout=3.0)
    except Exception:  # noqa: BLE001
        pass


@pytest.mark.asyncio
async def test_formatting_respects_indent_width(
    lsp_binary,
    iron_toml_workspace,
    clean_iron_source,
    init_params_factory,
):
    ws_root: pathlib.Path = iron_toml_workspace("[fmt]\nindent_width = 4\n")
    iron_file = ws_root / "x.iron"
    iron_file.write_text(clean_iron_source, encoding="utf-8")

    config = ClientServerConfig(server_command=[lsp_binary])
    lsp_client = await config.start()
    _install_feature_stubs(lsp_client)

    try:
        init = init_params_factory()
        init.root_uri = ws_root.as_uri()
        init.workspace_folders = [
            types.WorkspaceFolder(uri=ws_root.as_uri(), name="ws"),
        ]
        init.process_id = os.getpid()
        await lsp_client.initialize_session(init)

        lsp_client.text_document_did_open(
            types.DidOpenTextDocumentParams(
                text_document=types.TextDocumentItem(
                    uri=iron_file.as_uri(),
                    language_id="iron",
                    version=1,
                    text=clean_iron_source,
                ),
            ),
        )
        try:
            await asyncio.wait_for(
                lsp_client.wait_for_notification(
                    types.TEXT_DOCUMENT_PUBLISH_DIAGNOSTICS,
                ),
                timeout=5.0,
            )
        except asyncio.TimeoutError:
            pass

        edits = await asyncio.wait_for(
            lsp_client.text_document_formatting_async(
                types.DocumentFormattingParams(
                    text_document=types.TextDocumentIdentifier(
                        uri=iron_file.as_uri(),
                    ),
                    options=types.FormattingOptions(
                        tab_size=4, insert_spaces=True,
                    ),
                ),
            ),
            timeout=5.0,
        )

        assert isinstance(edits, (list, tuple))
        assert len(edits) == 1, f"expected 1 TextEdit, got {edits!r}"
        body = edits[0].new_text
        # 4-space indent on the body line.
        assert "    val" in body, (
            f"expected 4-space indent before val, got newText={body!r}"
        )
    finally:
        try:
            await asyncio.wait_for(lsp_client.shutdown_session(), timeout=2.0)
        except Exception:  # noqa: BLE001
            pass
        try:
            await asyncio.wait_for(lsp_client.stop(), timeout=3.0)
        except Exception:  # noqa: BLE001
            await _force_stop(lsp_client)
