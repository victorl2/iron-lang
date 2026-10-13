"""Smoke: language server bugs found by the editing-session fuzzer
(tests/fuzz/lsp_fuzz.py). Each test is the reduced session.
"""
from __future__ import annotations

import asyncio
import os

import pytest
from lsprotocol import types

from conftest import _default_init_params, _install_register_capability_handler


def _open(client, uri, text, version=1):
    client.text_document_did_open(types.DidOpenTextDocumentParams(
        text_document=types.TextDocumentItem(uri=uri, language_id="iron",
                                             version=version, text=text)))


async def _diagnostics_for_version(client, uri, version, timeout=10.0):
    """The diagnostics published for `uri` at `version`; fails when the
    server publishes no diagnostics with that version."""
    loop = asyncio.get_running_loop()
    end = loop.time() + timeout
    seen = []
    while loop.time() < end:
        try:
            params = await asyncio.wait_for(
                client.wait_for_notification(types.TEXT_DOCUMENT_PUBLISH_DIAGNOSTICS),
                timeout=max(0.1, end - loop.time()))
        except asyncio.TimeoutError:
            break
        if params.uri != uri:
            continue
        seen.append(params.version)
        if params.version == version:
            return params.diagnostics
    pytest.fail(f"no diagnostics for version {version}; published versions {seen}")


def _line_units(line, enc):
    if enc == "utf-8":
        return len(line.encode("utf-8"))
    return len(line.encode("utf-16-le")) // 2


def _assert_in_document(text, rng, enc):
    lines = text.replace("\r\n", "\n").split("\n")
    for p in (rng.start, rng.end):
        assert p.line < len(lines), f"{rng} past the last line {len(lines) - 1}"
        assert p.character <= _line_units(lines[p.line], enc), \
            f"{rng} past the end of line {p.line} {lines[p.line]!r}"


@pytest.mark.asyncio
async def test_several_changes_in_one_did_change_keep_the_version(client, tmp_path):
    # Events of one didChange make one version. The server used to give the
    # document the version of its last internal step (3 here), so every
    # later diagnostic carried a version the client never sent.
    uri = (tmp_path / "multi.iron").as_uri()
    _open(client, uri, "func main() {\n    val x = 1\n}\n")
    await _diagnostics_for_version(client, uri, 1)
    client.text_document_did_change(types.DidChangeTextDocumentParams(
        text_document=types.VersionedTextDocumentIdentifier(uri=uri, version=2),
        content_changes=[
            types.TextDocumentContentChangePartial(
                range=types.Range(start=types.Position(line=1, character=12),
                                  end=types.Position(line=1, character=13)),
                text="2"),
            types.TextDocumentContentChangePartial(
                range=types.Range(start=types.Position(line=1, character=8),
                                  end=types.Position(line=1, character=9)),
                text="y"),
        ]))
    await _diagnostics_for_version(client, uri, 2)


@pytest.mark.asyncio
async def test_utf8_positions_count_bytes(client, tmp_path):
    # The default client fixture negotiates utf-8: LSP counts UTF-8 code
    # units (bytes) then. The server counted code points.
    uri = (tmp_path / "utf8.iron").as_uri()
    text = 'func main() {\n    val s = "é😀" + zz\n}\n'
    _open(client, uri, text)
    diags = await _diagnostics_for_version(client, uri, 1)
    undefined = [d for d in diags if d.code == "E0200"]
    assert undefined, diags
    line = text.split("\n")[1]
    want = len(line[:line.index("zz")].encode("utf-8"))
    assert undefined[0].range.start.character == want, (undefined[0].range, want)


async def _utf16_client(lsp_binary):
    from pytest_lsp import ClientServerConfig
    c = await ClientServerConfig(server_command=[lsp_binary]).start()
    _install_register_capability_handler(c)
    params = _default_init_params()
    params.capabilities.general.position_encodings = [types.PositionEncodingKind.Utf16]
    await c.initialize_session(params)
    return c


