"""Edit-scope pytest-lsp conftest — Phase 4 Plan 04-01 Wave 0.

Inherits session + function fixtures (client, raw_client, lsp_binary,
init_params_factory, fixture_dir, canonical_hello) from the shared
parent conftest at `tests/lsp/smoke/conftest.py`. Edit-scope tests
under this directory exercise EDIT-01..EDIT-15 (code-actions,
formatting, rename, quickfix, etc.).

This conftest mirrors the Phase 3 M2 nav-scope precedent at
`tests/lsp/smoke/nav/conftest.py` — namespace a fixture-directory
helper for EDIT fixtures, re-export the pytest-lsp client via the
parent conftest.
"""
from __future__ import annotations

import pathlib

import pytest


EDIT_FIXTURE_DIR = pathlib.Path(__file__).parent.parent.parent / "fixtures" / "edit"


@pytest.fixture(scope="session")
def edit_fixture_dir() -> pathlib.Path:
    return EDIT_FIXTURE_DIR
