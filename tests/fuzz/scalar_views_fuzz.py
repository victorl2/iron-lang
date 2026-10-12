#!/usr/bin/env python3
"""Random scalar intrinsics, borrowed views and StringBuilder vs Python models."""

import argparse
import pathlib
import random
import shutil
import subprocess
import tempfile


TYPES = [
    ("Int", 64, True), ("Int8", 8, True), ("Int16", 16, True),
    ("Int32", 32, True), ("Int64", 64, True),
    ("UInt", 64, False), ("UInt8", 8, False), ("UInt16", 16, False),
    ("UInt32", 32, False), ("UInt64", 64, False),
]


def signed(bits, width):
    bits &= (1 << width) - 1
    return bits - (1 << width) if bits & (1 << (width - 1)) else bits


def value(bits, width, is_signed):
    return signed(bits, width) if is_signed else bits & ((1 << width) - 1)


def lit(type_name, n, width, is_signed):
    if is_signed:
        n = value(n, width, True)
        return "(%d)" % n if type_name == "Int" else "%s(%d)" % (type_name, n)
    n &= (1 << width) - 1
    if width == 64 and n > (1 << 63) - 1:
        return "(%s(0) - %s(%d))" % (type_name, type_name, (1 << 64) - n)
    return "%s(%d)" % (type_name, n)


def prefix_xor(bits, width):
    mask = (1 << width) - 1
    shift = 1
    while shift < width:
        bits ^= bits << shift
        bits &= mask
        shift *= 2
    return bits


def byte_swap(bits, width):
    count = width // 8
    return int.from_bytes((bits & ((1 << width) - 1)).to_bytes(count, "little"), "big")