async def _utf16_workspace_client(lsp_binary, root):
    from pytest_lsp import ClientServerConfig
    c = await ClientServerConfig(server_command=[lsp_binary]).start()
    _install_register_capability_handler(c)
    params = _default_init_params()
    params.capabilities.general.position_encodings = [types.PositionEncodingKind.Utf16]
    params.root_uri = root.as_uri()
    params.workspace_folders = [
        types.WorkspaceFolder(uri=root.as_uri(), name="range-regression")]
    await c.initialize_session(params)
    return c


async def _stop(c):
    try:
        await asyncio.wait_for(c.shutdown_session(), timeout=3.0)
        await asyncio.wait_for(c.stop(), timeout=3.0)
    except Exception:
        pass


@pytest.mark.asyncio
async def test_selection_range_stays_in_the_document(lsp_binary, tmp_path):
    # The module rung ended one character past the empty last line, and
    # positions and ranges were byte columns whatever the encoding.
    c = await _utf16_client(lsp_binary)
    try:
        uri = (tmp_path / "sel.iron").as_uri()
        text = 'func main() {\n    val s = "😀😀"\n    val t = s\n}\n'
        _open(c, uri, text)
        res = await c.text_document_selection_range_async(types.SelectionRangeParams(
            text_document=types.TextDocumentIdentifier(uri=uri),
            positions=[types.Position(line=1, character=16),
                       types.Position(line=2, character=12)]))
        assert res and len(res) == 2
        for sr in res:
            child = None
            while sr is not None:
                _assert_in_document(text, sr.range, "utf-16")
                if child is not None:
                    a, b = child.range, sr.range
                    assert (b.start.line, b.start.character) <= (a.start.line, a.start.character)
                    assert (a.end.line, a.end.character) <= (b.end.line, b.end.character)
                child, sr = sr, sr.parent
        # `t` on line 2 sits after no wide character; `s` in the string
        # line is after two surrogate pairs: its rung starts at their end.
        assert res[1].range.start == types.Position(line=2, character=12)
    finally:
        await _stop(c)


@pytest.mark.asyncio
async def test_workspace_symbol_uses_current_open_buffer(lsp_binary, tmp_path):
    # The workspace index used the file as warm-seeded from disk after an
    # open buffer changed. Its stale symbol range could then point past the
    # end of a shorter current line (#812).
    path = tmp_path / "symbols.iron"
    disk_text = "pub func a_very_long_name() {}\n"
    path.write_text(disk_text, encoding="utf-8")
    c = await _utf16_workspace_client(lsp_binary, tmp_path)
    try:
        uri = path.as_uri()
        _open(c, uri, disk_text)
        await _diagnostics_for_version(c, uri, 1)

        current = "pub func x() {}\n"
        c.text_document_did_change(types.DidChangeTextDocumentParams(
            text_document=types.VersionedTextDocumentIdentifier(uri=uri, version=2),
            content_changes=[types.TextDocumentContentChangeWholeDocument(text=current)]))
        await _diagnostics_for_version(c, uri, 2)

        result = await asyncio.wait_for(c.workspace_symbol_async(
            types.WorkspaceSymbolParams(query="")), timeout=10.0)
        own = [s for s in result or [] if s.location.uri == uri]
        assert [s.name for s in own] == ["x"], own
        for symbol in own:
            _assert_in_document(current, symbol.location.range, "utf-16")
    finally:
        await _stop(c)


@pytest.mark.asyncio
async def test_crlf_diagnostic_ends_inside_its_line(lsp_binary, tmp_path):
    # A CRLF line ends at its '\r'; a span reaching the line break (an
    # unterminated string runs to it) ended one character past the line.
    c = await _utf16_client(lsp_binary)
    try:
        uri = (tmp_path / "crlf.iron").as_uri()
        text = 'func main() {\r\n    val s = "abc\r\n}\r\n'
        _open(c, uri, text)
        diags = await _diagnostics_for_version(c, uri, 1)
        assert diags
        for d in diags:
            _assert_in_document(text, d.range, "utf-16")
    finally:
        await _stop(c)


