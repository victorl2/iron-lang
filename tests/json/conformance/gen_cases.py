#!/usr/bin/env python3
"""gen_cases.py - writes the JSON conformance manifests for conformance.iron.

usage: gen_cases.py <out_dir> [--random N] [--seed S]

Every manifest line is one case, tab separated:

    expect   input_hex   output_hex   opts   description

expect      accept | reject | reject:<code>   (codes: 4000 syntax, 4001 depth,
            4002 size, 4003 UTF-8, 4004 string, 4005 number)
input_hex   the document bytes, hex encoded (so any byte sequence fits)
output_hex  what Json.to_string() must print for an accepted document, or -
opts        - or depth=N,bytes=M (Json.parse_with limits)

The expected outputs come from an independent model of the json module's
documented behaviour (iron_number, iron_quote, iron_dump below), never from
running Iron: numbers are correctly rounded by Python's float(), shortest
digits come from Python's repr(), strings are decoded by Python's UTF-8
codec. Categories (one manifest each):

    numbers         hard cases for parsing and printing doubles and integers
    numbers_random  random doubles (every exponent, subnormals), decimal
                    strings, exact halfway points and integers (seeded)
    strings         escapes, surrogates, control characters, UTF-8 classes
    utf8_sweep      every 1 and 2 byte string body, sampled 3 and 4 byte ones
    string_offsets  a special byte at every offset of short strings (the
                    scanner reads strings eight bytes at a time)
    structure       grammar, whitespace, literals, top-level values, BOM,
                    duplicate keys, nesting and size limits
    jsonchecker     json.org JSON_checker (nativejson/jsonchecker)
    roundtrip       nativejson-benchmark roundtrip documents
"""
import json
import math
import os
import random
import struct
import sys
from decimal import Decimal, getcontext

getcontext().prec = 2000
HERE = os.path.dirname(os.path.abspath(__file__))
INT64_MIN, INT64_MAX = -(1 << 63), (1 << 63) - 1

# ── Model of the json module ────────────────────────────────────────────────


def iron_float(x):
    """Json.float(x).to_string(): shortest round-trip digits, plain notation
    for decimal exponents -7..20 (with ".0" on whole values), otherwise
    d.ddde+XX; -0.0 keeps its sign."""
    if x == 0:
        return "-0.0" if math.copysign(1.0, x) < 0 else "0.0"
    neg = x < 0
    sign, digits, exp = Decimal(repr(abs(x))).as_tuple()
    ds = "".join(map(str, digits))
    e = exp + len(ds) - 1          # exponent of the first digit
    ds = ds.rstrip("0") or "0"
    n = len(ds)
    if -7 <= e < 21:
        if e >= 0:
            whole = (ds + "0" * (e + 1))[: e + 1]
            frac = ds[e + 1:] or "0"
            s = whole + "." + frac
        else:
            s = "0." + "0" * (-e - 1) + ds
    else:
        s = ds[0] + ("." + ds[1:] if n > 1 else "") + "e" + ("-" if e < 0 else "+") + str(abs(e))
    return ("-" if neg else "") + s


def iron_number(text):
    """(accepted, output) for a syntactically valid JSON number: integers
    without fraction or exponent that fit in 64 bits stay exact Ints, every
    other number is the nearest double; a number that rounds to infinity is
    rejected."""
    if not any(c in text for c in ".eE"):
        v = int(text)
        if INT64_MIN <= v <= INT64_MAX:
            return True, str(v)
    f = float(text)
    if math.isinf(f):
        return False, None
    return True, iron_float(f)


def iron_quote(s):
    """The writer's string literal: \\" \\\\ \\n \\r \\t \\b \\f, other C0
    controls as \\u00xx (lowercase hex), everything else as raw UTF-8."""
    out = ['"']
    for ch in s:
        o = ord(ch)
        if ch == '"':
            out.append('\\"')
        elif ch == "\\":
            out.append("\\\\")
        elif ch == "\n":
            out.append("\\n")
        elif ch == "\r":
            out.append("\\r")
        elif ch == "\t":
            out.append("\\t")
        elif ch == "\b":
            out.append("\\b")
        elif ch == "\f":
            out.append("\\f")
        elif o < 0x20:
            out.append("\\u%04x" % o)
        else:
            out.append(ch)
    out.append('"')
    return "".join(out)


class Num:
    def __init__(self, text):
        self.text = text


def model_load(data):
    """Python's json on a document known to be valid, keeping number texts
    and duplicate keys (object_pairs_hook) so iron_dump can model Iron."""
    return json.loads(data.decode("utf-8"), parse_int=Num, parse_float=Num,
                      parse_constant=lambda c: 1 / 0, object_pairs_hook=lambda p: ("obj", p))


def iron_dump(v):
    if v is None:
        return "null"
    if v is True:
        return "true"
    if v is False:
        return "false"
    if isinstance(v, Num):
        ok, out = iron_number(v.text)
        if not ok:
            raise ValueError("out of range number in an accepted case")
        return out
    if isinstance(v, str):
        return iron_quote(v)
    if isinstance(v, list):
        return "[" + ",".join(iron_dump(x) for x in v) + "]"
    if isinstance(v, tuple) and v[0] == "obj":
        return "{" + ",".join(iron_quote(k) + ":" + iron_dump(x) for k, x in v[1]) + "}"
    raise TypeError(v)


