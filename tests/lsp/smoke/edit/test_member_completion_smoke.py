"""Completion after `recv.` lists the receiver's members, whatever the
receiver is: a local or parameter of any type, a field chain, a call
result, a literal, or a type name for its statics and variants."""
from __future__ import annotations

import asyncio

import pytest
from lsprotocol import types

from request_timeout import REQUEST_TIMEOUT_SECONDS

CASES = [
    # (source with `|` at the cursor, labels that must appear, labels that must not)
    ('func main() {\n    val s = "abc"\n    val t = s.|\n}\n', {"upper", "len", "split"}, {"push"}),
    ('func main() {\n    val xs = [1, 2]\n    xs.|\n}\n', {"push", "len", "map", "filter", "sum"}, {"upper"}),
    ('func main() {\n    var m = Map[String, Int]()\n    m.|\n}\n', {"put", "get", "has", "keys"}, {"inner"}),
    ('func main() {\n    var st = Set[Int]()\n    st.|\n}\n', {"add", "has", "values"}, set()),
    ('import math\nfunc main() {\n    val r = Math.|\n}\n', {"sqrt", "PI", "pow"}, set()),
    ('enum Color { Red, Green }\nfunc main() {\n    val c = Color.|\n}\n', {"Red", "Green"}, set()),
    ('object P {\n    val x: Int\n    var y: Int\n}\nfunc f(p: P) -> Int {\n    return p.|\n}\n', {"x", "y"}, set()),
    ('object Q {\n    val name: String\n}\nobject P {\n    val q: Q\n}\nfunc f(p: P) {\n    println(p.q.name.|)\n}\n', {"upper"}, set()),
    ('func g() -> [Int] { return [1] }\nfunc main() {\n    val n = g().|\n}\n', {"len", "push"}, set()),
    ('func main() {\n    println("x".|)\n}\n', {"upper", "len"}, set()),
    ('func main() {\n    val x = UInt32(7)\n    val y = x.|\n}\n',
     {"leading_zeros", "rotate_left", "mul_wide", "add_overflow"}, {"from_bits"}),
    ('func f(v: View[Int]) -> Int {\n    return v.|\n}\n',
     {"len", "slice", "index_of", "to_list"}, {"fill"}),
    ('func f(v: ViewMut[Int]) {\n    v.|\n}\n',
     {"len", "slice", "fill", "copy_from", "as_view"}, set()),
    ('func main() {\n    val x = Float.|\n}\n', {"from_bits"}, {"to_bits"}),
    ('func main() {\n    val x = Hint.|\n}\n', {"likely", "unlikely"}, set()),
    ('func main() {\n    var b = StringBuilder.new()\n    b.|\n}\n',
     {"push_byte", "push_str", "spare", "commit", "finish", "bytes"}, set()),
]


@pytest.mark.asyncio
@pytest.mark.parametrize("src,want,unwanted", CASES)
async def test_member_completion(client, tmp_path, src, want, unwanted):
    offset = src.index("|")
    text = src.replace("|", "")
    line = text[:offset].count("\n")
    character = offset - (text[:offset].rfind("\n") + 1)
    fp = tmp_path / "member.iron"
    fp.write_text(text, encoding="utf-8")
    uri = fp.as_uri()
    client.text_document_did_open(types.DidOpenTextDocumentParams(
        text_document=types.TextDocumentItem(uri=uri, language_id="iron", version=1, text=text)))
    result = await asyncio.wait_for(client.text_document_completion_async(types.CompletionParams(
        text_document=types.TextDocumentIdentifier(uri=uri),
        position=types.Position(line=line, character=character),
        context=types.CompletionContext(trigger_kind=types.CompletionTriggerKind.TriggerCharacter,
                                        trigger_character="."))),
        timeout=REQUEST_TIMEOUT_SECONDS)
    items = result.items if hasattr(result, "items") else (result or [])
    labels = {i.label for i in items}
    assert want <= labels, f"missing {sorted(want - labels)}; got {sorted(labels)}"
    assert not (unwanted & labels), f"unexpected {sorted(unwanted & labels)}"
