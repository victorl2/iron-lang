"""Phase 25 regression-anchor smoke: PTR-02/03 (E0289) + PTR-05 (E0294) +
UNCK-06 (E0295) diagnostics surface through publishDiagnostics.

NOT a Wave 0 RED gate — by the time this fixture runs, Plans 25-01/02 have
already committed IRON_ERR_PTR_REGIME_MISMATCH=289, IRON_ERR_PTR_AMP_NOT_UNCHECKED=294,
and IRON_ERR_PTR_ARITH_CHECKED=295. This smoke test is a regression anchor
verifying the CORE-22 facade still flows the new PTR + UNCK codes through
publishDiagnostics, locking in the "zero LSP code change required for new
compiler-side diagnostics" invariant.

Three scenarios:
- PTR-02/03 cross-regime assignment: assigns `*Point` to `*unchecked Point` slot,
  triggering IRON_ERR_PTR_REGIME_MISMATCH=289. Confirms code 289 surfaces via
  publishDiagnostics with the spec-locked message substring "regime" or "unchecked".
- PTR-05 amp to unchecked: uses `&local_point` assigned to a `*unchecked Point`
  slot, triggering IRON_ERR_PTR_AMP_NOT_UNCHECKED=294. Confirms code 294 surfaces
  with substring "unchecked" or "Box.unwrap".
- UNCK-06 pointer arithmetic on checked pointer: calls `Ptr.offset(p, 1)` where
  p is `*Point` (checked regime), triggering IRON_ERR_PTR_ARITH_CHECKED=295.
  Confirms code 295 surfaces with substring "unchecked" or "Ptr.offset".

Locked from Phase 25 CONTEXT.md and Phase 17-24 conventions:
- "ONE pytest-lsp regression-anchor smoke fixture verifying PTR + UNCK diagnostic
   codes flow through CORE-22 facade." (CONTEXT.md scope lock)
- "Existing CORE-22 facade automatically pipes new diagnostics through
   publishDiagnostics — zero LSP code change required for the new codes to
   appear in editors."
- LSP wire format E<NNN> zero-padded; _matches_code helper accepts int,
  bare-string, or E-prefixed-string (inherited from Phase 18-24 baseline).
"""
from __future__ import annotations

import asyncio
import pytest
from lsprotocol import types

# PTR-02/PTR-03 trigger: cross-regime assignment.
# Assigns *Point (checked) to a *unchecked Point (unchecked) slot.
# Triggers IRON_ERR_PTR_REGIME_MISMATCH=289 from src/analyzer/typecheck.c
# types_assignable cross-regime disjoint guard.
# §4.3-§4.4: *T and *unchecked T are disjoint; no implicit conversion either direction.
_CROSS_REGIME_ASSIGN_SOURCE = (
    "object Point {\n"
    "    val x: Int\n"
    "    val y: Int\n"
    "}\n"
    "func main() {\n"
    "    val p = heap Point(x: 1, y: 2)\n"
    "    val raw_ptr: *unchecked Point = p\n"
    "}\n"
)

# PTR-05 trigger: `&` cannot produce *unchecked T.
# Tries to assign &local_point to a *unchecked Point slot.
# Triggers IRON_ERR_PTR_AMP_NOT_UNCHECKED=294 from src/analyzer/typecheck.c
# val/var-decl arm when RHS is & expression and LHS type has is_unchecked=true.
# §4.3: only Box.unwrap() can produce *unchecked T; `&` is not a valid source.
_AMP_PRODUCES_UNCHECKED_SOURCE = (
    "object Point {\n"
    "    val x: Int\n"
    "    val y: Int\n"
    "}\n"
    "func main() {\n"
    "    val local_point = Point(x: 3, y: 4)\n"
    "    val raw_ptr: *unchecked Point = &local_point\n"
    "}\n"
)

# UNCK-06 trigger: Ptr.offset on checked pointer.
# Calls Ptr.offset(p, 1) where p is *Point (checked regime).
# Triggers IRON_ERR_PTR_ARITH_CHECKED=295 from src/analyzer/typecheck.c
# IRON_NODE_METHOD_CALL Ptr.offset dispatch with E0295 emission when
# argument is not *unchecked T.
# §4.3: Ptr.offset / Ptr.diff operate only on *unchecked T.
_PTR_ARITH_CHECKED_SOURCE = (
    "object Point {\n"
    "    val x: Int\n"
    "    val y: Int\n"
    "}\n"
    "func main() {\n"
    "    val p = heap Point(x: 5, y: 6)\n"
    "    val next = Ptr.offset(p, 1)\n"
    "}\n"
)