def expected_output(data):
    return iron_dump(model_load(data)).encode("utf-8")

# ── Manifest writing ───────────────────────────────────────────────────────


class Manifest:
    def __init__(self, name):
        self.name = name
        self.lines = []
        self.seen = set()

    def add(self, expect, data, desc, out=None, opts="-", model=True):
        if isinstance(data, str):
            data = data.encode("utf-8")
        key = (data, opts)
        if key in self.seen:
            return
        self.seen.add(key)
        if expect == "accept" and out is None and model:
            out = expected_output(data)
        if isinstance(out, str):
            out = out.encode("utf-8")
        desc = desc.replace("\t", " ").replace("\n", " ")
        self.lines.append("\t".join([expect, data.hex() or "-", out.hex() if out is not None else "-",
                                     opts, desc[:120]]))

    def write(self, out_dir):
        with open(os.path.join(out_dir, self.name + ".tsv"), "w") as f:
            for line in self.lines:
                f.write(line + "\n")
        return len(self.lines)


def add_number(m, text, desc, contexts=True):
    ok, out = iron_number(text)
    if ok:
        m.add("accept", text, desc, out=out)
        if contexts:
            m.add("accept", "[" + text + "]", desc + " (in an array)", out="[" + out + "]")
            m.add("accept", '{"n":' + text + "}", desc + " (member value)", out='{"n":' + out + "}")
    else:
        m.add("reject:4005", text, desc + " (rounds to infinity)")

# ── numbers ────────────────────────────────────────────────────────────────


