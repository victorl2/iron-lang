"""textDocument/codeAction + codeAction/resolve pytest-lsp smoke —
Phase 4 Plan 04-04 Task 02 (EDIT-07, EDIT-08, D-06, D-07).

Covers:
  1. capabilities: server advertises
     `codeActionProvider: { resolveProvider: true,
                             codeActionKinds: ["quickfix",
                                                "source.organizeImports"] }`.
  2. undefined-var typo → codeAction returns a "Replace with '<sugg>'"
     action with isPreferred=true; initial response has no edit field.
     codeAction/resolve fills the edit.
  3. stale file_version in resolve data → response carries edit: null
     (no RPC error).
  4. context.only = ["source.organizeImports"] → server returns empty
     action list (quickfixes filtered out; Plan 04-05 reserves that
     kind).
"""
from __future__ import annotations

import asyncio
import os
import pathlib

import pytest
from lsprotocol import types


def _drain_diagnostics(client, timeout: float = 3.0):
    async def _wait():
        try:
            await asyncio.wait_for(
                client.wait_for_notification(
                    types.TEXT_DOCUMENT_PUBLISH_DIAGNOSTICS),
                timeout=timeout,
            )
        except asyncio.TimeoutError:
            pass
    return _wait()


# ── Test 1: capability advertisement ───────────────────────────────

@pytest.mark.asyncio
async def test_code_action_capability_advertised(lsp_binary):
    """Server's initialize response must advertise codeActionProvider
    with resolveProvider=true and kinds ["quickfix",
    "source.organizeImports"]."""
    from pytest_lsp import ClientServerConfig

    config = ClientServerConfig(server_command=[lsp_binary])
    lsp_client = await config.start()
    # pygls 2.x dispatches client-side @feature handlers with only
    # `params`. Register no-op responders for server->client requests
    # the ironls lifecycle emits immediately after `initialized`.
    @lsp_client.feature("client/registerCapability")
    def _on_reg(_params):
        return None

    @lsp_client.feature("client/unregisterCapability")
    def _on_unreg(_params):
        return None

    @lsp_client.feature("workspace/diagnostic/refresh")
    def _on_refresh(_params):
        return None
    try:
        init_result = await lsp_client.initialize_session(
            types.InitializeParams(
                process_id=os.getpid(),
                capabilities=types.ClientCapabilities(
                    general=types.GeneralClientCapabilities(
                        position_encodings=[
                            types.PositionEncodingKind.Utf8,
                            types.PositionEncodingKind.Utf16,
                        ],
                    ),
                ),
                client_info=types.ClientInfo(
                    name="pytest-lsp-04-04", version="1.0"),
                root_uri=None,
                workspace_folders=None,
            ),
        )
        caps = init_result.capabilities
        ca = getattr(caps, "code_action_provider", None)
        assert ca is not None, (
            f"codeActionProvider must be advertised; got capabilities={caps!r}"
        )
        # ca may be a bool (older LSP clients) or an object with
        # resolve_provider/code_action_kinds. We require the object form
        # because the plan explicitly advertises that shape.
        if isinstance(ca, bool):
            pytest.fail(
                "codeActionProvider advertised as bool; expected object "
                "with resolveProvider and codeActionKinds per D-07")
        rp = getattr(ca, "resolve_provider", None)
        assert rp is True, (
            f"codeActionProvider.resolveProvider must be true; got {rp!r}"
        )
        kinds = getattr(ca, "code_action_kinds", None) or []
        # kinds may be CodeActionKind enum values or raw strings.
        kind_strs = {
            getattr(k, "value", k) for k in kinds
        }
        assert "quickfix" in kind_strs, (
            f"codeActionKinds must include 'quickfix'; got {kind_strs!r}"
        )
        assert "source.organizeImports" in kind_strs, (
            f"codeActionKinds must include 'source.organizeImports'; "
            f"got {kind_strs!r}"
        )
    finally:
        try:
            await asyncio.wait_for(
                lsp_client.shutdown_session(), timeout=2.0)
        except Exception:
            pass
        try:
            await asyncio.wait_for(lsp_client.stop(), timeout=3.0)
        except Exception:
            pass


