"""Numeric edge cases (manual 1.4, 2.1, 2.9, 3.2, 3.3, 9.4): NaN, infinities
and signed zeros through Float and Float32 arithmetic, comparisons,
interpolation, to_string and Math.sqrt / floor / ceil / round; huge,
subnormal and long literals; Float to integer conversions of every value
(saturating, NaN to 0: manual 2.9, #466) and constant ones that do not fit
(E0311); integer overflow at every width, MIN / -1, abs(MIN); shifts by
counts of every integer type, up to and far past the width, UInt64 counts
of 2^63 and more (#467); integer division and modulo by zero (a panic) and
negative shift counts (a panic).

The model is IEEE 754 for Float (binary64) and Float32 (binary32, each
operation rounded to binary32), the runtime's text form for floats (shortest
digits that read back; -0 prints as 0, NaN as NaN, infinities as inf and
-inf), and pf_num's wrapping integer arithmetic.
"""
import math
from decimal import Decimal

from pf_probe import Probe, ProbeGen
from pf_lang import Bail
from pf_num import INTS, wrapt, lo_hi, int_binop, fmt_float

INF = float('inf')
NAN = float('nan')
F32MAX = 3.4028234663852886e+38


def f32(x):
    """Round a double to binary32 (overflow to infinity)."""
    import struct
    if math.isnan(x) or math.isinf(x):
        return x
    try:
        return struct.unpack('<f', struct.pack('<f', x))[0]
    except OverflowError:
        return math.copysign(INF, x)


def int_to_f32(v):
    """An integer rounded once to binary32 (round to nearest even)."""
    if v == 0:
        return 0.0
    s = -1 if v < 0 else 1
    a = abs(v)
    n = a.bit_length()
    if n <= 24:
        return float(v)
    shift = n - 24
    q, rem = divmod(a, 1 << shift)
    half = 1 << (shift - 1)
    if rem > half or (rem == half and q & 1):
        q += 1
    r = q << shift
    if r > F32MAX * 1.0000001:
        return s * INF
    return s * float(r)


def fdiv(a, b):
    if b == 0:
        if a == 0 or math.isnan(a):
            return NAN
        neg = (math.copysign(1, a) < 0) != (math.copysign(1, b) < 0)
        return -INF if neg else INF
    return a / b


def fmod(a, b):
    if math.isnan(a) or math.isnan(b) or math.isinf(a) or b == 0:
        return NAN
    if math.isinf(b):
        return a
    return math.fmod(a, b)


def fop(op, a, b, t):
    if op == '+':
        r = a + b
    elif op == '-':
        r = a - b
    elif op == '*':
        r = a * b
    elif op == '/':
        r = fdiv(a, b)
    else:
        r = fmod(a, b)
    return f32(r) if t == 'Float32' else r


def c_sqrt(x):
    if math.isnan(x) or x < 0:
        return NAN
    return math.sqrt(x)   # sqrt(-0.0) is -0.0, sqrt(inf) inf


def c_floor(x):
    if math.isnan(x) or math.isinf(x) or x == 0:
        return x
    return math.copysign(float(math.floor(x)), x) if math.floor(x) == 0 else float(math.floor(x))


def c_ceil(x):
    if math.isnan(x) or math.isinf(x) or x == 0:
        return x
    r = float(math.ceil(x))
    return math.copysign(r, x) if r == 0 else r


def c_round(x):
    """C round(): half away from zero, sign kept on zero."""
    if math.isnan(x) or math.isinf(x) or x == 0:
        return x
    a = abs(x)
    if a >= 2.0 ** 52:
        return x
    f = math.floor(a)
    r = float(f + 1) if a - f >= 0.5 else float(f)
    return math.copysign(r, x)


def f2i(v, t):
    """Run-time Float to integer conversion: saturating, NaN to 0."""
    if math.isnan(v):
        return 0
    lo, hi = lo_hi(t)
    if math.isinf(v):
        return hi if v > 0 else lo
    iv = int(v)
    return max(lo, min(hi, iv))


