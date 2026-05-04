"""Fmt-scope pytest-lsp conftest -- Phase 5 Plan 05-02 (FMT-02, D-13).

Inherits session + function fixtures (client, raw_client, lsp_binary,
init_params_factory, fixture_dir, canonical_hello) from the shared
parent conftest at `tests/lsp/smoke/conftest.py`. Fmt-scope tests
under this directory exercise FMT-02 (full-document formatting) via
textDocument/formatting. Plans 05-03 / 05-04 append range + on-type
coverage here.

Adds two local helpers:

    clean_iron_source    -- a canonical parseable fixture used by the
                             happy-path and iron.toml tests.
    broken_iron_source   -- a canonical syntactically broken fixture
                             used by the refusal test.
    iron_toml_workspace  -- factory that writes an iron.toml at a
                             tmp_path root, returning the workspace
                             directory for clients that need a custom
                             [fmt] block loaded at initialize.
"""
from __future__ import annotations

import pathlib
import textwrap

import pytest


@pytest.fixture
def clean_iron_source() -> str:
    return textwrap.dedent(
        """\
        func main() {
            val x = 1
        }
        """
    )


@pytest.fixture
def broken_iron_source() -> str:
    # Unterminated paren list: the parser emits an error node and the
    # formatter refuses (D-03).
    return "func main(\n"


@pytest.fixture
def iron_toml_workspace(tmp_path: pathlib.Path):
    """Factory returning a tmp workspace directory with iron.toml written.

    Usage:
        root = iron_toml_workspace("[fmt]\\nindent_width = 4\\n")
    """

    def make(toml_content: str) -> pathlib.Path:
        root = tmp_path / "ws"
        root.mkdir(exist_ok=True)
        (root / "iron.toml").write_text(toml_content, encoding="utf-8")
        return root

    return make
