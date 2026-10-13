"""Regression coverage for the LSP smoke request timeout."""

from request_timeout import REQUEST_TIMEOUT_SECONDS


def test_request_timeout_allows_sanitizer_contention():
    assert REQUEST_TIMEOUT_SECONDS >= 30.0
