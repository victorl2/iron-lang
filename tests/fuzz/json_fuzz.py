#!/usr/bin/env python3
"""Differential fuzzer for the json module against Python's json.

Generates random JSON documents (nested values, numbers at the edges of
Int and Float, escapes, non-ASCII text, random whitespace) and random
corruptions of them (flipped, inserted, deleted and duplicated bytes,
truncation, stray tokens, invalid UTF-8, lone surrogates), parses each one
with Iron (tests/fuzz/json_fuzz_driver.iron) and with Python, and compares:

  - accept / reject must agree, after mapping Python's extensions onto the
    strict RFC 8259 rules the module implements (Python accepts NaN,
    Infinity, lone surrogate escapes and numbers that overflow a double;
    Iron rejects them);
  - accepted documents must have the same value: same structure, the same
    strings, Int where Python has an int that fits in 64 bits, a Float
    equal to Python's float otherwise (a larger int compares as the float
    it rounds to), the last of duplicate keys;
  - Iron's own round trips (compact and pretty output parse back to an
    equal value, compact output is a fixed point) must hold.

    json_fuzz.py IRONC OUTDIR SEED [COUNT] [-j N] [--valgrind]
    json_fuzz.py --suite IRONC OUTDIR      (the fixed seeds ctest runs)
    json_fuzz.py --gen SEED                (print one generated case)

Exit status 1 on any mismatch; failing inputs are kept under OUTDIR/fail/.
"""
import json
import math
import os
import random
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
DRIVER = os.path.join(HERE, 'json_fuzz_driver.iron')
INT_MIN, INT_MAX = -(1 << 63), (1 << 63) - 1
MAX_DEPTH = 512          # Json.default_limits()

WORDS = ['', 'a', 'id', 'name', 'value', 'key with space', 'émoji 😀', '中文',
         'quote"', 'back\\slash', 'nl\nx', 'tab\t', 'ctl\x01\x1f', ' ',
         'null', 'true', '0', '/slash', '\U0010ffff', 'ünïcödé']


# ── Generation ───────────────────────────────────────────────────────────

def rand_number_text(rng):
    """A number as text, valid JSON most of the time."""
    choice = rng.random()
    if choice < 0.15:
        return str(rng.choice([0, -1, 1, INT_MAX, INT_MIN, INT_MAX + 1, INT_MIN - 1,
                               2 ** 53, 2 ** 53 + 1, 10 ** 19, -(10 ** 19), 10 ** 30]))
    if choice < 0.35:
        return str(rng.randint(-10 ** rng.randint(1, 25), 10 ** rng.randint(1, 25)))
    if choice < 0.55:
        return repr(rng.uniform(-1e6, 1e6))
    if choice < 0.70:
        v = rng.choice([5e-324, 2.2250738585072014e-308, 1.7976931348623157e308,
                        0.1, 0.30000000000000004, 1e21, 1e-7, 123456789.123456789,
                        -0.0, 1e300, 1e-300, 4.35950752273729e-311])
        return repr(v)
    # Hand-built: sign, digits, fraction, exponent (invalid one time in ten).
    bad = rng.random() < 0.1
    s = rng.choice(['', '', '-'])
    s += rng.choice(['0', '1', '9', '12', '123456789012345678901234567890'] + (['007', ''] if bad else []))
    if rng.random() < 0.5:
        s += '.' + rng.choice(['0', '5', '25', '000001', '9999999999999999999999'] + ([''] if bad else []))
    if rng.random() < 0.4:
        s += rng.choice(['e', 'E']) + rng.choice(['', '+', '-']) + \
            rng.choice(['0', '1', '10', '308', '309', '400', '99999'] + (['', '-5'] if bad else []))
    return s


def rand_string(rng):
    if rng.random() < 0.5:
        return rng.choice(WORDS)
    n = rng.randint(0, 40)
    out = []
    for _ in range(n):
        r = rng.random()
        if r < 0.7:
            out.append(chr(rng.randint(0x20, 0x7e)))
        elif r < 0.8:
            out.append(chr(rng.randint(0, 0x1f)))
        elif r < 0.95:
            out.append(chr(rng.randint(0x80, 0xffff)))
        else:
            out.append(chr(rng.randint(0x10000, 0x10ffff)))
    s = ''.join(out)
    # Python strings may hold surrogates; keep generated values valid.
    return ''.join(c for c in s if not 0xd800 <= ord(c) <= 0xdfff)


