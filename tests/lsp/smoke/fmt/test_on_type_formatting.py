"""FMT-04 / D-05 smoke -- textDocument/onTypeFormatting on `}` trigger.

Plan 05-04. Three cases:

  1. Capability advertisement: initialize response contains
     documentOnTypeFormattingProvider with the object shape
     { firstTriggerCharacter: "}", moreTriggerCharacter: [] }
     per D-14.

  2. Indent fix: typing `}` after a misindented line returns the
     minimal TextEdit(s) that would correct the indentation of the
     enclosing block's interior lines.

  3. String-literal no-op: typing `}` inside a string literal at a
     correctly-indented enclosing block emits zero TextEdits
     (RESEARCH Pitfall 5 + minimal-edits contract).

Iron syntax: `func <name>() { ... }` + `val <name> = <expr>` (NOT
`fn`/`let` which the Iron grammar rejects).
"""
from __future__ import annotations

import asyncio
import pathlib

import pytest
from lsprotocol import types


def _open(client, uri: str, src: str) -> None:
    client.text_document_did_open(
        types.DidOpenTextDocumentParams(
            text_document=types.TextDocumentItem(
                uri=uri, language_id="iron", version=1, text=src,
            ),
        ),
    )


async def _settle(client, timeout: float = 5.0) -> None:
    try:
        await asyncio.wait_for(
            client.wait_for_notification(types.TEXT_DOCUMENT_PUBLISH_DIAGNOSTICS),
            timeout=timeout,
        )
    except asyncio.TimeoutError:
        pass


@pytest.mark.asyncio
async def test_on_type_capability_advertised(raw_client, init_params_factory):
    """D-14: documentOnTypeFormattingProvider must be advertised as an
    object with firstTriggerCharacter='}' and moreTriggerCharacter=[].
    (pytest-lsp's auto-initialized `client` fixture does not retain the
    server capabilities, so this test uses `raw_client` and drives the
    initialize itself — same pattern as test_formatting_capability_advertised.)"""
    result = await raw_client.initialize_session(init_params_factory())
    caps = result.capabilities
    assert caps is not None, "expected initialize result to include capabilities"

    cap = getattr(caps, "document_on_type_formatting_provider", None)
    assert cap is not None, (
        "expected documentOnTypeFormattingProvider to be advertised"
    )
    assert cap.first_trigger_character == "}", (
        f"expected firstTriggerCharacter='}}', got {cap.first_trigger_character!r}"
    )
    # LSP spec allows moreTriggerCharacter to be absent OR an empty list.
    # lsprotocol deserializes JSON arrays into tuples, so accept both.
    more = cap.more_trigger_character
    assert more is None or list(more) == [], (
        f"expected moreTriggerCharacter empty, got {more!r}"
    )


@pytest.mark.asyncio
async def test_on_type_emits_indent_edit(client, tmp_path):
    """Typing `}` after a misindented interior line produces a TextEdit
    replacing the bad leading whitespace with the canonical indent."""
    src = (
        "func alpha() {\n"
        " val x = 1\n"      # wrong: 1 space (should be 2)
        "}\n"
    )
    fp: pathlib.Path = tmp_path / "x.iron"
    fp.write_text(src, encoding="utf-8")
    uri = fp.as_uri()
    _open(client, uri, src)
    await _settle(client)

    edits = await asyncio.wait_for(
        client.text_document_on_type_formatting_async(
            types.DocumentOnTypeFormattingParams(
                text_document=types.TextDocumentIdentifier(uri=uri),
                position=types.Position(line=2, character=0),
                ch="}",
                options=types.FormattingOptions(
                    tab_size=2, insert_spaces=True,
                ),
            ),
        ),
        timeout=5.0,
    )

    assert isinstance(edits, (list, tuple)), f"expected list, got {type(edits).__name__}"
    assert len(edits) >= 1, f"expected at least 1 edit, got {edits!r}"
    # The fix should replace line 1's single-space indent with two spaces.
    matching = [
        e for e in edits
        if e.range.start.line == 1 and e.new_text == "  "
    ]
    assert matching, f"expected a line-1 indent fix in {edits!r}"


@pytest.mark.asyncio
async def test_on_type_inside_string_returns_empty(client, tmp_path):
    """RESEARCH Pitfall 5 + D-03: typing `}` inside a string literal at
    a correctly-indented enclosing block emits zero TextEdits."""
    src = (
        'func alpha() {\n'
        '  val s = "foo}bar"\n'   # canonical 2-space indent
        '}\n'
    )
    fp: pathlib.Path = tmp_path / "y.iron"
    fp.write_text(src, encoding="utf-8")
    uri = fp.as_uri()
    _open(client, uri, src)
    await _settle(client)

    # Position on the `}` byte INSIDE the string literal.
    # Line 1 layout (0-based char indices):
    #   0,1 = "  "   2..4 = "val"   5 = ' '   6 = 's'   7 = ' '   8 = '='
    #   9 = ' '   10 = '"'   11..13 = 'f','o','o'   14 = '}'
    edits = await asyncio.wait_for(
        client.text_document_on_type_formatting_async(
            types.DocumentOnTypeFormattingParams(
                text_document=types.TextDocumentIdentifier(uri=uri),
                position=types.Position(line=1, character=14),
                ch="}",
                options=types.FormattingOptions(
                    tab_size=2, insert_spaces=True,
                ),
            ),
        ),
        timeout=5.0,
    )

    assert isinstance(edits, (list, tuple)), f"expected list, got {type(edits).__name__}"
    assert len(edits) == 0, (
        f"expected empty edits inside string with canonical indent, got {edits!r}"
    )
