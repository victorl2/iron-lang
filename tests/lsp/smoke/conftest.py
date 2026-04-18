"""pytest-lsp fixtures for Iron LSP end-to-end smoke tests.

Every `test_*.py` in this directory consumes the `client` fixture,
which spawns a fresh `ironls` subprocess, performs the LSP initialize
handshake, yields the LanguageClient, then drives shutdown + exit on
teardown. The binary path is supplied via the `--lsp-binary` pytest
option (set by `tests/lsp/smoke/CMakeLists.txt`).

Fixtures:

    lsp_binary  -- absolute path to ironls. Session-scoped.
    client      -- pytest_lsp.LanguageClient connected to ironls via
                   stdio and through the initialize + initialized
                   handshake. Function-scoped so every test starts
                   with a fresh process.
    raw_client  -- pytest_lsp.LanguageClient connected to ironls but
                   NOT initialized. Tests that assert pre-init
                   behavior (-32002) use this.

The fixtures intentionally live in one conftest rather than
per-test so the init parameters stay consistent with the
smoke/parity contract.
"""
from __future__ import annotations

import asyncio
import os
import pathlib
import sys

import pytest
import pytest_asyncio
from lsprotocol import types
from pytest_lsp import ClientServerConfig, LanguageClient, make_test_lsp_client


FIXTURE_DIR = pathlib.Path(__file__).parent.parent / "fixtures"


def pytest_addoption(parser):
    parser.addoption(
        "--lsp-binary",
        action="store",
        default=None,
        help="Absolute path to the ironls binary under test.",
    )


@pytest.fixture(scope="session")
def lsp_binary(request) -> str:
    path = request.config.getoption("--lsp-binary")
    if not path:
        pytest.skip("--lsp-binary not provided")
    if not os.path.isfile(path) or not os.access(path, os.X_OK):
        pytest.fail(f"ironls binary not executable: {path}")
    return os.path.abspath(path)


def _default_init_params() -> types.InitializeParams:
    """The canonical initialize params used by every smoke test.

    Advertising positionEncodings=[utf-8, utf-16] exercises the
    Plan 03 negotiation path; the server picks utf-8 and every other
    test assumes that. Tests that need a different negotiation use
    raw_client and call initialize_session themselves.
    """
    return types.InitializeParams(
        process_id=os.getpid(),
        capabilities=types.ClientCapabilities(
            general=types.GeneralClientCapabilities(
                position_encodings=[
                    types.PositionEncodingKind.Utf8,
                    types.PositionEncodingKind.Utf16,
                ],
            ),
            text_document=types.TextDocumentClientCapabilities(
                publish_diagnostics=types.PublishDiagnosticsClientCapabilities(),
                diagnostic=types.DiagnosticClientCapabilities(
                    dynamic_registration=False,
                    related_document_support=False,
                ),
                synchronization=types.TextDocumentSyncClientCapabilities(
                    dynamic_registration=False,
                    will_save=False,
                    will_save_wait_until=False,
                    did_save=True,
                ),
            ),
            workspace=types.WorkspaceClientCapabilities(
                did_change_watched_files=types.DidChangeWatchedFilesClientCapabilities(
                    dynamic_registration=True,
                ),
            ),
        ),
        client_info=types.ClientInfo(name="pytest-lsp-smoke", version="1.0"),
        root_uri=None,
        workspace_folders=None,
    )


def _install_register_capability_handler(client: LanguageClient) -> None:
    """The server emits `client/registerCapability` immediately after
    `initialized` to subscribe to workspace/didChangeWatchedFiles. The
    pytest-lsp default `make_test_lsp_client()` doesn't register a
    handler for it, so pygls raises MethodNotFound and corrupts the
    initialize future. Register a no-op responder per LSP 3.17 spec
    ({"result": null})."""
    @client.feature("client/registerCapability")
    def _on_register_capability(_c: LanguageClient, _params):
        return None

    @client.feature("client/unregisterCapability")
    def _on_unregister_capability(_c: LanguageClient, _params):
        return None


@pytest_asyncio.fixture
async def client(lsp_binary):
    """Client that has completed initialize + initialized handshake."""
    config = ClientServerConfig(server_command=[lsp_binary])
    lsp_client = await config.start()
    _install_register_capability_handler(lsp_client)
    try:
        await lsp_client.initialize_session(_default_init_params())
        yield lsp_client
    finally:
        try:
            await asyncio.wait_for(
                lsp_client.shutdown_session(), timeout=2.0)
        except Exception:
            pass
        try:
            await asyncio.wait_for(lsp_client.stop(), timeout=3.0)
        except Exception:
            await _force_stop(lsp_client)


async def _force_stop(lsp_client) -> None:
    """Force-terminate the LSP subprocess if it's still alive, then
    drive the pygls stop sequence. We use this in teardown instead of
    `await lsp_client.stop()` alone because pygls's stop waits for the
    subprocess to exit -- which on tests that don't drive the LSP
    `exit` notification leaves us hung forever.
    """
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
            except Exception:
                pass
    try:
        await asyncio.wait_for(lsp_client.stop(), timeout=3.0)
    except Exception:
        pass


@pytest_asyncio.fixture
async def raw_client(lsp_binary):
    """Client connected but NOT initialized. Used by lifecycle tests."""
    config = ClientServerConfig(server_command=[lsp_binary])
    lsp_client = await config.start()
    _install_register_capability_handler(lsp_client)
    try:
        yield lsp_client
    finally:
        # Tests that left the server in a clean state can shutdown
        # through the LSP protocol; otherwise we SIGTERM the proc.
        did_clean = False
        if lsp_client.capabilities is not None and lsp_client.error is None:
            try:
                await asyncio.wait_for(
                    lsp_client.shutdown_session(), timeout=2.0)
                did_clean = True
            except Exception:
                did_clean = False
        if not did_clean:
            await _force_stop(lsp_client)
        else:
            try:
                await asyncio.wait_for(lsp_client.stop(), timeout=3.0)
            except Exception:
                await _force_stop(lsp_client)


@pytest.fixture
def canonical_hello() -> str:
    return (FIXTURE_DIR / "hello.iron").read_text(encoding="utf-8")


@pytest.fixture
def init_params_factory():
    """Factory for test-customized InitializeParams."""
    return _default_init_params


@pytest.fixture(scope="session")
def fixture_dir() -> pathlib.Path:
    return FIXTURE_DIR
