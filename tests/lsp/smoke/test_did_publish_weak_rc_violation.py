"""Phase 27 weak rc Policy LSP regression-anchor — verifies E0299 + E0300 +
E0296 (extended for weak rc) surface through publishDiagnostics via the
CORE-22 facade (zero src/lsp/ source modifications).

NOT a Wave 0 RED gate -- by the time this fixture runs, Plans 27-01/02/03
have already committed IRON_ERR_WEAK_RC_DEREF=299 (POL-08 deref
enforcement), IRON_ERR_WEAK_RC_DOWNGRADE_NOT_RC=300 (POL-08 downgrade
receiver type), and the extended message for IRON_ERR_PTR_AMP_ON_RC=296
(POL-07 + Phase 27 GA4 — `&` on weak rc covered by the same diagnostic
code with updated message text). This smoke test is a regression anchor
verifying the CORE-22 facade still flows the new weak-rc-policy
diagnostic codes through publishDiagnostics, locking in the "zero LSP
code change required for new compiler-side diagnostics" invariant.

Three scenarios:
- POL-08 deref of `weak rc T`: field-access on a weak rc value triggers
  IRON_ERR_WEAK_RC_DEREF=299. Confirms code 299 surfaces via
  publishDiagnostics with the hint substring referencing `.upgrade()`.
- POL-08 `.downgrade()` on non-rc receiver: invokes downgrade on a heap
  binding (not rc), triggering IRON_ERR_WEAK_RC_DOWNGRADE_NOT_RC=300.
  Confirms code 300 surfaces with a `rc` substring in the diagnostic
  message.
- Phase 27 GA4 E0296 extension: `&` on a weak rc value triggers
  IRON_ERR_PTR_AMP_ON_RC=296 with the extended message naming both
  `rc` and `weak rc` (the `rc/weak rc` substring proves the Plan 27-02
  message extension landed without allocating a new diagnostic code).

Locked from Phase 27 CONTEXT.md and Phase 17-26 conventions:
- "ONE pytest-lsp regression-anchor smoke fixture verifying weak-rc-
   policy diagnostic codes flow through CORE-22 facade." (CONTEXT.md
   scope lock)
- "Existing CORE-22 facade automatically pipes new diagnostics through
   publishDiagnostics -- zero LSP code change required for the new codes
   to appear in editors."
- LSP wire format E<NNN> zero-padded; _matches_code helper accepts int,
  bare-string, or E-prefixed-string (inherited from Phase 18-26 baseline).
"""
from __future__ import annotations

import asyncio
import pytest
from lsprotocol import types

# POL-08 trigger: direct dereference of `weak rc T`.
# Field access `w.tag` on a weak rc binding obtained via .downgrade().
# Triggers IRON_ERR_WEAK_RC_DEREF=299 from src/analyzer/typecheck.c
# IRON_NODE_FIELD_ACCESS / IRON_NODE_METHOD_CALL arms whenever the
# receiver resolves to IRON_TYPE_WEAK_RC.
# Spec POL-08: weak rc cannot be dereferenced directly; user must call
# .upgrade() to obtain a strong reference and check the T? result.
_DEREF_WEAK_RC_SOURCE = (
    "object Counter {\n"
    "    val tag: Int\n"
    "}\n"
    "func main() {\n"
    "    val strong = rc Counter(42)\n"
    "    val observer = strong.downgrade()\n"
    "    val v = observer.tag\n"
    "}\n"
)

# POL-08 trigger: `.downgrade()` on a non-rc receiver.
# Calls .downgrade() on a heap binding (heap T, not rc T) — the receiver
# type is IRON_TYPE_PTR/HEAP, not IRON_TYPE_RC.
# Triggers IRON_ERR_WEAK_RC_DOWNGRADE_NOT_RC=300 from src/analyzer/
# typecheck.c IRON_NODE_METHOD_CALL arm with `.downgrade()` receiver-type
# check.
# Spec POL-08: downgrade() is only available on rc T (the source of weak
# references). Calling it on heap T / stack T / primitives is a compile
# error with the suggestion to allocate via rc T(...) instead.
_DOWNGRADE_NOT_RC_SOURCE = (
    "object Counter {\n"
    "    val tag: Int\n"
    "}\n"
    "func main() {\n"
    "    val h = heap Counter(7)\n"
    "    val w = h.downgrade()\n"
    "}\n"
)

# Phase 27 GA4 E0296 message-extension trigger: `&` on a weak rc value.
# Takes the address of a weak rc binding obtained via .downgrade().
# Triggers IRON_ERR_PTR_AMP_ON_RC=296 (same code as Phase 26, no new code
# allocated per CONTEXT.md GA4); message text is updated to mention both
# `rc` and `weak rc` regimes — proved by the `rc/weak rc` substring on
# the diagnostic message.
# Spec POL-07 + POL-08: rc and weak rc values are non-addressable; use
# weak rc T (Phase 27) for non-owning references or upgrade() for
# nullable strong references.
_AMP_ON_WEAK_RC_SOURCE = (
    "object Counter {\n"
    "    val tag: Int\n"
    "}\n"
    "func main() {\n"
    "    val strong = rc Counter(99)\n"
    "    val observer = strong.downgrade()\n"
    "    val r = &observer\n"
    "}\n"
)


