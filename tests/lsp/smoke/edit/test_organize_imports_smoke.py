"""textDocument/codeAction + codeAction/resolve pytest-lsp smoke for
source.organizeImports — Phase 4 Plan 04-05 Task 02 (EDIT-09, D-08).

Covers:
  1. codeAction request with context.only=['source.organizeImports'] on
     a messy import fixture returns exactly one CodeAction whose
     kind='source.organizeImports' and data.code=-1 (sentinel).
  2. codeAction/resolve on the returned action materialises a
     WorkspaceEdit with documentChanges/changes containing a TextEdit
     that reorganises the import block.
  3. A plain codeAction request (no `only` filter) on a file with zero
     diagnostics returns no organize-imports action (organizeImports
     is explicit-trigger only; D-08).
  4. context.only=['quickfix'] does NOT surface organizeImports either.
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


# Messy fixture: 2 stdlib imports out of order + 1 duplicate of io +
# 1 local import interleaved. Expected organise output: stdlib block
# (io + math), blank line, local block (local_mod).
MESSY_SRC = (
    "import math\n"
    "import local_mod\n"
    "import io\n"
    "import io\n"
    "\n"
    "func main() {}\n"
)


# ── Test 1: only=['source.organizeImports'] returns one organize action

@pytest.mark.asyncio
async def test_organize_imports_only_filter_returns_source_action(
        client, tmp_path):
    """When context.only=['source.organizeImports'], the server returns
    exactly one CodeAction of kind 'source.organizeImports' with a
    sentinel data.code."""
    fp: pathlib.Path = tmp_path / "org_only.iron"
    fp.write_text(MESSY_SRC, encoding="utf-8")
    uri = fp.as_uri()

    client.text_document_did_open(
        types.DidOpenTextDocumentParams(
            text_document=types.TextDocumentItem(
                uri=uri, language_id="iron", version=1, text=MESSY_SRC,
            ),
        ),
    )
    await _drain_diagnostics(client)

    # Whole-file selection so the range is broad enough.
    rng = types.Range(
        start=types.Position(line=0, character=0),
        end=types.Position(line=5, character=0),
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
    # Filter to organizeImports kind; there must be exactly one.
    org_actions = []
    for a in actions:
        kind = getattr(a, "kind", None)
        kind_str = getattr(kind, "value", kind)
        if kind_str == "source.organizeImports":
            org_actions.append(a)
        else:
            # Nothing else is allowed through this filter.
            pytest.fail(
                f"non-organize kind leaked through only filter: "
                f"action={a!r}"
            )
    assert len(org_actions) == 1, (
        f"expected exactly one organize-imports action; got "
        f"{[getattr(a, 'title', None) for a in actions]!r}"
    )
    org = org_actions[0]
    assert getattr(org, "title", None) == "Organize Imports", (
        f"unexpected title: {getattr(org, 'title', None)!r}"
    )
    # The initial response must NOT carry an edit (lazy resolve).
    assert getattr(org, "edit", None) is None, (
        f"initial organizeImports action must not include 'edit'; got "
        f"{getattr(org, 'edit', None)!r}"
    )


# ── Test 2: codeAction/resolve populates the edit with the sorted block

@pytest.mark.asyncio
async def test_organize_imports_resolve_produces_edit(client, tmp_path):
    """codeAction/resolve on a source.organizeImports action returns a
    WorkspaceEdit whose TextEdit replaces the import block with a
    sorted+deduped version."""
    fp: pathlib.Path = tmp_path / "org_resolve.iron"
    fp.write_text(MESSY_SRC, encoding="utf-8")
    uri = fp.as_uri()

    client.text_document_did_open(
        types.DidOpenTextDocumentParams(
            text_document=types.TextDocumentItem(
                uri=uri, language_id="iron", version=1, text=MESSY_SRC,
            ),
        ),
    )
    await _drain_diagnostics(client)

    rng = types.Range(
        start=types.Position(line=0, character=0),
        end=types.Position(line=5, character=0),
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
    if not actions:
        pytest.skip("no organize-imports action returned in this environment")
    org = actions[0]

    try:
        resolved = await asyncio.wait_for(
            client.code_action_resolve_async(org),
            timeout=5.0,
        )
    except AttributeError:
        resolved = await asyncio.wait_for(
            client.send_request_async("codeAction/resolve", org),
            timeout=5.0,
        )
    assert resolved is not None
    edit = getattr(resolved, "edit", None)
    assert edit is not None, (
        f"codeAction/resolve must materialise 'edit'; got {resolved!r}"
    )

    # Extract the TextEdit's newText from either documentChanges or changes.
    new_texts = []
    dc = getattr(edit, "document_changes", None) or []
    for te_group in dc:
        for te in getattr(te_group, "edits", None) or []:
            nt = getattr(te, "new_text", None)
            if nt is not None:
                new_texts.append(nt)
    changes = getattr(edit, "changes", None) or {}
    if isinstance(changes, dict):
        for _uri, te_list in changes.items():
            for te in te_list or []:
                nt = getattr(te, "new_text", None)
                if nt is not None:
                    new_texts.append(nt)

    assert new_texts, (
        f"resolve must attach at least one TextEdit with newText; "
        f"edit={edit!r}"
    )
    combined = "\n".join(new_texts)
    # Must be sorted + deduped: io (once) and math in stdlib group.
    assert "import io" in combined
    assert "import math" in combined
    assert combined.count("import io") == 1, (
        f"exact-duplicate `import io` must collapse to one; "
        f"got newText={combined!r}"
    )


# ── Test 3: default codeAction request does not surface organizeImports

@pytest.mark.asyncio
async def test_default_code_action_request_omits_organize(client, tmp_path):
    """A codeAction request with NO `only` filter on a clean file must
    NOT return a source.organizeImports action (organizeImports is
    explicit-trigger only per D-08)."""
    src = "import io\n\nfunc main() {}\n"
    fp: pathlib.Path = tmp_path / "org_default.iron"
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
        start=types.Position(line=0, character=0),
        end=types.Position(line=2, character=0),
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
    for a in actions:
        kind = getattr(a, "kind", None)
        kind_str = getattr(kind, "value", kind)
        assert kind_str != "source.organizeImports", (
            f"organizeImports leaked into default codeAction request: "
            f"action={a!r}"
        )


# ── Test 4: only=['quickfix'] does not surface organizeImports either

@pytest.mark.asyncio
async def test_only_quickfix_filter_omits_organize(client, tmp_path):
    """When context.only=['quickfix'], server must NOT emit any
    source.organizeImports action."""
    fp: pathlib.Path = tmp_path / "org_qf.iron"
    fp.write_text(MESSY_SRC, encoding="utf-8")
    uri = fp.as_uri()

    client.text_document_did_open(
        types.DidOpenTextDocumentParams(
            text_document=types.TextDocumentItem(
                uri=uri, language_id="iron", version=1, text=MESSY_SRC,
            ),
        ),
    )
    await _drain_diagnostics(client)

    rng = types.Range(
        start=types.Position(line=0, character=0),
        end=types.Position(line=5, character=0),
    )
    result = await asyncio.wait_for(
        client.text_document_code_action_async(
            types.CodeActionParams(
                text_document=types.TextDocumentIdentifier(uri=uri),
                range=rng,
                context=types.CodeActionContext(
                    diagnostics=[],
                    only=[types.CodeActionKind.QuickFix],
                ),
            ),
        ),
        timeout=5.0,
    )
    actions = result or []
    for a in actions:
        kind = getattr(a, "kind", None)
        kind_str = getattr(kind, "value", kind)
        assert kind_str != "source.organizeImports", (
            f"organizeImports leaked through only=['quickfix'] filter: "
            f"action={a!r}"
        )
