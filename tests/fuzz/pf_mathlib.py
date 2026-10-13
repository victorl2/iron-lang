"""The math, time, hint modules and the number to_string methods (manual
9.2, 9.4, 9.6, 9.8, 6.5): every Math function on edge values (signed zeros,
subnormals, the largest double, infinities, NaN, halfway cases for round,
arguments outside the domain), the constants, seed determinism and the
ranges of random / random_int (inclusive at both ends, also the full Int
range and an empty range) / random_float; Time clocks (monotonic, sleep,
since), Timer on edge durations and updates, Duration factories at the
Int limits; Hint.black_box; Int / Int32 / Float to_string against the
interpolated text; Box cells holding values that own memory.

The model of each Math function is the platform C library called through
ctypes (Iron's Math is a thin layer over it), so the probes check the
plumbing: argument and result passing, the text form of the result
(pf_num.fmt_float), sign, lerp, the constants and the random contracts.
Timer converts seconds to milliseconds like an Iron Float to Int
conversion (truncating, saturating, NaN to 0; manual 2.9) and adds
saturating.
"""
import ctypes
import ctypes.util
import math
from decimal import Decimal

from stdfz_common import StdGen, Probe, b2s
from pf_num import fmt_float

_libm = ctypes.CDLL(ctypes.util.find_library('m') or None)
for _n in ('sin', 'cos', 'tan', 'asin', 'acos', 'sqrt', 'floor', 'ceil', 'round', 'log', 'log2',
           'exp'):
    getattr(_libm, _n).restype = ctypes.c_double
    getattr(_libm, _n).argtypes = [ctypes.c_double]
for _n in ('atan2', 'pow', 'hypot'):
    getattr(_libm, _n).restype = ctypes.c_double
    getattr(_libm, _n).argtypes = [ctypes.c_double, ctypes.c_double]

INF = float('inf')
NAN = float('nan')
I64MIN, I64MAX = -(1 << 63), (1 << 63) - 1

VALS = [0.0, -0.0, 1.0, -1.0, 0.5, -0.5, 2.0, 1.5, 2.5, -2.5, 3.5, 0.49999999999999994,
        4503599627370495.5, 4503599627370497.0, 0.1, 1e-310, 5e-324, -5e-324, 2.2250738585072014e-308,
        1.7976931348623157e308, -1.7976931348623157e308, INF, -INF, NAN, math.pi, -math.pi,
        math.pi / 2, math.e, 1e16, 1e22, -1e-5, 709.78, 710.0, -745.2, -750.0, 1e300, 0.9999999999999999,
        1.0000000000000002, 100.0, -100.0, 3.0, 1e-8, 123456.789]

ONE = ['sin', 'cos', 'tan', 'asin', 'acos', 'sqrt', 'floor', 'ceil', 'round', 'log', 'log2', 'exp']
TWO = ['atan2', 'pow', 'hypot']

INTS = [0, 1, -1, 2, 7, 1000, -1000, 999, 2147483647, -2147483648, 4294967296, I64MAX, I64MIN,
        I64MAX - 1, I64MIN + 1, 9223372036854775, 9223372036854776, 153722867280912930,
        153722867280912931, -153722867280912931]

HELPERS = '''func fid(x: Float) -> Float {
    return x
}

func inf() -> Float {
    return fid(1.0) / fid(0.0)
}

func nan() -> Float {
    return fid(0.0) / fid(0.0)
}

func neg_zero() -> Float {
    return fid(0.0) * fid(-1.0)
}'''


def flit(v):
    s = format(Decimal(repr(abs(v))), 'f')
    if '.' not in s:
        s += '.0'
    return s


def fexpr(v):
    if math.isnan(v):
        return 'nan()'
    if math.isinf(v):
        return 'inf()' if v > 0 else '(0.0 - inf())'
    if v == 0.0:
        return 'neg_zero()' if math.copysign(1, v) < 0 else 'fid(0.0)'
    return f'fid({"-" if v < 0 else ""}{flit(v)})'


def iexpr(v):
    if v == I64MIN:
        return '(-9223372036854775807 - 1)'
    return str(v)


