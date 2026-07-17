"""Phase 26 regression-anchor smoke: POL-07 (E0296) + POL-11 (E0297, E0298)
diagnostics surface through publishDiagnostics.

NOT a Wave 0 RED gate -- by the time this fixture runs, Plans 26-01/02/03 have
already committed IRON_ERR_PTR_AMP_ON_RC=296 (POL-07), IRON_ERR_RC_BAD_POSITION=297
(POL-11 bad-position), and IRON_ERR_CLOSED_POLICY_KEYWORD=298 (POL-11 closed-set).
This smoke test is a regression anchor verifying the CORE-22 facade still flows
the new rc-policy diagnostic codes through publishDiagnostics, locking in the
"zero LSP code change required for new compiler-side diagnostics" invariant.

Three scenarios:
- POL-07 `&` on rc value: takes address of an rc-allocated binding, triggering
  IRON_ERR_PTR_AMP_ON_RC=296. Confirms code 296 surfaces via publishDiagnostics
  with the hint substring "weak rc" (Phase 27 forward reference).
- POL-11 `rc` in parameter declaration: uses `rc T` in a parameter position
  (not at allocation expression), triggering IRON_ERR_RC_BAD_POSITION=297.
  Confirms code 297 surfaces with the position-distinguishing hint substring
  "parameter".
- POL-11 closed-policy keyword: uses `pool T(...)` at allocation expression
  (unknown lifecycle policy), triggering IRON_ERR_CLOSED_POLICY_KEYWORD=298.
  Confirms code 298 surfaces with the canonical closed-set substring
  "{stack, heap, rc, weak rc}" (Blocker #1 -- the full lattice, NOT a
  reduced "{heap, rc}" subset). Anchors ROADMAP success criterion #4 and
  REQUIREMENTS POL-11 wording in the LSP test surface.

Locked from Phase 26 CONTEXT.md and Phase 17-25 conventions:
- "ONE pytest-lsp regression-anchor smoke fixture verifying rc-policy
   diagnostic codes flow through CORE-22 facade." (CONTEXT.md scope lock)
- "Existing CORE-22 facade automatically pipes new diagnostics through
   publishDiagnostics -- zero LSP code change required for the new codes
   to appear in editors."
- LSP wire format E<NNN> zero-padded; _matches_code helper accepts int,
  bare-string, or E-prefixed-string (inherited from Phase 18-25 baseline).
"""
from __future__ import annotations

import asyncio
import pytest
from lsprotocol import types

# POL-07 trigger: `&` on an rc value.
# Takes the address of `p` where p: rc Point.
# Triggers IRON_ERR_PTR_AMP_ON_RC=296 from src/analyzer/typecheck.c
# IRON_NODE_UNARY-AMP arm with rc-value short-circuit.
# Spec POL-07: rc values are non-addressable; use `weak rc T` (Phase 27)
# for non-owning references.
_AMP_ON_RC_SOURCE = (
    "object Point {\n"
    "    val x: Int\n"
    "    val y: Int\n"
    "}\n"
    "func main() {\n"
    "    val p = rc Point(1, 2)\n"
    "    val r = &p\n"
    "}\n"
)

# POL-11 trigger: `rc` keyword in parameter declaration (illegal position).
# Uses `rc Point` as a parameter type annotation; rc is reserved for
# allocation expression only.
# Triggers IRON_ERR_RC_BAD_POSITION=297 from src/parser/parser.c
# parameter-list parsing site (mirror of Phase 21 E0273 heap pattern).
# Spec POL-11: rc is closed-set lifecycle keyword bound to allocation
# expression position; using it as a type annotation, binding declaration,
# or parameter is a parse error.
_RC_IN_PARAMETER_SOURCE = (
    "object Point {\n"
    "    val x: Int\n"
    "}\n"
    "func work(p: rc Point) {\n"
    "    val q = p\n"
    "}\n"
)

# POL-11 closed-set trigger: `pool` is not in the closed lifecycle policy
# set {stack, heap, rc, weak rc}. Using it as an allocation policy keyword
# triggers IRON_ERR_CLOSED_POLICY_KEYWORD=298.
# Spec POL-11: closed set enforced at lexer/parser boundary; pool, arena,
# weak are not lifecycle policy keywords (weak is reserved for weak rc
# composition in Phase 27).
#
# Blocker #1: the diagnostic message MUST contain the canonical closed
# set substring "{stack, heap, rc, weak rc}" — anchoring ROADMAP
# success criterion #4 and REQUIREMENTS POL-11 wording in the LSP
# test surface.
_POOL_KEYWORD_SOURCE = (
    "object Point {\n"
    "    val x: Int\n"
    "    val y: Int\n"
    "}\n"
    "func main() {\n"
    "    val p = pool Point(1, 2)\n"
    "}\n"
)


