"""Completion inside function bodies: the names in scope at the cursor
(parameters, bindings, `for` variables, lambda parameters, match
bindings), the stdlib prelude's types and the compiler's builtin
functions, and member completion on more receivers."""
from __future__ import annotations

import asyncio

import pytest
from lsprotocol import types

# (source with `|` at the cursor, labels that must appear, labels that must not)
CASES = [
    # Bindings of the enclosing function only, declared before the cursor.
    ('func f(width: Int) -> Int {\n    val area = width * 2\n    return |\n}\n'
     'func g(height: Int) -> Int {\n    val volume = height\n    return volume\n}\n',
     {"width", "area"}, {"height", "volume"}),
    ('func main() {\n    val before = 1\n    val x = |\n    val after = 2\n}\n',
     {"before"}, {"after", "x"}),
    ('func main() {\n    for item in [1, 2] {\n        val z = it|\n    }\n}\n', {"item"}, set()),
    ('func main() {\n    for item in [1, 2] {\n        println("{item}")\n    }\n    val z = it|\n}\n',
     set(), {"item"}),
    ('func main() {\n    val xs = [1, 2]\n    val ys = xs.map(func(elem: Int) -> Int { return el| })\n}\n',
     {"elem"}, set()),
    ('enum S { A(Int), B }\nfunc f(s: S) -> Int {\n    match s {\n'
     '        S.A(radius) -> return rad|\n        S.B -> return 0\n    }\n    return 0\n}\n',
     {"radius"}, set()),
    ('func main() {\n    if true {\n        val inner = 1\n    }\n    val y = inn|\n}\n',
     set(), {"inner"}),
    # The prelude and the builtins.
    ('func main() {\n    print|\n}\n', {"println", "print"}, set()),
    ('func main() {\n    var m = Ma|\n}\n', {"Map"}, set()),
    ('import math\nfunc main() {\n    val r = Ma|\n}\n', {"Math"}, set()),
    ('func f(m: Ma|) {\n}\n', {"Map"}, set()),
    # A method is reached through its receiver, never by its bare name.
    ('object P {\n    val x: Int\n    func area() -> Int { return self.x }\n}\n'
     'func main() {\n    val p = P(1)\n    a|\n}\n', set(), {"area"}),
    # Member completion on an indexed receiver.
    ('object P {\n    val x: Int\n}\nfunc main() {\n    val ps = [P(1)]\n    ps[0].|\n}\n',
     {"x"}, set()),
]


async def complete(client, tmp_path, src):
    offset = src.index("|")
    text = src.replace("|", "")
    line = text[:offset].count("\n")
    character = offset - (text[:offset].rfind("\n") + 1)
    fp = tmp_path / "body.iron"
    fp.write_text(text, encoding="utf-8")
    uri = fp.as_uri()
    client.text_document_did_open(types.DidOpenTextDocumentParams(
        text_document=types.TextDocumentItem(uri=uri, language_id="iron", version=1, text=text)))
    result = await asyncio.wait_for(client.text_document_completion_async(types.CompletionParams(
        text_document=types.TextDocumentIdentifier(uri=uri),
        position=types.Position(line=line, character=character))), timeout=10.0)
    return result.items if hasattr(result, "items") else (result or [])


@pytest.mark.asyncio
@pytest.mark.parametrize("src,want,unwanted", CASES)
async def test_body_completion(client, tmp_path, src, want, unwanted):
    labels = {i.label for i in await complete(client, tmp_path, src)}
    assert want <= labels, f"missing {sorted(want - labels)}; got {sorted(labels)[:60]}"
    assert not (unwanted & labels), f"unexpected {sorted(unwanted & labels)}"


@pytest.mark.asyncio
async def test_details_carry_types_and_signatures(client, tmp_path):
    src = ('object Vec {\n    val x: Int\n'
           '    readonly func length_sq() -> Int { return self.x * self.x }\n}\n'
           'func f(count: Int) {\n    val xs = [1]\n    var v = Vec(1)\n    v.|\n}\n')
    items = {i.label: i for i in await complete(client, tmp_path, src)}
    assert items["length_sq"].detail == "readonly func length_sq() -> Int"
    src = 'func f(count: Int) {\n    val xs = ["a"]\n    xs.pu|\n}\n'
    items = {i.label: i for i in await complete(client, tmp_path, src)}
    # The list's element type is written in for T.
    assert items["push"].detail == "func push(item: String)"
    src = 'func f(count: Int) {\n    val total = count * 2\n    co|\n}\n'
    items = {i.label: i for i in await complete(client, tmp_path, src)}
    assert items["count"].detail == "count: Int"
    src = 'func f(count: Int) {\n    val total = count * 2\n    to|\n}\n'
    items = {i.label: i for i in await complete(client, tmp_path, src)}
    assert items["total"].detail == "val total: Int"
    src = 'func f() {\n    printl|\n}\n'
    items = {i.label: i for i in await complete(client, tmp_path, src)}
    assert items["println"].detail == "func println(value: String)"