def wrap(v):
    v &= (1 << 64) - 1
    return v - (1 << 64) if v >= 1 << 63 else v


def sat(x):
    """An Iron Float to Int conversion (manual 2.9)."""
    if math.isnan(x):
        return 0
    if x >= 9.223372036854775807e18:
        return I64MAX
    if x <= -9.223372036854775808e18:
        return I64MIN
    return int(x)


def satadd(a, b):
    return max(I64MIN, min(I64MAX, a + b))


class MathGen(StdGen):
    NPROBES = (6, 14)

    def __init__(self, rng):
        super().__init__(rng)
        for m in ('math', 'time', 'hint'):
            self.need_import(m)
        self.need('mh', HELPERS)

    def p_math(self):
        r = self.r
        code, out = [], []
        exprs, texts = [], []
        for _ in range(r.randint(1, 6)):
            k = r.random()
            if k < 0.6:
                f = r.choice(ONE)
                x = r.choice(VALS)
                exprs.append(f'Math.{f}({fexpr(x)})')
                texts.append(fmt_float(getattr(_libm, f)(x)))
            elif k < 0.8:
                f = r.choice(TWO)
                x, y = r.choice(VALS), r.choice(VALS)
                exprs.append(f'Math.{f}({fexpr(x)}, {fexpr(y)})')
                texts.append(fmt_float(getattr(_libm, f)(x, y)))
            elif k < 0.9:
                a, b, t = r.choice(VALS), r.choice(VALS), r.choice(VALS + [0.25, 0.75])
                exprs.append(f'Math.lerp({fexpr(a)}, {fexpr(b)}, {fexpr(t)})')
                texts.append(fmt_float(a + (b - a) * t))
            else:
                x = r.choice(VALS)
                exprs.append(f'Math.sign({fexpr(x)})')
                texts.append(str(1 if x > 0 else -1 if x < 0 else 0))
        self.raw(code, out, exprs, texts)
        return Probe(code, out)

    def p_const(self):
        code, out = [], []
        self.raw(code, out, ['Math.PI', 'Math.TAU', 'Math.E', 'Math.PI * 2.0 == Math.TAU'],
                 [fmt_float(math.pi), fmt_float(math.tau), fmt_float(math.e), 'true'])
        return Probe(code, out)

    def p_random(self):
        r = self.r
        code, out = [], []
        k = r.choice(['seed', 'unit', 'int', 'int', 'float'])
        seed = r.choice([0, 1, -1, 42, I64MAX, I64MIN, r.randint(-10**12, 10**12)])
        if k == 'seed':
            a, b = self.fresh('a'), self.fresh('b')
            code += [f'Math.seed({iexpr(seed)})', f'var {a}: [Float] = []', 'for i in range(20) {',
                     f'    {a}.push(Math.random())', '}', f'var {a}i = Math.random_int(-50, 50)',
                     f'Math.seed({iexpr(seed)})', f'var {b} = true', 'for i in range(20) {',
                     f'    if Math.random() != {a}[i] {{', f'        {b} = false', '    }', '}',
                     f'if Math.random_int(-50, 50) != {a}i {{', f'    {b} = false', '}']
            self.raw(code, out, [b], ['true'])
        elif k == 'unit':
            ok = self.fresh('ok')
            n = r.choice([100, 2000])
            code += [f'Math.seed({iexpr(seed)})', f'var {ok} = true', f'for i in range({n}) {{',
                     '    val x = Math.random()', f'    if x < 0.0 or x >= 1.0 {{', f'        {ok} = false',
                     '    }', '}']
            self.raw(code, out, [ok], ['true'])
        elif k == 'int':
            lo = r.choice(INTS + [r.randint(-5, 5)])
            span = r.choice([0, 1, 2, 3, 10, 1000, None])
            if span is None:
                hi = r.choice(INTS)
            else:
                hi = max(I64MIN, min(I64MAX, lo + span))
            if r.random() < 0.1:
                lo, hi = I64MIN, I64MAX
            if r.random() < 0.1:
                lo, hi = hi, lo
            ok, lo_hit, hi_hit = self.fresh('ok'), self.fresh('lh'), self.fresh('hh')
            n = 400
            code += [f'Math.seed({iexpr(seed)})', f'var {ok} = true', f'var {lo_hit} = false',
                     f'var {hi_hit} = false', f'for i in range({n}) {{',
                     f'    val x = Math.random_int({iexpr(lo)}, {iexpr(hi)})',
                     f'    if x < {iexpr(min(lo, hi))} or x > {iexpr(max(lo, hi))} {{',
                     f'        {ok} = false', '    }',
                     f'    if x == {iexpr(lo)} {{', f'        {lo_hit} = true', '    }',
                     f'    if x == {iexpr(hi)} {{', f'        {hi_hit} = true', '    }', '}']
            if hi < lo:
                # an empty range gives min (documented)
                self.raw(code, out, [ok, lo_hit, hi_hit], ['true', 'true', 'false'])
            elif hi - lo <= 3:
                self.raw(code, out, [ok, lo_hit, hi_hit], ['true', 'true', 'true'])
            else:
                self.raw(code, out, [ok], ['true'])
        else:
            lo, hi = sorted([r.choice([0.0, -1.0, 1.0, 1e-300, 1e300, -1e300, 0.5, 2.0]),
                             r.choice([0.0, -1.0, 1.0, 1e-300, 1e300, 0.5, 2.0, 10.0])])
            ok = self.fresh('ok')
            code += [f'Math.seed({iexpr(seed)})', f'var {ok} = true', 'for i in range(500) {',
                     f'    val x = Math.random_float({fexpr(lo)}, {fexpr(hi)})',
                     f'    if x < {fexpr(lo)} or x > {fexpr(hi)} {{', f'        {ok} = false', '    }', '}']
            self.raw(code, out, [ok], ['true'])
        return Probe(code, out)

    def p_clock(self):
        r = self.r
        code, out = [], []
        k = r.choice(['mono', 'sleep', 'since', 'wall'])
        if k == 'mono':
            ok = self.fresh('ok')
            code += [f'var {ok} = true', 'var last_ms = Time.now_ms()', 'var last_ns = Time.now_ns()',
                     'for i in range(2000) {', '    val m = Time.now_ms()', '    val n = Time.now_ns()',
                     f'    if m < last_ms or n < last_ns {{', f'        {ok} = false', '    }',
                     '    last_ms = m', '    last_ns = n', '}']
            self.raw(code, out, [ok, 'Time.now_ns() / 1000000 - Time.now_ms() <= 1'], ['true', 'true'])
        elif k == 'sleep':
            ms = r.choice([-5, -1000, 0, 1, 3, 12])
            t = self.fresh('t')
            code += [f'val {t} = Time.now_ms()', f'Time.sleep({ms})']
            self.raw(code, out, [f'Time.now_ms() - {t} >= {max(0, ms)}', f'Time.now_ms() - {t} < 2000'],
                     ['true', 'true'])
        elif k == 'since':
            t = self.fresh('t')
            code += [f'val {t} = Time.now()', 'Time.sleep(2)']
            self.raw(code, out, [f'Time.since({t}) >= 0.001', f'Time.since({t}) < 5.0', f'{t} > 1600000000.0'],
                     ['true', 'true', 'true'])
        else:
            self.raw(code, out, ['Time.now() > 1600000000.0', 'Time.now() < 9999999999.0'], ['true', 'true'])
        return Probe(code, out)

    def p_timer(self):
        r = self.r
        code, out = [], []
        secs = r.choice(VALS + [0.001, 0.0005, 0.25])
        t = self.fresh('tm')
        code.append(f'var {t} = Time.Timer({fexpr(secs)})')
        dur = sat(secs * 1000.0)
        el = 0
        self.raw(code, out, [f'{t}.duration_ms', f'{t}.elapsed_ms', f'{t}.done()'], [dur, el, b2s(el >= dur)])
        exprs, texts = [], []
        for _ in range(r.randint(0, 4)):
            if r.random() < 0.8:
                dt = r.choice(VALS + [0.016, 0.0001])
                code.append(f'{t}.update({fexpr(dt)})')
                el = satadd(el, sat(dt * 1000.0))
            else:
                code.append(f'{t}.reset()')
                el = 0
        exprs += [f'{t}.elapsed_ms', f'{t}.done()']
        texts += [el, b2s(el >= dur)]
        self.raw(code, out, exprs, texts)
        return Probe(code, out)

    def p_duration(self):
        r = self.r
        code, out = [], []
        exprs, texts = [], []
        for _ in range(r.randint(1, 4)):
            n = r.choice(INTS)
            f = r.choice(['millis', 'seconds', 'minutes', 'from_ms'])
            m = {'millis': 1, 'seconds': 1000, 'minutes': 60000, 'from_ms': 1}[f]
            exprs.append(f'Duration.{f}({iexpr(n)}).to_ms()' if r.random() < 0.5 else
                         f'Duration.{f}({iexpr(n)}).ms')
            texts.append(wrap(n * m))
        self.raw(code, out, exprs, texts)
        return Probe(code, out)

    def p_tostring(self):
        r = self.r
        code, out = [], []
        exprs, texts = [], []
        for _ in range(r.randint(1, 5)):
            k = r.random()
            if k < 0.35:
                v = r.choice(INTS)
                exprs.append(f'Hint.black_box({iexpr(v)})' if r.random() < 0.5 else
                             f'({iexpr(v)}).to_string()')
                texts.append(str(v))
            elif k < 0.5:
                v = r.choice([0, 1, -1, 2147483647, -2147483647, 65536, -65536])
                n = self.fresh('i')
                code.append(f'val {n}: Int32 = {v}')
                exprs.append(f'{n}.to_string()')
                texts.append(str(v))
            else:
                v = r.choice(VALS)
                n = self.fresh('f')
                code.append(f'val {n} = {fexpr(v)}')
                exprs.append(f'{n}.to_string() == "{{{n}}}"')
                texts.append('true')
                exprs.append(f'{n}.to_string()')
                texts.append(fmt_float(v))
        self.raw(code, out, exprs, texts)
        return Probe(code, out)

    def p_box(self):
        r = self.r
        code, out = [], []
        b = self.fresh('bx')
        s = 'boxed string long enough to live on the heap ' * r.randint(1, 3)
        k = r.choice(['str', 'int', 'free', 'null', 'list'])
        if k == 'str':
            code += [f'val {b} = Box("{s}")', f'val p{b}: *var unchecked String = {b}.unwrap()']
            self.raw(code, out, [f'{b}.is_null()', f'p{b}.byte_len()'], ['false', len(s)])
        elif k == 'int':
            v = r.choice(INTS)
            code += [f'val {b} = Box({iexpr(v)})', f'val p{b}: *var unchecked Int = {b}.unwrap()']
            self.raw(code, out, [f'{b}.is_null()', f'p{b}'], ['false', v])
        elif k == 'free':
            code += [f'val {b} = Box("{s}")', f'{b}.free()']
            self.raw(code, out, [f'{b}.is_null()'], ['true'])
        elif k == 'null':
            code += [f'val {b}: Box[String] = Box.null()', f'{b}.free()']
            self.raw(code, out, [f'{b}.is_null()'], ['true'])
        else:
            code += [f'var {b}: [Box[String]] = []', 'for i in range(5) {',
                     f'    {b}.push(Box("{s}" + "{{i}}"))', '}', f'{b}[2].free()']
            self.raw(code, out, [f'{b}[2].is_null()', f'{b}[1].is_null()'], ['true', 'false'])
        return Probe(code, out)

    def p_box_null_unwrap(self):
        b = self.fresh('bx')
        return Probe([f'val {b}: Box[Int] = Box.null()', f'val p = {b}.unwrap()'], [],
                     panic='unwrap() on null Box')

    def probe_kinds(self):
        return [(8, self.p_math), (1, self.p_const), (4, self.p_random), (2, self.p_clock),
                (3, self.p_timer), (2, self.p_duration), (3, self.p_tostring), (2, self.p_box)]

    def panic_kinds(self):
        return [self.p_box_null_unwrap]


def generate(rng):
    g = MathGen(rng)
    g.program()
    return g