def f2i_fits(v, t):
    """Whether a constant conversion fits (no E0311)."""
    if math.isnan(v) or math.isinf(v):
        return False
    lo, hi = lo_hi(t)
    return lo <= int(v) <= hi


def flit(v):
    """Plain decimal digits for a finite non-negative double."""
    s = format(Decimal(repr(v)), 'f')
    if '.' not in s:
        s += '.0'
    return s


def fshow(v, t):
    return fmt_float(v, t == 'Float32')


class FE:
    """A float expression: source, value, type, and whether it is a
    constant expression (float literals with unary - and + - * /)."""

    def __init__(self, src, v, t, const=False):
        self.src, self.v, self.t, self.const = src, v, t, const


class IE:
    def __init__(self, src, v, t):
        self.src, self.v, self.t = src, v, t


HELPERS = '''func pk(n: Int) -> Int {
    return n
}

func pkf(x: Float) -> Float {
    return x
}'''

ORDINARY = [0.5, 1.5, 3.0, 0.1, 0.2, 0.3, 2.0, 7.25, 100.0, 1e-3, 123456.789, 1e15 + 0.5,
            9007199254740993.0, 0.0, 1.0, 10.0, 0.7, 2.5, 1e21, 1e20, 1e-6, 1e-7]
EXTREME = [1.7976931348623157e308, 1e308, 8.98846567431158e307, 2.2250738585072014e-308,
           5e-324, 1e-310, 4.9406564584124654e-322, 3.4028234663852886e+38, 3.4028235677973366e+38,
           1.401298464324817e-45, 1.1754943508222875e-38, 1e300, 1e-300, 2.0 ** 63, 2.0 ** 64,
           9223372036854775807.0, 18446744073709551615.0, 2147483648.0, 4294967296.0]


