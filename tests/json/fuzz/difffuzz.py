#!/usr/bin/env python3
"""difffuzz.py - differential fuzzing of JSON parsers.

usage: difffuzz.py --impl NAME=CMD [--impl NAME=CMD ...] [--count N] [--seed S]
                   [--batch FILE] [--out DIR] [--max-report K]

Every implementation is a command that takes a batch file (one document per
line, hex encoded) as its last argument and prints one line per document:
"ERR <anything>" or "OK <canonical form>" (the format jcanon.iron describes).
The first --impl is the subject; each of the others is compared with it.
Documents come from gen.py (--count, --seed) or from an existing --batch.

Implementations shipped next to this script:
    jcanon.iron        the Iron json module           (build with ironc)
    ref_python.py      Python's json held to RFC 8259  (python3 ref_python.py)
    ref_yyjson.c       yyjson, strict mode             (src/vendor/yyjson)
    ref_simdjson.cpp   simdjson DOM                    (fetched by run_difffuzz.sh)

To compare a new Iron json implementation with the current one and the
references, build jcanon.iron against each and pass both, e.g.
    --impl new=./jcanon_new --impl old=./jcanon_old --impl py="python3 ref_python.py"

Comparison rules ("careful" numbers, Iron's documented policy):
  * accept vs reject differs: mismatch. Error codes are not compared.
  * a reference result that Iron's policy rejects counts as a reject: nesting
    deeper than 512, a number that is infinite or NaN, a document that
    starts with a UTF-8 byte order mark (simdjson skips it);
  * values compare structurally, members in document order (duplicates kept);
  * numbers: two integers must be equal; two doubles must be bit-identical
    (so -0.0 differs from 0.0); an integer against a double compares the
    nearest double of the integer with the double (== , so the integer 0
    matches -0.0: a reference may read "-0" as a double);
  * results a reference cannot express are skipped, not counted: simdjson's
    BIGINT element and its BIGINT_ERROR (integers beyond 64 bits).

Exit status 0 when no mismatch was found. Mismatching documents are saved to
--out (default difffuzz-out/) as <n>.json with a <n>.txt explanation.
"""
import argparse
import math
import os
import shlex
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
MAX_DEPTH = 512


class Skip(Exception):
    pass


def tokenize(s):
    i, n = 0, len(s)
    while i < n:
        c = s[i]
        if c in "[]{},:":
            yield c
            i += 1
            continue
        j = i + 2 if s.startswith(("i:", "u:", "f:", "s:"), i) else i
        while j < n and s[j] not in "[]{},:":
            j += 1
        yield s[i:j]
        i = j


def parse_canon(s):
    toks = list(tokenize(s))
    pos = 0

    def value():
        nonlocal pos
        t = toks[pos]
        pos += 1
        if t == "[":
            items = []
            if toks[pos] == "]":
                pos += 1
                return ("arr", items)
            while True:
                items.append(value())
                t = toks[pos]
                pos += 1
                if t == "]":
                    return ("arr", items)
        if t == "{":
            members = []
            if toks[pos] == "}":
                pos += 1
                return ("obj", members)
            while True:
                k = toks[pos]
                pos += 2            # key, ':'
                members.append((k, value()))
                t = toks[pos]
                pos += 1
                if t == "}":
                    return ("obj", members)
        if t in ("null", "true", "false"):
            return (t,)
        if t == "bigint":
            raise Skip("bigint")
        if t.startswith(("i:", "u:")):
            return ("int", int(t[2:]))
        if t.startswith("f:"):
            return ("float", float(t[2:]))
        if t.startswith("s:"):
            return ("str", t[2:])
        raise ValueError("bad canonical token %r" % t)

    v = value()
    if pos != len(toks):
        raise ValueError("trailing canonical tokens")
    return v


def depth(v):
    best, stack = 0, [(v, 1)]
    while stack:
        x, d = stack.pop()
        if x[0] == "arr":
            best = max(best, d)
            stack.extend((y, d + 1) for y in x[1])
        elif x[0] == "obj":
            best = max(best, d)
            stack.extend((y, d + 1) for _, y in x[1])
    return best


def bad_numbers(v):
    stack = [v]
    while stack:
        x = stack.pop()
        if x[0] == "float" and not math.isfinite(x[1]):
            return True
        if x[0] == "arr":
            stack.extend(x[1])
        elif x[0] == "obj":
            stack.extend(y for _, y in x[1])
    return False


def bits(f):
    return struct.unpack("<Q", struct.pack("<d", f))[0]


def num_equal(a, b):
    if a[0] == "int" and b[0] == "int":
        return a[1] == b[1]
    if a[0] == "float" and b[0] == "float":
        return bits(a[1]) == bits(b[1])
    i, f = (a, b) if a[0] == "int" else (b, a)
    try:
        return float(i[1]) == f[1]
    except OverflowError:
        return False


