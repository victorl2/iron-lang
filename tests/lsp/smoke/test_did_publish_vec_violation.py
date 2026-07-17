"""Phase 23 regression-anchor smoke: VEC-04 (E0282) + VEC-283 (E0283) diagnostics
surface through publishDiagnostics.

NOT a Wave 0 RED gate — by the time this fixture runs, Plans 23-01/02 have
already committed IRON_ERR_VEC_STRICT_LENGTH_MISMATCH=282 (VEC-04) and
IRON_ERR_VEC_BOUNDED_TO_FIXED_FORBIDDEN=283. This smoke test is a regression
anchor verifying the CORE-22 facade still flows the new VEC codes through
publishDiagnostics, locking in the "zero LSP code change required for
new compiler-side diagnostics" invariant.

Two scenarios:
- VEC-04 strict-length mismatch: declares [Int; 3] and initialises with 2
  elements — triggers IRON_ERR_VEC_STRICT_LENGTH_MISMATCH=282. Confirms
  the diagnostic code 282 surfaces via publishDiagnostics with the
  spec-locked message substring "exactly".
- VEC-283 bounded-to-fixed cross-assignment: assigns a [Int; <=3] bounded
  vector to a [Int; 3] strict binding — triggers
  IRON_ERR_VEC_BOUNDED_TO_FIXED_FORBIDDEN=283. Confirms code 283 surfaces
  with the spec-locked message substring "disjoint" or "Phase 33".

Locked from Phase 23 CONTEXT.md and Phase 17/18/20/21/22 conventions:
- "ONE pytest-lsp regression-anchor smoke fixture verifying VEC diagnostic
   codes flow through CORE-22 facade." (CONTEXT.md scope lock)
- "Existing CORE-22 facade automatically pipes new diagnostics through
   publishDiagnostics — zero LSP code change required for the new
   codes to appear in editors."
- LSP wire format E<NNN> zero-padded; _matches_code helper accepts int,
   bare-string, or E-prefixed-string (inherited from Phase 18-22 baseline).
"""
from __future__ import annotations

import asyncio
import pytest
from lsprotocol import types

# VEC-04 trigger: [Int; 3] declaration with 2-element array literal.
# Triggers IRON_ERR_VEC_STRICT_LENGTH_MISMATCH=282 from
# src/analyzer/typecheck.c VAL_DECL / VAR_DECL arm (the !types_assignable
# branch specialization that checks literal element count vs strict-array size).
# §3.3: [T; N] requires exactly N elements in the initializer literal.
_VEC_04_SOURCE = (
    "func main() {\n"
    "    val v: [Int; 3] = [1, 2]\n"
    "}\n"
)

# VEC-283 trigger: bounded [Int; <=3] value assigned to strict [Int; 3] binding.
# Triggers IRON_ERR_VEC_BOUNDED_TO_FIXED_FORBIDDEN=283 from
# src/analyzer/typecheck.c VAL_DECL arm (types_assignable disjoint check for
# bounded ↔ strict cross-assignment when both arrays have same elem + explicit size).
# §3.3: [T; <=N] and [T; N] are disjoint types; Phase 33 ships to_fixed()/to_bounded().
_VEC_283_SOURCE = (
    "func main() {\n"
    "    var a: [Int; <=3]\n"
    "    a.push(1)\n"
    "    val b: [Int; 3] = a\n"
    "}\n"
)


def _matches_code(diag_code, numeric_tail: str) -> bool:
    """Match a diagnostic code against a numeric tail.

    Diagnostic code may be int (282), string ("282"), or LSP-wire
    E-prefixed string ("E0282") depending on the serializer. The
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
async def test_vec_04_strict_mismatch_publishes(client, tmp_path):
    """End-to-end VEC-04 -> publishDiagnostics regression-anchor smoke.

    Verifies IRON_ERR_VEC_STRICT_LENGTH_MISMATCH=282 surfaces through the
    CORE-22 facade. Locks the strict-array literal element-count check
    (§3.3: [T; N] requires exactly N elements in the initializer literal).
    """
    uri = (tmp_path / "strict_mismatch.iron").as_uri()
    client.text_document_did_open(
        types.DidOpenTextDocumentParams(
            text_document=types.TextDocumentItem(
                uri=uri,
                language_id="iron",
                version=1,
                text=_VEC_04_SOURCE,
            ),
        ),
    )
    await asyncio.wait_for(
        client.wait_for_notification(types.TEXT_DOCUMENT_PUBLISH_DIAGNOSTICS),
        timeout=5.0,
    )
    diags = client.diagnostics.get(uri, [])
    assert len(diags) >= 1, (
        f"expected >=1 diagnostic for VEC-04 strict mismatch, "
        f"got {diags!r}"
    )
    matching = [d for d in diags if _matches_code(d.code, "282")]
    assert matching, (
        f"expected diagnostic code 282 (IRON_ERR_VEC_STRICT_LENGTH_MISMATCH), "
        f"got {[d.code for d in diags]!r}"
    )
    messages = [d.message for d in matching]
    assert any("exactly" in m for m in messages), (
        f"expected substring 'exactly' in diag messages, "
        f"got {messages!r}"
    )


@pytest.mark.asyncio
async def test_vec_283_bounded_to_fixed_publishes(client, tmp_path):
    """End-to-end VEC-283 -> publishDiagnostics regression-anchor smoke.

    Verifies IRON_ERR_VEC_BOUNDED_TO_FIXED_FORBIDDEN=283 surfaces through the
    CORE-22 facade. Locks the disjoint bounded ↔ strict cross-assignment
    check (§3.3: [T; <=N] and [T; N] are disjoint types; Phase 33 ships
    to_fixed()/to_bounded() conversion helpers).
    """
    uri = (tmp_path / "bounded_to_fixed.iron").as_uri()
    client.text_document_did_open(
        types.DidOpenTextDocumentParams(
            text_document=types.TextDocumentItem(
                uri=uri,
                language_id="iron",
                version=1,
                text=_VEC_283_SOURCE,
            ),
        ),
    )
    await asyncio.wait_for(
        client.wait_for_notification(types.TEXT_DOCUMENT_PUBLISH_DIAGNOSTICS),
        timeout=5.0,
    )
    diags = client.diagnostics.get(uri, [])
    assert len(diags) >= 1, (
        f"expected >=1 diagnostic for VEC-283 bounded-to-fixed, "
        f"got {diags!r}"
    )
    matching = [d for d in diags if _matches_code(d.code, "283")]
    assert matching, (
        f"expected diagnostic code 283 (IRON_ERR_VEC_BOUNDED_TO_FIXED_FORBIDDEN), "
        f"got {[d.code for d in diags]!r}"
    )
    messages = [d.message for d in matching]
    assert any(
        ("disjoint" in m or "Phase 33" in m or "bounded" in m.lower())
        for m in messages
    ), (
        f"expected substring 'disjoint'/'Phase 33'/'bounded' in diag messages, "
        f"got {messages!r}"
    )
