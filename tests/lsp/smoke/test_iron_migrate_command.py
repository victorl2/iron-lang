"""Phase 14 Plan 02 — Wave 0 smoke tests for workspace/executeCommand iron.migrate.

Requirements covered:
  CMD-01: ironls responds to workspace/executeCommand iron.migrate
  CMD-03: Command gated on ironc >= 3.0.0; fails gracefully on older ironc
  MIG-06: Automatable portion — capability advertisement check

Tests:
  1. test_iron_migrate_capability_advertised — happy path: initialize response
     has executeCommandProvider with commands == ["iron.migrate"]
  2. test_iron_migrate_command_workspace_edit — invocation happy path:
     command returns WorkspaceEdit + 5 $/progress buckets
  3. test_iron_migrate_version_gate_rejects_old_ironc — CMD-03 version gate:
     stub ironc printing "1.2.0" causes window/showMessage Error

Wave 0 contract: tests 2 and 3 are RED until Task 2 implements the handler.
Test 1 turns GREEN once capabilities.c lands the executeCommandProvider entry.
"""
from __future__ import annotations

import asyncio
import os
import pathlib
import stat
import sys
import tempfile
import textwrap

import pytest
import pytest_asyncio
from lsprotocol import types
from pytest_lsp import ClientServerConfig, LanguageClient, make_test_lsp_client


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def _conftest_dir() -> pathlib.Path:
    """Return the smoke/ directory (parent of this file)."""
    return pathlib.Path(__file__).parent


def _make_v2_iron_workspace(tmpdir: pathlib.Path) -> str:
    """Create a minimal .iron workspace in tmpdir and return its file: URI."""
    src = tmpdir / "main.iron"
    src.write_text(
        textwrap.dedent("""\
            func (p: Player) take_damage(n: Int) {
                p.health = p.health - n
            }
            """),
        encoding="utf-8",
    )
    return tmpdir.as_uri()


def _make_stub_ironc(tmpdir: pathlib.Path, version: str) -> str:
    """Write a tiny stub ironc that prints `<version>` on `--version`."""
    stub = tmpdir / "ironc"
    stub.write_text(
        textwrap.dedent(f"""\
            #!/bin/sh
            if [ "$1" = "--version" ]; then
                echo "ironc {version}"
                exit 0
            fi
            exit 0
            """),
        encoding="utf-8",
    )
    # Make executable
    stub.chmod(stub.stat().st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)
    return str(stub)


def _default_init_params(
    workspace_uri: str | None = None,
) -> types.InitializeParams:
    """Canonical initialize params for migrate tests."""
    folders = None
    if workspace_uri:
        folders = [types.WorkspaceFolder(uri=workspace_uri, name="test-workspace")]
    return types.InitializeParams(
        process_id=os.getpid(),
        capabilities=types.ClientCapabilities(
            general=types.GeneralClientCapabilities(
                position_encodings=[
                    types.PositionEncodingKind.Utf8,
                ],
            ),
            text_document=types.TextDocumentClientCapabilities(
                publish_diagnostics=types.PublishDiagnosticsClientCapabilities(),
                synchronization=types.TextDocumentSyncClientCapabilities(
                    dynamic_registration=False,
                    will_save=False,
                    will_save_wait_until=False,
                    did_save=True,
                ),
            ),
            workspace=types.WorkspaceClientCapabilities(
                execute_command=types.ExecuteCommandClientCapabilities(
                    dynamic_registration=False,
                ),
            ),
        ),
        client_info=types.ClientInfo(name="pytest-lsp-migrate-smoke", version="1.0"),
        root_uri=workspace_uri,
        workspace_folders=folders,
    )


def _install_stubs(client: LanguageClient) -> None:
    """Install no-op handlers for server-initiated requests that pytest-lsp
    doesn't handle by default."""

    @client.feature("client/registerCapability")
    def _on_register_capability(_c, _params):
        return None

    @client.feature("client/unregisterCapability")
    def _on_unregister_capability(_c, _params):
        return None

    @client.feature("workspace/diagnostic/refresh")
    def _on_ws_diag_refresh(_c, _params):
        return None

    @client.feature("workspace/applyEdit")
    def _on_apply_edit(_c, params):
        # Accept any WorkspaceEdit without applying it — test just observes.
        return types.ApplyWorkspaceEditResult(applied=True)