@pytest.mark.asyncio
async def test_pull_diagnostics_always_answered(lsp_binary, tmp_path):
    # A pull for a document that is not open (or was closed while the pull
    # waited) got no response at all.
    from pytest_lsp import ClientServerConfig
    c = await ClientServerConfig(server_command=[lsp_binary]).start()
    _install_register_capability_handler(c)
    params = _default_init_params()
    params.capabilities.text_document.diagnostic = types.DiagnosticClientCapabilities()
    await c.initialize_session(params)
    try:
        unknown = (tmp_path / "never_opened.iron").as_uri()
        rep = await asyncio.wait_for(c.text_document_diagnostic_async(
            types.DocumentDiagnosticParams(
                text_document=types.TextDocumentIdentifier(uri=unknown))), timeout=5.0)
        assert rep is not None and len(rep.items) == 0, rep

        uri = (tmp_path / "closing.iron").as_uri()
        _open(c, uri, "func main() {\n    val x: Int = \"s\"\n}\n")
        fut = c.text_document_diagnostic_async(types.DocumentDiagnosticParams(
            text_document=types.TextDocumentIdentifier(uri=uri)))
        c.text_document_did_close(types.DidCloseTextDocumentParams(
            text_document=types.TextDocumentIdentifier(uri=uri)))
        rep = await asyncio.wait_for(fut, timeout=5.0)
        assert rep is not None
    finally:
        await _stop(c)


@pytest.mark.asyncio
async def test_defer_free_snippet_with_other_candidates(lsp_binary, tmp_path):
    # With a snippet-capable client, `def` at a statement head after a heap
    # binding appends `defer free <name>` snippets. They were arrput onto
    # the candidate array, which is a plain arena array, not an stb_ds one:
    # heap corruption (a UBSan pointer overflow under the sanitizers).
    from pytest_lsp import ClientServerConfig
    c = await ClientServerConfig(server_command=[lsp_binary]).start()
    _install_register_capability_handler(c)
    params = _default_init_params()
    params.capabilities.text_document.completion = types.CompletionClientCapabilities(
        completion_item=types.ClientCompletionItemOptions(snippet_support=True))
    await c.initialize_session(params)
    try:
        uri = (tmp_path / "snip.iron").as_uri()
        text = ("object Buffer { val size: Int }\n"
                "func main() {\n"
                "    val buffer = heap Buffer(1024)\n"
                "    def\n"
                "}\n")
        _open(c, uri, text)
        res = await asyncio.wait_for(c.text_document_completion_async(types.CompletionParams(
            text_document=types.TextDocumentIdentifier(uri=uri),
            position=types.Position(line=3, character=7))), timeout=10.0)
        items = res.items if hasattr(res, "items") else res
        labels = [i.label for i in items or []]
        assert "defer free buffer" in labels, labels
        assert "defer" in labels, labels
        # still alive
        await asyncio.wait_for(c.text_document_hover_async(types.HoverParams(
            text_document=types.TextDocumentIdentifier(uri=uri),
            position=types.Position(line=2, character=9))), timeout=10.0)
    finally:
        await _stop(c)


@pytest.mark.asyncio
async def test_invalid_character_reported_once(lsp_binary, tmp_path):
    # A non-ASCII character in code gave one E0002 per byte, and the parser
    # then met one error token per byte: a match arm on 😀 reported the same
    # "expected '->'" twice at the same position.
    c = await _utf16_client(lsp_binary)
    try:
        uri = (tmp_path / "chars.iron").as_uri()
        text = ("func main() {\n"
                "    val y = 1 中 2\n"
                "    val x = 1\n"
                "    match x {\n"
                "        😀 -> 1\n"
                "        else -> 2\n"
                "    }\n"
                "}\n")
        _open(c, uri, text)
        diags = await _diagnostics_for_version(c, uri, 1)
        keys = [(d.code, d.range.start.line, d.range.start.character, d.message) for d in diags]
        assert len(keys) == len(set(keys)), keys
        bad = [d for d in diags if d.code == "E0002"]
        assert [(d.range.start.line, d.range.start.character, d.range.end.character)
                for d in bad] == [(1, 14, 15), (4, 8, 10)], bad
        assert all("IRON_TOK" not in d.message for d in diags), diags
    finally:
        await _stop(c)