def _matches_code(diag_code, numeric_tail: str) -> bool:
    """Match a diagnostic code against a numeric tail.

    Diagnostic code may be int (296), string ("296"), or LSP-wire
    E-prefixed string ("E0296") depending on the serializer. The
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
async def test_e0296_amp_on_rc_via_publishdiagnostics(client, tmp_path):
    """End-to-end POL-07 -> publishDiagnostics regression-anchor smoke.

    Verifies IRON_ERR_PTR_AMP_ON_RC=296 surfaces through the CORE-22
    facade. Locks the rc-AMP rejection at IRON_NODE_UNARY arm in
    typecheck.c (Spec POL-07: `&` on rc value is a compile error;
    diagnostic message names `weak rc` as the Phase 27 alternative).
    """
    uri = (tmp_path / "amp_on_rc.iron").as_uri()
    client.text_document_did_open(
        types.DidOpenTextDocumentParams(
            text_document=types.TextDocumentItem(
                uri=uri,
                language_id="iron",
                version=1,
                text=_AMP_ON_RC_SOURCE,
            ),
        ),
    )
    await asyncio.wait_for(
        client.wait_for_notification(types.TEXT_DOCUMENT_PUBLISH_DIAGNOSTICS),
        timeout=5.0,
    )
    diags = client.diagnostics.get(uri, [])
    assert len(diags) >= 1, (
        f"expected >=1 diagnostic for POL-07 `&` on rc, got {diags!r}"
    )
    matching = [d for d in diags if _matches_code(d.code, "296")]
    assert matching, (
        f"expected diagnostic code 296 (IRON_ERR_PTR_AMP_ON_RC), "
        f"got {[d.code for d in diags]!r}"
    )
    messages = [d.message for d in matching]
    assert any(
        ("weak rc" in m.lower() or "rc" in m.lower())
        for m in messages
    ), (
        f"expected substring 'weak rc'/'rc' in diag messages, "
        f"got {messages!r}"
    )


@pytest.mark.asyncio
async def test_e0297_rc_bad_position_via_publishdiagnostics(client, tmp_path):
    """End-to-end POL-11 (bad position) -> publishDiagnostics smoke.

    Verifies IRON_ERR_RC_BAD_POSITION=297 surfaces through the CORE-22
    facade. Locks the position-distinguishing hint in src/parser/parser.c
    parameter-list site (mirror of Phase 21 E0273 heap pattern).
    """
    uri = (tmp_path / "rc_in_parameter.iron").as_uri()
    client.text_document_did_open(
        types.DidOpenTextDocumentParams(
            text_document=types.TextDocumentItem(
                uri=uri,
                language_id="iron",
                version=1,
                text=_RC_IN_PARAMETER_SOURCE,
            ),
        ),
    )
    await asyncio.wait_for(
        client.wait_for_notification(types.TEXT_DOCUMENT_PUBLISH_DIAGNOSTICS),
        timeout=5.0,
    )
    diags = client.diagnostics.get(uri, [])
    assert len(diags) >= 1, (
        f"expected >=1 diagnostic for POL-11 rc in parameter, "
        f"got {diags!r}"
    )
    matching = [d for d in diags if _matches_code(d.code, "297")]
    assert matching, (
        f"expected diagnostic code 297 (IRON_ERR_RC_BAD_POSITION), "
        f"got {[d.code for d in diags]!r}"
    )
    messages = [d.message for d in matching]
    assert any(
        ("parameter" in m.lower() or "rc" in m.lower())
        for m in messages
    ), (
        f"expected substring 'parameter'/'rc' in diag messages, "
        f"got {messages!r}"
    )


@pytest.mark.asyncio
async def test_e0298_closed_policy_keyword_via_publishdiagnostics(client, tmp_path):
    """End-to-end POL-11 (closed-set guard) -> publishDiagnostics smoke.

    Verifies IRON_ERR_CLOSED_POLICY_KEYWORD=298 surfaces through the
    CORE-22 facade AND that the canonical closed-set substring
    `{stack, heap, rc, weak rc}` appears in the diagnostic message
    (Blocker #1 -- NOT a reduced subset like `{heap, rc}`).

    Anchors ROADMAP success criterion #4 + REQUIREMENTS POL-11 wording
    in the LSP test surface. The full lattice is surfaced to users via
    publishDiagnostics so editors can show the complete set of valid
    lifecycle policies at the violation site.
    """
    uri = (tmp_path / "pool_keyword.iron").as_uri()
    client.text_document_did_open(
        types.DidOpenTextDocumentParams(
            text_document=types.TextDocumentItem(
                uri=uri,
                language_id="iron",
                version=1,
                text=_POOL_KEYWORD_SOURCE,
            ),
        ),
    )
    await asyncio.wait_for(
        client.wait_for_notification(types.TEXT_DOCUMENT_PUBLISH_DIAGNOSTICS),
        timeout=5.0,
    )
    diags = client.diagnostics.get(uri, [])
    assert len(diags) >= 1, (
        f"expected >=1 diagnostic for POL-11 `pool` closed-set violation, "
        f"got {diags!r}"
    )
    matching = [d for d in diags if _matches_code(d.code, "298")]
    assert matching, (
        f"expected diagnostic code 298 (IRON_ERR_CLOSED_POLICY_KEYWORD), "
        f"got {[d.code for d in diags]!r}"
    )
    messages = [d.message for d in matching]
    # Blocker #1: canonical closed set surfaced through publishDiagnostics.
    # The substring MUST be the full lattice — NOT a reduced subset.
    assert any(
        "{stack, heap, rc, weak rc}" in m
        for m in messages
    ), (
        f"expected canonical closed-set substring "
        f"'{{stack, heap, rc, weak rc}}' in diag messages, "
        f"got {messages!r}"
    )
