"""documentSymbol ranges, selectionRange and nesting; folding without
duplicates (#362).

- range covers the declaration and nothing after it (a field's range
  used to run into the next declaration);
- selectionRange is the declaration's name, inside range (it used to be
  the whole range);
- an object's methods are children of the object, not top-level
  `Type.method` symbols;
- foldingRange returns each region once (a function used to come back
  twice, once for the declaration and once for its body).
"""
from __future__ import annotations

import asyncio

import pytest
from lsprotocol import types

SOURCE = """object Point {
    val x: Float
    val y: Float

    readonly func length_sq() -> Float {
        return self.x * self.x + self.y * self.y
    }
}

func area(width: Float, height: Float) -> Float {
    return width * height
}

enum Shape {
    Circle,
    Square,
}

func main() {
    val p = Point(1.0, 2.0)
    println("{p.length_sq()} {area(2.0, 3.0)}")
}

interface Sized {
    func size() -> Float
}
"""
LINES = SOURCE.split("\n")


def _name_range(line0: int, name: str, nth: int = 0) -> tuple:
    col = -1
    for _ in range(nth + 1):
        col = LINES[line0].index(name, col + 1)
    return ((line0, col), (line0, col + len(name)))


def _rng(r: types.Range) -> tuple:
    return ((r.start.line, r.start.character), (r.end.line, r.end.character))


def _contains(outer: types.Range, inner: types.Range) -> bool:
    o, i = _rng(outer), _rng(inner)
    return o[0] <= i[0] and i[1] <= o[1]


async def _open(client, tmp_path) -> str:
    path = tmp_path / "symbols.iron"
    path.write_text(SOURCE, encoding="utf-8")
    uri = path.as_uri()
    client.text_document_did_open(types.DidOpenTextDocumentParams(
        text_document=types.TextDocumentItem(
            uri=uri, language_id="iron", version=1, text=SOURCE)))
    await asyncio.wait_for(
        client.wait_for_notification(types.TEXT_DOCUMENT_PUBLISH_DIAGNOSTICS),
        timeout=10.0)
    return uri


async def _symbols(client, uri):
    syms = await client.text_document_document_symbol_async(
        types.DocumentSymbolParams(text_document=types.TextDocumentIdentifier(uri=uri)))
    assert syms, "no document symbols"
    assert all(isinstance(s, types.DocumentSymbol) for s in syms), syms
    return {s.name: s for s in syms}


def _walk(syms, parent=None):
    for s in syms:
        yield s, parent
        yield from _walk(s.children or [], s)


@pytest.mark.asyncio
async def test_document_symbol_ranges(client, tmp_path):
    uri = await _open(client, tmp_path)
    top = await _symbols(client, uri)

    assert set(top) == {"Point", "area", "Shape", "main", "Sized"}, sorted(top)

    point = top["Point"]
    assert _rng(point.range) == ((0, 0), (7, 1))
    assert _rng(point.selection_range) == _name_range(0, "Point")

    kids = {c.name: c for c in point.children or []}
    assert set(kids) == {"x", "y", "length_sq"}, sorted(kids)

    # Fields end where their declaration ends, on their own line.
    assert _rng(kids["x"].range) == ((1, 4), (1, 16))
    assert _rng(kids["x"].selection_range) == _name_range(1, "x")
    assert _rng(kids["y"].range) == ((2, 4), (2, 16))
    assert _rng(kids["y"].selection_range) == _name_range(2, "y")

    m = kids["length_sq"]
    assert m.kind == types.SymbolKind.Method
    assert _rng(m.range) == ((4, 4), (6, 5))
    assert _rng(m.selection_range) == _name_range(4, "length_sq")

    area = top["area"]
    assert area.kind == types.SymbolKind.Function
    assert _rng(area.range) == ((9, 0), (11, 1))
    assert _rng(area.selection_range) == _name_range(9, "area")

    shape = top["Shape"]
    assert _rng(shape.selection_range) == _name_range(13, "Shape")
    variants = {c.name: c for c in shape.children or []}
    assert _rng(variants["Circle"].selection_range) == _name_range(14, "Circle")

    # An interface method ends with its signature, not at the next token.
    sig = (top["Sized"].children or [None])[0]
    assert sig is not None and sig.name == "size"
    assert _rng(sig.range) == ((24, 4), (24, 24))
    assert _rng(sig.selection_range) == _name_range(24, "size")

    # LSP: selectionRange inside range; children inside their parent;
    # siblings do not overlap.
    for s, parent in _walk(list(top.values())):
        assert _contains(s.range, s.selection_range), s.name
        if parent is not None:
            assert _contains(parent.range, s.range), (parent.name, s.name)
    for s, _ in _walk(list(top.values())):
        kids = sorted(s.children or [], key=lambda c: _rng(c.range))
        for a, b in zip(kids, kids[1:]):
            assert _rng(a.range)[1] <= _rng(b.range)[0], (a.name, b.name)


@pytest.mark.asyncio
async def test_folding_ranges_not_duplicated(client, tmp_path):
    uri = await _open(client, tmp_path)
    folds = await client.text_document_folding_range_async(
        types.FoldingRangeParams(text_document=types.TextDocumentIdentifier(uri=uri)))
    assert folds, "no folding ranges"
    spans = [(f.start_line, f.end_line) for f in folds]
    assert len(spans) == len(set(spans)), f"duplicate folding ranges: {sorted(spans)}"
    # The object, its method, both functions, the enum and the interface.
    for want in [(0, 7), (4, 6), (9, 11), (13, 16), (18, 21), (23, 25)]:
        assert any(s[0] == want[0] and s[1] in (want[1], want[1] - 1) for s in spans), (
            want, sorted(spans))