@pytest.mark.asyncio
async def test_no_never_reassigned_warning_in_generic_function(client, tmp_path):
    # The language server ran the passes after type checking on generic
    # templates too: a `var` set mutated through a method in a generic
    # function was "never reassigned; declare as 'val'" in the editor (and
    # its quickfix would break the code); ironc check reports nothing.
    uri = (tmp_path / "generic_var.iron").as_uri()
    text = ("func distinct[K: Hashable](xs: [K]) -> Int {\n"
            "    var s = Set[K]()\n"
            "    for x in xs {\n"
            "        s.add(x)\n"
            "    }\n"
            "    return s.len()\n"
            "}\n"
            "\n"
            "func main() {\n"
            "    println(\"{distinct([1, 2, 2])}\")\n"
            "}\n")
    _open(client, uri, text)
    diags = await _diagnostics_for_version(client, uri, 1)
    assert len(diags) == 0, diags


@pytest.mark.asyncio
async def test_formatting_edit_ends_at_the_document_end(lsp_binary, tmp_path):
    # The whole-document edit ended at the byte length of the last line,
    # past its end when that line holds non-ASCII text.
    c = await _utf16_client(lsp_binary)
    try:
        uri = (tmp_path / "fmtend.iron").as_uri()
        text = 'func main() {\n      println("a")\n}\n-- é😀'
        _open(c, uri, text)
        edits = await asyncio.wait_for(c.text_document_formatting_async(
            types.DocumentFormattingParams(
                text_document=types.TextDocumentIdentifier(uri=uri),
                options=types.FormattingOptions(tab_size=4, insert_spaces=True))), timeout=10.0)
        assert edits, edits
        assert edits[-1].range.end == types.Position(line=3, character=6), edits[-1].range
    finally:
        await _stop(c)


@pytest.mark.asyncio
async def test_tuple_binding_symbol_has_a_name(client, tmp_path):
    # `val (x, y) = ...` has no single name; its document symbol was named
    # "", which clients reject (VS Code drops the whole outline).
    uri = (tmp_path / "tuple.iron").as_uri()
    text = ("func pair() -> (Int, Int) {\n"
            "    return (1, 2)\n"
            "}\n"
            "\n"
            "val (x, y) = pair()\n")
    _open(client, uri, text)
    syms = await asyncio.wait_for(client.text_document_document_symbol_async(
        types.DocumentSymbolParams(text_document=types.TextDocumentIdentifier(uri=uri))),
        timeout=10.0)

    def names(ss):
        for s in ss or []:
            yield s.name
            yield from names(getattr(s, "children", None))
    got = list(names(syms))
    assert all(got), got
    assert "(x, y)" in got, got


@pytest.mark.asyncio
async def test_workspace_symbol_uses_unsaved_document_ranges(lsp_binary, tmp_path):
    # The workspace index is built from disk. After an unsaved edit made a
    # declaration shorter, workspace/symbol returned the saved declaration's
    # stale range, one UTF-16 unit beyond the end of the edited line.
    from pytest_lsp import ClientServerConfig
    (tmp_path / "iron.toml").write_text(
        '[package]\nname="range_repro"\nversion="0.1.0"\n', encoding="utf-8")
    source = tmp_path / "main.iron"
    saved = "pub func alphabeticallyLongName() {}\n"
    current = "pub func a() {}\n"
    source.write_text(saved, encoding="utf-8")

    c = await ClientServerConfig(server_command=[lsp_binary]).start()
    _install_register_capability_handler(c)
    params = _default_init_params()
    params.capabilities.general.position_encodings = [types.PositionEncodingKind.Utf16]
    params.root_uri = tmp_path.as_uri()
    params.workspace_folders = [
        types.WorkspaceFolder(uri=tmp_path.as_uri(), name="range-repro")]
    await c.initialize_session(params)
    try:
        uri = source.as_uri()
        _open(c, uri, saved)

        # Wait for warm seeding to make the saved symbol observable. This
        # makes the test fail on the old implementation instead of passing
        # vacuously when the background index has not finished yet.
        loop = asyncio.get_running_loop()
        end = loop.time() + 10.0
        while True:
            saved_symbols = await c.workspace_symbol_async(
                types.WorkspaceSymbolParams(query="alphabeticallyLongName"))
            if saved_symbols:
                break
            if loop.time() >= end:
                pytest.fail("workspace index did not expose the saved symbol")
            await asyncio.sleep(0.05)

        c.text_document_did_change(types.DidChangeTextDocumentParams(
            text_document=types.VersionedTextDocumentIdentifier(uri=uri, version=2),
            content_changes=[types.TextDocumentContentChangeWholeDocument(text=current)]))
        symbols = await asyncio.wait_for(c.workspace_symbol_async(
            types.WorkspaceSymbolParams(query="a")), timeout=10.0)
        assert symbols
        in_file = [s for s in symbols if s.location.uri == uri]
        assert in_file
        for symbol in in_file:
            _assert_in_document(current, symbol.location.range, "utf-16")
        assert any(s.name == "a" for s in in_file)
        assert all(s.name != "alphabeticallyLongName" for s in in_file)
    finally:
        await _stop(c)