def gen_numbers():
    m = Manifest("numbers")
    cases = [
        ("0", "zero"), ("-0", "negative zero integer (an Int 0)"), ("0.0", "zero float"),
        ("-0.0", "negative zero float keeps its sign"), ("0e0", "zero with exponent"),
        ("-0e-0", "negative zero with exponent"), ("0E+5", "zero, capital E"),
        ("0e999999999", "zero with a huge exponent"), ("-0.0e-999999999", "negative zero, huge negative exponent"),
        ("1", "one"), ("-1", "minus one"), ("1.0", "whole float"), ("1E2", "capital E"), ("1e+2", "explicit plus"),
        ("1e-2", "negative exponent"), ("1e0000000000000000000002", "exponent with leading zeros"),
        ("123456789", "integer"), ("0.1", "0.1"), ("0.2", "0.2"), ("0.3", "0.3"), ("1e23", "1e23 (classic misround)"),
        ("8.98846567431158e307", "2^1023 neighbourhood"), ("9007199254740992", "2^53 integer"),
        ("9007199254740993", "2^53+1 integer stays exact as an Int"),
        ("9007199254740993.0", "2^53+1 as a float rounds to even"),
        ("9007199254740995.0", "2^53+3 as a float rounds up to even"),
        ("1.00000000000000011102230246251565404236316680908203125", "1 + 2^-53 exactly: halfway, to even (1.0)"),
        ("1.00000000000000011102230246251565404236316680908203126", "just above 1 + 2^-53: rounds up"),
        ("1.00000000000000011102230246251565404236316680908203124", "just below 1 + 2^-53: rounds down"),
        ("7.2057594037927933e16", "Eisel-Lemire hard case"), ("1448997445238699", "fast_float issue case"),
        ("2.2250738585072011e-308", "the PHP/Java hang value (largest subnormal neighbourhood)"),
        ("2.2250738585072012e-308", "rounds to the smallest normal"),
        ("2.2250738585072014e-308", "smallest normal"), ("2.225073858507201e-308", "largest subnormal"),
        ("4.9406564584124654e-324", "smallest subnormal"), ("5e-324", "smallest subnormal, short"),
        ("2.4703282292062327e-324", "just below half the smallest subnormal: 0"),
        ("2.4703282292062328e-324", "just above half the smallest subnormal: rounds up"),
        ("2.47032822920623272088284396434110686182529901307162382212792841250337753635104375932649918180817996189898282347722858865463328355177969898199387398005390939063150356595155702263922908583924491051844359318028499365361525003193704576782492193656236698636584807570015857692699037063119282795585513329278343384093519780155312465972635795746227664652728272200563740064854999770965994704540208281662262378573934507363390079677619305775067401763246736009689513405355374585166611342237666786041621596804619144672918403005300575308490487653917113865916462395249126236538818796362393732804238910186723484976682350898633885879256283027559956575244555072551893136908362547791869486679949683240497058210285131854513962138377228261454376934125320985913276672363281255",
          "exact half of the smallest subnormal plus a tail digit: rounds up"),
        ("2e-324", "below half the smallest subnormal: 0"), ("3e-324", "rounds to the smallest subnormal"),
        ("1e-400", "underflows to 0"), ("-1e-400", "underflows to -0.0"),
        ("1.7976931348623157e308", "largest double"), ("1.7976931348623158e308", "rounds to the largest double"),
        ("1.7976931348623158079e308", "just below the overflow threshold"),
        ("1.7976931348623159e308", "rounds to infinity"), ("1e309", "overflow"), ("-1e309", "negative overflow"),
        ("1e400", "overflow"), ("123e-456789", "very small"), ("123e456789", "very large"),
        ("9223372036854775807", "INT64_MAX"), ("-9223372036854775808", "INT64_MIN"),
        ("9223372036854775808", "INT64_MAX + 1 becomes a Float"), ("-9223372036854775809", "INT64_MIN - 1 becomes a Float"),
        ("18446744073709551615", "UINT64_MAX"), ("18446744073709551616", "2^64"), ("-18446744073709551616", "-2^64"),
        ("100000000000000000000", "10^20"), ("1000000000000000000000", "10^21 (exponent form)"),
        ("123456789012345678901234567890", "30 digit integer"), ("1" + "0" * 308, "10^308 as an integer literal"),
        ("1" + "0" * 309, "10^309 as an integer literal (overflow)"),
        (str(2 ** 1024), "2^1024 as an integer literal (overflow)"), (str(2 ** 1024 - 2 ** 970), "the overflow threshold exactly (rounds to infinity)"),
        (str(2 ** 1024 - 2 ** 970 - 1), "one below the overflow threshold: largest double"),
        ("0." + "0" * 400 + "1e401", "400 leading fraction zeros scaled back to 1"),
        ("1" + "0" * 400 + "e-400", "400 trailing integer zeros scaled back to 1"),
        ("0." + "0" * 30 + "123456789012345678901234567890e30", "many digits after leading zeros"),
        ("3.14159265358979323846264338327950288419716939937510582097494459", "pi, 63 digits"),
        ("1.0000000000000000000000000000000000000000000000000000000000000000000000001", "long tail just above 1"),
        ("0.99999999999999999999999999999999999999999999999999999999999999999", "long tail just below 1"),
        ("4.35679e-10", "simdjson issue case"), ("1.2345678901234567e10", "17 digits"),
        ("12345678901234567890", "20 digit integer: a Float"),
        ("0.000001", "1e-6 plain"), ("0.0000001", "1e-7 boundary of plain notation"), ("0.00000001", "1e-8 exponent form"),
        ("123456789012345680000.0", "1.2345678901234568e20 plain"), ("1.5e300", "large"), ("1.5e-300", "small"),
        ("-5e-324", "negative smallest subnormal"), ("1e-7", "1e-7"), ("1e21", "1e21"), ("1e20", "1e20"),
        ("9.999999999999999e22", "just below 1e23"), ("1.7976931348623157E+308", "max with E+"),
        ("0.5e1", "0.5e1"), ("10.0e-1", "10.0e-1"), ("-123.456e-789", "underflow negative"),
    ]
    for text, desc in cases:
        add_number(m, text, desc)
    # Exponent saturation: a huge exponent next to many digits that cancel it.
    z = "0" * 100000
    add_number(m, "0." + z + "1e1000000", "100000 fraction zeros then exponent 10^6: overflow", contexts=False)
    add_number(m, "1" + z + "e-1000000", "100000 integer zeros then exponent -10^6: underflow to 0", contexts=False)
    add_number(m, "0." + z + "1e100001", "100000 fraction zeros cancelled by exponent 100001: 1.0", contexts=False)
    add_number(m, "1" + z + "e-100000", "100000 integer zeros cancelled: 1.0", contexts=False)
    add_number(m, "1e" + "9" * 30, "exponent with 30 digits: overflow", contexts=False)
    add_number(m, "1e-" + "9" * 30, "negative exponent with 30 digits: 0", contexts=False)
    add_number(m, "0e" + "9" * 30, "zero with a 30 digit exponent", contexts=False)
    # Powers of ten and two, and their neighbours.
    for e in range(-330, 311):
        add_number(m, "1e%d" % e, "power of ten", contexts=False)
    for e in range(-1074, 1024, 7):
        x = math.ldexp(1.0, e)
        add_number(m, repr(x), "power of two (repr)", contexts=False)
        add_number(m, "%.17g" % x, "power of two (%.17g)", contexts=False)
        add_number(m, iron_float(x), "power of two (Iron form)", contexts=False)
    # Integers around the 64-bit limits and around 2^53.
    for base in (INT64_MAX, INT64_MIN, 1 << 53, -(1 << 53), 1 << 64, 10 ** 19, 0):
        for d in range(-3, 4):
            add_number(m, str(base + d), "integer near a limit", contexts=False)
    # Invalid number syntax.
    for text, desc in [
        ("01", "leading zero"), ("-01", "negative leading zero"), ("00", "double zero"), ("00.1", "double zero fraction"),
        ("1.", "no fraction digits"), (".1", "no integer part"), ("-", "minus only"), ("+1", "plus sign"),
        ("1e", "empty exponent"), ("1e+", "exponent sign only"), ("1E-", "exponent sign only"), ("0x10", "hex"),
        ("1.0e", "empty exponent after fraction"), ("--1", "double minus"), ("-.1", "minus dot"), ("1.e1", "dot then e"),
        ("Infinity", "Infinity"), ("-Infinity", "-Infinity"), ("NaN", "NaN"), ("-NaN", "-NaN"), ("1_000", "underscore"),
        ("1e1.5", "fractional exponent"), ("- 1", "space after minus"), ("1 .5", "space before fraction"),
        ("0.e1", "zero dot e"), ("1ee2", "double e"), ("1e+-2", "two signs"), ("1.5.2", "two dots"),
        ("١", "Arabic-Indic digit"), ("１", "fullwidth digit"), ("1 ", "trailing NBSP"),
        ("-0x0", "negative hex"), ("0b1", "binary"), ("1f", "C suffix"), ("1L", "C suffix"), ("1e5x", "trailing letter"),
        ("[1.]", "no fraction digits in an array"), ("[-]", "minus only in an array"), ("[.5]", "dot first in an array"),
        ("[1e]", "empty exponent in an array"), ('{"a":01}', "leading zero member"), ("012", "leading zero"),
    ]:
        m.add("reject", text, desc)
    return m