def _matches_code(diag_code, numeric_tail: str) -> bool:
    """Match a diagnostic code against a numeric tail.

    Diagnostic code may be int (299), string ("299"), or LSP-wire
    E-prefixed string ("E0299") depending on the serializer. The
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
async def test_e0299_weak_rc_deref_via_publishdiagnostics(client, tmp_path):
    """End-to-end POL-08 (weak rc deref) -> publishDiagnostics smoke.

    Verifies IRON_ERR_WEAK_RC_DEREF=299 surfaces through the CORE-22
    facade. Locks the weak-rc dereference rejection in typecheck.c
    (Spec POL-08: `weak rc T` cannot be dereferenced directly;
    diagnostic suggests `.upgrade()` to obtain a nullable strong).
    """
    uri = (tmp_path / "deref_weak_rc.iron").as_uri()
    client.text_document_did_open(
        types.DidOpenTextDocumentParams(
            text_document=types.TextDocumentItem(
                uri=uri,
                language_id="iron",
                version=1,
                text=_DEREF_WEAK_RC_SOURCE,
            ),
        ),
    )
    await asyncio.wait_for(
        client.wait_for_notification(types.TEXT_DOCUMENT_PUBLISH_DIAGNOSTICS),
        timeout=5.0,
    )
    diags = client.diagnostics.get(uri, [])
    assert len(diags) >= 1, (
        f"expected >=1 diagnostic for POL-08 deref weak rc, got {diags!r}"
    )
    matching = [d for d in diags if _matches_code(d.code, "299")]
    assert matching, (
        f"expected diagnostic code 299 (IRON_ERR_WEAK_RC_DEREF), "
        f"got {[d.code for d in diags]!r}"
    )
    messages = [d.message for d in matching]
    # Hint should reference upgrade() / weak rc / strong reference
    assert any(
        (
            "upgrade" in m.lower()
            or "weak rc" in m.lower()
            or "strong" in m.lower()
        )
        for m in messages
    ), (
        f"expected substring 'upgrade'/'weak rc'/'strong' in diag messages, "
        f"got {messages!r}"
    )


@pytest.mark.asyncio
async def test_e0300_downgrade_not_rc_via_publishdiagnostics(client, tmp_path):
    """End-to-end POL-08 (downgrade not rc) -> publishDiagnostics smoke.

    Verifies IRON_ERR_WEAK_RC_DOWNGRADE_NOT_RC=300 surfaces through the
    CORE-22 facade. Locks the .downgrade() receiver-type check in
    typecheck.c (Spec POL-08: .downgrade() is only available on rc T;
    diagnostic mentions the rc-only restriction).
    """
    uri = (tmp_path / "downgrade_not_rc.iron").as_uri()
    client.text_document_did_open(
        types.DidOpenTextDocumentParams(
            text_document=types.TextDocumentItem(
                uri=uri,
                language_id="iron",
                version=1,
                text=_DOWNGRADE_NOT_RC_SOURCE,
            ),
        ),
    )
    await asyncio.wait_for(
        client.wait_for_notification(types.TEXT_DOCUMENT_PUBLISH_DIAGNOSTICS),
        timeout=5.0,
    )
    diags = client.diagnostics.get(uri, [])
    assert len(diags) >= 1, (
        f"expected >=1 diagnostic for POL-08 downgrade on non-rc, "
        f"got {diags!r}"
    )
    matching = [d for d in diags if _matches_code(d.code, "300")]
    assert matching, (
        f"expected diagnostic code 300 (IRON_ERR_WEAK_RC_DOWNGRADE_NOT_RC), "
        f"got {[d.code for d in diags]!r}"
    )
    messages = [d.message for d in matching]
    # Message should reference rc as the only valid receiver
    assert any(
        ("rc" in m.lower() or "downgrade" in m.lower())
        for m in messages
    ), (
        f"expected substring 'rc'/'downgrade' in diag messages, "
        f"got {messages!r}"
    )


@pytest.mark.asyncio
async def test_e0296_extended_amp_on_weak_rc_via_publishdiagnostics(client, tmp_path):
    """End-to-end Phase 27 GA4 (E0296 message extension) -> publishDiagnostics.

    Verifies the extended IRON_ERR_PTR_AMP_ON_RC=296 message (no new
    code allocated) surfaces through the CORE-22 facade with the
    `rc/weak rc` substring proving the Plan 27-02 message extension
    landed. CONTEXT.md GA4 explicitly forbids allocating a new code
    for the weak-rc address-of case; the same code 296 must cover both
    regimes.
    """
    uri = (tmp_path / "amp_on_weak_rc.iron").as_uri()
    client.text_document_did_open(
        types.DidOpenTextDocumentParams(
            text_document=types.TextDocumentItem(
                uri=uri,
                language_id="iron",
                version=1,
                text=_AMP_ON_WEAK_RC_SOURCE,
            ),
        ),
    )
    await asyncio.wait_for(
        client.wait_for_notification(types.TEXT_DOCUMENT_PUBLISH_DIAGNOSTICS),
        timeout=5.0,
    )
    diags = client.diagnostics.get(uri, [])
    assert len(diags) >= 1, (
        f"expected >=1 diagnostic for Phase 27 GA4 `&` on weak rc, "
        f"got {diags!r}"
    )
    matching = [d for d in diags if _matches_code(d.code, "296")]
    assert matching, (
        f"expected diagnostic code 296 (IRON_ERR_PTR_AMP_ON_RC), "
        f"got {[d.code for d in diags]!r}"
    )
    messages = [d.message for d in matching]
    # Phase 27 GA4 lock: the substring `rc/weak rc` proves the message
    # extension landed (the bare `rc` would match the Phase 26 message too).
    assert any(
        "rc/weak rc" in m
        for m in messages
    ), (
        f"expected substring 'rc/weak rc' in diag messages "
        f"(Phase 27 GA4 message extension), got {messages!r}"
    )