def rand_value(rng, depth):
    r = rng.random()
    if depth > 6 or r < 0.35:
        k = rng.random()
        if k < 0.1:
            return None
        if k < 0.2:
            return rng.random() < 0.5
        if k < 0.6:
            return ('num', rand_number_text(rng))
        return rand_string(rng)
    if r < 0.65:
        return [rand_value(rng, depth + 1) for _ in range(rng.randint(0, 6))]
    obj = []
    for _ in range(rng.randint(0, 6)):
        key = rand_string(rng)
        if obj and rng.random() < 0.1:
            key = obj[0][0]                      # duplicate key
        obj.append((key, rand_value(rng, depth + 1)))
    return ('obj', obj)


def ws(rng):
    r = rng.random()
    if r < 0.7:
        return ''
    if r < 0.85:
        # Indentation-like runs (the scanner skips spaces eight at a time).
        return rng.choice(['\n', '\r\n', '']) + ' ' * rng.randint(1, 40) + rng.choice(['', '\t', ' '])
    return ''.join(rng.choice(' \t\n\r') for _ in range(rng.randint(1, 3)))


def encode_string(rng, s):
    out = ['"']
    for c in s:
        o = ord(c)
        if c == '"' or c == '\\':
            out.append('\\' + c)
        elif o < 0x20:
            short = {'\b': '\\b', '\f': '\\f', '\n': '\\n', '\r': '\\r', '\t': '\\t'}
            out.append(short.get(c) if c in short and rng.random() < 0.7 else '\\u%04x' % o)
        elif c == '/' and rng.random() < 0.5:
            out.append('\\/')
        elif o > 0x7f and rng.random() < 0.3:
            if o > 0xffff:
                v = o - 0x10000
                hi, lo = 0xd800 + (v >> 10), 0xdc00 + (v & 0x3ff)
                out.append(('\\u%04x\\u%04x' if rng.random() < 0.5 else '\\u%04X\\u%04X') % (hi, lo))
            else:
                out.append('\\u%04x' % o)
        else:
            out.append(c)
    out.append('"')
    return ''.join(out)


def encode(rng, v):
    if v is None:
        return 'null'
    if v is True:
        return 'true'
    if v is False:
        return 'false'
    if isinstance(v, str):
        return encode_string(rng, v)
    if isinstance(v, list):
        return '[' + ws(rng) + (',' + ws(rng)).join(encode(rng, x) + ws(rng) for x in v) + ']'
    tag, payload = v
    if tag == 'num':
        return payload
    parts = [ws(rng) + encode_string(rng, k) + ws(rng) + ':' + ws(rng) + encode(rng, x) + ws(rng)
             for k, x in payload]
    return '{' + ws(rng) + ','.join(parts) + '}'


STRAY = [b',', b']', b'}', b'[', b'{', b':', b'"', b'\\', b'0', b'-', b'.', b'e', b'+',
         b'\x00', b'\x01', b'\x1f', b'\x7f', b'\xff', b'\xc0\x80', b'\xed\xa0\x80',
         b'\xf4\x90\x80\x80', b'\xe2\x82', b'\\ud800', b'\\udc00', b'\\u12', b'\\x41',
         b'NaN', b'Infinity', b'-Infinity', b'tru', b'nul', b"'", b'/*c*/', b'//',
         b'\xef\xbb\xbf', b' ', b'\n', b'1e999', b'00', b'01']


def corrupt(rng, data):
    data = bytearray(data)
    for _ in range(rng.randint(1, 3)):
        op = rng.random()
        pos = rng.randint(0, len(data))
        if op < 0.25 and data:
            pos = min(pos, len(data) - 1)
            data[pos] = rng.randint(0, 255)
        elif op < 0.5:
            data[pos:pos] = rng.choice(STRAY)
        elif op < 0.65 and data:
            end = min(len(data), pos + rng.randint(1, 4))
            del data[pos:end]
        elif op < 0.8:
            data = data[:pos]
        else:
            end = min(len(data), pos + rng.randint(1, 8))
            data[pos:pos] = data[pos:end]
    return bytes(data)


