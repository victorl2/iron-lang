"""Phase 24 regression-anchor smoke: DROP-01 (E0284) + DROP-08 (E0286) diagnostics
surface through publishDiagnostics.

NOT a Wave 0 RED gate — by the time this fixture runs, Plans 24-01/02 have
already committed IRON_ERR_DROP_DUPLICATE=284 (DROP-01) and
IRON_ERR_COPY_OF_NOCOPY_TYPE=286 (DROP-08). This smoke test is a regression
anchor verifying the CORE-22 facade still flows the new DROP codes through
publishDiagnostics, locking in the "zero LSP code change required for
new compiler-side diagnostics" invariant.

Two scenarios:
- DROP-01 duplicate drop block: declares an object with two `drop { ... }`
  blocks — triggers IRON_ERR_DROP_DUPLICATE=284. Confirms the diagnostic
  code 284 surfaces via publishDiagnostics with the spec-locked message
  substring "duplicate" or "drop".
- DROP-08 copy of nocopy type: assigns a nocopy object to a new binding,
  triggering a value-copy of a nocopy type — triggers
  IRON_ERR_COPY_OF_NOCOPY_TYPE=286. Confirms code 286 surfaces with the
  spec-locked message substring "nocopy" or "copy".

Locked from Phase 24 CONTEXT.md and Phase 17/18/20/21/22/23 conventions:
- "ONE pytest-lsp regression-anchor smoke fixture verifying DROP diagnostic
   codes flow through CORE-22 facade." (CONTEXT.md scope lock)
- "Existing CORE-22 facade automatically pipes new diagnostics through
   publishDiagnostics — zero LSP code change required for the new
   codes to appear in editors."
- LSP wire format E<NNN> zero-padded; _matches_code helper accepts int,
   bare-string, or E-prefixed-string (inherited from Phase 18-23 baseline).
"""
from __future__ import annotations

import asyncio
import pytest
from lsprotocol import types

# DROP-01 trigger: object with duplicate drop blocks.
# Triggers IRON_ERR_DROP_DUPLICATE=284 from src/analyzer/typecheck.c
# object-declaration arm (top-level dispatch walk finds second drop block
# after the first has been registered in the symbol table).
# §7.1: an object may declare at most one `drop { ... }` block.
_DROP_DUPLICATE_SOURCE = (
    "object Leaky {\n"
    "    val id: Int\n"
    "    drop {\n"
    "        println(\"first drop\")\n"
    "    }\n"
    "    drop {\n"
    "        println(\"second drop\")\n"
    "    }\n"
    "}\n"
    "func main() {\n"
    "    val x = Leaky(id: 1)\n"
    "}\n"
)

# DROP-08 trigger: value-copy of a nocopy object.
# Triggers IRON_ERR_COPY_OF_NOCOPY_TYPE=286 from src/analyzer/typecheck.c
# VAL_DECL arm (assignment of nocopy binding to new val binding forces
# an implicit copy which is forbidden for nocopy types).
# §7.2: nocopy objects may not be copied, passed by value, or assigned.
_COPY_OF_NOCOPY_SOURCE = (
    "nocopy object Handle {\n"
    "    val fd: Int\n"
    "}\n"
    "func main() {\n"
    "    val h1 = Handle(fd: 3)\n"
    "    val h2 = h1\n"
    "}\n"
)


def _matches_code(diag_code, numeric_tail: str) -> bool:
    """Match a diagnostic code against a numeric tail.

    Diagnostic code may be int (284), string ("284"), or LSP-wire
    E-prefixed string ("E0284") depending on the serializer. The
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
async def test_drop_duplicate_publishes(client, tmp_path):
    """End-to-end DROP-01 -> publishDiagnostics regression-anchor smoke.

    Verifies IRON_ERR_DROP_DUPLICATE=284 surfaces through the CORE-22 facade.
    Locks the duplicate-drop-block detection (§7.1: an object may declare at
    most one drop block; top-level dispatch walk finds the second block).
    """
    uri = (tmp_path / "drop_duplicate.iron").as_uri()
    client.text_document_did_open(
        types.DidOpenTextDocumentParams(
            text_document=types.TextDocumentItem(
                uri=uri,
                language_id="iron",
                version=1,
                text=_DROP_DUPLICATE_SOURCE,
            ),
        ),
    )
    await asyncio.wait_for(
        client.wait_for_notification(types.TEXT_DOCUMENT_PUBLISH_DIAGNOSTICS),
        timeout=5.0,
    )
    diags = client.diagnostics.get(uri, [])
    assert len(diags) >= 1, (
        f"expected >=1 diagnostic for DROP-01 duplicate drop, "
        f"got {diags!r}"
    )
    matching = [d for d in diags if _matches_code(d.code, "284")]
    assert matching, (
        f"expected diagnostic code 284 (IRON_ERR_DROP_DUPLICATE), "
        f"got {[d.code for d in diags]!r}"
    )
    messages = [d.message for d in matching]
    assert any(
        ("duplicate" in m.lower() or "drop" in m.lower())
        for m in messages
    ), (
        f"expected substring 'duplicate'/'drop' in diag messages, "
        f"got {messages!r}"
    )


@pytest.mark.asyncio
async def test_copy_of_nocopy_type_publishes(client, tmp_path):
    """End-to-end DROP-08 -> publishDiagnostics regression-anchor smoke.

    Verifies IRON_ERR_COPY_OF_NOCOPY_TYPE=286 surfaces through the CORE-22
    facade. Locks the nocopy copy-enforcement at val-decl site (§7.2: nocopy
    objects may not be copied, passed by value, or assigned to new bindings).
    """
    uri = (tmp_path / "copy_of_nocopy.iron").as_uri()
    client.text_document_did_open(
        types.DidOpenTextDocumentParams(
            text_document=types.TextDocumentItem(
                uri=uri,
                language_id="iron",
                version=1,
                text=_COPY_OF_NOCOPY_SOURCE,
            ),
        ),
    )
    await asyncio.wait_for(
        client.wait_for_notification(types.TEXT_DOCUMENT_PUBLISH_DIAGNOSTICS),
        timeout=5.0,
    )
    diags = client.diagnostics.get(uri, [])
    assert len(diags) >= 1, (
        f"expected >=1 diagnostic for DROP-08 copy of nocopy, "
        f"got {diags!r}"
    )
    matching = [d for d in diags if _matches_code(d.code, "286")]
    assert matching, (
        f"expected diagnostic code 286 (IRON_ERR_COPY_OF_NOCOPY_TYPE), "
        f"got {[d.code for d in diags]!r}"
    )
    messages = [d.message for d in matching]
    assert any(
        ("nocopy" in m.lower() or "copy" in m.lower())
        for m in messages
    ), (
        f"expected substring 'nocopy'/'copy' in diag messages, "
        f"got {messages!r}"
    )
