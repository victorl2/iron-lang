"""Signature help on method calls: stdlib methods, statics and user
methods, with the implicit receiver left out of the parameter list."""
from __future__ import annotations

import asyncio

import pytest
from lsprotocol import types

CASES = [
    ('func main() {\n    val s = "abc"\n    val t = s.replace(|)\n}\n',
     "readonly func String.replace(old: String, new: String) -> String", 0),
    ('import math\nfunc main() {\n    val r = Math.pow(2.0, |)\n}\n',
     "readonly func Math.pow(base: Float, exp: Float) -> Float", 1),
    ('object P {\n    val x: Int\n    func moved(dx: Int, dy: Int) -> P { return P(self.x + dx) }\n}\n'
     'func main() {\n    val p = P(1)\n    val q = p.moved(1, |)\n}\n',
     "func P.moved(dx: Int, dy: Int) -> P", 1),
]


@pytest.mark.asyncio
@pytest.mark.parametrize("src,label,active", CASES)
async def test_method_signature_help(client, tmp_path, src, label, active):
    offset = src.index("|")
    text = src.replace("|", "")
    line = text[:offset].count("\n")
    character = offset - (text[:offset].rfind("\n") + 1)
    fp = tmp_path / "sig.iron"
    fp.write_text(text, encoding="utf-8")
    uri = fp.as_uri()
    client.text_document_did_open(types.DidOpenTextDocumentParams(
        text_document=types.TextDocumentItem(uri=uri, language_id="iron", version=1, text=text)))
    result = await asyncio.wait_for(client.text_document_signature_help_async(
        types.SignatureHelpParams(text_document=types.TextDocumentIdentifier(uri=uri),
                                  position=types.Position(line=line, character=character))),
        timeout=10.0)
    assert result and result.signatures, "no signature"
    assert result.signatures[0].label == label
    assert result.active_parameter == active
