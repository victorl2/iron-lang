#!/usr/bin/env python3
"""gen.py - random and mutated JSON documents for difffuzz.py.

usage: gen.py <count> <seed> > batch.hex     (one document per line, hex)

About two in five documents are valid JSON drawn from a grammar biased
towards the hard parts (escapes, surrogate pairs, multi-byte UTF-8, numbers
at the edges of the double and int64 ranges, deep nesting, duplicate keys,
odd whitespace); the rest are such documents mutated at the byte level
(flip, insert, delete, duplicate or truncate a span, splice in a token or an
invalid UTF-8 sequence).
"""
import random
import sys

HARD_NUMBERS = [
    "0", "-0", "0.0", "-0.0", "1", "-1", "1e0", "1E+0", "1e-0", "0e5", "1.5", "-2.5e-3", "9007199254740993",
    "9007199254740993.0", "9223372036854775807", "9223372036854775808", "-9223372036854775808",
    "-9223372036854775809", "18446744073709551615", "18446744073709551616", "1e23", "8.98846567431158e307",
    "1.7976931348623157e308", "1.7976931348623158e308", "1.7976931348623159e308", "1e309", "-1e309",
    "2.2250738585072011e-308", "2.2250738585072014e-308", "4.9406564584124654e-324", "2.4703282292062327e-324",
    "2.4703282292062328e-324", "1e-400", "-1e-400", "0.1", "0.2", "0.30000000000000004", "123456789012345678901234567890",
    "1.00000000000000011102230246251565404236316680908203125", "7.2057594037927933e16", "1448997445238699",
    "4.35679e-10", "1e-7", "1e21", "100000000000000000000", "1e1000000", "0." + "0" * 30 + "1", "1" + "0" * 40,
]
HARD_STRINGS = [
    "", "a", "\\\"", "\\\\", "\\/", "\\b\\f\\n\\r\\t", "\\u0000", "\\u001f", "\\u007f", "\\u00e9", "\\u00E9",
    "\\ud834\\udd1e", "\\uD83D\\uDE00", "\\udbff\\udfff", "\\ud800\\udc00", "\\ufeff", "\\uffff", "\\ufffe",
    "café", "日本", "\U0001F600", "\U0010FFFF", "  ", "\x7f", "﻿", "�",
    "tab\\there", "a" * 40, "é" * 20, "\\u0041\\u0042", "ends with backslash \\\\",
]
WS = [" ", "\t", "\n", "\r", "  ", "\r\n", ""]
INVALID_BYTES = [b"\x80", b"\xc0\x80", b"\xed\xa0\x80", b"\xf4\x90\x80\x80", b"\xff", b"\xc2", b"\xe0\xa0",
                 b"\xf0\x90\x80", b"\xef\xbb\xbf", b"\x00", b"\x1f", b"\\", b'"', b"\\u", b"\\ud800", b"\\udc00"]
TOKENS = [b"{", b"}", b"[", b"]", b",", b":", b'"', b"null", b"true", b"false", b"-", b".", b"e", b"E", b"+",
          b"0", b"1", b"NaN", b"Infinity", b"\\", b" ", b"\n", b"\t", b"/*", b"//", b"'"]


class Gen:
    def __init__(self, rng):
        self.r = rng

    def number(self):
        r = self.r
        k = r.random()
        if k < 0.35:
            return r.choice(HARD_NUMBERS)
        if k < 0.6:
            return str(r.randint(-10 ** r.randint(1, 20), 10 ** r.randint(1, 20)))
        if k < 0.8:
            return repr(r.uniform(-1, 1) * 10 ** r.randint(-320, 308))
        text = ("-" if r.random() < 0.3 else "") + str(r.randint(1, 9))
        text += "".join(str(r.randint(0, 9)) for _ in range(r.randint(0, 22)))
        if r.random() < 0.6:
            text += "." + "".join(str(r.randint(0, 9)) for _ in range(r.randint(1, 20)))
        if r.random() < 0.6:
            text += r.choice("eE") + r.choice(["", "+", "-"]) + str(r.randint(0, 400))
        return text

    def string(self):
        r = self.r
        parts = [r.choice(HARD_STRINGS) for _ in range(r.randint(0, 4))]
        if r.random() < 0.3:
            parts.append("".join(chr(r.choice([r.randint(0x20, 0x7e), r.randint(0xa0, 0xd7ff),
                                                r.randint(0xe000, 0xfffd), r.randint(0x10000, 0x10ffff)]))
                                 for _ in range(r.randint(1, 12))).replace("\\", "\\\\").replace('"', '\\"'))
        return '"' + "".join(parts) + '"'

    def ws(self):
        return self.r.choice(WS) if self.r.random() < 0.3 else ""

    def value(self, depth):
        r = self.r
        k = r.random()
        if depth > 0 and k < 0.25:
            items = [self.value(depth - 1) for _ in range(r.randint(0, 6))]
            return "[" + self.ws() + ("," + self.ws()).join(items) + self.ws() + "]"
        if depth > 0 and k < 0.5:
            keys = [self.string() for _ in range(r.randint(0, 6))]
            if keys and r.random() < 0.2:
                keys.append(r.choice(keys))                 # duplicate key
            members = [self.ws() + key + self.ws() + ":" + self.ws() + self.value(depth - 1) for key in keys]
            return "{" + ",".join(members) + self.ws() + "}"
        if k < 0.7:
            return self.number()
        if k < 0.9:
            return self.string()
        return r.choice(["null", "true", "false"])

    def document(self):
        r = self.r
        if r.random() < 0.03:
            n = r.choice([511, 512, 513, 600])
            return ("[" * n + "]" * n).encode()
        return (self.ws() + self.value(r.randint(0, 6)) + self.ws()).encode("utf-8")

    def mutate(self, data):
        r = self.r
        data = bytearray(data)
        for _ in range(r.randint(1, 3)):
            n = len(data)
            op = r.randrange(7)
            at = r.randint(0, n)
            if op == 0 and n:
                data[min(at, n - 1)] ^= 1 << r.randrange(8)
            elif op == 1:
                data[at:at] = bytes([r.randrange(256)])
            elif op == 2 and n:
                del data[at:at + r.randint(1, 4)]
            elif op == 3 and n:
                span = data[at:at + r.randint(1, 8)]
                data[at:at] = span
            elif op == 4:
                del data[at:]
            elif op == 5:
                data[at:at] = r.choice(INVALID_BYTES)
            else:
                data[at:at] = r.choice(TOKENS)
        return bytes(data)


def main():
    count, seed = int(sys.argv[1]), int(sys.argv[2])
    g = Gen(random.Random(seed))
    out = sys.stdout
    for _ in range(count):
        doc = g.document()
        if g.r.random() < 0.6:
            doc = g.mutate(doc)
        out.write((doc.hex() or "-") + "\n")


if __name__ == "__main__":
    main()