def _matches_code(diag_code, numeric_tail: str) -> bool:
    """Match a diagnostic code against a numeric tail.

    Diagnostic code may be int (289), string ("289"), or LSP-wire
    E-prefixed string ("E0289") depending on the serializer. The
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
async def test_cross_regime_assign_publishes(client, tmp_path):
    """End-to-end PTR-02/03 -> publishDiagnostics regression-anchor smoke.

    Verifies IRON_ERR_PTR_REGIME_MISMATCH=289 surfaces through the CORE-22
    facade. Locks the cross-regime assignment detection at val/var-decl site
    (§4.3-§4.4: *T and *unchecked T are disjoint; types_assignable cross-regime
    disjoint guard emits E0289 with spec-locked §4.4 hint substring).
    """
    uri = (tmp_path / "cross_regime_assign.iron").as_uri()
    client.text_document_did_open(
        types.DidOpenTextDocumentParams(
            text_document=types.TextDocumentItem(
                uri=uri,
                language_id="iron",
                version=1,
                text=_CROSS_REGIME_ASSIGN_SOURCE,
            ),
        ),
    )
    await asyncio.wait_for(
        client.wait_for_notification(types.TEXT_DOCUMENT_PUBLISH_DIAGNOSTICS),
        timeout=5.0,
    )
    diags = client.diagnostics.get(uri, [])
    assert len(diags) >= 1, (
        f"expected >=1 diagnostic for PTR-02/03 cross-regime assignment, "
        f"got {diags!r}"
    )
    matching = [d for d in diags if _matches_code(d.code, "289")]
    assert matching, (
        f"expected diagnostic code 289 (IRON_ERR_PTR_REGIME_MISMATCH), "
        f"got {[d.code for d in diags]!r}"
    )
    messages = [d.message for d in matching]
    assert any(
        ("regime" in m.lower() or "unchecked" in m.lower())
        for m in messages
    ), (
        f"expected substring 'regime'/'unchecked' in diag messages, "
        f"got {messages!r}"
    )


@pytest.mark.asyncio
async def test_amp_produces_unchecked_publishes(client, tmp_path):
    """End-to-end PTR-05 -> publishDiagnostics regression-anchor smoke.

    Verifies IRON_ERR_PTR_AMP_NOT_UNCHECKED=294 surfaces through the CORE-22
    facade. Locks the & rejection at val/var-decl when LHS type is *unchecked T
    (§4.3: only Box.unwrap() or RawPtr (Phase 33) can produce *unchecked T;
    `&` is forbidden as a source of unchecked pointers).
    """
    uri = (tmp_path / "amp_produces_unchecked.iron").as_uri()
    client.text_document_did_open(
        types.DidOpenTextDocumentParams(
            text_document=types.TextDocumentItem(
                uri=uri,
                language_id="iron",
                version=1,
                text=_AMP_PRODUCES_UNCHECKED_SOURCE,
            ),
        ),
    )
    await asyncio.wait_for(
        client.wait_for_notification(types.TEXT_DOCUMENT_PUBLISH_DIAGNOSTICS),
        timeout=5.0,
    )
    diags = client.diagnostics.get(uri, [])
    assert len(diags) >= 1, (
        f"expected >=1 diagnostic for PTR-05 amp to unchecked, "
        f"got {diags!r}"
    )
    matching = [d for d in diags if _matches_code(d.code, "294")]
    assert matching, (
        f"expected diagnostic code 294 (IRON_ERR_PTR_AMP_NOT_UNCHECKED), "
        f"got {[d.code for d in diags]!r}"
    )
    messages = [d.message for d in matching]
    assert any(
        ("unchecked" in m.lower() or "box.unwrap" in m.lower() or "&" in m)
        for m in messages
    ), (
        f"expected substring 'unchecked'/'Box.unwrap'/'&' in diag messages, "
        f"got {messages!r}"
    )


@pytest.mark.asyncio
async def test_ptr_arith_checked_publishes(client, tmp_path):
    """End-to-end UNCK-06 -> publishDiagnostics regression-anchor smoke.

    Verifies IRON_ERR_PTR_ARITH_CHECKED=295 surfaces through the CORE-22
    facade. Locks the Ptr.offset rejection when argument is not *unchecked T
    (§4.3: Ptr.offset / Ptr.diff operate only on *unchecked T; calling with
    a checked pointer emits E0295 with unchecked regime hint).
    """
    uri = (tmp_path / "ptr_arith_checked.iron").as_uri()
    client.text_document_did_open(
        types.DidOpenTextDocumentParams(
            text_document=types.TextDocumentItem(
                uri=uri,
                language_id="iron",
                version=1,
                text=_PTR_ARITH_CHECKED_SOURCE,
            ),
        ),
    )
    await asyncio.wait_for(
        client.wait_for_notification(types.TEXT_DOCUMENT_PUBLISH_DIAGNOSTICS),
        timeout=5.0,
    )
    diags = client.diagnostics.get(uri, [])
    assert len(diags) >= 1, (
        f"expected >=1 diagnostic for UNCK-06 pointer arithmetic on checked pointer, "
        f"got {diags!r}"
    )
    matching = [d for d in diags if _matches_code(d.code, "295")]
    assert matching, (
        f"expected diagnostic code 295 (IRON_ERR_PTR_ARITH_CHECKED), "
        f"got {[d.code for d in diags]!r}"
    )
    messages = [d.message for d in matching]
    assert any(
        ("unchecked" in m.lower() or "ptr.offset" in m.lower() or "arith" in m.lower())
        for m in messages
    ), (
        f"expected substring 'unchecked'/'Ptr.offset'/'arith' in diag messages, "
        f"got {messages!r}"
    )