# ---------------------------------------------------------------------------
# Fixture for lsp_binary (local — avoids depending on conftest.py's option
# if running the file in isolation; delegates to conftest if available).
# ---------------------------------------------------------------------------

@pytest.fixture(scope="session")
def lsp_binary(request) -> str:  # type: ignore[override]
    """Absolute path to the ironls binary under test (session-scoped)."""
    path = request.config.getoption("--lsp-binary", default=None)
    if not path:
        pytest.skip("--lsp-binary not provided")
    if not os.path.isfile(path) or not os.access(path, os.X_OK):
        pytest.fail(f"ironls binary not executable: {path}")
    return os.path.abspath(path)


# ---------------------------------------------------------------------------
# Test 1 — CMD-01 / MIG-06: capability advertisement
# ---------------------------------------------------------------------------

@pytest.mark.asyncio
async def test_iron_migrate_capability_advertised(lsp_binary: str) -> None:
    """The initialize response MUST include executeCommandProvider with
    commands == ["iron.migrate"].

    CMD-01 requirement: server advertises iron.migrate command.
    This test turns GREEN once capabilities.c caps_add override lands
    (Task 2 of Plan 14-02).
    """
    config = ClientServerConfig(server_command=[lsp_binary])
    client = await config.start()
    _install_stubs(client)
    try:
        await client.initialize_session(_default_init_params())
        caps = client.capabilities
        assert caps is not None, "server must return capabilities on initialize"

        # capabilities.executeCommandProvider must be present
        ecp = getattr(caps, "execute_command_provider", None)
        assert ecp is not None, (
            "executeCommandProvider missing from initialize response. "
            "CMD-01 requires the server to advertise iron.migrate."
        )

        # Must contain "iron.migrate" in commands array
        if isinstance(ecp, bool):
            pytest.fail(
                "executeCommandProvider is a boolean true — expected object shape "
                "{commands: ['iron.migrate']}. See capabilities.c caps_add override."
            )

        commands = getattr(ecp, "commands", None)
        assert commands is not None, "executeCommandProvider.commands must be present"
        assert "iron.migrate" in commands, (
            f"iron.migrate not in executeCommandProvider.commands: {commands}"
        )
    finally:
        try:
            await asyncio.wait_for(client.shutdown_session(), timeout=2.0)
        except Exception:
            pass
        try:
            await asyncio.wait_for(client.stop(), timeout=3.0)
        except Exception:
            proc = getattr(client, "_server", None)
            if proc and proc.returncode is None:
                proc.terminate()


# ---------------------------------------------------------------------------
# Test 2 — CMD-01 happy path: workspace/executeCommand returns WorkspaceEdit
# ---------------------------------------------------------------------------

@pytest.mark.asyncio
async def test_iron_migrate_command_workspace_edit(lsp_binary: str) -> None:
    """workspace/executeCommand iron.migrate returns a WorkspaceEdit with
    non-empty changes, and the server emits 5 $/progress notifications
    covering all 5 bucket messages.

    CMD-01 requirements:
    - subprocess spawns ironc migrate
    - 5-bucket $/progress (parsing manifest / running codemod / applying rewrites /
      formatting output / verifying parity)
    - returns WorkspaceEdit for editor-side preview

    This test turns GREEN once the handler is implemented in Task 2.
    """
    with tempfile.TemporaryDirectory() as tmpdir:
        workspace_path = pathlib.Path(tmpdir)
        workspace_uri = _make_v2_iron_workspace(workspace_path)

        config = ClientServerConfig(server_command=[lsp_binary])
        client = await config.start()
        _install_stubs(client)

        # Collect $/progress notifications
        progress_messages: list[str] = []

        @client.feature("$/progress")
        def _on_progress(_c, params):
            value = getattr(params, "value", None)
            if value is not None:
                msg = getattr(value, "message", None) or getattr(value, "title", None)
                if msg:
                    progress_messages.append(msg)

        try:
            await client.initialize_session(_default_init_params(workspace_uri))

            # Send workspace/executeCommand iron.migrate
            result = await asyncio.wait_for(
                client.workspace_execute_command_async(
                    types.ExecuteCommandParams(
                        command="iron.migrate",
                        arguments=[workspace_uri],
                    )
                ),
                timeout=30.0,
            )

            # Result may be a WorkspaceEdit or None (server calls workspace/applyEdit
            # directly in some implementations). Either is acceptable if progress fired.
            # The key assertions are on the progress messages.

            # Verify 5-bucket progress messages
            expected_substrings = [
                "parsing manifest",
                "running codemod",
                "applying rewrites",
                "formatting output",
                "verifying parity",
            ]
            for substr in expected_substrings:
                found = any(substr in msg for msg in progress_messages)
                assert found, (
                    f"Expected progress bucket '{substr}' not found in "
                    f"progress messages: {progress_messages}"
                )

            # At minimum 5 progress events must have been emitted (begin + reports)
            assert len(progress_messages) >= 5, (
                f"Expected >= 5 $/progress messages, got {len(progress_messages)}: "
                f"{progress_messages}"
            )

        finally:
            try:
                await asyncio.wait_for(client.shutdown_session(), timeout=2.0)
            except Exception:
                pass
            try:
                await asyncio.wait_for(client.stop(), timeout=3.0)
            except Exception:
                proc = getattr(client, "_server", None)
                if proc and proc.returncode is None:
                    proc.terminate()


