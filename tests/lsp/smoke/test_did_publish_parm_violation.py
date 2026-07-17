"""Phase 18 regression-anchor smoke: PARM-01/PARM-03 diagnostics surface through publishDiagnostics.

NOT a Wave 0 RED gate -- by the time this fixture runs, Plans 18-01/02
have already committed IRON_ERR_PARM_READ_ONLY=266 (PARM-01) and
IRON_ERR_PARM_VAR_SLOT_NEEDS_MUT=267 (PARM-03). This smoke test is a
regression anchor verifying the CORE-22 facade still flows the new
codes through publishDiagnostics, locking in the "zero LSP code change
required for new compiler-side diagnostics" invariant for Phase 34
LSP-06 to lean against when it adds quickfix code-actions.

Two scenarios:
- PARM-01 read-only parameter mutation: opens a buffer with
  `func bad(p: Int) { p = 99 }` and confirms the LSP server emits a
  Diagnostic carrying code 266 (IRON_ERR_PARM_READ_ONLY) with the
  spec-locked message substring "cannot mutate read-only parameter".
- PARM-03 read-only-arg-to-var-slot: opens a buffer with
  `func mutate(var x: Int) { ... }; func main() { val original = 0; mutate(original) }`
  and confirms the LSP server emits a Diagnostic carrying code 267
  (IRON_ERR_PARM_VAR_SLOT_NEEDS_MUT) with the spec-locked substring
  "cannot pass read-only argument".

Locked from Phase 18 CONTEXT.md and Phase 17 conventions:
- "One pytest-lsp smoke fixture added to verify the new diagnostics
   surface through the facade end-to-end."
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

# PARM-01 trigger: read-only parameter direct rebind. Triggers
# IRON_ERR_PARM_READ_ONLY=266 from src/analyzer/typecheck.c
# IRON_NODE_ASSIGN handler when sym_kind == IRON_SYM_PARAM and the
# parameter has is_var=false (read-only default).
_PARM_01_SOURCE = "func bad(p: Int) {\n    p = 99\n}\n\nfunc main() {\n    bad(5)\n}\n"

# PARM-03 trigger: read-only argument passed to var slot. Triggers
# IRON_ERR_PARM_VAR_SLOT_NEEDS_MUT=267 from src/analyzer/typecheck.c
# IRON_NODE_CALL handler when callee param has is_var=true and the
# argument source resolves to is_mutable=false (val binding here).
_PARM_03_SOURCE = (
    "func mutate(var x: Int) {\n"
    "    x = 42\n"
    "}\n"
    "\n"
    "func main() {\n"
    "    val original = 7\n"
    "    mutate(original)\n"
    "}\n"
)


def _matches_code(diag_code, numeric_tail: str) -> bool:
    """Match a diagnostic code against a numeric tail.

    Diagnostic code may be int (266), string ("266"), or LSP-wire
    E-prefixed string ("E0266") depending on the serializer. The
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
async def test_parm_read_only_mutation_publishes(client, tmp_path):
    """End-to-end PARM-01 -> publishDiagnostics regression-anchor smoke.

    Verifies IRON_ERR_PARM_READ_ONLY=266 surfaces with spec-locked
    message substring 'cannot mutate read-only parameter' through
    the CORE-22 facade.
    """
    uri = (tmp_path / "parm_read_only.iron").as_uri()
    client.text_document_did_open(
        types.DidOpenTextDocumentParams(
            text_document=types.TextDocumentItem(
                uri=uri,
                language_id="iron",
                version=1,
                text=_PARM_01_SOURCE,
            ),
        ),
    )
    await asyncio.wait_for(
        client.wait_for_notification(types.TEXT_DOCUMENT_PUBLISH_DIAGNOSTICS),
        timeout=5.0,
    )
    diags = client.diagnostics.get(uri, [])
    assert len(diags) >= 1, (
        f"expected >=1 diagnostic for PARM-01 read-only param mutation, "
        f"got {diags!r}"
    )
    matching = [d for d in diags if _matches_code(d.code, "266")]
    assert matching, (
        f"expected diagnostic code 266 (IRON_ERR_PARM_READ_ONLY), "
        f"got {[d.code for d in diags]!r}"
    )
    # Spec-locked substring; v4-fail/5-mutability/mutate_readonly_param.expected
    # asserts the same string. Both surfaces (CLI stderr + LSP message)
    # carry the same wording -- that is the CORE-22 invariant.
    messages = [d.message for d in matching]
    assert any("cannot mutate read-only parameter" in m for m in messages), (
        f"expected substring 'cannot mutate read-only parameter' in diag messages, "
        f"got {messages!r}"
    )


@pytest.mark.asyncio
async def test_parm_var_slot_needs_mut_publishes(client, tmp_path):
    """End-to-end PARM-03 -> publishDiagnostics regression-anchor smoke.

    Verifies IRON_ERR_PARM_VAR_SLOT_NEEDS_MUT=267 surfaces with the
    spec-locked message substring 'cannot pass read-only argument'
    through the CORE-22 facade. Locks the call-site (argument span)
    diagnostic distinct from PARM-01's body-span diagnostic.
    """
    uri = (tmp_path / "parm_var_slot.iron").as_uri()
    client.text_document_did_open(
        types.DidOpenTextDocumentParams(
            text_document=types.TextDocumentItem(
                uri=uri,
                language_id="iron",
                version=1,
                text=_PARM_03_SOURCE,
            ),
        ),
    )
    await asyncio.wait_for(
        client.wait_for_notification(types.TEXT_DOCUMENT_PUBLISH_DIAGNOSTICS),
        timeout=5.0,
    )
    diags = client.diagnostics.get(uri, [])
    assert len(diags) >= 1, (
        f"expected >=1 diagnostic for PARM-03 val-to-var-slot, "
        f"got {diags!r}"
    )
    matching = [d for d in diags if _matches_code(d.code, "267")]
    assert matching, (
        f"expected diagnostic code 267 (IRON_ERR_PARM_VAR_SLOT_NEEDS_MUT), "
        f"got {[d.code for d in diags]!r}"
    )
    messages = [d.message for d in matching]
    assert any("cannot pass read-only argument" in m for m in messages), (
        f"expected substring 'cannot pass read-only argument' in diag messages, "
        f"got {messages!r}"
    )
