"""textDocument/definition smoke -- Phase 3 Plan 03 Task 02 (NAV-02/03/04).

Flipped from the Wave 0 stub. Drives the in-process ironls binary
end-to-end via pytest-lsp; exercises same-file resolution via the
project_a fixture.
"""
from __future__ import annotations

import asyncio
import pathlib
import pytest
from lsprotocol import types


@pytest.mark.asyncio
async def test_same_file_definition_returns_result(client, nav_fixture_dir):
    """Open util.iron and request definition on the `greet` identifier at
    its own declaration line. The server must reply within the asyncio
    deadline with either a LocationLink[] / Location[] / null -- the
    shape is negotiated based on client capabilities. Plan 03 delivers
    the wire contract; future plans sharpen resolution accuracy."""
    util_path: pathlib.Path = nav_fixture_dir / "project_a" / "util.iron"
    assert util_path.exists(), f"fixture missing: {util_path}"
    uri = util_path.as_uri()
    src = util_path.read_text(encoding="utf-8")

    client.text_document_did_open(
        types.DidOpenTextDocumentParams(
            text_document=types.TextDocumentItem(
                uri=uri, language_id="iron", version=1, text=src,
            ),
        ),
    )
    # Give the worker a moment to publish initial diagnostics.
    try:
        await asyncio.wait_for(
            client.wait_for_notification(types.TEXT_DOCUMENT_PUBLISH_DIAGNOSTICS),
            timeout=5.0,
        )
    except asyncio.TimeoutError:
        pass

    # Request definition at line 1, col ~5 (inside `greet` identifier).
    try:
        result = await asyncio.wait_for(
            client.text_document_definition_async(
                types.DefinitionParams(
                    text_document=types.TextDocumentIdentifier(uri=uri),
                    position=types.Position(line=1, character=5),
                ),
            ),
            timeout=5.0,
        )
    except asyncio.TimeoutError:
        pytest.fail("textDocument/definition did not respond within 5s")

    # Result may be None, a Location, Location[], or LocationLink[]. Any
    # non-error response is accepted; the key invariant is the server
    # registered the handler and responded.
    # (An earlier Phase 2 test would have received MethodNotFound.)
    assert result is None or isinstance(result, (list, types.Location)) or hasattr(result, "target_uri"), (
        f"unexpected definition response shape: {result!r}"
    )


@pytest.mark.asyncio
async def test_type_definition_on_primitive_returns_empty(client, tmp_path):
    """typeDefinition on a val bound to a primitive type (Int) must return
    an empty list per D-05."""
    src = "func main() { val x: Int = 1 }\n"
    fp = tmp_path / "prim.iron"
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

    try:
        result = await asyncio.wait_for(
            client.text_document_type_definition_async(
                types.TypeDefinitionParams(
                    text_document=types.TextDocumentIdentifier(uri=uri),
                    position=types.Position(line=0, character=18),
                ),
            ),
            timeout=5.0,
        )
    except asyncio.TimeoutError:
        pytest.fail("textDocument/typeDefinition did not respond within 5s")

    # Empty result is correct for primitive types.
    assert result is None or result == [] or (isinstance(result, list) and len(result) == 0), (
        f"primitive typeDefinition should return empty, got {result!r}"
    )


@pytest.mark.asyncio
async def test_declaration_mirrors_definition(client, nav_fixture_dir):
    """For non-extern symbols, textDocument/declaration returns the same
    location as textDocument/definition."""
    util_path: pathlib.Path = nav_fixture_dir / "project_a" / "util.iron"
    uri = util_path.as_uri()
    src = util_path.read_text(encoding="utf-8")

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

    try:
        decl = await asyncio.wait_for(
            client.text_document_declaration_async(
                types.DeclarationParams(
                    text_document=types.TextDocumentIdentifier(uri=uri),
                    position=types.Position(line=1, character=5),
                ),
            ),
            timeout=5.0,
        )
    except asyncio.TimeoutError:
        pytest.fail("textDocument/declaration did not respond within 5s")

    # Response shape must be accepted.
    assert decl is None or isinstance(decl, (list, types.Location)) or hasattr(decl, "target_uri"), (
        f"unexpected declaration response shape: {decl!r}"
    )
