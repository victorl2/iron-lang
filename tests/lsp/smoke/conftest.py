"""Phase 2 pytest-lsp conftest. Populated in Plan 06 with LanguageClient
fixtures, the canonical .iron corpus, and the real ironls launcher. This
stub exists so CMake's find_package(Python3) check has a directory to
point pytest at and so Plan 06 can grow fixtures without touching CMake."""

import pytest


def pytest_addoption(parser):
    parser.addoption(
        "--lsp-binary",
        action="store",
        default=None,
        help="Path to ironls binary",
    )


@pytest.fixture
def lsp_binary(request):
    return request.config.getoption("--lsp-binary")
