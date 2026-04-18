"""Phase 4 Plan 04-01 Wave 0 scaffold smoke — placeholder test file.

This file exists so pytest's collect phase is non-empty under
`tests/lsp/smoke/edit/`. Later plans under Phase 4 (04-02..04-07)
flip individual test_*_smoke.py files from stubs to live assertions;
this file is the anchor that keeps `lsp_edit_smoke` in the CTest
`phase-m3-invariant` label green from Task 01 onward.

Marked `pytest.mark.skip` on every test so the harness exits 0 until
the real end-to-end EDIT tests land.
"""
from __future__ import annotations

import pytest


pytestmark = pytest.mark.skip(reason="Wave 0 scaffold -- flipped in 04-02+")


def test_edit_smoke_scaffold_placeholder():
    """Anchor test — keeps pytest's collection non-empty so the
    `lsp_edit_smoke` CTest target exits 0 even before any real
    `test_*_smoke.py` lands."""
    assert True
