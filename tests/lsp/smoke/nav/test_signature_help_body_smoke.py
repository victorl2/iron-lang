"""Signature help while typing in a body: calls not closed yet, calls
nested in lambdas, interpolations, list literals and match arms, the
compiler's builtins (`println`, `xs.push`, `m.put`), constructions and
calls through a binding holding a function."""
from __future__ import annotations

import asyncio

import pytest
from lsprotocol import types

AREA = "func area(w: Int, h: Int) -> Int { return w * h }\n"
G = "func g(k: Int) -> Int { return k }\n"

# (source with `|` at the cursor, expected label, expected active parameter)
CASES = [
    (AREA + "func main() {\n    val a = area(|\n}\n", "func area(w: Int, h: Int) -> Int", 0),
    (AREA + "func main() {\n    val a = area(1, |\n}\n", "func area(w: Int, h: Int) -> Int", 1),
    (AREA + 'func main() {\n    val a = area(1, |\n    println("x")\n}\n',
     "func area(w: Int, h: Int) -> Int", 1),
    ('func main() {\n    val s = "abc"\n    val t = s.replace(|\n}\n',
     "readonly func String.replace(old: String, new: String) -> String", 0),
    (AREA + G + "func main() {\n    val a = area(g(|), 2)\n}\n", "func g(k: Int) -> Int", 0),
    (AREA + G + "func main() {\n    val a = area(g(1), |)\n}\n", "func area(w: Int, h: Int) -> Int", 1),
    (G + "func main() {\n    val xs = [1]\n    val ys = xs.map(func(x: Int) -> Int { return g(|) })\n}\n",
     "func g(k: Int) -> Int", 0),
    (G + 'func main() {\n    println("{g(|)}")\n}\n', "func g(k: Int) -> Int", 0),
    (G + "func main() {\n    val xs = [g(|)]\n}\n", "func g(k: Int) -> Int", 0),
    (G + "enum S { A(Int), B }\nfunc f(s: S) -> Int {\n    match s {\n"
     "        S.A(r) -> return g(|)\n        S.B -> return 0\n    }\n    return 0\n}\n",
     "func g(k: Int) -> Int", 0),
    # A comma inside a string literal does not move the active parameter.
    ('func area(w: String, h: Int, d: Int) -> Int { return h }\nfunc main() {\n'
     '    val a = area("a,b", |)\n}\n', "func area(w: String, h: Int, d: Int) -> Int", 1),
    # Builtins, with the receiver's element types.
    ("func main() {\n    println(|)\n}\n", "func println(value: String)", 0),
    ("func main() {\n    var xs = [1]\n    xs.push(|)\n}\n", "func push(item: Int)", 0),
    ('func main() {\n    var m = Map[String, Int]()\n    m.put("a", |)\n}\n',
     "func put(key: String, value: Int)", 1),
    # A construction lists the object's fields.
    ("object P {\n    val x: Int\n    val y: Int\n}\nfunc main() {\n    val p = P(1, |)\n}\n",
     "P(x: Int, y: Int)", 1),
    # A list extension method shows `[T]`, not the internal type name.
    ("func main() {\n    val xs = [1]\n    val ys = xs.filter(|)\n}\n", "func [T].filter(", 0),
    # A binding holding a function.
    ("func main() {\n    val double = func(x: Int) -> Int { return x * 2 }\n    val y = double(|)\n}\n",
     "func double(Int) -> Int", 0),
]


@pytest.mark.asyncio
@pytest.mark.parametrize("src,label,active", CASES)
async def test_signature_help_in_body(client, tmp_path, src, label, active):
    offset = src.index("|")
    text = src.replace("|", "")
    line = text[:offset].count("\n")
    character = offset - (text[:offset].rfind("\n") + 1)
    fp = tmp_path / "sig_body.iron"
    fp.write_text(text, encoding="utf-8")
    uri = fp.as_uri()
    client.text_document_did_open(types.DidOpenTextDocumentParams(
        text_document=types.TextDocumentItem(uri=uri, language_id="iron", version=1, text=text)))
    result = await asyncio.wait_for(client.text_document_signature_help_async(
        types.SignatureHelpParams(text_document=types.TextDocumentIdentifier(uri=uri),
                                  position=types.Position(line=line, character=character))),
        timeout=10.0)
    assert result and result.signatures, "no signature"
    sig = result.signatures[result.active_signature or 0]
    assert sig.label.startswith(label), sig.label
    assert result.active_parameter == active
    # Every parameter range lies inside the label.
    for p in sig.parameters or []:
        if isinstance(p.label, (list, tuple)):
            assert 0 <= p.label[0] < p.label[1] <= len(sig.label)


@pytest.mark.asyncio
async def test_no_signature_outside_a_call(client, tmp_path):
    text = AREA + "func main() {\n    val a = area(1, 2)\n    val b = 3\n}\n"
    fp = tmp_path / "sig_none.iron"
    fp.write_text(text, encoding="utf-8")
    uri = fp.as_uri()
    client.text_document_did_open(types.DidOpenTextDocumentParams(
        text_document=types.TextDocumentItem(uri=uri, language_id="iron", version=1, text=text)))
    result = await asyncio.wait_for(client.text_document_signature_help_async(
        types.SignatureHelpParams(text_document=types.TextDocumentIdentifier(uri=uri),
                                  position=types.Position(line=3, character=12))), timeout=10.0)
    assert not (result and result.signatures)
