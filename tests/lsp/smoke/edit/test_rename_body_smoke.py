"""References and rename across body expressions: locals, parameters,
`for` variables, lambda parameters, match bindings, captured bindings,
fields, methods, functions, enum variants and types, from the
declaration or from any use, with exact name ranges."""
from __future__ import annotations

import asyncio

import pytest
from lsprotocol import types

# Every expected occurrence is marked «...»; the cursor goes on the first
# or the last one.
CASES = [
    ("param", 'func f(«width»: Int) -> Int {\n    val a = «width» * 2\n    return «width» + a\n}\n'
              'func g(width: Int) -> Int { return width }\n'),
    ("local", 'func main() {\n    val «n» = 1\n    println("{«n»}")\n    val m = «n» + 1\n}\n'
              'func h() {\n    val n = 2\n    println("{n}")\n}\n'),
    ("var_assign", 'func main() {\n    var «t» = 0\n    «t» += 1\n    «t» = «t» * 2\n    println("{«t»}")\n}\n'),
    ("for_var", 'func main() {\n    for «it» in [1, 2] {\n        println("{«it»}")\n        val z = «it» + 1\n    }\n}\n'),
    ("lambda_param", 'func main() {\n    val xs = [1]\n'
                     '    val ys = xs.map(func(«e»: Int) -> Int { return «e» * «e» })\n}\n'),
    ("match_binding", 'enum S { A(Int), B }\nfunc f(s: S) -> Int {\n    match s {\n'
                      '        S.A(«r») -> return «r» * «r»\n        S.B -> return 0\n    }\n    return 0\n}\n'),
    ("captured", 'func main() {\n    val «k» = 3\n    val f = func(x: Int) -> Int { return x + «k» }\n'
                 '    println("{f(1)}")\n}\n'),
    ("field", 'object P {\n    val «x»: Int\n    readonly func get() -> Int { return self.«x» }\n}\n'
              'func main() {\n    val p = P(1)\n    println("{p.«x»}")\n}\n'),
    ("method", 'object P {\n    val x: Int\n    readonly func «get»() -> Int { return self.x }\n}\n'
               'func main() {\n    val p = P(1)\n    println("{p.«get»()}")\n    val q = p.«get»()\n}\n'),
    ("function", 'func «area»(w: Int) -> Int { return w }\nfunc main() {\n    val xs = [1]\n'
                 '    val ys = xs.map(func(x: Int) -> Int { return «area»(x) })\n    println("{«area»(2)}")\n}\n'),
    ("enum_variant", 'enum S { «A»(Int), B }\nfunc f(s: S) -> Int {\n    match s {\n'
                     '        S.«A»(r) -> return r\n        S.B -> return 0\n    }\n    return 0\n}\n'
                     'func main() {\n    val s = S.«A»(1)\n}\n'),
    ("type", 'object «P» {\n    val x: Int\n}\nfunc mk() -> «P» { return «P»(1) }\nfunc use(p: «P») { }\n'),
]


def parse(src):
    text, marks, i = "", [], 0
    while i < len(src):
        if src[i] == "«":
            j = src.index("»", i)
            start = len(text)
            text += src[i + 1:j]
            marks.append((start, len(text)))
            i = j + 1
        else:
            text += src[i]
            i += 1

    def pos(off):
        line = text[:off].count("\n")
        return (line, off - (text[:off].rfind("\n") + 1))
    return text, sorted((pos(a), pos(b)) for a, b in marks)


async def open_doc(client, tmp_path, text):
    fp = tmp_path / "rename_body.iron"
    fp.write_text(text, encoding="utf-8")
    uri = fp.as_uri()
    client.text_document_did_open(types.DidOpenTextDocumentParams(
        text_document=types.TextDocumentItem(uri=uri, language_id="iron", version=1, text=text)))
    return uri


def rng(r):
    return ((r.start.line, r.start.character), (r.end.line, r.end.character))


@pytest.mark.asyncio
@pytest.mark.parametrize("which", [0, -1], ids=["from_first", "from_last"])
@pytest.mark.parametrize("name,src", CASES, ids=[c[0] for c in CASES])
async def test_references(client, tmp_path, name, src, which):
    text, marks = parse(src)
    uri = await open_doc(client, tmp_path, text)
    (line, ch), _ = marks[which]
    refs = await asyncio.wait_for(client.text_document_references_async(types.ReferenceParams(
        text_document=types.TextDocumentIdentifier(uri=uri),
        position=types.Position(line=line, character=ch),
        context=types.ReferenceContext(include_declaration=True))), timeout=10.0)
    assert sorted(rng(r.range) for r in refs or [] if r.uri == uri) == marks


@pytest.mark.asyncio
@pytest.mark.parametrize("which", [0, -1], ids=["from_first", "from_last"])
@pytest.mark.parametrize("name,src", CASES, ids=[c[0] for c in CASES])
async def test_rename(client, tmp_path, name, src, which):
    text, marks = parse(src)
    uri = await open_doc(client, tmp_path, text)
    (line, ch), _ = marks[which]
    doc = types.TextDocumentIdentifier(uri=uri)
    pos = types.Position(line=line, character=ch)
    prep = await asyncio.wait_for(client.text_document_prepare_rename_async(
        types.PrepareRenameParams(text_document=doc, position=pos)), timeout=10.0)
    assert prep is not None, "prepareRename refused"
    prep_range = getattr(prep, "range", prep)
    assert rng(prep_range) == marks[which]
    edit = await asyncio.wait_for(client.text_document_rename_async(
        types.RenameParams(text_document=doc, position=pos, new_name="zz")), timeout=10.0)
    assert edit is not None
    edits = (edit.changes or {}).get(uri) or [
        e for dc in (edit.document_changes or []) for e in dc.edits]
    assert sorted(rng(e.range) for e in edits) == marks
    assert all(e.new_text == "zz" for e in edits)


@pytest.mark.asyncio
async def test_stdlib_member_is_not_renamed(client, tmp_path):
    text = 'func main() {\n    val s = "abc"\n    val t = s.upper()\n}\n'
    uri = await open_doc(client, tmp_path, text)
    doc = types.TextDocumentIdentifier(uri=uri)
    pos = types.Position(line=2, character=15)
    prep = await asyncio.wait_for(client.text_document_prepare_rename_async(
        types.PrepareRenameParams(text_document=doc, position=pos)), timeout=10.0)
    assert prep is None
    edit = await asyncio.wait_for(client.text_document_rename_async(
        types.RenameParams(text_document=doc, position=pos, new_name="zz")), timeout=10.0)
    edits = [] if edit is None else ((edit.changes or {}).get(uri) or [
        e for dc in (edit.document_changes or []) for e in dc.edits])
    assert edits == []
    # References still find its uses in the file.
    refs = await asyncio.wait_for(client.text_document_references_async(types.ReferenceParams(
        text_document=doc, position=pos, context=types.ReferenceContext(include_declaration=False))),
        timeout=10.0)
    assert [rng(r.range) for r in refs or [] if r.uri == uri] == [((2, 14), (2, 19))]
