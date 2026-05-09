"""Phase 22 regression-anchor smoke: READ-04 + READ-06 diagnostics surface through publishDiagnostics.

NOT a Wave 0 RED gate -- by the time this fixture runs, Plans 22-01/02 have
already committed IRON_ERR_READONLY_IO=278 (READ-04) and
IRON_ERR_READONLY_RETURN_TYPE=280 (READ-06). This smoke test is a regression
anchor verifying the CORE-22 facade still flows the new codes through
publishDiagnostics, locking in the "zero LSP code change required for
new compiler-side diagnostics" invariant for Phase 34 LSP-10 to lean
against when it adds quickfix code-actions for readonly diagnostics.

Two scenarios:
- READ-04 readonly I/O: opens a buffer with a readonly method that calls
  println and confirms the LSP server emits a Diagnostic carrying code 278
  (IRON_ERR_READONLY_IO) with the spec-locked message substring containing
  'readonly'.
- READ-06 readonly return type: opens a buffer with a readonly function
  whose declared return type is Map[String, Int] (not readonly-compatible)
  and confirms the LSP server emits a Diagnostic carrying code 280
  (IRON_ERR_READONLY_RETURN_TYPE) with the spec-locked substring
  'not readonly-compatible'.

Locked from Phase 22 CONTEXT.md and Phase 17/18/20/21 conventions:
- "ONE pytest-lsp regression-anchor smoke fixture verifying READ diagnostic
   codes flow through CORE-22 facade." (CONTEXT.md scope lock)
- "Existing CORE-22 facade automatically pipes new diagnostics through
   publishDiagnostics -- zero LSP code change required for the new
   codes to appear in editors."
- LSP wire format `E<NNN>` zero-padded; matchers accept integer,
   bare-string, or E-prefixed-string forms (see Phase 17 Plan 03 lock).
"""
from __future__ import annotations

import asyncio
import pytest
from lsprotocol import types

# READ-04 trigger: readonly object method calls println (I/O).
# Triggers IRON_ERR_READONLY_IO=278 from src/analyzer/typecheck.c
# IRON_LIR_CALL arm when the callee is in the I/O builtin set
# (println, print, readline) inside a readonly method context.
# Iron readonly modifier is object-block-only (parser enforces per Phase 84);
# the valid form is `readonly func name() -> T { ... }` inside an object block.
_READ_04_SOURCE = (
    "object X {\n"
    "    readonly func check() -> Int {\n"
    "        println(\"x\")\n"
    "        return 1\n"
    "    }\n"
    "}\n"
    "func main() {}\n"
)

# READ-06 trigger: readonly object method declares Map[String, Int] return type.
# Triggers IRON_ERR_READONLY_RETURN_TYPE=280 from src/analyzer/typecheck.c
# check_method_decl (declaration-site check) when is_readonly_compatible_type
# returns false for Map[K,V] (not in the closed whitelist: primitives,
# fixed structs, [T; N], [T; <=N], tuples thereof, T?).
_READ_06_SOURCE = (
    "object Y {\n"
    "    readonly func make_map() -> Map[String, Int] {\n"
    "        return Map.new()\n"
    "    }\n"
    "}\n"
    "func main() {}\n"
)


def _matches_code(diag_code, numeric_tail: str) -> bool:
    """Match a diagnostic code against a numeric tail.

    Diagnostic code may be int (278), string ("278"), or LSP-wire
    E-prefixed string ("E0278") depending on the serializer. The
    current ironls JSON path formats parser/semantic codes as
    "E<3-digit>" on the wire (zero-padded). Accept any of these forms;
    the invariant is the numeric tail equals the requested value.
    """
    if diag_code is None:
        return False
    s = str(diag_code)
    return (
        s == numeric_tail
        or s == f"E0{numeric_tail}"
        or s.endswith(numeric_tail)
    )


@pytest.mark.asyncio
async def test_read_04_readonly_io_publishes(client, tmp_path):
    """End-to-end READ-04 -> publishDiagnostics regression-anchor smoke.

    Verifies IRON_ERR_READONLY_IO=278 surfaces through the CORE-22 facade.
    Locks the I/O builtin check (println/print/readline) inside readonly
    method bodies (§6: readonly methods may not perform I/O).
    """
    uri = (tmp_path / "readonly_io.iron").as_uri()
    client.text_document_did_open(
        types.DidOpenTextDocumentParams(
            text_document=types.TextDocumentItem(
                uri=uri,
                language_id="iron",
                version=1,
                text=_READ_04_SOURCE,
            ),
        ),
    )
    await asyncio.wait_for(
        client.wait_for_notification(types.TEXT_DOCUMENT_PUBLISH_DIAGNOSTICS),
        timeout=5.0,
    )
    diags = client.diagnostics.get(uri, [])
    assert len(diags) >= 1, (
        f"expected >=1 diagnostic for READ-04 readonly I/O, "
        f"got {diags!r}"
    )
    matching = [d for d in diags if _matches_code(d.code, "278")]
    assert matching, (
        f"expected diagnostic code 278 (IRON_ERR_READONLY_IO), "
        f"got {[d.code for d in diags]!r}"
    )
    messages = [d.message for d in matching]
    assert any("readonly" in m for m in messages), (
        f"expected substring 'readonly' in diag messages, "
        f"got {messages!r}"
    )


@pytest.mark.asyncio
async def test_read_06_readonly_return_type_publishes(client, tmp_path):
    """End-to-end READ-06 -> publishDiagnostics regression-anchor smoke.

    Verifies IRON_ERR_READONLY_RETURN_TYPE=280 surfaces through the CORE-22
    facade. Locks the declaration-site return-type whitelist check (§6:
    readonly functions may only return primitives, fixed structs, [T; N],
    [T; <=N], tuples thereof, T?).
    """
    uri = (tmp_path / "readonly_return_type.iron").as_uri()
    client.text_document_did_open(
        types.DidOpenTextDocumentParams(
            text_document=types.TextDocumentItem(
                uri=uri,
                language_id="iron",
                version=1,
                text=_READ_06_SOURCE,
            ),
        ),
    )
    await asyncio.wait_for(
        client.wait_for_notification(types.TEXT_DOCUMENT_PUBLISH_DIAGNOSTICS),
        timeout=5.0,
    )
    diags = client.diagnostics.get(uri, [])
    assert len(diags) >= 1, (
        f"expected >=1 diagnostic for READ-06 readonly return type, "
        f"got {diags!r}"
    )
    matching = [d for d in diags if _matches_code(d.code, "280")]
    assert matching, (
        f"expected diagnostic code 280 (IRON_ERR_READONLY_RETURN_TYPE), "
        f"got {[d.code for d in diags]!r}"
    )
    messages = [d.message for d in matching]
    assert any("readonly" in m.lower() for m in messages), (
        f"expected substring 'readonly' in diag messages, "
        f"got {messages!r}"
    )
