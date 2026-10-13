"""String methods and list edges pf_strcoll leaves out (manual 9.2, 9.3):
upper / lower on every script with simple case forms (accented Greek,
Cyrillic, Armenian, Georgian, Cherokee, letters whose case form is several
characters and keep theirs), to_int / to_float on signed, exponent,
fraction-only, hexadecimal, inf / nan, out of range and malformed text,
String.from_byte outside 0..255, byte_slice at every clamp, split /
replace / count with an empty pattern, pad_left / pad_right with a pad of
several characters or none, Float lists sorted with NaN and signed zeros,
contains(NaN), list slices and inserts at the ends.

The rules (documented in the manual with these fixes): case mapping is the
simple Unicode mapping of src/runtime/iron_casemap.h (the model reads that
table); to_float takes optional whitespace, a sign, digits with an
optional fraction or a fraction alone, an optional exponent, and nothing
else (0 otherwise; a value past the Float range is an infinity); to_int
takes a sign and decimal digits within Int (0 otherwise); from_byte of a
value outside 0..255 is ""; split("") gives the characters, replace("",
x) leaves the string as it is, count("") is 0; a pad takes the first
character of `ch`, and an empty `ch` pads with spaces; sort puts NaN last.
"""
import math
import os
import re

from stdfz_common import StdGen, Probe, lit, qtext
from pf_num import fmt_float

HERE = os.path.dirname(os.path.abspath(__file__))
_RUN = re.compile(r'\{0x([0-9A-F]+), 0x([0-9A-F]+), (-?\d+), (\d)\}')


def _load_table():
    path = os.path.normpath(os.path.join(HERE, '..', '..', 'src', 'runtime', 'iron_casemap.h'))
    up, lo, cur = [], [], None
    for line in open(path, encoding='utf-8'):
        if 'iron_case_upper' in line:
            cur = up
        elif 'iron_case_lower' in line:
            cur = lo
        m = _RUN.search(line)
        if m and cur is not None:
            cur.append((int(m.group(1), 16), int(m.group(2), 16), int(m.group(3)), int(m.group(4))))
    return up, lo


UPPER, LOWER = _load_table()


def cmap(table, ch):
    c = ord(ch)
    for a, b, d, st in table:
        if a <= c <= b and (c - a) % st == 0:
            return chr(c + d)
    return ch


def upper(s):
    return ''.join(cmap(UPPER, ch) if ord(ch) >= 0x80 else ch.upper() for ch in s)


def lower(s):
    return ''.join(cmap(LOWER, ch) if ord(ch) >= 0x80 else ch.lower() for ch in s)


CASE_POOL = list('aZ ,') + list('éÉßµÿŸıİſǅǄǆ'
                                'άΆϊΪςΣϐѐЀҋԱա'
                                'აႠᎠꭰKΩẞɐⱯևﬀ'
                                '\U00010400\U00010428\U0001e900\U0001f600日')
WS = ' \t\n\r\x0b\x0c'
FLOAT_RE = re.compile(r'[+-]?([0-9]+(\.[0-9]*)?|\.[0-9]+)([eE][+-]?[0-9]+)?\Z')
INT_RE = re.compile(r'[+-]?[0-9]+\Z')


def to_float(s):
    t = s.strip(WS)
    if not FLOAT_RE.match(t):
        return 0.0
    return float(t)


def to_int(s):
    t = s.strip(WS)
    if not INT_RE.match(t):
        return 0
    v = int(t)
    return v if -(1 << 63) <= v < (1 << 63) else 0