def generate(seed):
    rng = random.Random(seed)
    lines = [
        "-- Generated deterministically by tests/fuzz/scalar_views_fuzz.py.",
        "func main() {",
    ]
    checks = 0

    def check(expr, want, label):
        nonlocal checks
        lines.append('    assert(%s == %s, "%s")' % (expr, want, label))
        checks += 1

    for type_name, width, is_signed in TYPES:
        mask = (1 << width) - 1
        lo = -(1 << (width - 1)) if is_signed else 0
        hi = (1 << (width - 1)) - 1 if is_signed else mask
        unsigned_name = "UInt" if type_name == "Int" else (
            "UInt%d" % width if is_signed else type_name)
        for sample in range(3):
            bits = rng.getrandbits(width)
            n = value(bits, width, is_signed)
            x = lit(type_name, n, width, is_signed)
            tag = "%s_%d" % (type_name.lower(), sample)
            check("%s.leading_zeros()" % x,
                  str(width if bits == 0 else width - bits.bit_length()), tag + "_lz")
            trailing = width if bits == 0 else (bits & -bits).bit_length() - 1
            check("%s.trailing_zeros()" % x, str(trailing), tag + "_tz")
            check("%s.count_ones()" % x, str(bits.bit_count()), tag + "_ones")
            check("%s.clear_lowest_bit()" % x,
                  lit(type_name, bits & (bits - 1), width, is_signed), tag + "_clear")
            check("%s.lowest_bit()" % x,
                  lit(type_name, bits & -bits, width, is_signed), tag + "_low")
            amount = rng.randint(-3 * width, 3 * width)
            k = amount % width
            left = ((bits << k) | (bits >> ((width - k) % width))) & mask
            right = ((bits >> k) | (bits << ((width - k) % width))) & mask
            check("%s.rotate_left(%d)" % (x, amount),
                  lit(type_name, left, width, is_signed), tag + "_rol")
            check("%s.rotate_right(%d)" % (x, amount),
                  lit(type_name, right, width, is_signed), tag + "_ror")
            check("%s.byte_swap()" % x,
                  lit(type_name, byte_swap(bits, width), width, is_signed), tag + "_swap")
            check("%s.prefix_xor()" % x,
                  lit(type_name, prefix_xor(bits, width), width, is_signed), tag + "_prefix")

        for sample in range(3):
            a = rng.randint(lo, hi)
            b = rng.randint(lo, hi)
            for method, exact in (("add_overflow", a + b),
                                  ("sub_overflow", a - b),
                                  ("mul_overflow", a * b)):
                wrapped = value(exact, width, is_signed)
                overflow = exact < lo or exact > hi
                expr = "%s.%s(%s)" % (lit(type_name, a, width, is_signed), method,
                                        lit(type_name, b, width, is_signed))
                lines.append("    val (%s_v, %s_o) = %s" %
                             (("r%d" % checks), ("r%d" % checks), expr))
                var = "r%d" % checks
                check(var + "_v", lit(type_name, wrapped, width, is_signed),
                      "%s_%d_%s_value" % (type_name.lower(), sample, method))
                check(var + "_o", "true" if overflow else "false",
                      "%s_%d_%s_flag" % (type_name.lower(), sample, method))

            product = a * b
            full = product & ((1 << (2 * width)) - 1)
            high = (full >> width) & mask
            low = full & mask
            base = "w%d" % checks
            lines.append("    val (%s_hi, %s_lo) = %s.mul_wide(%s)" %
                         (base, base, lit(type_name, a, width, is_signed),
                          lit(type_name, b, width, is_signed)))
            check(base + "_hi", lit(type_name, high, width, is_signed),
                  "%s_%d_wide_high" % (type_name.lower(), sample))
            check(base + "_lo", lit(unsigned_name, low, width, False),
                  "%s_%d_wide_low" % (type_name.lower(), sample))
            check("%s.mul_high(%s)" % (lit(type_name, a, width, is_signed),
                                        lit(type_name, b, width, is_signed)),
                  lit(type_name, high, width, is_signed),
                  "%s_%d_mul_high" % (type_name.lower(), sample))

    alphabet = "abcdefghijklmnopqrstuvwxyz0123456789"
    for sample in range(20):
        text = "".join(rng.choice(alphabet) for _ in range(rng.randint(3, 24)))
        start = rng.randrange(len(text) + 1)
        end = rng.randrange(start, len(text) + 1)
        lines.append('    val bytes_%d = "%s".bytes()' % (sample, text))
        check("bytes_%d.len()" % sample, str(len(text)), "bytes_%d_len" % sample)
        check("bytes_%d.slice(%d, %d).to_string()" % (sample, start, end),
              '"%s"' % text[start:end], "bytes_%d_slice" % sample)
        if text:
            index = rng.randrange(len(text))
            check("bytes_%d[%d]" % (sample, index), "UInt8(%d)" % ord(text[index]),
                  "bytes_%d_index" % sample)

    chunks = ["".join(rng.choice(alphabet) for _ in range(rng.randint(1, 8)))
              for _ in range(16)]
    lines.append("    var builder = StringBuilder.new()")
    for chunk in chunks:
        lines.append('    builder.push_str("%s")' % chunk)
    number = rng.randint(-1000000, 1000000)
    lines.append("    builder.push_int(%d)" % number)
    lines.append("    val room = builder.spare(3)")
    for i, byte in enumerate((33, 63, 46)):
        lines.append("    room[%d] = UInt8(%d)" % (i, byte))
    lines.append("    builder.commit(3)")
    expected = "".join(chunks) + str(number) + "!?."
    check("builder.finish()", '"%s"' % expected, "builder")

    values = [rng.randrange(256) for _ in range(24)]
    lines.append("    val values: [UInt8] = [%s]" %
                 ", ".join("UInt8(%d)" % x for x in values))
    start, end = 4, 19
    lines.append("    val middle = values.view().slice(%d, %d)" % (start, end))
    for i, want in enumerate(values[start:end]):
        check("middle[%d]" % i, "UInt8(%d)" % want, "view_%d" % i)
    lines.append('    println("random scalar views: %d checks")' % checks)
    lines.append("}")
    return "\n".join(lines) + "\n", checks


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("ironc")
    parser.add_argument("workdir")
    parser.add_argument("--seed", type=int, default=658)
    parser.add_argument("--valgrind", action="store_true")
    args = parser.parse_args()
    source, checks = generate(args.seed)
    root = pathlib.Path(args.workdir)
    root.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="scalar_views_", dir=root) as tmp:
        tmp = pathlib.Path(tmp)
        src = tmp / "randomized.iron"
        exe = tmp / "randomized"
        src.write_text(source, encoding="utf-8")
        subprocess.run([args.ironc, "build", str(src), "--release", "-o", str(exe)],
                       check=True)
        command = [str(exe)]
        if args.valgrind:
            valgrind = shutil.which("valgrind")
            if not valgrind:
                raise SystemExit("valgrind not found")
            command = [valgrind, "--leak-check=full", "--error-exitcode=99", str(exe)]
        result = subprocess.run(command, check=True, text=True, capture_output=True)
        want = "random scalar views: %d checks" % checks
        if result.stdout.strip() != want:
            raise SystemExit("unexpected output: %r, wanted %r" % (result.stdout, want))
        print("%s: PASS" % want)


if __name__ == "__main__":
    main()
