"""Nav-scope conftest -- inherits session fixtures from parent tests/lsp/smoke/conftest.py."""
from __future__ import annotations

import pathlib

import pytest


NAV_FIXTURE_DIR = pathlib.Path(__file__).parent.parent.parent / "fixtures" / "nav"


@pytest.fixture(scope="session")
def nav_fixture_dir() -> pathlib.Path:
    return NAV_FIXTURE_DIR
