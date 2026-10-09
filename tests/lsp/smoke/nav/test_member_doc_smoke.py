"""Hover shows the `///` doc comment of a method declared inside an object
or patch body (#322: only top-level declarations, fields, interface
signatures and enum variants kept theirs).
"""
from __future__ import annotations

import asyncio

import pytest
from lsprotocol import types

SOURCE = """object Counter {
    var n: Int

    init() {
        self.n = 0
    }

    /// Adds one to the count.
    func bump() {
        self.n += 1
    }
}

patch object Counter {
    /// Twice the count.
    readonly func doubled() -> Int {
        return self.n * 2
    }
}

func main() {
    var c = Counter()
    c.bump()
    println("{c.doubled()}")
}
"""
LINES = SOURCE.split("\n")


def _pos(line1: int, word: str) -> types.Position:
    return types.Position(line=line1 - 1, character=LINES[line1 - 1].index(word))


@pytest.mark.asyncio
async def test_hover_shows_member_doc(client, tmp_path):
    fp = tmp_path / "member_doc.iron"
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
    doc = types.TextDocumentIdentifier(uri=uri)
    for pos, expected in [(_pos(23, "bump"), "Adds one to the count."),
                          (_pos(24, "doubled"), "Twice the count.")]:
        result = await client.text_document_hover_async(
            types.HoverParams(text_document=doc, position=pos))
        assert result is not None, f"hover at {pos} returned null"
        text = result.contents.value if hasattr(result.contents, "value") else str(result.contents)
        assert expected in text, f"hover at {pos}: expected {expected!r} in {text!r}"