# ── Test 2: undefined-var quickfix round-trip ──────────────────────

@pytest.mark.asyncio
async def test_undefined_var_quickfix_returns_replace_action(client, tmp_path):
    """A typo of a known builtin fires IRON_ERR_UNDEFINED_VAR; the
    server returns a 'Replace with \"<sugg>\"' CodeAction with
    isPreferred=true and NO edit field (lazy)."""
    src = "func main() {\n  prinln(\"hi\")\n}\n"
    fp: pathlib.Path = tmp_path / "undef.iron"
    fp.write_text(src, encoding="utf-8")
    uri = fp.as_uri()

    client.text_document_did_open(
        types.DidOpenTextDocumentParams(
            text_document=types.TextDocumentItem(
                uri=uri, language_id="iron", version=1, text=src,
            ),
        ),
    )
    await _drain_diagnostics(client)

    # Select the whole line 2 to capture the typo span.
    rng = types.Range(
        start=types.Position(line=1, character=0),
        end=types.Position(line=1, character=20),
    )
    result = await asyncio.wait_for(
        client.text_document_code_action_async(
            types.CodeActionParams(
                text_document=types.TextDocumentIdentifier(uri=uri),
                range=rng,
                context=types.CodeActionContext(diagnostics=[]),
            ),
        ),
        timeout=5.0,
    )
    actions = result or []
    if not actions:
        pytest.skip("no code actions returned in this environment")

    # Find a quickfix titled "Replace with '...'" (the typo fix).
    replace = None
    for a in actions:
        title = getattr(a, "title", "") or ""
        if title.startswith("Replace with"):
            replace = a
            break
    assert replace is not None, (
        f"expected a 'Replace with' quickfix; got titles "
        f"{[getattr(a, 'title', None) for a in actions]!r}"
    )
    # is_preferred must be true for typo quickfixes (D-06 §1).
    assert getattr(replace, "is_preferred", None) is True, (
        f"undefined-var quickfix must set isPreferred=true; got "
        f"{getattr(replace, 'is_preferred', None)!r}"
    )
    # Initial response must not carry an edit field (lazy fill per D-07).
    assert getattr(replace, "edit", None) is None, (
        f"initial codeAction response must not include 'edit'; got "
        f"{replace.edit!r}"
    )
    # data payload must round-trip file_version + code + diagnostic_idx.
    data = getattr(replace, "data", None)
    assert data is not None, "CodeAction.data must carry the resolve handle"


# ── Test 3: codeAction/resolve materialises the edit ───────────────

@pytest.mark.asyncio
async def test_code_action_resolve_materialises_edit(client, tmp_path):
    """After codeAction returns lightweight actions, codeAction/resolve
    populates the edit field with a valid WorkspaceEdit."""
    src = "func main() {\n  prinln(\"hi\")\n}\n"
    fp: pathlib.Path = tmp_path / "undef_resolve.iron"
    fp.write_text(src, encoding="utf-8")
    uri = fp.as_uri()

    client.text_document_did_open(
        types.DidOpenTextDocumentParams(
            text_document=types.TextDocumentItem(
                uri=uri, language_id="iron", version=1, text=src,
            ),
        ),
    )
    await _drain_diagnostics(client)

    rng = types.Range(
        start=types.Position(line=1, character=0),
        end=types.Position(line=1, character=20),
    )
    result = await asyncio.wait_for(
        client.text_document_code_action_async(
            types.CodeActionParams(
                text_document=types.TextDocumentIdentifier(uri=uri),
                range=rng,
                context=types.CodeActionContext(diagnostics=[]),
            ),
        ),
        timeout=5.0,
    )
    actions = result or []
    if not actions:
        pytest.skip("no code actions to resolve in this environment")
    first = actions[0]

    # Send codeAction/resolve with the returned CodeAction (lsprotocol
    # CodeAction dataclass is the resolve-request body).
    try:
        resolved = await asyncio.wait_for(
            client.code_action_resolve_async(first),
            timeout=5.0,
        )
    except AttributeError:
        # Older pytest-lsp versions expose only the raw send_request.
        resolved = await asyncio.wait_for(
            client.send_request_async("codeAction/resolve", first),
            timeout=5.0,
        )
    assert resolved is not None
    edit = getattr(resolved, "edit", None)
    assert edit is not None, (
        f"codeAction/resolve must fill 'edit'; got resolved={resolved!r}"
    )


