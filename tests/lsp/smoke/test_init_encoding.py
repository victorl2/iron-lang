"""Smoke: positionEncoding negotiation (CORE-07).

The server advertises support for both utf-8 and utf-16; per
capabilities.c it prefers utf-8 when the client's general list
includes it, else utf-16, else utf-16 default.

Each case uses its own initialize invocation inside a single spawned
client to avoid the cost of four subprocess startups. The client
fixture's default init is skipped by using raw_client.
"""
from __future__ import annotations

import asyncio
import os
import pytest
from lsprotocol import types
from pygls.exceptions import JsonRpcException


async def _negotiate_then_shutdown(raw_client, encodings):
    """Run a single initialize with the given encodings and return the
    server's chosen positionEncoding, then drive shutdown+exit so the
    server process terminates cleanly."""
    caps = types.ClientCapabilities()
    if encodings is not None:
        caps = types.ClientCapabilities(
            general=types.GeneralClientCapabilities(
                position_encodings=encodings,
            ),
        )
    init = types.InitializeParams(
        process_id=os.getpid(),
        capabilities=caps,
    )
    # initialize_session = initialize_async + initialized; we need the
    # session primitive so pytest-lsp accepts subsequent protocol calls
    # in teardown (shutdown_session also uses this state flag).
    result = await raw_client.initialize_session(init)
    return result.capabilities.position_encoding


@pytest.mark.asyncio
async def test_client_offers_utf8_only(raw_client):
    chosen = await _negotiate_then_shutdown(
        raw_client, [types.PositionEncodingKind.Utf8])
    assert chosen == types.PositionEncodingKind.Utf8


@pytest.mark.asyncio
async def test_client_offers_utf16_only(raw_client):
    chosen = await _negotiate_then_shutdown(
        raw_client, [types.PositionEncodingKind.Utf16])
    assert chosen == types.PositionEncodingKind.Utf16


@pytest.mark.asyncio
async def test_client_offers_both_prefers_utf8(raw_client):
    chosen = await _negotiate_then_shutdown(raw_client, [
        types.PositionEncodingKind.Utf8,
        types.PositionEncodingKind.Utf16,
    ])
    assert chosen == types.PositionEncodingKind.Utf8


@pytest.mark.asyncio
async def test_client_offers_none_falls_back_utf16(raw_client):
    chosen = await _negotiate_then_shutdown(raw_client, None)
    assert chosen == types.PositionEncodingKind.Utf16
