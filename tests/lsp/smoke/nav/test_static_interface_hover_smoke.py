"""Hover resolves static functions on concrete and constrained type receivers."""
from __future__ import annotations

import asyncio

import pytest
from lsprotocol import types


SOURCE = """interface Seeded {
    static func seed(n: Int) -> Self
}

object Point impl Seeded {
    val n: Int
    static func seed(n: Int) -> Point {
        return Point(n)
    }
}

func build[T: Seeded](n: Int) -> T {
    return T.seed(n)
}

func main() {
    val p = Point.seed(3)
    println("{p.n}")
}
"""


def _position(line1: int, word: str) -> types.Position:
    line = SOURCE.splitlines()[line1 - 1]
    return types.Position(line=line1 - 1, character=line.index(word))


def _hover_text(result) -> str:
    assert result is not None, "hover returned null"
    return result.contents.value if hasattr(result.contents, "value") else str(result.contents)


@pytest.mark.asyncio
async def test_static_function_hover(client, tmp_path):
    fp = tmp_path / "static_hover.iron"
    fp.write_text(SOURCE, encoding="utf-8")
    uri = fp.as_uri()
    client.text_document_did_open(types.DidOpenTextDocumentParams(
        text_document=types.TextDocumentItem(
            uri=uri, language_id="iron", version=1, text=SOURCE)))
    await asyncio.wait_for(
        client.wait_for_notification(types.TEXT_DOCUMENT_PUBLISH_DIAGNOSTICS),
        timeout=5.0)
    doc = types.TextDocumentIdentifier(uri=uri)

    constrained = await client.text_document_hover_async(types.HoverParams(
        text_document=doc, position=_position(13, "seed")))
    assert "static func seed(n: Int) -> Self" in _hover_text(constrained)

    concrete = await client.text_document_hover_async(types.HoverParams(
        text_document=doc, position=_position(17, "seed")))
    assert "static func Point.seed(n: Int) -> Point" in _hover_text(concrete)