class NumEdgeGen(ProbeGen):
    NPROBES = (5, 12)

    def __init__(self, rng):
        super().__init__(rng)
        self.need('helpers', HELPERS)

    # ------------------------------------------------------------ floats
    def fval(self):
        r = self.r
        c = r.random()
        if c < 0.5:
            return r.choice(ORDINARY)
        return r.choice(EXTREME)

    def fleaf(self, t, env):
        """A Float or Float32 leaf: a binding, a literal, an opaque value or
        a special (NaN, +-inf, -0)."""
        r = self.r
        names = [n for n, (tt, v) in env.items() if tt == t]
        c = r.random()
        if names and c < 0.35:
            n = r.choice(names)
            return FE(n, env[n][1], t)
        if c < 0.6:
            k = r.choice(['nan', 'inf', 'ninf', 'nzero'])
            src, v = {'nan': ('(pkf(0.0) / pkf(0.0))', NAN), 'inf': ('(1.0 / pkf(0.0))', INF),
                      'ninf': ('(-1.0 / pkf(0.0))', -INF), 'nzero': ('(-pkf(0.0))', -0.0)}[k]
        else:
            v = self.fval()
            if r.random() < 0.3:
                v = -v
            lit = flit(abs(v))
            if r.random() < 0.3 and t == 'Float':
                # a constant: folded by the compiler
                src = f'(-{lit})' if math.copysign(1, v) < 0 else lit
                return FE(src, v, t, const=True)
            src = f'pkf({"-" if math.copysign(1, v) < 0 else ""}{lit})'
        if t == 'Float32':
            return FE(f'Float32({src})', f32(v), t)
        return FE(src, v, t)

    def fexpr(self, t, env, d):
        r = self.r
        if d <= 0 or r.random() < 0.3:
            return self.fleaf(t, env)
        c = r.random()
        if c < 0.12:
            a = self.fexpr(t, env, d - 1)
            return FE(f'(-{a.src})', -a.v, t, a.const)
        if c < 0.2:
            # a conversion between Float and Float32
            o = 'Float32' if t == 'Float' else 'Float'
            a = self.fexpr(o, env, d - 1)
            v = f32(a.v) if t == 'Float32' else a.v
            return FE(f'{t}({a.src})', v, t)
        if c < 0.27 and t == 'Float':
            fn = r.choice(['sqrt', 'floor', 'ceil', 'round'])
            self.need_import('math')
            a = self.fexpr(t, env, d - 1)
            f = {'sqrt': c_sqrt, 'floor': c_floor, 'ceil': c_ceil, 'round': c_round}[fn]
            return FE(f'Math.{fn}({a.src})', f(a.v), t)
        if c < 0.32:
            # a float from an integer
            it = r.choice(list(INTS))
            a = self.iexpr(it, {}, 1)
            if t == 'Float32':
                v = int_to_f32(a.v)
            else:
                v = float(a.v)
            return FE(f'{t}({a.src})', v, t)
        op = r.choice(['+', '-', '*', '/', '%', '*', '/'])
        a = self.fexpr(t, env, d - 1)
        b = self.fexpr(t, env, d - 1)
        const = a.const and b.const and op != '%'
        return FE(f'({a.src} {op} {b.src})', fop(op, a.v, b.v, t), t, const)

    def fenv(self, code, n=None):
        """Declare a few float bindings; returns the environment."""
        r = self.r
        env = {}
        for _ in range(r.randint(0, 3) if n is None else n):
            t = r.choice(['Float', 'Float', 'Float32'])
            e = self.fexpr(t, env, 1)
            name = self.fresh('f')
            ann = f': {t}' if t == 'Float32' or r.random() < 0.3 else ''
            code.append(f'val {name}{ann} = {e.src}')
            env[name] = (t, e.v)
        return env

    def p_fexpr(self):
        r = self.r
        code, out = [], []
        env = self.fenv(code)
        for _ in range(r.randint(1, 4)):
            parts, texts = [], []
            for _ in range(r.randint(1, 3)):
                k = r.random()
                t = r.choice(['Float', 'Float', 'Float32'])
                if k < 0.25:
                    a, b = self.fexpr(t, env, 1), self.fexpr(t, env, 1)
                    op = r.choice(['==', '!=', '<', '<=', '>', '>='])
                    v = {'==': a.v == b.v, '!=': a.v != b.v, '<': a.v < b.v, '<=': a.v <= b.v,
                         '>': a.v > b.v, '>=': a.v >= b.v}[op]
                    if a.const and b.const:
                        continue
                    parts.append(f'{{{a.src} {op} {b.src}}}')
                    texts.append('true' if v else 'false')
                elif k < 0.35 and t == 'Float':
                    a = self.fexpr(t, env, 2)
                    parts.append(f'{{{a.src}.to_string()}}')
                    texts.append(fshow(a.v, t))
                else:
                    a = self.fexpr(t, env, r.randint(1, 3))
                    if a.const and r.random() < 0.5:
                        continue
                    parts.append(f'{{{a.src}}}')
                    texts.append(fshow(a.v, t))
            if parts:
                tag = self.fresh('t')
                code.append(f'println("{tag} ' + ' '.join(parts) + '")')
                out.append(f'{tag} ' + ' '.join(texts))
        if not out:
            return None
        return Probe(code, out)

    def p_fconv(self):
        """Float to integer conversions of every kind of value."""
        r = self.r
        code, out = [], []
        env = self.fenv(code, r.randint(0, 2))
        parts, texts = [], []
        for _ in range(r.randint(2, 6)):
            t = r.choice(list(INTS))
            ft = r.choice(['Float', 'Float', 'Float32'])
            a = self.fexpr(ft, env, r.randint(0, 2))
            if a.const:
                if not f2i_fits(a.v, t):
                    continue        # an error program (p_bad_conv) does this
                parts.append(f'{{{t}({a.src})}}')
                texts.append(str(int(a.v)))
                continue
            parts.append(f'{{{t}({a.src})}}')
            texts.append(str(f2i(a.v, t)))
        if not parts:
            return None
        tag = self.fresh('c')
        code.append(f'println("{tag} ' + ' '.join(parts) + '")')
        out.append(f'{tag} ' + ' '.join(texts))
        return Probe(code, out)

    def p_floop(self):
        """A float scaled in a loop until it overflows or underflows."""
        r = self.r
        t = r.choice(['Float', 'Float32'])
        n = self.fresh('x')
        start = r.choice([1.0, 3.0, 0.1, 1.5, 7.0])
        k = r.choice([2.0, 10.0, 0.5, 0.1, 1.5, 16.0, 0.0625])
        steps = r.randint(10, 1100)
        op = r.choice(['*', '/'])
        v = f32(start) if t == 'Float32' else start
        kv = f32(k) if t == 'Float32' else k
        code = [f'var {n}: {t} = {t}(pkf({flit(start)}))',
                f'for i in range({steps}) {{',
                f'    {n} = {n} {op} {flit(k)}',
                '}']
        for _ in range(steps):
            v = fop(op, v, kv, t)
        tag = self.fresh('l')
        code.append(f'println("{tag} {{{n}}} {{{n} == 0.0}} {{{t}(1.0) / {n}}}")')
        out = [f'{tag} {fshow(v, t)} {"true" if v == 0 else "false"} '
               f'{fshow(fop("/", f32(1.0) if t == "Float32" else 1.0, v, t), t)}']
        return Probe(code, out)

    def p_ftext(self):
        """Literals printed: the shortest digits that read back."""
        r = self.r
        parts, texts = [], []
        for _ in range(r.randint(2, 6)):
            v = self.fval()
            if r.random() < 0.2:
                v = -v
            t = r.choice(['Float', 'Float32'])
            lit = flit(abs(v))
            src = f'pkf({"-" if v < 0 else ""}{lit})'
            if t == 'Float32':
                src = f'Float32({src})'
                v = f32(v)
            parts.append(f'{{{src}}}')
            texts.append(fshow(v, t))
        tag = self.fresh('x')
        return Probe([f'println("{tag} ' + ' '.join(parts) + '")'], [f'{tag} ' + ' '.join(texts)])

    # ---------------------------------------------------------- integers
    def ival(self, t):
        r = self.r
        lo, hi = lo_hi(t)
        c = r.random()
        if c < 0.45:
            return r.choice([lo, hi, lo + 1, hi - 1, 0, 1, -1 if lo < 0 else 2, hi // 2 + 1])
        if c < 0.7:
            return r.randint(max(lo, -100), min(hi, 100))
        return r.randint(lo, hi)

    def ileaf(self, t, v=None):
        if v is None:
            v = self.ival(t)
        iv = wrapt('Int', v)
        arg = f'pk({iv})' if iv >= 0 else f'pk(0 - {-iv})'
        if iv == -(1 << 63):
            arg = 'pk(0 - 9223372036854775807 - 1)'
        return IE(arg if t == 'Int' else f'{t}({arg})', v, t)

    def iexpr(self, t, env, d):
        r = self.r
        names = [n for n, (tt, v) in env.items() if tt == t]
        if d <= 0 or r.random() < 0.3:
            if names and r.random() < 0.5:
                n = r.choice(names)
                return IE(n, env[n][1], t)
            return self.ileaf(t)
        c = r.random()
        if c < 0.1 and (INTS[t][1] or r.random() < 0.3):
            a = self.iexpr(t, env, d - 1)
            return IE(f'(-{a.src})', wrapt(t, -a.v), t)
        if c < 0.16:
            a = self.iexpr(t, env, d - 1)
            return IE(f'(~{a.src})', wrapt(t, ~a.v), t)
        if c < 0.3:
            return self.ishift(t, env, d)
        if c < 0.42:
            a, b = self.iexpr(t, env, d - 1), self.iexpr(t, env, d - 1)
            if b.v == 0:
                b = IE(f'({b.src} | 1)', b.v | 1, t)
            op = r.choice(['/', '%'])
            return IE(f'({a.src} {op} {b.src})', int_binop(t, op, a.v, b.v), t)
        if c < 0.5:
            # a conversion from another width (wraps)
            s = r.choice(list(INTS))
            a = self.iexpr(s, {}, d - 1)
            return IE(f'{t}({a.src})', wrapt(t, a.v), t)
        op = r.choice(['+', '-', '*', '&', '|', '^', '+', '*'])
        a, b = self.iexpr(t, env, d - 1), self.iexpr(t, env, d - 1)
        return IE(f'({a.src} {op} {b.src})', int_binop(t, op, a.v, b.v), t)

    def count(self, w, negative=False):
        """A shift count of a random integer type: (src, value)."""
        r = self.r
        kt = r.choice(list(INTS))
        lo, hi = lo_hi(kt)
        if negative:
            kt = r.choice([k for k in INTS if INTS[k][1]])
            c = r.choice([-1, -2, -64, -(1 << 31), -100])
            c = max(c, lo_hi(kt)[0])
            return self.ileaf(kt, c).src, c
        cands = [0, 1, 2, w - 1, w, w + 1, 63, 64, 65, 127, 128, 255, 1000, 1 << 31, 1 << 32,
                 (1 << 63) - 1, 1 << 63, (1 << 64) - 1]
        c = r.choice([x for x in cands if x <= hi])
        if r.random() < 0.3 and kt == 'Int' and c < 200:
            return str(c), c          # a literal count (keeps type Int)
        return self.ileaf(kt, c).src, c

    def ishift(self, t, env, d):
        r = self.r
        a = self.iexpr(t, env, d - 1)
        ks, kv = self.count(INTS[t][0])
        op = r.choice(['<<', '>>'])
        return IE(f'({a.src} {op} {ks})', int_binop(t, op, a.v, kv), t)

    def p_iarith(self):
        r = self.r
        code, out = [], []
        env = {}
        for _ in range(r.randint(0, 3)):
            t = r.choice(list(INTS))
            e = self.iexpr(t, env, 1)
            n = self.fresh('n')
            code.append(f'val {n}: {t} = {e.src}')
            env[n] = (t, e.v)
        for _ in range(r.randint(1, 3)):
            parts, texts = [], []
            for _ in range(r.randint(1, 4)):
                t = r.choice(list(INTS))
                e = self.iexpr(t, env, r.randint(1, 3))
                parts.append(f'{{{e.src}}}')
                texts.append(str(e.v))
            tag = self.fresh('i')
            code.append(f'println("{tag} ' + ' '.join(parts) + '")')
            out.append(f'{tag} ' + ' '.join(texts))
        return Probe(code, out)

    def p_icompound(self):
        """Compound assignments at the edges of a width, in a loop."""
        r = self.r
        t = r.choice(list(INTS))
        n = self.fresh('a')
        start = self.ival(t)
        code = [f'var {n}: {t} = {self.ileaf(t, start).src}']
        v = start
        out = []
        for _ in range(r.randint(1, 5)):
            op = r.choice(['+=', '-=', '*=', '/=', '&=', '|=', '^=', '<<=', '>>='])
            if op in ('<<=', '>>='):
                ks, kv = self.count(INTS[t][0])
                code.append(f'{n} {op} {ks}')
                v = int_binop(t, op[:-1], v, kv)
                continue
            b = self.iexpr(t, {}, 1)
            if op == '/=' and b.v == 0:
                b = IE(f'({b.src} | 1)', b.v | 1, t)
            if r.random() < 0.4 and op != '/=':
                k = r.randint(2, 12)
                code += [f'for i in range({k}) {{', f'    {n} {op} {b.src}', '}']
                for _ in range(k):
                    v = int_binop(t, op[:-1], v, b.v)
            else:
                code.append(f'{n} {op} {b.src}')
                v = int_binop(t, op[:-1], v, b.v)
        tag = self.fresh('k')
        code.append(f'println("{tag} {{{n}}}")')
        out.append(f'{tag} {v}')
        return Probe(code, out)

    def p_ibuiltin(self):
        """abs / min / max / clamp on Int at the edges."""
        r = self.r
        parts, texts = [], []
        for _ in range(r.randint(1, 4)):
            a, b, c = self.ileaf('Int'), self.ileaf('Int'), self.ileaf('Int')
            k = r.choice(['abs', 'min', 'max', 'clamp'])
            if k == 'abs':
                parts.append(f'{{abs({a.src})}}')
                texts.append(str(wrapt('Int', abs(a.v))))
            elif k == 'clamp':
                lo, hi = sorted([b.v, c.v])
                bl = b if b.v == lo else c
                bh = c if bl is b else b
                parts.append(f'{{clamp({a.src}, {bl.src}, {bh.src})}}')
                texts.append(str(max(lo, min(hi, a.v))))
            else:
                parts.append(f'{{{k}({a.src}, {b.src})}}')
                texts.append(str(min(a.v, b.v) if k == 'min' else max(a.v, b.v)))
        tag = self.fresh('b')
        return Probe([f'println("{tag} ' + ' '.join(parts) + '")'], [f'{tag} ' + ' '.join(texts)])

    # ------------------------------------------------------------ panics
    def p_divzero(self):
        r = self.r
        t = r.choice(list(INTS))
        a = self.iexpr(t, {}, 1)
        z = self.ileaf(t, 0)
        op = r.choice(['/', '%', '/='])
        tag = self.fresh('z')
        code = [f'println("{tag} before")']
        if op in ('/', '%'):
            code.append(f'println("{{{a.src} {op} {z.src}}}")')
        else:
            n = self.fresh('q')
            code += [f'var {n}: {t} = {a.src}', f'{n} {op} {z.src}', f'println("{{{n}}}")']
        return Probe(code, [f'{tag} before'], panic='integer division by zero')

    def p_negshift(self):
        r = self.r
        t = r.choice(list(INTS))
        a = self.iexpr(t, {}, 1)
        ks, kv = self.count(INTS[t][0], negative=True)
        op = r.choice(['<<', '>>', '<<=', '>>='])
        tag = self.fresh('s')
        code = [f'println("{tag} before")']
        if op in ('<<', '>>'):
            code.append(f'println("{{{a.src} {op} {ks}}}")')
        else:
            n = self.fresh('q')
            code += [f'var {n}: {t} = {a.src}', f'{n} {op} {ks}', f'println("{{{n}}}")']
        return Probe(code, [f'{tag} before'], panic='shift by a negative count')

    # ------------------------------------------------------------ errors
    def p_bad_conv(self):
        """A float constant that does not fit the integer type (E0311)."""
        r = self.r
        t = r.choice(list(INTS))
        lo, hi = lo_hi(t)
        v = r.choice([hi + 1.5, lo - 1.5, 1e300, -1e300, hi * 2.0 + 10]) if INTS[t][0] < 64 else \
            r.choice([1e19 * 2, -1e300, 1e300, 18446744073709551616.0])
        if f2i_fits(v, t):
            return None
        lit = flit(abs(v))
        src = f'(-{lit})' if v < 0 else lit
        if r.random() < 0.3:
            src = f'({src} * 2.0)'
        if r.random() < 0.2:
            src = '(1.0 / 0.0)'
        n = self.fresh('bad')
        return Probe([f'val {n} = {t}({src})', f'println("{{{n}}}")'], errors={'E0311'})

    def p_bad_exp(self):
        n = self.fresh('bad')
        lit = self.r.choice(['1e5', '2.5e3', '1.0e-3', '3E2'])
        return Probe([f'val {n} = {lit}', f'println("{{{n}}}")'], errors={'E0003'})

    def p_bad_mix(self):
        n = self.fresh('bad')
        a, b = ('Float32', 'Float') if self.r.random() < 0.5 else ('Float', 'Float32')
        return Probe([f'val {n}x: {a} = {a}(pkf(1.5))', f'val {n}y: {b} = {b}(pkf(2.5))',
                      f'println("{{{n}x * {n}y}}")'], errors={'E0202'})

    def probe_kinds(self):
        return [(5, self.p_fexpr), (3, self.p_fconv), (1, self.p_floop), (2, self.p_ftext),
                (4, self.p_iarith), (3, self.p_icompound), (1, self.p_ibuiltin)]

    def panic_kinds(self):
        return [self.p_divzero, self.p_negshift]

    def error_kinds(self):
        return [self.p_bad_conv, self.p_bad_conv, self.p_bad_exp, self.p_bad_mix]


def generate(rng):
    g = NumEdgeGen(rng)
    g.program()
    return g
