"""Phase 20 regression-anchor smoke: PTR-11 + PTR-13 diagnostics surface through publishDiagnostics.

NOT a Wave 0 RED gate -- by the time this fixture runs, Plans 20-01/02a/02b
have already committed IRON_ERR_PTR_NO_ARITH=268 (PTR-11) and
IRON_ERR_PTR_NULL_DEREF=272 (PTR-13). This smoke test is a regression
anchor verifying the CORE-22 facade still flows the new codes through
publishDiagnostics, locking in the "zero LSP code change required for
new compiler-side diagnostics" invariant for Phase 34 LSP-06 to lean
against when it adds quickfix code-actions for checked-pointer
diagnostics.

Two scenarios:
- PTR-11 pointer arithmetic: opens a buffer with
  `func main() { val x = 1; val p: *Int = &x; val r = p + 1 }` and
  confirms the LSP server emits a Diagnostic carrying code 268
  (IRON_ERR_PTR_NO_ARITH) with the spec-locked message substring
  "no pointer arithmetic in checked regime".
- PTR-13 null-to-non-nullable-pointer binding: opens a buffer with
  `object Point { val x: Int }\nfunc main() { val p: *Point = null }`
  and confirms the LSP server emits a Diagnostic carrying code 272
  (IRON_ERR_PTR_NULL_DEREF) with the spec-locked substring
  "cannot assign null to non-nullable pointer type".

Locked from Phase 20 CONTEXT.md and Phase 17/18 conventions:
- "ONE pytest-lsp regression-anchor smoke fixture verifying PTR
   diagnostic codes flow through CORE-22 facade." (CONTEXT.md scope lock)
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

# PTR-11 trigger: pointer arithmetic in checked regime. Triggers
# IRON_ERR_PTR_NO_ARITH=268 from src/analyzer/typecheck.c IRON_NODE_BINARY
# handler when either operand resolves to IRON_TYPE_PTR.
_PTR_11_SOURCE = (
    "func main() {\n"
    "    val x: Int = 1\n"
    "    val p: *Int = &x\n"
    "    val r = p + 1\n"
    "}\n"
)

# PTR-13 trigger: null assigned to non-nullable pointer type. Triggers
# IRON_ERR_PTR_NULL_DEREF=272 from src/analyzer/typecheck.c at val/var
# binding-init when annotation is IRON_TYPE_PTR (not nullable) and the
# initializer is the null literal.
_PTR_13_SOURCE = (
    "object Point {\n"
    "    val x: Int\n"
    "    val y: Int\n"
    "}\n"
    "\n"
    "func main() {\n"
    "    val p: *Point = null\n"
    "}\n"
)


def _matches_code(diag_code, numeric_tail: str) -> bool:
    """Match a diagnostic code against a numeric tail.

    Diagnostic code may be int (268), string ("268"), or LSP-wire
    E-prefixed string ("E0268") depending on the serializer. The
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
async def test_ptr_11_no_arith_publishes(client, tmp_path):
    """End-to-end PTR-11 -> publishDiagnostics regression-anchor smoke.

    Verifies IRON_ERR_PTR_NO_ARITH=268 surfaces with spec-locked
    message substring 'no pointer arithmetic in checked regime'
    through the CORE-22 facade.
    """
    uri = (tmp_path / "ptr_no_arith.iron").as_uri()
    client.text_document_did_open(
        types.DidOpenTextDocumentParams(
            text_document=types.TextDocumentItem(
                uri=uri,
                language_id="iron",
                version=1,
                text=_PTR_11_SOURCE,
            ),
        ),
    )
    await asyncio.wait_for(
        client.wait_for_notification(types.TEXT_DOCUMENT_PUBLISH_DIAGNOSTICS),
        timeout=5.0,
    )
    diags = client.diagnostics.get(uri, [])
    assert len(diags) >= 1, (
        f"expected >=1 diagnostic for PTR-11 pointer arithmetic, "
        f"got {diags!r}"
    )
    matching = [d for d in diags if _matches_code(d.code, "268")]
    assert matching, (
        f"expected diagnostic code 268 (IRON_ERR_PTR_NO_ARITH), "
        f"got {[d.code for d in diags]!r}"
    )
    # Spec-locked substring; both surfaces (CLI stderr + LSP message)
    # carry the same wording -- that is the CORE-22 invariant.
    messages = [d.message for d in matching]
    assert any("no pointer arithmetic in checked regime" in m for m in messages), (
        f"expected substring 'no pointer arithmetic in checked regime' in diag messages, "
        f"got {messages!r}"
    )


@pytest.mark.asyncio
async def test_ptr_13_null_to_non_nullable_publishes(client, tmp_path):
    """End-to-end PTR-13 -> publishDiagnostics regression-anchor smoke.

    Verifies IRON_ERR_PTR_NULL_DEREF=272 surfaces with the spec-locked
    message substring 'cannot assign null to non-nullable pointer type'
    through the CORE-22 facade. Locks the binding-init compile-time
    emission distinct from runtime null-deref panics (both reuse
    code 272 per CONTEXT.md OQ-02 / diagnostic-codes.md Notes).
    """
    uri = (tmp_path / "ptr_null_deref.iron").as_uri()
    client.text_document_did_open(
        types.DidOpenTextDocumentParams(
            text_document=types.TextDocumentItem(
                uri=uri,
                language_id="iron",
                version=1,
                text=_PTR_13_SOURCE,
            ),
        ),
    )
    await asyncio.wait_for(
        client.wait_for_notification(types.TEXT_DOCUMENT_PUBLISH_DIAGNOSTICS),
        timeout=5.0,
    )
    diags = client.diagnostics.get(uri, [])
    assert len(diags) >= 1, (
        f"expected >=1 diagnostic for PTR-13 null-to-non-nullable, "
        f"got {diags!r}"
    )
    matching = [d for d in diags if _matches_code(d.code, "272")]
    assert matching, (
        f"expected diagnostic code 272 (IRON_ERR_PTR_NULL_DEREF), "
        f"got {[d.code for d in diags]!r}"
    )
    messages = [d.message for d in matching]
    assert any("cannot assign null to non-nullable pointer type" in m for m in messages), (
        f"expected substring 'cannot assign null to non-nullable pointer type' in diag messages, "
        f"got {messages!r}"
    )
