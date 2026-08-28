#!/usr/bin/env python3
"""Small dependency-free RFC 6455 client used to interoperate with Iron WSS."""

from __future__ import annotations

import base64
import hashlib
import os
import socket
import ssl
import struct
import sys


def read_exact(stream: ssl.SSLSocket, length: int) -> bytes:
    chunks = bytearray()
    while len(chunks) < length:
        chunk = stream.recv(length - len(chunks))
        if not chunk:
            raise RuntimeError("unexpected EOF")
        chunks.extend(chunk)
    return bytes(chunks)


def send_masked(stream: ssl.SSLSocket, opcode: int, payload: bytes) -> None:
    mask = os.urandom(4)
    if len(payload) < 126:
        header = bytes((0x80 | opcode, 0x80 | len(payload)))
    elif len(payload) <= 0xFFFF:
        header = bytes((0x80 | opcode, 0xFE)) + struct.pack("!H", len(payload))
    else:
        header = bytes((0x80 | opcode, 0xFF)) + struct.pack("!Q", len(payload))
    masked = bytes(byte ^ mask[index & 3] for index, byte in enumerate(payload))
    stream.sendall(header + mask + masked)


def receive(stream: ssl.SSLSocket) -> tuple[int, bytes]:
    first, second = read_exact(stream, 2)
    if not first & 0x80 or second & 0x80:
        raise RuntimeError("Iron server emitted an invalid frame")
    length = second & 0x7F
    if length == 126:
        length = struct.unpack("!H", read_exact(stream, 2))[0]
    elif length == 127:
        length = struct.unpack("!Q", read_exact(stream, 8))[0]
    return first & 0x0F, read_exact(stream, length)


def main() -> int:
    certificate, port_text = sys.argv[1:3]
    port = int(port_text)
    context = ssl.create_default_context(cafile=certificate)
    with socket.create_connection(("127.0.0.1", port), timeout=5) as tcp:
        with context.wrap_socket(tcp, server_hostname="localhost") as stream:
            nonce = os.urandom(16)
            key = base64.b64encode(nonce).decode("ascii")
            request = (
                f"GET /echo HTTP/1.1\r\nHost: localhost:{port}\r\n"
                "Upgrade: websocket\r\nConnection: Upgrade\r\n"
                f"Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n"
            )
            stream.sendall(request.encode("ascii"))
            response = bytearray()
            while b"\r\n\r\n" not in response:
                response.extend(stream.recv(4096))
            expected = base64.b64encode(hashlib.sha1(
                (key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode("ascii")
            ).digest())
            if b"HTTP/1.1 101" not in response or expected not in response:
                raise RuntimeError(f"bad handshake: {response!r}")
            payload = b"python-to-iron\x00\xff"
            send_masked(stream, 2, payload)
            opcode, echoed = receive(stream)
            if opcode != 2 or echoed != payload:
                raise RuntimeError("binary echo mismatch")
            send_masked(stream, 8, struct.pack("!H", 1000) + b"done")
            opcode, _ = receive(stream)
            if opcode != 8:
                raise RuntimeError("missing close response")
    print("WSS interop ok: verified TLS, upgrade, binary echo, close")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