@pytest.mark.asyncio
async def test_close_clears_published_diagnostics(client, tmp_path):
    # Closing a document left its last diagnostics in the client (and a
    # publish its worker sent before stopping could arrive after a reopen):
    # the server now publishes an empty list for the closed URI.
    uri = (tmp_path / "closed.iron").as_uri()
    _open(client, uri, "func main() {\n    val x: Int = \"s\"\n}\n")
    diags = await _diagnostics_for_version(client, uri, 1)
    assert diags
    client.text_document_did_close(types.DidCloseTextDocumentParams(
        text_document=types.TextDocumentIdentifier(uri=uri)))
    loop = asyncio.get_running_loop()
    end = loop.time() + 10.0
    while loop.time() < end:
        params = await asyncio.wait_for(
            client.wait_for_notification(types.TEXT_DOCUMENT_PUBLISH_DIAGNOSTICS),
            timeout=max(0.1, end - loop.time()))
        if params.uri == uri and params.version is None:
            assert len(params.diagnostics) == 0
            return
    pytest.fail("no cleared publishDiagnostics after didClose")


def _apply_utf16(text, edits):
    lines = text.split("\n")
    starts = [0]
    for ln in lines:
        starts.append(starts[-1] + len(ln) + 1)

    def idx(p):
        if p.line >= len(lines):
            return len(text)
        return min(starts[p.line] + p.character, starts[p.line] + len(lines[p.line]))
    out = text
    for e in sorted(edits, key=lambda e: (e.range.start.line, e.range.start.character),
                    reverse=True):
        out = out[:idx(e.range.start)] + e.new_text + out[idx(e.range.end):]
    return out


@pytest.mark.asyncio
async def test_range_formatting_whole_crlf_document(lsp_binary, tmp_path):
    # Range formatting only touched the declarations in the range (an
    # indented comment between them stayed indented) and wrote LF lines
    # into a CRLF document.
    c = await _utf16_client(lsp_binary)
    try:
        uri = (tmp_path / "rangefmt.iron").as_uri()
        text = ("   -- header\r\n"
                "func main() {\r\n"
                "        println(\"a\")\r\n"
                "}\r\n")
        _open(c, uri, text)
        edits = await asyncio.wait_for(c.text_document_range_formatting_async(
            types.DocumentRangeFormattingParams(
                text_document=types.TextDocumentIdentifier(uri=uri),
                range=types.Range(start=types.Position(line=0, character=0),
                                  end=types.Position(line=4, character=0)),
                options=types.FormattingOptions(tab_size=4, insert_spaces=True))),
            timeout=10.0)
        out = _apply_utf16(text, edits or [])
        assert out.count("\n") == out.count("\r\n"), repr(out)
        assert out.startswith("-- header\r\n"), repr(out)
        assert "\r\n    println(\"a\")\r\n" in out, repr(out)
    finally:
        await _stop(c)