def random_double(rng):
    while True:
        bits = rng.getrandbits(64)
        kind = rng.random()
        if kind < 0.15:                      # subnormal
            bits &= (1 << 63) | ((1 << 52) - 1)
        elif kind < 0.25:                    # few significant bits
            bits &= ~((1 << rng.randint(20, 52)) - 1)
        x = struct.unpack("<d", struct.pack("<Q", bits))[0]
        if math.isfinite(x):
            return x


def gen_numbers_random(n, rng):
    m = Manifest("numbers_random")
    for i in range(n):
        kind = i % 6
        if kind == 0:
            x = random_double(rng)
            add_number(m, repr(x), "random double, repr", contexts=False)
        elif kind == 1:
            x = random_double(rng)
            add_number(m, "%.17g" % x, "random double, 17 digits", contexts=False)
            add_number(m, "%.17e" % x, "random double, 18 digits", contexts=False)
        elif kind == 2:
            nd = rng.randint(1, 25)
            digits = str(rng.randint(1, 9)) + "".join(str(rng.randint(0, 9)) for _ in range(nd - 1))
            fd = rng.randint(0, 20)
            text = ("-" if rng.random() < 0.5 else "") + digits
            if fd:
                text += "." + "".join(str(rng.randint(0, 9)) for _ in range(fd))
            text += "e%d" % rng.randint(-345, 310)
            add_number(m, text, "random decimal string", contexts=False)
        elif kind == 3:
            # Exactly halfway between two doubles, and just either side.
            x = abs(random_double(rng))
            up = math.nextafter(x, math.inf)
            if math.isinf(up):
                continue
            mid = (Decimal(x) + Decimal(up)) / 2
            tiny = Decimal(10) ** (mid.adjusted() - 40)
            for v, what in ((mid, "exact halfway point"), (mid + tiny, "just above halfway"),
                            (mid - tiny, "just below halfway")):
                text = str(v).replace("E", "e")
                if "." not in text and "e" not in text and rng.random() < 0.5:
                    text += ".0"
                add_number(m, text, what, contexts=False)
        elif kind == 4:
            nd = rng.randint(1, 22)
            v = rng.randint(10 ** (nd - 1), 10 ** nd - 1) * (-1 if rng.random() < 0.5 else 1)
            add_number(m, str(v), "random integer", contexts=False)
        else:
            # Shortest forms printed by Iron must read back as themselves.
            x = random_double(rng)
            add_number(m, iron_float(x), "random double, Iron form", contexts=False)
    return m

# ── strings ────────────────────────────────────────────────────────────────


def jstr(body):
    """A JSON string document around raw body bytes."""
    if isinstance(body, str):
        body = body.encode("utf-8")
    return b'"' + body + b'"'


VALID_UTF8 = [
    (b"\xc2\x80", "U+0080 first 2-byte"), (b"\xdf\xbf", "U+07FF last 2-byte"),
    (b"\xe0\xa0\x80", "U+0800 first 3-byte"), (b"\xed\x9f\xbf", "U+D7FF before surrogates"),
    (b"\xee\x80\x80", "U+E000 after surrogates"), (b"\xef\xbf\xbd", "U+FFFD"),
    (b"\xef\xbf\xbe", "U+FFFE noncharacter"), (b"\xef\xbf\xbf", "U+FFFF noncharacter"),
    (b"\xef\xb7\x90", "U+FDD0 noncharacter"), (b"\xef\xb7\xaf", "U+FDEF noncharacter"),
    (b"\xf0\x90\x80\x80", "U+10000 first 4-byte"), (b"\xf0\x9f\xbf\xbe", "U+1FFFE noncharacter"),
    (b"\xf4\x8f\xbf\xbe", "U+10FFFE noncharacter"), (b"\xf4\x8f\xbf\xbf", "U+10FFFF last code point"),
    (b"\xef\xbb\xbf", "U+FEFF (BOM) inside a string"), (b"\xe2\x80\xa8\xe2\x80\xa9", "U+2028 U+2029"),
    (b"\x7f", "DEL is not a control character in JSON"), (b"\xf0\x9f\x98\x80", "emoji"),
    ("日本語テキスト".encode(), "CJK"), ("ﬃ ǅ Ω".encode(), "compatibility characters"),
]

