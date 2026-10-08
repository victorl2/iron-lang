"""Hover, go to definition, semantic tokens and inlay hints inside function
bodies, including names that resolve into the stdlib prelude (#310, #311).
"""
from __future__ import annotations

import asyncio

import pytest
from lsprotocol import types

SOURCE = """import math

object Point {
    val x: Float
    var y: Float
}

enum Shape { Circle, Square }

func dist(p: Point, q: Point) -> Float {
    val dx = p.x - q.x
    var total = Math.sqrt(dx * dx)
    total = total + 1.0
    return total
}

func main() {
    val pt = Point(1.0, 2.0)
    val s = Shape.Circle
    val names = ["a", "b"].map(func(n: String) -> String { return n.upper() })
    println("{dist(pt, pt)} {names.len()}")
}
"""
LINES = SOURCE.split("\n")


def _pos(line1: int, word: str, nth: int = 0) -> types.Position:
    """0-based position of the nth `word` on 1-based line `line1`."""
    text = LINES[line1 - 1]
    col = -1
    for _ in range(nth + 1):
        col = text.index(word, col + 1)
    return types.Position(line=line1 - 1, character=col)


async def _open(client, tmp_path):
    fp = tmp_path / "semantics.iron"
    fp.write_text(SOURCE, encoding="utf-8")
    uri = fp.as_uri()
    client.text_document_did_open(
        types.DidOpenTextDocumentParams(
            text_document=types.TextDocumentItem(
                uri=uri, language_id="iron", version=1, text=SOURCE,
            ),
        ),
    )
    await asyncio.wait_for(
        client.wait_for_notification(types.TEXT_DOCUMENT_PUBLISH_DIAGNOSTICS),
        timeout=5.0,
    )
    return uri


def _hover_text(result) -> str:
    assert result is not None, "hover returned null"
    contents = result.contents
    return contents.value if hasattr(contents, "value") else str(contents)


def _first_link(result):
    assert result, f"definition returned nothing: {result!r}"
    link = result[0] if isinstance(result, list) else result
    uri = getattr(link, "target_uri", None) or getattr(link, "uri", None)
    rng = getattr(link, "target_range", None) or getattr(link, "range", None)
    return uri, rng


@pytest.mark.asyncio
async def test_valid_program_has_no_errors(client, tmp_path):
    uri = await _open(client, tmp_path)
    errors = [d for d in client.diagnostics.get(uri, [])
              if d.severity in (None, types.DiagnosticSeverity.Error)]
    assert errors == [], f"false errors in the editor: {[d.message for d in errors]}"


@pytest.mark.asyncio
async def test_hover_inside_bodies(client, tmp_path):
    uri = await _open(client, tmp_path)
    doc = types.TextDocumentIdentifier(uri=uri)
    cases = [
        (_pos(21, "dist"), "func dist(p: Point, q: Point) -> Float"),
        (_pos(12, "sqrt"), "readonly func Math.sqrt(x: Float) -> Float"),
        (_pos(20, "upper"), "readonly func String.upper() -> String"),
        (_pos(11, "p.x"), "p: Point"),
        (_pos(19, "Circle"), "Circle"),
    ]
    for pos, expected in cases:
        result = await client.text_document_hover_async(
            types.HoverParams(text_document=doc, position=pos))
        text = _hover_text(result)
        assert expected in text, f"hover at {pos}: expected {expected!r} in {text!r}"


@pytest.mark.asyncio
async def test_definition_inside_bodies(client, tmp_path):
    uri = await _open(client, tmp_path)
    doc = types.TextDocumentIdentifier(uri=uri)

    # A call to a function in the same file.
    target, rng = _first_link(await client.text_document_definition_async(
        types.DefinitionParams(text_document=doc, position=_pos(21, "dist"))))
    assert target == uri
    assert rng.start.line == 9, f"dist is declared on line 10, got {rng}"

    # A stdlib method: the definition opens the stdlib file at the method.
    target, rng = _first_link(await client.text_document_definition_async(
        types.DefinitionParams(text_document=doc, position=_pos(20, "upper"))))
    assert target.endswith("/stdlib/string.iron"), target

    # A field.
    target, rng = _first_link(await client.text_document_definition_async(
        types.DefinitionParams(text_document=doc, position=_pos(11, "x", 1))))
    assert target == uri and rng.start.line == 3, f"field x is on line 4, got {rng}"


@pytest.mark.asyncio
async def test_semantic_tokens(client, tmp_path):
    uri = await _open(client, tmp_path)
    result = await client.text_document_semantic_tokens_full_async(
        types.SemanticTokensParams(text_document=types.TextDocumentIdentifier(uri=uri)))
    data = result.data
    assert data and len(data) % 5 == 0

    legend_types = ["type", "class", "enum", "interface", "enumMember",
                    "parameter", "variable", "property", "function", "method"]
    legend_mods = ["declaration", "readonly", "defaultLibrary"]
    tokens = set()
    line = ch = 0
    for i in range(0, len(data), 5):
        dl, dc, ln, ty, md = data[i:i + 5]
        if dl:
            line += dl
            ch = dc
        else:
            ch += dc
        text = LINES[line][ch:ch + ln]
        mods = tuple(m for b, m in enumerate(legend_mods) if md >> b & 1)
        tokens.add((line + 1, text, legend_types[ty], mods))

    expected = {
        (3, "Point", "class", ("declaration",)),
        (4, "x", "property", ("declaration",)),
        (8, "Circle", "enumMember", ("declaration",)),
        (10, "p", "parameter", ("declaration",)),
        (10, "Point", "class", ()),
        (11, "dx", "variable", ("declaration", "readonly")),
        (11, "x", "property", ("readonly",)),
        (12, "total", "variable", ("declaration",)),
        (12, "Math", "class", ("defaultLibrary",)),
        (12, "sqrt", "method", ("defaultLibrary",)),
        (19, "Shape", "enum", ()),
        (19, "Circle", "enumMember", ()),
        (20, "map", "method", ("defaultLibrary",)),
        (21, "println", "function", ("defaultLibrary",)),
        (21, "dist", "function", ()),
    }
    missing = expected - tokens
    assert not missing, f"missing tokens: {sorted(missing)}"


@pytest.mark.asyncio
async def test_inlay_hints(client, tmp_path):
    uri = await _open(client, tmp_path)
    hints = await client.text_document_inlay_hint_async(
        types.InlayHintParams(
            text_document=types.TextDocumentIdentifier(uri=uri),
            range=types.Range(start=types.Position(line=0, character=0),
                              end=types.Position(line=len(LINES), character=0)),
        ))
    got = {(h.position.line + 1, LINES[h.position.line][:h.position.character].split()[-1],
            h.label) for h in hints}
    for want in [(11, "dx", ": Float"), (12, "total", ": Float"),
                 (18, "pt", ": Point"), (19, "s", ": Shape"),
                 (20, "names", ": [String]")]:
        assert want in got, f"missing inlay hint {want}; got {sorted(got)}"
    # A binding with a written type gets no hint.
    assert not any(line in (4, 5) for line, _, _ in got)
