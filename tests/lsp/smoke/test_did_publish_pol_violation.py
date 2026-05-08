"""Phase 21 regression-anchor smoke: POL-03 + POL-04 diagnostics surface through publishDiagnostics.

NOT a Wave 0 RED gate -- by the time this fixture runs, Plans 21-01/02 have
already committed IRON_ERR_HEAP_BAD_POSITION=273 (POL-03) and
IRON_ERR_FREE_NOT_BINDING=274 (POL-04). This smoke test is a regression
anchor verifying the CORE-22 facade still flows the new codes through
publishDiagnostics, locking in the "zero LSP code change required for
new compiler-side diagnostics" invariant for Phase 34 LSP-06 to lean
against when it adds quickfix code-actions for heap-policy diagnostics.

Two scenarios:
- POL-03 heap bad position: opens a buffer with
  `func main() {\\n    heap val p = 42\\n}\\n` and confirms the LSP server
  emits a Diagnostic carrying code 273 (IRON_ERR_HEAP_BAD_POSITION) with
  the spec-locked message substring containing both 'heap' and
  'allocation expression'.
- POL-04 free non-binding: opens a buffer with a C-style object where
  `free c.v` is used (non-identifier target) and confirms the LSP server
  emits a Diagnostic carrying code 274 (IRON_ERR_FREE_NOT_BINDING) with
  the spec-locked substring 'must be a binding name'.

Locked from Phase 21 CONTEXT.md and Phase 17/18/20 conventions:
- "ONE pytest-lsp regression-anchor smoke fixture verifying POL diagnostic
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

# POL-03 trigger: `heap` keyword in binding-declaration position.
# Triggers IRON_ERR_HEAP_BAD_POSITION=273 from src/parser/parser.c
# Site 2 (binding-declaration intercept per RESEARCH Pitfall 5:
# token stream is [HEAP][VAL][IDENT], intercept at stmt-level BEFORE
# expression-statement fallthrough).
_POL_03_SOURCE = (
    "func main() {\n"
    "    heap val p = 42\n"
    "}\n"
)

# POL-04 trigger: `free` with a non-identifier target (field access).
# Triggers IRON_ERR_FREE_NOT_BINDING=274 from src/analyzer/typecheck.c
# FREE arm when the target expression kind != IRON_NODE_IDENT.
_POL_04_SOURCE = (
    "object C { var v: Int }\n"
    "func main() {\n"
    "    val c = heap C(0)\n"
    "    free c.v\n"
    "}\n"
)


def _matches_code(diag_code, numeric_tail: str) -> bool:
    """Match a diagnostic code against a numeric tail.

    Diagnostic code may be int (273), string ("273"), or LSP-wire
    E-prefixed string ("E0273") depending on the serializer. The
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
async def test_pol_03_heap_bad_position_publishes(client, tmp_path):
    """End-to-end POL-03 -> publishDiagnostics regression-anchor smoke.

    Verifies IRON_ERR_HEAP_BAD_POSITION=273 surfaces with spec-locked
    message substrings 'heap' and 'allocation expression' through the
    CORE-22 facade. Locks the binding-declaration intercept (RESEARCH
    Pitfall 5 — stmt-level BEFORE expression-statement fallthrough).
    """
    uri = (tmp_path / "heap_bad_position.iron").as_uri()
    client.text_document_did_open(
        types.DidOpenTextDocumentParams(
            text_document=types.TextDocumentItem(
                uri=uri,
                language_id="iron",
                version=1,
                text=_POL_03_SOURCE,
            ),
        ),
    )
    await asyncio.wait_for(
        client.wait_for_notification(types.TEXT_DOCUMENT_PUBLISH_DIAGNOSTICS),
        timeout=5.0,
    )
    diags = client.diagnostics.get(uri, [])
    assert len(diags) >= 1, (
        f"expected >=1 diagnostic for POL-03 heap bad position, "
        f"got {diags!r}"
    )
    matching = [d for d in diags if _matches_code(d.code, "273")]
    assert matching, (
        f"expected diagnostic code 273 (IRON_ERR_HEAP_BAD_POSITION), "
        f"got {[d.code for d in diags]!r}"
    )
    # Spec-locked substrings; both surfaces (CLI stderr + LSP message)
    # carry the same wording -- that is the CORE-22 invariant.
    messages = [d.message for d in matching]
    assert any("heap" in m and "allocation expression" in m for m in messages), (
        f"expected substrings 'heap' and 'allocation expression' in diag messages, "
        f"got {messages!r}"
    )


@pytest.mark.asyncio
async def test_pol_04_free_not_binding_publishes(client, tmp_path):
    """End-to-end POL-04 -> publishDiagnostics regression-anchor smoke.

    Verifies IRON_ERR_FREE_NOT_BINDING=274 surfaces with spec-locked
    message substring 'must be a binding name' through the CORE-22 facade.
    Locks the identifier-only target restriction for `free` (spec §4.6:
    `free` operates on bindings, not on arbitrary expressions).
    """
    uri = (tmp_path / "free_not_binding.iron").as_uri()
    client.text_document_did_open(
        types.DidOpenTextDocumentParams(
            text_document=types.TextDocumentItem(
                uri=uri,
                language_id="iron",
                version=1,
                text=_POL_04_SOURCE,
            ),
        ),
    )
    await asyncio.wait_for(
        client.wait_for_notification(types.TEXT_DOCUMENT_PUBLISH_DIAGNOSTICS),
        timeout=5.0,
    )
    diags = client.diagnostics.get(uri, [])
    assert len(diags) >= 1, (
        f"expected >=1 diagnostic for POL-04 free non-binding, "
        f"got {diags!r}"
    )
    matching = [d for d in diags if _matches_code(d.code, "274")]
    assert matching, (
        f"expected diagnostic code 274 (IRON_ERR_FREE_NOT_BINDING), "
        f"got {[d.code for d in diags]!r}"
    )
    messages = [d.message for d in matching]
    assert any("must be a binding name" in m for m in messages), (
        f"expected substring 'must be a binding name' in diag messages, "
        f"got {messages!r}"
    )