INVALID_UTF8 = [
    (b"\x80", "lone continuation 0x80"), (b"\xbf", "lone continuation 0xBF"), (b"\x80\x80", "two continuations"),
    (b"\xc0\x80", "overlong NUL (C0 80)"), (b"\xc0\xaf", "overlong '/'"), (b"\xc1\xbf", "overlong 2-byte (C1)"),
    (b"\xe0\x80\x80", "overlong 3-byte NUL"), (b"\xe0\x9f\xbf", "overlong 3-byte U+07FF"),
    (b"\xf0\x80\x80\x80", "overlong 4-byte NUL"), (b"\xf0\x8f\xbf\xbf", "overlong 4-byte U+FFFF"),
    (b"\xed\xa0\x80", "encoded high surrogate U+D800"), (b"\xed\xaf\xbf", "encoded U+DBFF"),
    (b"\xed\xb0\x80", "encoded low surrogate U+DC00"), (b"\xed\xbf\xbf", "encoded U+DFFF"),
    (b"\xed\xa0\xbd\xed\xb2\xa9", "CESU-8 surrogate pair"), (b"\xf4\x90\x80\x80", "U+110000 above the maximum"),
    (b"\xf5\x80\x80\x80", "lead F5"), (b"\xf7\xbf\xbf\xbf", "lead F7"), (b"\xf8\x88\x80\x80\x80", "5-byte form"),
    (b"\xfc\x84\x80\x80\x80\x80", "6-byte form"), (b"\xfe", "byte FE"), (b"\xff", "byte FF"),
    (b"\xfe\xff", "UTF-16 BE BOM bytes"), (b"\xc2", "truncated 2-byte"), (b"\xe0\xa0", "truncated 3-byte"),
    (b"\xe2\x82", "truncated 3-byte (euro)"), (b"\xf0\x90\x80", "truncated 4-byte"), (b"\xf0\x90", "truncated 4-byte"),
    (b"\xf0", "lone 4-byte lead"), (b"\xc2a", "2-byte lead then ASCII"), (b"\xe0\xa0a", "3-byte cut by ASCII"),
    (b"\xf0\x90\x80a", "4-byte cut by ASCII"), (b"\xe0AA", "3-byte lead then ASCII"), (b"\xc2\xc2\x80", "lead then lead"),
    (b"\xe9", "Latin-1 e acute"), (b"caf\xe9!", "Latin-1 inside text"), (b"\xc3\x28", "invalid continuation"),
    (b"\xa0\xa1", "invalid continuation pair"), (b"\xe2\x28\xa1", "invalid 3-byte (2nd)"),
    (b"\xe2\x82\x28", "invalid 3-byte (3rd)"), (b"\xf0\x28\x8c\xbc", "invalid 4-byte (2nd)"),
    (b"\xf0\x90\x28\xbc", "invalid 4-byte (3rd)"), (b"\xf0\x28\x8c\x28", "invalid 4-byte (2nd, 4th)"),
]