@pytest.mark.asyncio
async def test_forgotten_free_quickfix_names_the_binding(client, tmp_path):
    # W0606 points at the heap expression; the quickfix took its text as the
    # name and offered "Insert 'defer free heap Buf(8'". With a byte-order
    # mark before the expression the slice also cut a character, the
    # response failed to serialize, and the client got no answer at all.
    uri = (tmp_path / "forgot.iron").as_uri()
    text = ("object Buf {\n"
            "    val n: Int\n"
            "}\n"
            "\n"
            "func main() {\n"
            "    val buf = ﻿heap Buf(8)\n"
            "    println(\"{buf.n}\")\n"
            "}\n")
    _open(client, uri, text)
    diags = await _diagnostics_for_version(client, uri, 1)
    w = [d for d in diags if d.code in ("E0606", "W0606")]
    assert w, diags
    res = await asyncio.wait_for(client.text_document_code_action_async(types.CodeActionParams(
        text_document=types.TextDocumentIdentifier(uri=uri), range=w[0].range,
        context=types.CodeActionContext(diagnostics=w))), timeout=10.0)
    titles = [a.title for a in res or []]
    assert "Insert 'defer free buf'" in titles, titles


@pytest.mark.asyncio
async def test_signature_help_documentation(client, tmp_path):
    # The documentation of a signature pointed into the analysis arena,
    # freed before the response was built (use after free).
    uri = (tmp_path / "sighelp.iron").as_uri()
    text = ("/// Adds two numbers.\n"
            "func add(a: Int, b: Int) -> Int {\n"
            "    return a + b\n"
            "}\n"
            "\n"
            "func main() {\n"
            "    println(\"{add(1, 2)}\")\n"
            "}\n")
    _open(client, uri, text)
    await _diagnostics_for_version(client, uri, 1)
    res = await asyncio.wait_for(client.text_document_signature_help_async(
        types.SignatureHelpParams(text_document=types.TextDocumentIdentifier(uri=uri),
                                  position=types.Position(line=6, character=18))),
        timeout=10.0)
    assert res and res.signatures, res
    doc = res.signatures[0].documentation
    doc = getattr(doc, "value", doc)
    assert doc and "Adds two numbers." in doc, res.signatures[0]


@pytest.mark.asyncio
async def test_range_formatting_last_line_without_newline(lsp_binary, tmp_path):
    # The edit replaced up to the start of the line after the last one,
    # which does not exist when the file does not end in a newline.
    c = await _utf16_client(lsp_binary)
    try:
        uri = (tmp_path / "noeol.iron").as_uri()
        text = "func main() {\n      println(\"é\")\n   }"
        _open(c, uri, text)
        edits = await asyncio.wait_for(c.text_document_range_formatting_async(
            types.DocumentRangeFormattingParams(
                text_document=types.TextDocumentIdentifier(uri=uri),
                range=types.Range(start=types.Position(line=0, character=0),
                                  end=types.Position(line=2, character=4)),
                options=types.FormattingOptions(tab_size=4, insert_spaces=True))),
            timeout=10.0)
        assert edits
        for e in edits:
            _assert_in_document(text, e.range, "utf-16")
        assert _apply_utf16(text, edits) == "func main() {\n    println(\"é\")\n}"
    finally:
        await _stop(c)


@pytest.mark.asyncio
async def test_no_repeated_diagnostics_from_generic_instances(lsp_binary, tmp_path):
    # The same error was published twice at the same range: once per
    # generic instance of a broken body (their spans differ, their ranges
    # do not).
    c = await _utf16_client(lsp_binary)
    try:
        uri = (tmp_path / "dup.iron").as_uri()
        text = 'func fold[T(xs:T\nvar g0=4\nfunc main){fold("",fold(g0'
        _open(c, uri, text)
        diags = await _diagnostics_for_version(c, uri, 1)
        keys = [(d.range.start.line, d.range.start.character, d.range.end.line,
                 d.range.end.character, d.code, d.message) for d in diags]
        assert len(keys) == len(set(keys)), keys
    finally:
        await _stop(c)


@pytest.mark.asyncio
async def test_deep_expression_analyzed(client, tmp_path):
    # A sanitizer build of ironls overflowed its 8 MB stacks on nesting a
    # release build handles; its threads now get sanitizer-sized stacks.
    uri = (tmp_path / "deep.iron").as_uri()
    text = ("func main() {\n    val y = 1\n    val x = " + " + ".join(["y"] * 1000)
            + "\n    val bad: Int = \"text\"\n}\n")
    _open(client, uri, text)
    diags = await _diagnostics_for_version(client, uri, 1, timeout=60.0)
    assert any(d.code == "E0202" or "mismatch" in d.message for d in diags), diags
