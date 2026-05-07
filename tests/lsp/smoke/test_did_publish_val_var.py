"""Phase 17 regression-anchor smoke: VAL-01 diagnostic surfaces through publishDiagnostics.

NOT a Wave 0 RED gate -- by the time this fixture runs, Plans 17-01/02
have already committed IRON_ERR_MISSING_VAL_VAR=176 plus the three
other Phase 17 diagnostic codes (265, 613, 614). This smoke test is a
regression anchor verifying the CORE-22 facade still flows the new
codes through publishDiagnostics, locking in the "zero LSP code change
required for new compiler-side diagnostics" invariant for Phase 34
LSP-06 to lean against when it adds quickfix code-actions.

Opens a buffer with a bare local binding (`x = 5` -- no val/var) and
confirms the LSP server emits a Diagnostic carrying code 176
(IRON_ERR_MISSING_VAL_VAR) with the spec-locked message substring
"must specify val or var".

Locked from Phase 17 CONTEXT.md:
- "One pytest-lsp smoke fixture added to verify the new diagnostics
   surface through the facade end-to-end."
- "Existing CORE-22 facade automatically pipes new diagnostics through
   publishDiagnostics -- zero LSP code change required for the new
   codes to appear in editors."
"""
from __future__ import annotations

import asyncio
import pytest
from lsprotocol import types

# Bare assignment at statement position -- VAL-01 trigger.
# Triggers IRON_ERR_MISSING_VAL_VAR=176 from src/analyzer/resolve.c
# emit_undefined branch when ResolveCtx.is_assign_lhs is true.
_BAD_SOURCE = "func main() {\n    x = 5\n}\n"


@pytest.mark.asyncio
async def test_missing_val_var_publishes(client, tmp_path):
    """End-to-end VAL-01 -> publishDiagnostics regression-anchor smoke."""
    uri = (tmp_path / "missing_val_var.iron").as_uri()
    client.text_document_did_open(
        types.DidOpenTextDocumentParams(
            text_document=types.TextDocumentItem(
                uri=uri,
                language_id="iron",
                version=1,
                text=_BAD_SOURCE,
            ),
        ),
    )
    await asyncio.wait_for(
        client.wait_for_notification(types.TEXT_DOCUMENT_PUBLISH_DIAGNOSTICS),
        timeout=5.0,
    )
    diags = client.diagnostics.get(uri, [])
    assert len(diags) >= 1, (
        f"expected >=1 diagnostic for bare 'x = 5', got {diags!r}"
    )
    # Diagnostic code may be int (176), string ("176"), or LSP-wire
    # E-prefixed string ("E0176") depending on the serializer. The
    # current ironls JSON path formats parser/semantic codes as
    # "E<3-digit>" on the wire (zero-padded). Accept any of these forms;
    # the invariant is the numeric tail equals 176.
    def _matches_176(code):
        if code is None:
            return False
        s = str(code)
        return s == "176" or s == "E0176" or s.endswith("176")

    matching = [d for d in diags if _matches_176(d.code)]
    assert matching, (
        f"expected diagnostic code 176 (IRON_ERR_MISSING_VAL_VAR), "
        f"got {[d.code for d in diags]!r}"
    )
    # Spec-locked substring; v4-fail/5-mutability/missing_val_var.expected
    # asserts the same string. Both surfaces (CLI stderr + LSP message)
    # carry the same wording -- that is the CORE-22 invariant.
    messages = [d.message for d in matching]
    assert any("must specify val or var" in m for m in messages), (
        f"expected substring 'must specify val or var' in diag messages, "
        f"got {messages!r}"
    )
