#!/usr/bin/env python3
"""Generate deterministic HTTP/1.x request heads as one hex message per line."""
import argparse
import random
import string
import sys

TCHARS = string.ascii_letters + string.digits + "!#$%&'*+-.^_`|~"
VISIBLE = string.ascii_letters + string.digits + " !#$%&'*+-.^_`|~/?:;=,()[]{}"


def token(rng, lo=1, hi=14):
    return "".join(rng.choice(TCHARS) for _ in range(rng.randint(lo, hi)))


def target(rng):
    pieces = []
    for _ in range(rng.randint(0, 5)):
        part = "".join(rng.choice(string.ascii_letters + string.digits + "-._~")
                       for _ in range(rng.randint(0, 10)))
        if rng.random() < 0.25:
            part += "%{:02X}".format(rng.randrange(256))
        pieces.append(part)
    out = "/" + "/".join(pieces)
    if rng.random() < 0.45:
        out += "?" + "".join(rng.choice(string.ascii_letters + string.digits + "=&._~-")
                               for _ in range(rng.randint(0, 20)))
    return out


def field_value(rng):
    value = "".join(rng.choice(VISIBLE + "\t") for _ in range(rng.randint(0, 30)))
    return " " * rng.randint(0, 2) + value + "\t" * rng.randint(0, 2)


def valid(rng):
    version = rng.choice(["HTTP/1.0", "HTTP/1.1"])
    lines = [f"{token(rng)} {target(rng)} {version}"]
    if version == "HTTP/1.1" or rng.random() < 0.8:
        lines.append("Host: example.test")
    for _ in range(rng.randint(0, 8)):
        name = token(rng)
        if name.lower() in ("host", "content-length", "transfer-encoding", "expect"):
            name = "X-" + name
        lines.append(name + ":" + field_value(rng))
    if rng.random() < 0.2:
        lines.extend(["Connection: upgrade", "Connection: keep-alive"])
    return ("\r\n".join(lines) + "\r\n\r\n").encode("latin1")


def invalid(rng):
    message = bytearray(valid(rng))
    head_end = message.find(b"\r\n")
    kind = rng.randrange(6)
    if kind == 0:
        message[0] = ord("(")
    elif kind == 1:
        version = message.find(b"HTTP/1.")
        message[version] = ord("h")
    elif kind == 2:
        insert = head_end + 2
        message[insert:insert] = b"Bad Name: x\r\n"
    elif kind == 3:
        insert = head_end + 2
        message[insert:insert] = b"Bad(Name: x\r\n"
    elif kind == 4:
        insert = head_end + 2
        message[insert:insert] = b"X-Bad: a\x01b\r\n"
    else:
        message[-4:] = b"\rX\r\n"
    return bytes(message)


def generate(count, seed):
    rng = random.Random(seed)
    fixed = [
        b"GET / HTTP/1.1\r\nHost: x\r\n\r\n",
        b"OPTIONS * HTTP/1.1\r\nHost: x\r\n\r\n",
        b"CONNECT example.com:443 HTTP/1.1\r\nHost: example.com:443\r\n\r\n",
        b"GET http://example.test/a?q=1 HTTP/1.1\r\nHost: example.test\r\n\r\n",
    ]
    for message in fixed:
        yield message
    for _ in range(count - len(fixed)):
        yield valid(rng) if rng.random() < 0.7 else invalid(rng)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("count", type=int)
    parser.add_argument("seed", type=int)
    args = parser.parse_args()
    for message in generate(args.count, args.seed):
        print(message.hex())


if __name__ == "__main__":
    main()
