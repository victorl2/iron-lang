"""Smoke: the editor reports the errors `ironc check` reports (#361).

`ironc check` parses with the strict v3 checks on (E0260..E0264: removed
receiver-method syntax, `mut`, inline field defaults, an object with
mutable fields and no init). ironls used to parse leniently and showed
none of them. Each file below gets the same error codes from both.
"""
from __future__ import annotations

import asyncio
import os
import re
import subprocess

import pytest
from lsprotocol import types


_CASES = {
    "E0264": (
        "object Point {\n"
        "    var x: Float\n"
        "    val y: Float\n"
        "}\n"
        "\n"
        "func main() {\n"
        "}\n"
    ),
    "E0262": (
        "object Counter {\n"
        "    var n: Int = 0\n"
        "\n"
        "    init() {\n"
        "        self.n = 0\n"
        "    }\n"
        "}\n"
        "\n"
        "func main() {\n"
        "}\n"
    ),
}


def _ironc_codes(lsp_binary: str, path) -> set[str] | None:
    """Error codes `ironc check` reports for `path`, or None when there is
    no ironc next to ironls."""
    ironc = os.path.join(os.path.dirname(lsp_binary),
                         "ironc.exe" if os.name == "nt" else "ironc")
    if not os.path.isfile(ironc):
        return None
    out = subprocess.run([ironc, "check", str(path)], capture_output=True,
                         text=True, timeout=60)
    text = out.stdout + out.stderr
    return {f"E{c}" for c in re.findall(r"error\[E(\d+)\]", text)}


@pytest.mark.asyncio
@pytest.mark.parametrize("code", sorted(_CASES))
async def test_editor_reports_strict_error(client, lsp_binary, tmp_path, code):
    path = tmp_path / f"strict_{code}.iron"
    path.write_text(_CASES[code], encoding="utf-8")
    uri = path.as_uri()
    client.text_document_did_open(
        types.DidOpenTextDocumentParams(
            text_document=types.TextDocumentItem(
                uri=uri, language_id="iron", version=1, text=_CASES[code],
            ),
        ),
    )
    await asyncio.wait_for(
        client.wait_for_notification(types.TEXT_DOCUMENT_PUBLISH_DIAGNOSTICS),
        timeout=5.0,
    )
    diags = client.diagnostics.get(uri, [])
    editor_codes = {
        d.code for d in diags
        if d.severity == types.DiagnosticSeverity.Error
    }
    assert code in editor_codes, (
        f"ironls did not report {code}; got {[(d.code, d.message) for d in diags]!r}")

    cli_codes = _ironc_codes(lsp_binary, path)
    if cli_codes is not None:
        assert code in cli_codes, f"ironc check no longer reports {code}: {cli_codes!r}"
        assert editor_codes == cli_codes, (
            f"editor errors {sorted(editor_codes)} != ironc check errors {sorted(cli_codes)}")