def gen_strings():
    m = Manifest("strings")
    m.add("accept", jstr(""), "empty string")
    for e, ch in [('\\"', '"'), ("\\\\", "\\"), ("\\/", "/"), ("\\b", "\b"), ("\\f", "\f"),
                  ("\\n", "\n"), ("\\r", "\r"), ("\\t", "\t")]:
        m.add("accept", jstr(e), "escape " + e, out=iron_quote(ch))
        m.add("accept", jstr("a" + e + "b"), "escape " + e + " between letters", out=iron_quote("a" + ch + "b"))
    for cp in list(range(0, 0x80)) + [0x80, 0xff, 0x100, 0x7ff, 0x800, 0xfff, 0x1000, 0xd7ff, 0xe000,
                                       0xfdd0, 0xfdef, 0xfeff, 0xfffd, 0xfffe, 0xffff]:
        for fmt in ("\\u%04x", "\\u%04X"):
            e = fmt % cp
            m.add("accept", jstr(e), "escape " + e, out=iron_quote(chr(cp)))
    m.add("accept", jstr("\\u00E9\\u00e9"), "upper and lower case hex", out=iron_quote("éé"))
    m.add("accept", jstr("\\uDbFf\\uDfFf"), "mixed case surrogate pair", out=iron_quote("\U0010ffff"))
    m.add("accept", jstr("\\u0000"), "escaped NUL survives", out=iron_quote("\0"))
    m.add("accept", jstr("a\\u0000b\\u0000"), "embedded NULs", out=iron_quote("a\0b\0"))
    for hi, lo, cp in [(0xD800, 0xDC00, 0x10000), (0xDBFF, 0xDFFF, 0x10FFFF), (0xD834, 0xDD1E, 0x1D11E),
                       (0xD83D, 0xDE00, 0x1F600), (0xDBFF, 0xDC00, 0x10FC00), (0xD800, 0xDFFF, 0x103FF)]:
        m.add("accept", jstr("\\u%04x\\u%04x" % (hi, lo)), "surrogate pair U+%X" % cp, out=iron_quote(chr(cp)))
    for body, desc in [
        ("\\ud800", "lone high surrogate at the end"), ("\\ud800a", "high surrogate then a letter"),
        ("\\ud800\\u0041", "high surrogate then a non-surrogate escape"), ("\\ud800\\ud800", "two high surrogates"),
        ("\\udc00", "lone low surrogate"), ("\\udfff", "lone low surrogate U+DFFF"),
        ("\\udc00\\ud800", "inverted pair"), ("\\ud800\\n", "high surrogate then \\n"),
        ("\\ud800\\u", "high surrogate then a cut escape"), ("\\ud800\\udc0", "low half too short"),
        ("\\ud800\\uzzzz", "low half not hex"), ("a\\udc00b", "low surrogate between letters"),
        ("\\ud800\\\\udc00", "high surrogate, escaped backslash, text"),
    ]:
        m.add("reject:4004", jstr(body), desc)
    for body, desc in [
        ("\\u12", "short \\u escape"), ("\\u12G4", "non-hex digit"), ("\\u", "bare \\u"), ("\\U0041", "capital U"),
        ("\\x41", "\\x escape"), ("\\'", "escaped single quote"), ("\\a", "\\a"), ("\\0", "\\0"), ("\\v", "\\v"),
        ("\\ ", "escaped space"), ("\\e", "\\e"), ("\\N", "capital N"), ("\\B", "capital B"), ("\\é", "escaped e acute"),
        ("\\\n", "escaped newline"), ("\\u-123", "minus in \\u"), ("\\u 123", "space in \\u"), ("\\u+123", "plus in \\u"),
    ]:
        m.add("reject:4004", jstr(body), desc)
    for c in range(0x20):
        m.add("reject:4004", jstr(bytes([c])), "raw control character 0x%02X" % c)
        m.add("reject:4004", jstr(b"abcdefghij" + bytes([c]) + b"klm"), "raw control 0x%02X after 10 bytes" % c)
    for body, desc in [(b'"abc', "unterminated"), (b'"abc\\"', "unterminated after an escaped quote"), (b'"', "lone quote"),
                       (b'"\\', "backslash at the end"), (b'"\\u00', "cut \\u escape at the end"), (b"'abc'", "single quotes"),
                       (b'["abc', "unterminated in an array"), (b'{"abc', "unterminated key"), (b'{"a":"b', "unterminated member")]:
        m.add("reject", body, desc)
    for raw, desc in VALID_UTF8:
        text = raw.decode("utf-8")
        m.add("accept", jstr(raw), "valid UTF-8: " + desc, out=iron_quote(text))
        m.add("accept", b'{"' + raw + b'":1}', "valid UTF-8 in a key: " + desc, out="{" + iron_quote(text) + ":1}")
        m.add("accept", jstr(b"0123456789" + raw + b"abc"), "valid UTF-8 after 10 bytes: " + desc,
              out=iron_quote("0123456789" + text + "abc"))
    for raw, desc in INVALID_UTF8:
        m.add("reject:4003", jstr(raw), "invalid UTF-8: " + desc)
        m.add("reject:4003", b'{"' + raw + b'":1}', "invalid UTF-8 in a key: " + desc)
        m.add("reject:4003", jstr(b"0123456789" + raw + b"abc"), "invalid UTF-8 after 10 bytes: " + desc)
        m.add("reject:4003", jstr(b"x\\n" + raw), "invalid UTF-8 after an escape: " + desc)
        m.add("reject", b"[" + raw + b"]", "invalid UTF-8 outside a string: " + desc)
    big = "".join(chr(c) for c in range(0x20, 0x3000) if not 0xD800 <= c <= 0xDFFF)
    m.add("accept", jstr(big.replace("\\", "\\\\").replace('"', '\\"')), "every code point U+0020..U+2FFF",
          out=iron_quote(big))
    return m


def gen_utf8_sweep():
    m = Manifest("utf8_sweep")

    def case(body, desc):
        try:
            text = body.decode("utf-8")
            valid = True
        except UnicodeDecodeError:
            valid = False
        if any(b < 0x20 for b in body):
            m.add("reject:4004" if valid else "reject", jstr(body), desc)
        elif not valid:
            m.add("reject:4003", jstr(body), desc)
        else:
            m.add("accept", jstr(body), desc, out=iron_quote(text))

    for a in range(256):
        if a in (0x22, 0x5C):
            continue
        case(bytes([a]), "byte %02X" % a)
        for b in range(256):
            if b in (0x22, 0x5C) or (a < 0x80 and b < 0x80 and a >= 0x20 and b >= 0x20 and (a + b) % 7):
                continue  # plain ASCII pairs: a seventh of them is plenty
            case(bytes([a, b]), "bytes %02X %02X" % (a, b))
    edges = [0x00, 0x41, 0x7F, 0x80, 0x8F, 0x90, 0x9F, 0xA0, 0xBF, 0xC0, 0xC1, 0xC2, 0xDF, 0xE0, 0xED, 0xEF, 0xF0,
             0xF4, 0xF5, 0xFF]
    for a in range(0xE0, 0x100):
        for b in range(0x70, 0xD0):
            for c in edges:
                if c in (0x22, 0x5C):
                    continue
                case(bytes([a, b, c]), "bytes %02X %02X %02X" % (a, b, c))
    for a in range(0xF0, 0x100):
        for b in range(0x70, 0xD0, 3):
            for c in (0x7F, 0x80, 0xBF, 0xC0):
                for d in (0x41, 0x80, 0xBF, 0xC0):
                    case(bytes([a, b, c, d]), "bytes %02X %02X %02X %02X" % (a, b, c, d))
    return m