def make_case(seed):
    rng = random.Random('json-%s' % seed)
    if rng.random() < 0.1:
        data = encode(rng, ('num', rand_number_text(rng))).encode()
    else:
        data = (ws(rng) + encode(rng, rand_value(rng, 0)) + ws(rng)).encode('utf-8', 'surrogatepass')
    if rng.random() < 0.35:
        data = corrupt(rng, data)
    return data


# ── Python's verdict ─────────────────────────────────────────────────────

class Reject(Exception):
    pass


def _no_constants(name):
    raise Reject('constant %s' % name)


def _check_strict(v):
    """Raise Reject for what strict RFC 8259 (and Iron) refuses."""
    if isinstance(v, str):
        if any(0xd800 <= ord(c) <= 0xdfff for c in v):
            raise Reject('lone surrogate')
    elif isinstance(v, float):
        if math.isinf(v) or math.isnan(v):
            raise Reject('overflow')
    elif isinstance(v, int) and not isinstance(v, bool):
        if not INT_MIN <= v <= INT_MAX:
            try:
                f = float(v)
            except OverflowError:
                raise Reject('int overflow')
            if math.isinf(f):
                raise Reject('int overflow')
    elif isinstance(v, list):
        for x in v:
            _check_strict(x)
    elif isinstance(v, dict):
        for k, x in v.items():
            _check_strict(k)
            _check_strict(x)


def _pairs(items):
    # Every member is checked, also those a later duplicate key replaces:
    # Iron rejects an out-of-range number wherever it appears.
    for k, x in items:
        if any(0xd800 <= ord(c) <= 0xdfff for c in k):
            raise Reject('lone surrogate in key')
        _check_strict(x)
    return dict(items)


def depth_of(data):
    d = best = 0
    in_str = esc = False
    for b in data:
        if in_str:
            if esc:
                esc = False
            elif b == 0x5c:
                esc = True
            elif b == 0x22:
                in_str = False
        elif b == 0x22:
            in_str = True
        elif b in (0x5b, 0x7b):
            d += 1
            best = max(best, d)
        elif b in (0x5d, 0x7d):
            d -= 1
    return best


def python_parse(data):
    """('accept', value) or ('reject', reason)."""
    try:
        text = data.decode('utf-8')
    except UnicodeDecodeError:
        return ('reject', 'utf-8')
    try:
        v = json.loads(text, parse_constant=_no_constants, object_pairs_hook=_pairs)
        _check_strict(v)
        return ('accept', v)
    except (ValueError, Reject, RecursionError) as e:
        return ('reject', str(e)[:60])


# ── Comparison ───────────────────────────────────────────────────────────

def same(expected, got):
    """expected: Python's parse of the input; got: Python's parse of Iron's
    canonical output. Iron's Int is Python int, Iron's Float is float."""
    if expected is None or isinstance(expected, bool):
        return expected is got
    if isinstance(expected, int):
        if INT_MIN <= expected <= INT_MAX:
            return type(got) is int and got == expected
        return type(got) is float and got == float(expected)
    if isinstance(expected, float):
        return type(got) is float and got == expected and \
            math.copysign(1, got) == math.copysign(1, expected)
    if isinstance(expected, str):
        return got == expected
    if isinstance(expected, list):
        return isinstance(got, list) and len(got) == len(expected) and \
            all(same(a, b) for a, b in zip(expected, got))
    if isinstance(expected, dict):
        return isinstance(got, dict) and set(got) == set(expected) and \
            all(same(expected[k], got[k]) for k in expected)
    return False


# ── Running ──────────────────────────────────────────────────────────────

