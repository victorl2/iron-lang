#!/usr/bin/env python3
"""ref_python.py - Python's json as a strict RFC 8259 reference for difffuzz.py.

usage: ref_python.py <batch file>   (one document per line, hex encoded)

Prints "ERR <reason>" or "OK <canonical form>" per document, in the format
of jcanon.iron. json.loads is lenient in a few places, so the input is held
to RFC 8259 around it:

  * the bytes must be well-formed UTF-8 (decoded here with errors=strict;
    json.loads(bytes) would also guess UTF-16/32), and a BOM is an error;
  * NaN, Infinity and -Infinity are errors (parse_constant raises);
  * a string with a lone surrogate (from a \\uD800 style escape) is an error;
  * a number that rounds to infinity is an error;
  * strict=True (the default) already rejects raw control characters.

Numbers: an integer literal that fits in 64 bits is i:<n>, a larger one or
any number with a fraction or exponent is f:<repr of the nearest double>.
"""
import json
import math
import sys

INT64_MIN, INT64_MAX = -(1 << 63), (1 << 63) - 1


class Reject(Exception):
    pass


def number(text):
    if not any(c in text for c in ".eE"):
        if len(text) <= 20:
            v = int(text)
            if INT64_MIN <= v <= INT64_MAX:
                return "i:%d" % v
    f = float(text)
    if math.isinf(f):
        raise Reject("number out of range")
    return "f:" + repr(f)


def string(s):
    try:
        return "s:" + s.encode("utf-8").hex()
    except UnicodeEncodeError:
        raise Reject("lone surrogate")


def constant(name):
    raise Reject("non-standard constant " + name)


def canon(v):
    if v is None:
        return "null"
    if v is True:
        return "true"
    if v is False:
        return "false"
    if isinstance(v, str):
        return string(v)
    if isinstance(v, tuple):
        kind, payload = v
        if kind == "num":
            return payload
        return "{" + ",".join(string(k) + ":" + canon(x) for k, x in payload) + "}"
    if isinstance(v, list):
        return "[" + ",".join(canon(x) for x in v) + "]"
    raise TypeError(type(v))


def parse(data):
    try:
        text = data.decode("utf-8")
    except UnicodeDecodeError:
        raise Reject("invalid UTF-8")
    if text.startswith("﻿"):
        raise Reject("BOM")
    try:
        v = json.loads(text, parse_int=lambda s: ("num", number(s)), parse_float=lambda s: ("num", number(s)),
                       parse_constant=constant, object_pairs_hook=lambda p: ("obj", p))
    except (ValueError, RecursionError) as e:
        raise Reject(type(e).__name__)
    return canon(v)


def main():
    sys.setrecursionlimit(20000)
    out = []
    with open(sys.argv[1]) as f:
        for line in f:
            line = line.rstrip("\n")
            if not line:
                continue
            data = b"" if line == "-" else bytes.fromhex(line)
            try:
                out.append("OK " + parse(data))
            except Reject as e:
                out.append("ERR " + str(e).split("\n")[0][:60])
    sys.stdout.write("\n".join(out) + ("\n" if out else ""))


if __name__ == "__main__":
    main()
