"""A nocopy capture that is not a borrow (#512) is E0286 with a label, a
note, a help naming `rc` and the declaration as a second location. Editors
mostly show only the message, so it must carry all of that text, and
`ironc check` and the server must agree on it."""
from __future__ import annotations

import asyncio
import pytest
from lsprotocol import types

_SOURCE = (
    "nocopy object Res {\n"
    "    val id: Int\n"
    "}\n"
    "func main() {\n"
    "    val h = Res(1)\n"
    "    val reader = func() -> Int { return h.id }\n"
    "    println(\"{reader()}\")\n"
    "}\n"
)


@pytest.mark.asyncio
async def test_nocopy_capture_full_message(client, tmp_path):
    uri = (tmp_path / "nocopy_capture.iron").as_uri()
    client.text_document_did_open(
        types.DidOpenTextDocumentParams(
            text_document=types.TextDocumentItem(
                uri=uri, language_id="iron", version=1, text=_SOURCE,
            ),
        ),
    )
    await asyncio.wait_for(
        client.wait_for_notification(types.TEXT_DOCUMENT_PUBLISH_DIAGNOSTICS),
        timeout=5.0,
    )
    diags = [d for d in client.diagnostics.get(uri, []) if str(d.code).endswith("286")]
    assert len(diags) == 1, f"expected one E0286, got {client.diagnostics.get(uri, [])!r}"
    d = diags[0]
    m = d.message
    assert "closure captures the nocopy value `h`, but the closure can outlive it" in m, m
    assert "captured here by a closure that is stored in `reader`" in m, m
    assert "note: a nocopy value can only be borrowed" in m, m
    assert "val h = rc Res(1)" in m, m
    # the primary range is the capture (line 6, `h` in `return h.id`)
    assert d.range.start.line == 5, d.range
    rel = d.related_information or []
    decl = [r for r in rel if "is declared here" in r.message]
    assert decl, rel
    assert decl[0].location.range.start.line == 4, decl[0].location.range
    assert decl[0].location.range.start.character == 8, decl[0].location.range