def build_driver(ironc, outdir, sanitize):
    exe = os.path.join(outdir, 'json_fuzz_driver')
    r = subprocess.run([ironc, 'build', DRIVER, '-o', exe], cwd=outdir,
                       capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit('json_fuzz: driver build failed:\n' + r.stdout + r.stderr)
    return exe


def run_batch(exe, outdir, seeds, valgrind):
    casedir = os.path.join(outdir, 'cases')
    shutil.rmtree(casedir, ignore_errors=True)
    os.makedirs(casedir)
    datas = {}
    for s in seeds:
        name = 'c%s.json' % s
        datas[name] = make_case(s)
        with open(os.path.join(casedir, name), 'wb') as f:
            f.write(datas[name])
    listfile = os.path.join(outdir, 'cases.txt')
    with open(listfile, 'w') as f:
        f.write('\n'.join(sorted(datas)) + '\n')
    cmd = [exe, casedir, listfile]
    if valgrind:
        cmd = ['valgrind', '--quiet', '--leak-check=full', '--errors-for-leak-kinds=definite,indirect',
               '--error-exitcode=9'] + cmd
    r = subprocess.run(cmd, capture_output=True)
    failures = []
    if r.returncode != 0:
        failures.append(('driver', 'exit %d: %s' % (r.returncode, r.stderr.decode(errors='replace')[-2000:])))
    seen = set()
    for raw in r.stdout.split(b'\n'):
        if not raw:
            continue
        parts = raw.split(b' ', 2)
        name = parts[0].decode()
        verdict = parts[1].decode() if len(parts) > 1 else '?'
        rest = parts[2] if len(parts) > 2 else b''
        seen.add(name)
        data = datas.get(name)
        if data is None:
            failures.append((name, 'unknown result line'))
            continue
        if verdict == 'roundtrip':
            failures.append((name, 'iron round trip failed: ' + rest.decode()))
            continue
        exp = python_parse(data)
        deep = depth_of(data) >= MAX_DEPTH - 2
        if verdict == 'reject':
            if exp[0] == 'accept' and not deep:
                failures.append((name, 'iron rejects (%s), python accepts' % rest.decode()))
            continue
        if exp[0] == 'reject':
            failures.append((name, 'iron accepts, python rejects: %s' % exp[1]))
            continue
        try:
            got = json.loads(rest.decode('utf-8'))
        except ValueError as e:
            failures.append((name, 'iron canonical output is not JSON: %s' % e))
            continue
        if not same(exp[1], got):
            failures.append((name, 'values differ:\n  python: %r\n  iron:   %s' %
                             (exp[1], rest.decode('utf-8', 'replace')[:400])))
    for name in datas:
        if name not in seen and not any(f[0] == 'driver' for f in failures):
            failures.append((name, 'no result line'))
    return failures, datas


def campaign(ironc, outdir, seed, count, jobs, valgrind, batch=2000):
    os.makedirs(outdir, exist_ok=True)
    exe = build_driver(ironc, outdir, False)
    seeds = ['%s-%d' % (seed, i) for i in range(count)]
    batches = [seeds[i:i + batch] for i in range(0, len(seeds), batch)]
    total_fail = 0
    faildir = os.path.join(outdir, 'fail')

    def one(idx_batch):
        idx, b = idx_batch
        sub = os.path.join(outdir, 'b%d' % idx)
        os.makedirs(sub, exist_ok=True)
        return run_batch(exe, sub, b, valgrind)

    results = []
    if jobs > 1:
        from concurrent.futures import ThreadPoolExecutor
        with ThreadPoolExecutor(jobs) as pool:
            results = list(pool.map(one, enumerate(batches)))
    else:
        results = [one(x) for x in enumerate(batches)]
    for failures, datas in results:
        for name, why in failures:
            total_fail += 1
            if total_fail <= 20:
                print('FAIL %s: %s' % (name, why))
            if name in datas:
                os.makedirs(faildir, exist_ok=True)
                with open(os.path.join(faildir, name), 'wb') as f:
                    f.write(datas[name])
    print('json_fuzz: %d cases, %d failures' % (count, total_fail))
    return 1 if total_fail else 0


def main(argv):
    if argv and argv[0] == '--gen':
        sys.stdout.buffer.write(make_case(argv[1]) + b'\n')
        return 0
    if argv and argv[0] == '--suite':
        ironc, outdir = argv[1], argv[2]
        return campaign(os.path.abspath(ironc), os.path.abspath(outdir), 'suite', 4000, 2, False)
    jobs = 1
    valgrind = '--valgrind' in argv
    args = [a for a in argv if a != '--valgrind']
    if '-j' in args:
        i = args.index('-j')
        jobs = int(args[i + 1])
        del args[i:i + 2]
    if len(args) < 3:
        print(__doc__)
        return 2
    ironc, outdir, seed = args[0], args[1], args[2]
    count = int(args[3]) if len(args) > 3 else 2000
    return campaign(os.path.abspath(ironc), os.path.abspath(outdir), seed, count, jobs, valgrind)


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