def diff(a, b, path="$"):
    """None when equal, otherwise where and how they differ."""
    stack = [(a, b, path)]
    while stack:
        x, y, p = stack.pop()
        if x[0] in ("int", "float") and y[0] in ("int", "float"):
            if not num_equal(x, y):
                return "%s: number %r vs %r" % (p, x[1], y[1])
            continue
        if x[0] != y[0]:
            return "%s: %s vs %s" % (p, x[0], y[0])
        if x[0] == "str" and x[1] != y[1]:
            return "%s: string %r vs %r" % (p, bytes.fromhex(x[1]), bytes.fromhex(y[1]))
        if x[0] == "arr":
            if len(x[1]) != len(y[1]):
                return "%s: %d vs %d elements" % (p, len(x[1]), len(y[1]))
            stack.extend((x[1][i], y[1][i], "%s[%d]" % (p, i)) for i in range(len(x[1])))
        if x[0] == "obj":
            if len(x[1]) != len(y[1]):
                return "%s: %d vs %d members" % (p, len(x[1]), len(y[1]))
            for i, ((kx, vx), (ky, vy)) in enumerate(zip(x[1], y[1])):
                if kx != ky:
                    return "%s: key %r vs %r" % (p, bytes.fromhex(kx[2:]), bytes.fromhex(ky[2:]))
                stack.append((vx, vy, "%s.%s" % (p, bytes.fromhex(kx[2:]).decode("utf-8", "replace"))))
    return None


def interpret(line, policy, doc=""):
    """('reject', why) | ('accept', tree) | ('skip', why)."""
    if policy and doc.startswith("efbbbf") and line.startswith("OK"):
        return ("reject", "leading byte order mark (Iron policy)")
    if line.startswith("ERR"):
        if "BIGINT" in line.upper() or "big integer" in line.lower():
            return ("skip", "reference cannot hold a big integer")
        return ("reject", line[4:])
    if not line.startswith("OK "):
        return ("reject", "unexpected output %r" % line[:60])
    try:
        tree = parse_canon(line[3:])
    except Skip as e:
        return ("skip", str(e))
    if policy:
        if depth(tree) > MAX_DEPTH:
            return ("reject", "deeper than %d (Iron policy)" % MAX_DEPTH)
        if bad_numbers(tree):
            return ("reject", "infinite number (Iron policy)")
    return ("accept", tree)


def run_impl(cmd, batch):
    out = subprocess.run(shlex.split(cmd) + [batch], capture_output=True, text=True, errors="replace")
    lines = out.stdout.split("\n")
    if lines and lines[-1] == "":
        lines.pop()
    return lines, out.returncode, out.stderr


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--impl", action="append", required=True)
    ap.add_argument("--count", type=int, default=2000)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--batch")
    ap.add_argument("--out", default="difffuzz-out")
    ap.add_argument("--max-report", type=int, default=20)
    a = ap.parse_args()
    impls = [s.split("=", 1) for s in a.impl]
    tmp = tempfile.mkdtemp(prefix="difffuzz_")
    batch = a.batch
    if not batch:
        batch = os.path.join(tmp, "batch.hex")
        with open(batch, "w") as f:
            subprocess.run([sys.executable, os.path.join(HERE, "gen.py"), str(a.count), str(a.seed)], stdout=f, check=True)
    docs = [l for l in open(batch).read().split("\n") if l]
    results = {}
    for name, cmd in impls:
        lines, rc, err = run_impl(cmd, batch)
        if len(lines) != len(docs):
            print("%s: %d results for %d documents (exit %d)\n%s" % (name, len(lines), len(docs), rc, err[-2000:]))
            sys.exit(2)
        results[name] = lines
    subject = impls[0][0]
    total_bad = 0
    reported = 0
    os.makedirs(a.out, exist_ok=True)
    for name, _ in impls[1:]:
        agree = bad = skipped = acc = 0
        for i, doc in enumerate(docs):
            s = interpret(results[subject][i], policy=False)
            r = interpret(results[name][i], policy=True, doc=doc)
            if r[0] == "skip" or s[0] == "skip":
                skipped += 1
                continue
            why = None
            if s[0] != r[0]:
                why = "%s %ss (%s), %s %ss (%s)" % (subject, s[0], s[1] if s[0] == "reject" else "",
                                                   name, r[0], r[1] if r[0] == "reject" else "")
            elif s[0] == "accept":
                d = diff(s[1], r[1])
                if d:
                    why = "values differ at " + d
            if why is None:
                agree += 1
                acc += s[0] == "accept"
                continue
            bad += 1
            if reported < a.max_report:
                data = b"" if doc == "-" else bytes.fromhex(doc)
                with open(os.path.join(a.out, "%d.json" % reported), "wb") as f:
                    f.write(data)
                with open(os.path.join(a.out, "%d.txt" % reported), "w") as f:
                    f.write("%s vs %s: %s\n%s: %s\n%s: %s\n" % (subject, name, why, subject, results[subject][i][:2000],
                                                            name, results[name][i][:2000]))
                print("MISMATCH %s vs %s doc %d (%s/%d.json): %s" % (subject, name, i, a.out, reported, why[:300]))
                reported += 1
        total_bad += bad
        print("%s vs %s: %d documents, %d agree (%d accepted by both), %d mismatches, %d skipped" %
              (subject, name, len(docs), agree, acc, bad, skipped))
    sys.exit(1 if total_bad else 0)


if __name__ == "__main__":
    main()