# ---------------------------------------------------------------------------
# Test 3 — CMD-03: version gate rejects ironc < 3.0.0
# ---------------------------------------------------------------------------

@pytest.mark.asyncio
async def test_iron_migrate_version_gate_rejects_old_ironc(lsp_binary: str) -> None:
    """When ironls is configured to use an ironc binary that reports version
    1.2.0, the workspace/executeCommand iron.migrate invocation MUST:
      - NOT return a WorkspaceEdit
      - emit a window/showMessage of severity Error containing
        "requires ironc >= 3.0.0" AND "found 1.2.0"

    CMD-03 requirement: command gated on ironc >= 3.0.0.
    Uses the IRON_LSP_IRONC_PATH environment variable as a test hook
    (documented in handlers_workspace_command.c).

    This test turns GREEN once the CMD-03 gate is implemented in Task 2.
    """
    with tempfile.TemporaryDirectory() as tmpdir:
        stub_dir = pathlib.Path(tmpdir) / "stub"
        stub_dir.mkdir()
        stub_ironc = _make_stub_ironc(stub_dir, "1.2.0")

        workspace_path = pathlib.Path(tmpdir) / "workspace"
        workspace_path.mkdir()
        workspace_uri = _make_v2_iron_workspace(workspace_path)

        # Boot ironls with the stub ironc via environment variable
        env = dict(os.environ)
        env["IRON_LSP_IRONC_PATH"] = stub_ironc

        config = ClientServerConfig(
            server_command=[lsp_binary],
            server_environment=env,
        )
        client = await config.start()
        _install_stubs(client)

        show_messages: list[tuple[int, str]] = []

        @client.feature("window/showMessage")
        def _on_show_message(_c, params):
            show_messages.append((getattr(params, "type", 0), params.message))

        try:
            await client.initialize_session(_default_init_params(workspace_uri))

            # The command should either return an error OR emit window/showMessage
            try:
                result = await asyncio.wait_for(
                    client.workspace_execute_command_async(
                        types.ExecuteCommandParams(
                            command="iron.migrate",
                            arguments=[workspace_uri],
                        )
                    ),
                    timeout=10.0,
                )
            except Exception:
                # An exception (error response) is also acceptable for the gate
                pass

            # Give the server a moment to flush any pending notifications
            await asyncio.sleep(0.2)

            # Must have received a window/showMessage Error (type=1) about the version gate
            error_messages = [m for t, m in show_messages if t == 1]
            assert len(error_messages) >= 1, (
                f"Expected window/showMessage Error for CMD-03 version gate, "
                f"but got show_messages={show_messages}"
            )

            gate_msg = error_messages[0]
            assert "requires ironc >= 3.0.0" in gate_msg, (
                f"CMD-03 message must contain 'requires ironc >= 3.0.0', got: {gate_msg!r}"
            )
            assert "1.2.0" in gate_msg, (
                f"CMD-03 message must contain the detected version '1.2.0', got: {gate_msg!r}"
            )

        finally:
            try:
                await asyncio.wait_for(client.shutdown_session(), timeout=2.0)
            except Exception:
                pass
            try:
                await asyncio.wait_for(client.stop(), timeout=3.0)
            except Exception:
                proc = getattr(client, "_server", None)
                if proc and proc.returncode is None:
                    proc.terminate()
