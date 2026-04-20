"""FMT-05 / D-13 smoke -- iron.toml [fmt] change mid-session reloads.

Initial format uses iron.toml indent_width=2; a mid-session rewrite to
indent_width=4 plus a workspace/didChangeWatchedFiles event should
cause the next textDocument/formatting response to use the new
options.
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
async def test_formatting_reloads_on_iron_toml_change(
    lsp_binary,
    iron_toml_workspace,
    clean_iron_source,
    init_params_factory,
):
    ws_root: pathlib.Path = iron_toml_workspace("[fmt]\nindent_width = 2\n")
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

        # First format: expects 2-space indent from iron.toml.
        edits1 = await asyncio.wait_for(
            lsp_client.text_document_formatting_async(
                types.DocumentFormattingParams(
                    text_document=types.TextDocumentIdentifier(
                        uri=iron_file.as_uri(),
                    ),
                    options=types.FormattingOptions(
                        tab_size=2, insert_spaces=True,
                    ),
                ),
            ),
            timeout=5.0,
        )
        assert isinstance(edits1, (list, tuple)) and len(edits1) == 1
        body1 = edits1[0].new_text
        assert "  val" in body1, (
            f"expected initial 2-space indent, got {body1!r}"
        )
        assert "    val" not in body1, (
            f"did not expect 4-space indent initially, got {body1!r}"
        )

        # Rewrite iron.toml + notify watched files.
        (ws_root / "iron.toml").write_text(
            "[fmt]\nindent_width = 4\n", encoding="utf-8",
        )
        lsp_client.workspace_did_change_watched_files(
            types.DidChangeWatchedFilesParams(
                changes=[
                    types.FileEvent(
                        uri=(ws_root / "iron.toml").as_uri(),
                        type=types.FileChangeType.Changed,
                    ),
                ],
            ),
        )
        # Small settle window so the reload happens before the next request.
        await asyncio.sleep(0.2)

        # Second format: should pick up the new 4-space indent.
        edits2 = await asyncio.wait_for(
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
        assert isinstance(edits2, (list, tuple)) and len(edits2) == 1
        body2 = edits2[0].new_text
        assert "    val" in body2, (
            f"expected 4-space indent after reload, got {body2!r}"
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