def gen_string_offsets():
    m = Manifest("string_offsets")
    specials = [(b'\\"', "escaped quote"), (b"\\n", "escape"), (b"\\u00e9", "unicode escape"),
                (b"\xc3\xa9", "2-byte UTF-8"), (b"\xe2\x82\xac", "3-byte UTF-8"), (b"\xf0\x9f\x98\x80", "4-byte UTF-8"),
                (b"\x01", "control"), (b"\x80", "bad UTF-8"), (b"\xe2\x82", "cut UTF-8"), (b"\x7f", "DEL")]
    for n in range(0, 34):
        for k in range(0, n + 1):
            for sp, desc in specials:
                body = b"abcdefghijklmnopqrstuvwxyz012345678"[:k] + sp + b"ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"[: n - k]
                if sp == b"\x01":
                    m.add("reject:4004", jstr(body), "%s at offset %d of %d" % (desc, k, n))
                elif sp in (b"\x80", b"\xe2\x82"):
                    m.add("reject:4003", jstr(body), "%s at offset %d of %d" % (desc, k, n))
                else:
                    m.add("accept", jstr(body), "%s at offset %d of %d" % (desc, k, n))
        # The document ends right after the string body: unterminated.
        m.add("reject:4000", b'"' + b"x" * n, "unterminated string of %d bytes" % n)
    return m

# ── structure ──────────────────────────────────────────────────────────────


def gen_structure():
    m = Manifest("structure")
    for doc, desc in [
        ("null", "top-level null"), ("true", "top-level true"), ("false", "top-level false"), ("0", "top-level 0"),
        ('"x"', "top-level string"), ("{}", "empty object"), ("[]", "empty array"), (" \t\r\n1 \t\r\n", "whitespace around"),
        ("[ ]", "array with a space"), ("{ }", "object with a space"), ("[[],[[]],{},[{}]]", "nested empties"),
        ('{"a":{"b":{"c":[1,{"d":null}]}}}', "nested object"), ('[1,"a",true,false,null,1.5,{},[]]', "mixed array"),
        (' [ 1 , 2 ] ', "spaces between tokens"), ('{\n\t"a" :\r\n 1 ,\t"b":2}', "every whitespace character"),
        ('{"":""}', "empty key and value"), ('{"a":[],"b":{}}', "empty containers as members"),
        ('{"\\u0000":1}', "NUL in a key"), ('{"a\\"b":1}', "escaped quote in a key"),
        ("[" + ",".join(str(i) for i in range(1000)) + "]", "1000 integers"),
        ('{"a":1,"a":2}', "duplicate key: both members kept, in order"), ('{"a":1,"b":2,"a":3}', "duplicate key apart"),
        ('{"a":1,"a":"x","a":[1],"a":{}}', "duplicate keys of different types"), ('{"":1,"":2}', "duplicate empty keys"),
        ('{"a":{"x":1,"x":2},"a":{"y":3}}', "nested duplicate keys"), ('[{"k":1,"k":1}]', "duplicate identical members"),
    ]:
        m.add("accept", doc, desc)
    for doc, desc in [
        ("", "empty input"), (" ", "only a space"), ("\n\t\r ", "only whitespace"), ("nul", "cut null"), ("nulll", "null plus l"),
        ("True", "capital True"), ("NULL", "capital NULL"), ("tru e", "split true"), ("[true false]", "missing comma"),
        ("{} x", "garbage after the value"), ("[] []", "two values"), ("1 2", "two numbers"), ('"a""b"', "two strings"),
        ("{}\x00", "NUL after the value"), ("[1,]", "trailing comma"), ("[,]", "comma only"), ("[1,,2]", "double comma"),
        ("[,1]", "leading comma"), ('{"a":1,}', "trailing comma in an object"), ('{,"a":1}', "leading comma in an object"),
        ('{"a" 1}', "missing colon"), ('{"a"::1}', "double colon"), ('{"a":}', "missing value"), ('{:1}', "missing key"),
        ("{1:1}", "number key"), ("{a:1}", "bare key"), ("{'a':1}", "single quoted key"), ('{"a":1 "b":2}', "missing comma"),
        ('["a":1]', "colon in an array"), ("[1}", "mismatched close"), ('{"a":1]', "mismatched close"), ("]", "lone close"),
        ("[", "lone open"), ("{", "lone brace"), ("[[]", "unclosed"), ("[]]", "extra close"), ("{}}", "extra brace"),
        ("[1] // c", "comment"), ("/* c */ 1", "block comment"), ("# c\n1", "hash comment"), ("[\f1]", "form feed"),
        ("[\v1]", "vertical tab"), ("[\x001]", "NUL whitespace"), ("[ 1]", "NBSP whitespace"),
        ("[ 1]", "U+2028 whitespace"), ("[﻿1]", "BOM between tokens"), ("1\x1a", "trailing ctrl-Z"),
        ("﻿{}", "UTF-8 BOM before the value (RFC 8259 8.1: not allowed; rejected)"), ("﻿", "BOM only"),
        ("[undefined]", "undefined"), ("[None]", "None"), ("(1)", "parentheses"), ("<1>", "angle brackets"),
        ('{"a":1}{"b":2}', "two objects (NDJSON is not one document)"), ("[1]\n[2]", "two lines"),
    ]:
        m.add("reject", doc, desc)
    m.add("reject", b"\xff\xfe[\x001\x00]\x00", "UTF-16 LE with BOM")
    m.add("reject", b"\x00[\x001\x00]", "UTF-16 BE without BOM")
    m.add("reject", b"\x00\x00\x00[", "UTF-32")
    # Nesting against the default limit (512) and custom limits.
    for n in (1, 2, 100, 511, 512):
        # Compact already: the expected output is the input.
        d = "[" * n + "]" * n
        m.add("accept", d, "%d nested arrays (default limit 512)" % n, out=d)
        d = '{"a":' * n + "1" + "}" * n
        m.add("accept", d, "%d nested objects" % n, out=d)
    m.add("reject:4001", "[" * 513 + "]" * 513, "513 nested arrays: over the default limit")
    m.add("reject:4001", '{"a":' * 513 + "1" + "}" * 513, "513 nested objects")
    m.add("reject:4001", "[" * 100000, "100000 open brackets: depth error, not a crash")
    m.add("reject:4001", '[{"a":' * 300 + "1" + "}]" * 300, "600 mixed levels")
    d = "[" * 300 + "]" * 300
    m.add("accept", d, "300 levels", out=d, opts="depth=300,bytes=-1")
    m.add("reject:4001", "[" * 301 + "]" * 301, "301 levels over limit 300", opts="depth=300,bytes=-1")
    m.add("accept", "[]", "depth limit 1 allows one container", opts="depth=1,bytes=-1")
    m.add("reject:4001", "[[]]", "depth limit 1 rejects two", opts="depth=1,bytes=-1")
    m.add("accept", "1", "depth limit 1 allows scalars", opts="depth=1,bytes=-1")
    for n in (1000, 4095):
        d = "[" * n + "]" * n
        m.add("accept", d, "%d levels with a raised limit" % n, out=d, opts="depth=%d,bytes=-1" % n)
        d = '{"a":' * n + "[]" + "}" * n
        m.add("accept", d, "%d object levels with a raised limit" % n, out=d, opts="depth=%d,bytes=-1" % (n + 1))
    # Limits above 4096 are clamped: deeper documents are always rejected.
    d = "[" * 4096 + "]" * 4096
    m.add("accept", d, "4096 levels, the maximum", out=d, opts="depth=1000000,bytes=-1")
    m.add("reject:4001", "[" * 4097 + "]" * 4097, "4097 levels: over the maximum whatever the limit",
          opts="depth=1000000,bytes=-1")
    m.add("accept", "[1,2]", "exactly the size limit", opts="depth=512,bytes=5")
    m.add("reject:4002", "[1,2]", "one byte over the size limit", opts="depth=512,bytes=4")
    m.add("reject:4000", "", "empty input with a zero size limit", opts="depth=512,bytes=0")
    m.add("accept", "1", "unlimited size", opts="depth=512,bytes=-1")
    return m

