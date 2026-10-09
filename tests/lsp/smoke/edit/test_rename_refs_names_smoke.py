"""References, rename and highlight work from a declaration's name, and
the declaration's own result is its name: renaming a function must not
replace the whole function."""
from __future__ import annotations

import asyncio

import pytest
from lsprotocol import types

SOURCE = """func area(w: Int, h: Int) -> Int {
    val product = w * h
    return product
}

func main() {
    var total = 0
    total = total + area(1, 2)
    total = total + area(3, 4)
    println("{total}")
}
"""
LINES = SOURCE.split("\n")


def _at(line1: int, word: str, nth: int = 0) -> types.Position:
    col = -1
    for _ in range(nth + 1):
        col = LINES[line1 - 1].index(word, col + 1)
    return types.Position(line=line1 - 1, character=col)


async def _open(client, tmp_path):
    fp = tmp_path / "names.iron"
    fp.write_text(SOURCE, encoding="utf-8")
    uri = fp.as_uri()
    client.text_document_did_open(types.DidOpenTextDocumentParams(
        text_document=types.TextDocumentItem(uri=uri, language_id="iron", version=1, text=SOURCE)))
    await asyncio.wait_for(
        client.wait_for_notification(types.TEXT_DOCUMENT_PUBLISH_DIAGNOSTICS), timeout=5.0)
    return uri


def _starts(locs):
    return sorted((l.range.start.line + 1, l.range.start.character + 1) for l in locs)


@pytest.mark.asyncio
async def test_references_from_declarations(client, tmp_path):
    uri = await _open(client, tmp_path)
    doc = types.TextDocumentIdentifier(uri=uri)
    ctx = types.ReferenceContext(include_declaration=True)
    for pos, want in [
        (_at(1, "area"), [(1, 6), (8, 21), (9, 21)]),
        (_at(7, "total"), [(7, 9), (8, 5), (8, 13), (9, 5), (9, 13), (10, 15)]),
        (_at(1, "w"), [(1, 11), (2, 19)]),
    ]:
        refs = await client.text_document_references_async(
            types.ReferenceParams(text_document=doc, position=pos, context=ctx))
        assert _starts(refs or []) == want


@pytest.mark.asyncio
async def test_rename_edits_only_names(client, tmp_path):
    uri = await _open(client, tmp_path)
    doc = types.TextDocumentIdentifier(uri=uri)
    edit = await client.text_document_rename_async(
        types.RenameParams(text_document=doc, position=_at(8, "area"), new_name="surface"))
    edits = (edit.changes or {}).get(uri) or [
        e for dc in (edit.document_changes or []) for e in dc.edits]
    assert sorted((e.range.start.line + 1, e.range.start.character + 1,
                   e.range.end.line + 1, e.range.end.character + 1) for e in edits) == [
        (1, 6, 1, 10), (8, 21, 8, 25), (9, 21, 9, 25)]
    # From the declaration of a local.
    edit = await client.text_document_rename_async(
        types.RenameParams(text_document=doc, position=_at(7, "total"), new_name="sum"))
    edits = (edit.changes or {}).get(uri) or [
        e for dc in (edit.document_changes or []) for e in dc.edits]
    assert len(edits) == 6
    assert all(e.range.end.character - e.range.start.character == 5 for e in edits)


@pytest.mark.asyncio
async def test_highlight_marks_the_declared_name(client, tmp_path):
    uri = await _open(client, tmp_path)
    hl = await client.text_document_document_highlight_async(types.DocumentHighlightParams(
        text_document=types.TextDocumentIdentifier(uri=uri), position=_at(8, "total", 1)))
    assert _starts(hl) == [(7, 9), (8, 5), (8, 13), (9, 5), (9, 13), (10, 15)]