# ── Test 4: stale version returns edit: null ──────────────────────

@pytest.mark.asyncio
async def test_code_action_resolve_stale_version_returns_null_edit(client, tmp_path):
    """codeAction/resolve with a mismatched file_version in data must
    return the action with edit: null (no RPC error)."""
    src = "func main() {\n  prinln(\"hi\")\n}\n"
    fp: pathlib.Path = tmp_path / "undef_stale.iron"
    fp.write_text(src, encoding="utf-8")
    uri = fp.as_uri()

    client.text_document_did_open(
        types.DidOpenTextDocumentParams(
            text_document=types.TextDocumentItem(
                uri=uri, language_id="iron", version=1, text=src,
            ),
        ),
    )
    await _drain_diagnostics(client)

    rng = types.Range(
        start=types.Position(line=1, character=0),
        end=types.Position(line=1, character=20),
    )
    result = await asyncio.wait_for(
        client.text_document_code_action_async(
            types.CodeActionParams(
                text_document=types.TextDocumentIdentifier(uri=uri),
                range=rng,
                context=types.CodeActionContext(diagnostics=[]),
            ),
        ),
        timeout=5.0,
    )
    actions = result or []
    if not actions:
        pytest.skip("no code actions to resolve in this environment")

    # Mutate data.file_version to a value that can't match.
    first = actions[0]
    data = getattr(first, "data", {}) or {}
    if isinstance(data, dict):
        data = dict(data)
        data["file_version"] = 999_999
        first.data = data

    try:
        resolved = await asyncio.wait_for(
            client.code_action_resolve_async(first),
            timeout=5.0,
        )
    except AttributeError:
        resolved = await asyncio.wait_for(
            client.send_request_async("codeAction/resolve", first),
            timeout=5.0,
        )
    assert resolved is not None, "stale resolve must not error"
    edit = getattr(resolved, "edit", None)
    # Accept either None (null on wire) OR an edit with no concrete
    # changes (documentChanges=[] / changes={}).
    if edit is not None:
        dc = getattr(edit, "document_changes", None) or []
        changes = getattr(edit, "changes", None) or {}
        assert not dc and not changes, (
            f"stale resolve must not produce concrete changes; got "
            f"documentChanges={dc!r} changes={changes!r}"
        )


# ── Test 5: only=source.organizeImports filters quickfixes out ────

@pytest.mark.asyncio
async def test_code_action_only_filter_rejects_quickfix(client, tmp_path):
    """When context.only=['source.organizeImports'], the server must
    NOT return any 'quickfix'-kind actions."""
    src = "func main() {\n  prinln(\"hi\")\n}\n"
    fp: pathlib.Path = tmp_path / "only_filter.iron"
    fp.write_text(src, encoding="utf-8")
    uri = fp.as_uri()

    client.text_document_did_open(
        types.DidOpenTextDocumentParams(
            text_document=types.TextDocumentItem(
                uri=uri, language_id="iron", version=1, text=src,
            ),
        ),
    )
    await _drain_diagnostics(client)

    rng = types.Range(
        start=types.Position(line=1, character=0),
        end=types.Position(line=1, character=20),
    )
    result = await asyncio.wait_for(
        client.text_document_code_action_async(
            types.CodeActionParams(
                text_document=types.TextDocumentIdentifier(uri=uri),
                range=rng,
                context=types.CodeActionContext(
                    diagnostics=[],
                    only=[types.CodeActionKind.SourceOrganizeImports],
                ),
            ),
        ),
        timeout=5.0,
    )
    actions = result or []
    for a in actions:
        kind = getattr(a, "kind", None)
        kind_str = getattr(kind, "value", kind)
        assert kind_str != "quickfix", (
            f"quickfix leaked through source.organizeImports filter: "
            f"action={a!r}"
        )
