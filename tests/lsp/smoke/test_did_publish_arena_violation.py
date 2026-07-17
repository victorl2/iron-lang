"""Phase 28 Arena Allocation LSP regression-anchor — verifies E0301
(rc/weak-rc-in-arena) surfaces through publishDiagnostics via the CORE-22
facade (zero src/lsp/ source modifications).

CORE-22 parity lock: the LSP must publish the SAME E0301 diagnostic that
`ironc check` emits on the CLI for an `in arena { val x = rc T(...) }` buffer.
The existing CORE-22 facade automatically pipes new compiler-side diagnostics
through publishDiagnostics — zero LSP code change is required for the new
Phase 28 codes to appear in editors. This smoke test is the regression anchor
locking that invariant for the arena-policy diagnostics.

Wave 0 RED note: by the time this fixture runs GREEN, Plan 28-03 has landed
IRON_ERR_RC_IN_ARENA=E0301 (extended to reject both rc and weak rc inside an
arena per 28-CONTEXT.md GA3). Until then the diagnostic does not yet surface
and the test is RED — the established TDD RED-anchor pattern (mirrors Phase 27
test_did_publish_weak_rc_violation.py).

Two scenarios:
- ARENA-08 rc-in-arena: `rc T(...)` inside an `in arena {}` block triggers
  IRON_ERR_RC_IN_ARENA=E0301. Confirms code 301 surfaces via
  publishDiagnostics with the `arena` substring in the message (the
  closed-policy fix points the user at non-arena allocation).
- GA3 weak-rc-in-arena extension: `weak rc T(...)` inside an `in arena {}`
  block triggers the SAME code E0301 (no new code allocated) with the
  message naming `weak rc`.

LSP wire format: E<NNN> zero-padded; _matches_code helper accepts int,
bare-string, or E-prefixed-string (inherited from the Phase 18-27 baseline).
"""
from __future__ import annotations

import asyncio
import pytest
from lsprotocol import types

# ARENA-08 trigger: `rc T(...)` inside an `in arena {}` block.
# `arena` is an ordinary identifier (NOT a keyword — see arena_keyword
# negative which stays failing). The analyzer flags the rc allocation
# lexically inside the in-arena block with IRON_ERR_RC_IN_ARENA=E0301.
# Spec ARENA-08 / 28-CONTEXT.md GA3: rc lifecycle conflicts with arena
# reset batch-invalidation; the closed-policy lattice forbids it.
_RC_IN_ARENA_SOURCE = (
    "object Texture {\n"
    "    val w: Int\n"
    "}\n"
    "func main() {\n"
    "    val arena = Arena.with_capacity(4096)\n"
    "    in arena {\n"
    "        val tex = rc Texture(w: 64)\n"
    "    }\n"
    "}\n"
)

# GA3 extension trigger: `weak rc T(...)` inside an `in arena {}` block.
# Same E0301 code (no new code allocated per 28-CONTEXT.md GA3) — the
# message names `weak rc`.
_WEAK_RC_IN_ARENA_SOURCE = (
    "object Texture {\n"
    "    val w: Int\n"
    "}\n"
    "func main() {\n"
    "    val arena = Arena.with_capacity(4096)\n"
    "    in arena {\n"
    "        val w = weak rc Texture(w: 64)\n"
    "    }\n"
    "}\n"
)


def _matches_code(diag_code, numeric_tail: str) -> bool:
    """Match a diagnostic code against a numeric tail.

    Diagnostic code may be int (301), string ("301"), or LSP-wire
    E-prefixed string ("E0301") depending on the serializer. The
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
async def test_e0301_rc_in_arena_via_publishdiagnostics(client, tmp_path):
    """End-to-end ARENA-08 (rc in arena) -> publishDiagnostics smoke.

    Verifies IRON_ERR_RC_IN_ARENA=E0301 surfaces through the CORE-22
    facade — the LSP publishes the same diagnostic `ironc check` emits
    (CORE-22 parity). Locks the rc-in-arena rejection (Spec ARENA-08:
    rc cannot be allocated in an arena; the closed-policy lattice points
    the user at non-arena allocation).
    """
    uri = (tmp_path / "rc_in_arena.iron").as_uri()
    client.text_document_did_open(
        types.DidOpenTextDocumentParams(
            text_document=types.TextDocumentItem(
                uri=uri,
                language_id="iron",
                version=1,
                text=_RC_IN_ARENA_SOURCE,
            ),
        ),
    )
    await asyncio.wait_for(
        client.wait_for_notification(types.TEXT_DOCUMENT_PUBLISH_DIAGNOSTICS),
        timeout=5.0,
    )
    diags = client.diagnostics.get(uri, [])
    assert len(diags) >= 1, (
        f"expected >=1 diagnostic for ARENA-08 rc in arena, got {diags!r}"
    )
    matching = [d for d in diags if _matches_code(d.code, "301")]
    assert matching, (
        f"expected diagnostic code 301 (IRON_ERR_RC_IN_ARENA), "
        f"got {[d.code for d in diags]!r}"
    )
    messages = [d.message for d in matching]
    # Message should reference arena (closed-policy fix: use non-arena alloc).
    assert any("arena" in m.lower() for m in messages), (
        f"expected substring 'arena' in diag messages, got {messages!r}"
    )


@pytest.mark.asyncio
async def test_e0301_weak_rc_in_arena_via_publishdiagnostics(client, tmp_path):
    """End-to-end GA3 (weak rc in arena) -> publishDiagnostics smoke.

    Verifies the GA3-extended IRON_ERR_RC_IN_ARENA=E0301 (no new code
    allocated) surfaces through the CORE-22 facade for `weak rc` inside
    an arena, with the `weak rc` substring proving the closed-policy
    lattice extension landed (28-CONTEXT.md GA3).
    """
    uri = (tmp_path / "weak_rc_in_arena.iron").as_uri()
    client.text_document_did_open(
        types.DidOpenTextDocumentParams(
            text_document=types.TextDocumentItem(
                uri=uri,
                language_id="iron",
                version=1,
                text=_WEAK_RC_IN_ARENA_SOURCE,
            ),
        ),
    )
    await asyncio.wait_for(
        client.wait_for_notification(types.TEXT_DOCUMENT_PUBLISH_DIAGNOSTICS),
        timeout=5.0,
    )
    diags = client.diagnostics.get(uri, [])
    assert len(diags) >= 1, (
        f"expected >=1 diagnostic for GA3 weak rc in arena, got {diags!r}"
    )
    matching = [d for d in diags if _matches_code(d.code, "301")]
    assert matching, (
        f"expected diagnostic code 301 (IRON_ERR_RC_IN_ARENA, GA3-extended), "
        f"got {[d.code for d in diags]!r}"
    )
    messages = [d.message for d in matching]
    # GA3 lock: the substring `weak rc` proves the extension covers weak rc.
    assert any("weak rc" in m.lower() for m in messages), (
        f"expected substring 'weak rc' in diag messages "
        f"(GA3 closed-policy extension), got {messages!r}"
    )