class StrMoreGen(StdGen):
    NPROBES = (6, 14)

    def p_case(self):
        r = self.r
        code, out = [], []
        for _ in range(r.randint(1, 4)):
            s = ''.join(r.choice(CASE_POOL) for _ in range(r.randint(0, 8)))
            k = r.random()
            if k < 0.5:
                self.line(code, out, [f'{lit(s)}.upper()'], [upper(s)])
            else:
                self.line(code, out, [f'{lit(s)}.lower()'], [lower(s)])
        return Probe(code, out)

    def rnum(self):
        r = self.r
        sign = r.choice(['', '', '-', '+', '--', '+-'])
        body = r.choice(['0', '7', '42', '007', '9223372036854775807', '9223372036854775808',
                         '99999999999999999999', '1.5', '.5', '5.', '.', '1e3', '1E-3', '2.5e+10', '1e',
                         '1e+', 'e5', '0x10', '0x1p3', 'inf', 'infinity', 'nan', 'NaN', '1_000', '1,5',
                         '1e400', '1e-400', '4.9e-324', '1.7976931348623157e308', '0.1', '12abc',
                         '١', '1\x002', '', ' '])
        lead = r.choice(['', '', ' ', '\t', '\n '])
        trail = r.choice(['', '', ' ', '\n', ' x'])
        return lead + sign + body + trail

    def p_parse(self):
        r = self.r
        code, out = [], []
        exprs, texts = [], []
        for _ in range(r.randint(1, 5)):
            s = self.rnum()
            if r.random() < 0.5:
                exprs.append(f'{lit(s)}.to_float()')
                v = to_float(s)
                texts.append(fmt_float(v))
            else:
                exprs.append(f'{lit(s)}.to_int()')
                texts.append(str(to_int(s)))
        self.raw(code, out, exprs, texts)
        return Probe(code, out)

    def p_bytes(self):
        r = self.r
        code, out = [], []
        exprs, texts = [], []
        for _ in range(r.randint(1, 5)):
            k = r.random()
            if k < 0.4:
                n = r.choice([-1, -255, 0, 1, 65, 127, 128, 255, 256, 1000, -9223372036854775807])
                ok = 0 <= n <= 255
                exprs.append(f'String.from_byte({n}).byte_len()')
                texts.append(1 if ok else 0)
                if ok:
                    exprs.append(f'String.from_byte({n}).byte_at(0)')
                    texts.append(n)
            else:
                b = ''.join(r.choice(['a', 'é', '日', '\x00', '😀', 'z']) for _ in range(r.randint(0, 5))).encode()
                L = len(b)
                a = r.choice([-5, -1, 0, 1, 2, L - 1, L, L + 3, 100])
                c = r.choice([-1, 0, 1, a, a + 1, L, L + 5])
                aa, cc = max(0, min(a, L)), max(0, min(c, L))
                self.line(code, out, [f'{lit(b)}.byte_slice({a}, {c})'], [b[aa:cc] if aa < cc else b''])
        if exprs:
            self.raw(code, out, exprs, texts)
        return Probe(code, out)

    def p_empty_patterns(self):
        r = self.r
        code, out = [], []
        s = ''.join(r.choice(['a', 'b', 'é', '日', ' ', '😀']) for _ in range(r.randint(0, 5)))
        new = r.choice(['', '-', 'xy'])
        self.raw(code, out, [f'{lit(s)}.split("").len()', f'{lit(s)}.count("")'], [len(s), 0])
        if s == '':
            # "".split("") is one empty part, like any split of ""
            pass
        self.line(code, out, [f'{lit(s)}.replace("", {lit(new)})', f'"|".join({lit(s)}.split(""))'],
                  [s, '|'.join(list(s)) if s else ''])
        ch = r.choice(['ab', '', '日本', '😀x', 'é'])
        w = r.choice([0, len(s), len(s) + 2, 6])
        p = ch[0] if ch else ' '
        left = p * (w - len(s)) + s if len(s) < w else s
        right = s + p * (w - len(s)) if len(s) < w else s
        self.line(code, out, [f'{lit(s)}.pad_left({w}, {lit(ch)})', f'{lit(s)}.pad_right({w}, {lit(ch)})'],
                  [left, right])
        return Probe(code, out)

    def p_float_list(self):
        r = self.r
        code, out = [], []
        vals = [r.choice([0.0, -0.0, 1.5, -2.0, float('nan'), float('inf'), -float('inf'), 1e-300, 3.0])
                for _ in range(r.randint(0, 7))]

        def fx(v):
            if math.isnan(v):
                return '(0.0 / 0.0)'
            if math.isinf(v):
                return '(1.0 / 0.0)' if v > 0 else '(-1.0 / 0.0)'
            if v == 0 and math.copysign(1, v) < 0:
                return '(0.0 * -1.0)'
            return repr(v) if 'e' not in repr(v) else format(v, '.400f').rstrip('0')
        n = self.fresh('fl')
        code.append(f'var {n}: [Float] = [' + ', '.join(fx(v) for v in vals) + ']')
        code.append(f'{n}.sort()')
        code.append(f'var {n}s = ""')
        code.append(f'for x in {n} {{')
        code.append(f'    {n}s += "{{x}} "')
        code.append('}')
        srt = sorted([v for v in vals if not math.isnan(v)]) + [v for v in vals if math.isnan(v)]
        self.raw(code, out, [f'{n}s', f'{n}.contains(0.0 / 0.0)', f'{n}.contains(0.0)'],
                 [''.join(fmt_float(v) + ' ' for v in srt), 'false', 'true' if any(v == 0 for v in vals) else 'false'])
        return Probe(code, out)

    def p_list_edges(self):
        r = self.r
        code, out = [], []
        xs = [r.randint(0, 9) for _ in range(r.randint(0, 6))]
        n = self.fresh('xs')
        code.append(f'var {n}: [Int] = [' + ', '.join(map(str, xs)) + ']')
        L = len(xs)
        a = r.randint(0, L)
        b = r.randint(a, L)
        code.append(f'{n}.insert({n}.len(), 77)')
        code.append(f'{n}.insert(0, 66)')
        ys = [66] + xs + [77]
        code.append(f'val {n}s = {n}[pk({a})..pk({b + 1})]')
        sl = ys[a:b + 1]
        self.raw(code, out, [f'{n}s.len()', f'{n}.len()', f'{n}[0]', f'{n}[{n}.len() - 1]'],
                 [len(sl), len(ys), 66, 77])
        return Probe(code, out)

    def p_slice_oob(self):
        n = self.fresh('xs')
        return Probe([f'var {n}: [Int] = [1, 2, 3]', f'val bad = {n}[pk(1)..pk(9)]',
                      'println("{bad.len()}")'], [], panic='index out of bounds')

    def probe_kinds(self):
        return [(4, self.p_case), (4, self.p_parse), (3, self.p_bytes), (2, self.p_empty_patterns),
                (2, self.p_float_list), (2, self.p_list_edges)]

    def panic_kinds(self):
        return [self.p_slice_oob]


def generate(rng):
    g = StrMoreGen(rng)
    g.program()
    return g