# ── vendored suites ────────────────────────────────────────────────────────


def gen_jsonchecker():
    m = Manifest("jsonchecker")
    d = os.path.join(HERE, "nativejson", "jsonchecker")
    # Accepted although JSON_checker rejects them: RFC 8259 allows any value
    # at the top level (fail01) and Iron's default depth limit is 512, not 19
    # (fail18).
    accepted_fails = {"fail01_EXCLUDE.json", "fail18_EXCLUDE.json"}
    for name in sorted(os.listdir(d)):
        if not name.endswith(".json"):
            continue
        data = open(os.path.join(d, name), "rb").read()
        if name.startswith("pass") or name in accepted_fails:
            m.add("accept", data, name)
        else:
            m.add("reject", data, name)
    return m


def gen_roundtrip():
    m = Manifest("roundtrip")
    d = os.path.join(HERE, "nativejson", "roundtrip")
    for name in sorted(os.listdir(d)):
        data = open(os.path.join(d, name), "rb").read().strip()
        m.add("accept", data, name)
    return m


def main():
    args = sys.argv[1:]
    if not args:
        print(__doc__)
        sys.exit(2)
    out_dir = args[0]
    n_random = 60000
    seed = 20261011
    if "--random" in args:
        n_random = int(args[args.index("--random") + 1])
    if "--seed" in args:
        seed = int(args[args.index("--seed") + 1])
    rng = random.Random(seed)
    os.makedirs(out_dir, exist_ok=True)
    for m in (gen_numbers(), gen_numbers_random(n_random, rng), gen_strings(), gen_utf8_sweep(),
              gen_string_offsets(), gen_structure(), gen_jsonchecker(), gen_roundtrip()):
        print("%-16s %6d cases" % (m.name, m.write(out_dir)))


if __name__ == "__main__":
    main()
