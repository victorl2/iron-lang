"""Numeric programs: variables of every integer width (Int8 .. Int64,
UInt8 .. UInt64, Int, UInt) and of Float and Float32, mutated in loops and
branches with plain and compound assignments, mixed through conversions
(`Int8(x)`, `Float(n)`, `Int(f)`), shifts by counts up to and past the
width, comparisons, functions with numeric val and var parameters, and
floats printed by interpolation. Some programs instead carry one statement
the manual rejects (mixing widths E0202, Int with Float E0222, a literal
that does not fit E0292 / E0311, a bitwise operator on a float E0233, a
conversion to Bool E0310) and must fail to build with that code.

The model follows manual 1.4, 2.1, 2.9, 3.2 and 3.3:
  - integer arithmetic wraps to the type's width, division truncates
    toward zero, and MIN / -1 wraps to MIN;
  - a shift by the width or more gives 0 (`<<`) or the sign (`>>`);
  - a conversion between integer types wraps (two's complement: the
    manual does not say, this is what C does and what the compiler does);
  - float to integer truncates; only values that fit the target are
    converted (C leaves the rest undefined; the manual is silent);
  - Float is an IEEE double and Float32 an IEEE single, each operation
    rounded on its own (no fused multiply-add);
  - a float prints as the shortest decimal that reads back to the same
    value; the layout (plain digits from 1e-6 to 1e21, an exponent
    outside) is the runtime's, the manual gives only the digits.
"""
import math
import struct

from pf_lang import *
from pf_lang import _cmp
from pf_stmt import Gen, Ctx

INTS = {'Int': (64, True), 'Int8': (8, True), 'Int16': (16, True), 'Int32': (32, True),
        'Int64': (64, True), 'UInt8': (8, False), 'UInt16': (16, False),
        'UInt32': (32, False), 'UInt64': (64, False), 'UInt': (64, False)}
FLOATS = ('Float', 'Float32')
NUMS = tuple(INTS) + FLOATS
# A UInt64 literal above the largest Int (manual 1.4 allows it) is rejected
# with E0292: issue filed, kept out of the programs until it is fixed.
BIG_U64 = False


def wrapt(t, x):
    w, sg = INTS[t]
    x &= (1 << w) - 1
    return x - (1 << w) if sg and x >> (w - 1) else x


def lo_hi(t):
    w, sg = INTS[t]
    return (-(1 << (w - 1)), (1 << (w - 1)) - 1) if sg else (0, (1 << w) - 1)


def f32(x):
    try:
        return struct.unpack('<f', struct.pack('<f', x))[0]
    except OverflowError:
        raise Bail('float32 overflow')


def fround(t, x):
    if not math.isfinite(x) or abs(x) > 1e30:
        raise Bail('float range')
    return f32(x) if t == 'Float32' else x


def fmt_float(v, is_f32=False):
    """iron_fmt_float (src/runtime/iron_fmt.c)."""
    if math.isnan(v):
        return 'NaN'
    if math.isinf(v):
        return 'inf' if v > 0 else '-inf'
    if v == 0.0:
        return '0'
    for p in range(1, (9 if is_f32 else 17) + 1):
        sci = '%.*e' % (p - 1, v)
        back = float(sci)
        if (f32(back) == f32(v)) if is_f32 else back == v:
            break
    neg = sci.startswith('-')
    body = sci[1:] if neg else sci
    mant, e = body.split('e')
    e = int(e)
    digits = mant.replace('.', '')
    digits = digits.rstrip('0') or '0'
    n = len(digits)
    out = '-' if neg else ''
    if -6 <= e < 21:
        if e >= 0:
            out += ''.join(digits[i] if i < n else '0' for i in range(e + 1))
            if n > e + 1:
                out += '.' + digits[e + 1:]
        else:
            out += '0.' + '0' * (-e - 1) + digits
    else:
        out += digits[0] + ('.' + digits[1:] if n > 1 else '')
        out += 'e' + ('-' if e < 0 else '+') + str(abs(e))
    return out


def float_lit_src(v):
    """A float literal (digits '.' digits), parenthesized when negative."""
    s = repr(abs(v))
    if 'e' in s or 'n' in s or '.' not in s:
        raise Bail('float literal form')
    return f'(-{s})' if v < 0 else s


# ------------------------------------------------------------ expressions

class TLit(Expr):
    def __init__(self, v, t):
        self.v, self.t = v, t

    def src(self, parent=0):
        if self.t in FLOATS:
            return float_lit_src(self.v)
        if isinstance(self.v, bool):
            return 'true' if self.v else 'false'
        return str(self.v) if self.v >= 0 else f'({self.v})'

    def ev(self, m, f):
        if self.t == 'Float32':
            return f32(self.v)
        return self.v


def int_binop(t, op, a, b):
    w, sg = INTS[t]
    if op == '+':
        return wrapt(t, a + b)
    if op == '-':
        return wrapt(t, a - b)
    if op == '*':
        return wrapt(t, a * b)
    if op in ('/', '%'):
        if b == 0:
            raise Bail('div0')
        q = tdiv(a, b)
        return wrapt(t, q if op == '/' else a - q * b)
    if op == '&':
        return wrapt(t, a & b)
    if op == '|':
        return wrapt(t, a | b)
    if op == '^':
        return wrapt(t, a ^ b)
    if op in ('<<', '>>'):
        if b < 0:
            raise Bail('negative shift')
        if op == '<<':
            return 0 if b >= w else wrapt(t, a << b)
        if b >= w:
            return -1 if a < 0 else 0
        return a >> b
    raise ValueError(op)


def float_binop(t, op, a, b):
    if op == '+':
        r = a + b
    elif op == '-':
        r = a - b
    elif op == '*':
        r = a * b
    elif op == '/':
        if b == 0:
            raise Bail('fdiv0')
        r = a / b
    elif op == '%':
        if b == 0:
            raise Bail('fmod0')
        r = math.fmod(a, b)
    else:
        raise ValueError(op)
    return fround(t, r)


CMP = ('==', '!=', '<', '>', '<=', '>=')


class TBin(Expr):
    """a op b on operands of type t (the result's type, Bool for a
    comparison). A shift's count may have any integer type."""

    def __init__(self, op, a, b, t):
        self.op, self.a, self.b, self.t = op, a, b, t

    def src(self, parent=0):
        p = PREC[self.op]
        s = f'{self.a.src(p)} {self.op} {self.b.src(p + 1)}'
        return f'({s})' if p < parent else s

    def ev(self, m, f):
        a = self.a.ev(m, f)
        b = self.b.ev(m, f)
        if self.op in CMP:
            return _cmp(self.op, a, b)
        if self.t in FLOATS:
            return float_binop(self.t, self.op, a, b)
        return int_binop(self.t, self.op, a, b)


class TUn(Expr):
    def __init__(self, op, a, t):
        self.op, self.a, self.t = op, a, t

    def src(self, parent=0):
        inner = self.a.src(UNARY)
        if self.op == '-' and inner.startswith('-'):
            inner = f'({inner})'
        s = self.op + inner
        return f'({s})' if parent > UNARY else s

    def ev(self, m, f):
        v = self.a.ev(m, f)
        if self.t in FLOATS:
            return fround(self.t, -v)
        if self.op == '-':
            return wrapt(self.t, -v)
        return wrapt(self.t, ~v)


class TConv(Expr):
    """T(e) from a value of type src."""

    def __init__(self, t, e, srct):
        self.t, self.e, self.srct = t, e, srct

    def src(self, parent=0):
        return f'{self.t}({self.e.src()})'

    def ev(self, m, f):
        v = self.e.ev(m, f)
        if isinstance(v, bool):
            v = int(v)
        if self.t in FLOATS:
            if self.srct in INTS and self.t == 'Float32' and abs(v) >= 1 << 53:
                raise Bail('double rounding')
            return fround(self.t, float(v))
        if self.srct in FLOATS:
            lo, hi = lo_hi(self.t)
            iv = int(v)          # truncates toward zero
            if not lo <= iv <= hi:
                raise Bail('float conversion out of range')
            return iv
        return wrapt(self.t, v)


class FShow(Expr):
    """A numeric value interpolated: its text form."""

    def __init__(self, e, t):
        self.e, self.t = e, t

    def src(self, parent=0):
        return self.e.src(parent)

    def ev(self, m, f):
        v = self.e.ev(m, f)
        if isinstance(v, bool):
            return 'true' if v else 'false'
        if self.t in FLOATS:
            return fmt_float(v, self.t == 'Float32')
        return str(v)


class PkCall(Expr):
    """pk(v) / pkf(v): a value the compiler cannot fold."""

    def __init__(self, v, fl=False):
        self.v, self.fl = v, fl

    def src(self, parent=0):
        if self.fl:
            return f'pkf({float_lit_src(self.v)})'
        return f'pk({self.v})' if self.v >= 0 else f'pk(0 - {-self.v})'

    def ev(self, m, f):
        return self.v


def is_const(e):
    """A constant expression (manual 1.4): folded like a literal, so its
    value must fit its type and it does not wrap."""
    if isinstance(e, (TLit, Lit)):
        return True
    if isinstance(e, Bin):
        return is_const(e.a) and is_const(e.b)
    if isinstance(e, (Un, Paren)):
        return is_const(e.a)
    if isinstance(e, TBin):
        return is_const(e.a) and is_const(e.b)
    if isinstance(e, TUn):
        return is_const(e.a)
    if isinstance(e, TConv):
        return is_const(e.e)
    return False


class TAssign(Stmt):
    def __init__(self, target, op, expr, t, uid):
        self.target, self.op, self.expr, self.t, self.uid = target, op, expr, t, uid

    def lines(self, ind):
        return [f'{ind}{self.target} {self.op} {self.expr.src()}']

    def run(self, m, f):
        c = f.look(self.uid)
        v = self.expr.ev(m, f)
        if self.op == '=':
            c.v = v
        elif self.t in FLOATS:
            c.v = float_binop(self.t, self.op[:-1], c.v, v)
        else:
            c.v = int_binop(self.t, self.op[:-1], c.v, v)


# -------------------------------------------------------------- generator

class NumGen(Gen):
    NFUNCS = (1, 4)
    NMAIN = (8, 22)

    def __init__(self, rng):
        super().__init__(rng)
        r = rng
        ints = [t for t in INTS if t != 'Int']
        self.types = ['Int'] + r.sample(ints, r.randint(2, 5))
        if r.random() < 0.8:
            self.types.append('Float')
        if r.random() < 0.4:
            self.types.append('Float32')
        self.nfuncs = []      # (FuncDef, param types, ret type)
        self.bad = None       # (statement, codes) for an error program

    # ---- typed expressions
    def lit(self, t):
        r = self.r
        if t in FLOATS:
            v = r.choice([0.5, 0.25, 1.5, 2.75, 3.0, 10.0, 0.125, 100.0, 1.75, 0.1,
                          0.3, 2.5, 7.0, 0.2, 1000.5, 0.01])
            if t == 'Float32':
                v = r.choice([0.5, 0.25, 1.5, 2.75, 3.0, 10.0, 0.125, 1.75, 0.1, 2.5])
            return TLit(-v if r.random() < 0.2 else v, t)
        lo, hi = lo_hi(t)
        c = r.random()
        if c < 0.6:
            v = r.randint(max(lo, -20), min(hi, 40))
        elif c < 0.8:
            v = r.choice([lo, hi, lo + 1, hi - 1])
            if v < 0 and v == lo and INTS[t][0] == 64:
                v = lo + 1          # -9223372036854775808 is `-` applied to a literal too large
        else:
            v = r.randint(lo, hi) if INTS[t][0] < 64 else r.randint(-(1 << 40), 1 << 40)
            if not INTS[t][1]:
                v = abs(v)
        if v > (1 << 63) - 1 and not BIG_U64:
            v = (1 << 63) - 1 - self.r.randint(0, 3)
        return TLit(v, t)

    def tleaf(self, t):
        vs = self.names(t)
        if vs and self.r.random() < 0.65:
            return self.ref(self.r.choice(vs))
        return self.lit(t)

    def opaque(self, t):
        """A leaf of type t that is not a constant."""
        vs = self.names(t)
        if vs and self.r.random() < 0.7:
            return self.ref(self.r.choice(vs))
        lit = self.lit(t)
        if t == 'Int':
            return PkCall(lit.v)
        if t == 'Float':
            return PkCall(lit.v, True)
        if t == 'Float32':
            return TConv(t, PkCall(lit.v, True), 'Float')
        return TConv(t, PkCall(wrapt('Int', lit.v)), 'Int')

    def texpr(self, t, d):
        """An expression of numeric type t that is not a constant one
        unless it is a plain literal (a constant expression is folded at
        its own type: it must fit it and does not wrap)."""
        e = self.texpr1(t, d)
        if isinstance(e, TLit) or not is_const(e):
            return e
        if isinstance(e, TBin) and e.op not in ('<<', '>>') and self.r.random() < 0.5:
            e.b = self.opaque(t)
            return e
        if isinstance(e, TBin):
            e.a = self.opaque(t)
            return e
        return self.opaque(t)

    def texpr1(self, t, d):
        if t == 'Int' and self.r.random() < 0.15:
            return self.int_expr(min(d, 2), Ctx(None).sub(pure=True, no_rec=True))
        r = self.r
        if d <= 0 or r.random() < 0.25:
            return self.tleaf(t)
        c = r.random()
        if c < 0.12:
            # a conversion from another numeric type
            src = r.choice(self.types + ['Bool'] * (1 if t not in FLOATS else 0))
            if src == 'Bool':
                return TConv(t, self.bool_expr(1, Ctx(None).sub(pure=True, no_rec=True)), 'Bool')
            if src == t:
                return self.tleaf(t)
            return TConv(t, self.texpr(src, d - 1), src)
        if c < 0.2:
            fs = [(fd, pt) for fd, pt, rt in self.nfuncs if rt == t]
            if fs:
                fd, pt = r.choice(fs)
                return Call(fd, [self.texpr(p, d - 1) for p in pt])
        if c < 0.28 and (t in FLOATS or INTS[t][1] or r.random() < 0.4):
            return TUn('-', self.texpr(t, d - 1), t)
        if t in FLOATS:
            op = r.choice(['+', '-', '*', '/', '+', '*', '%'])
            b = self.texpr(t, d - 1)
            if op in ('/', '%'):
                b = self.lit(t)
                if b.v == 0:
                    b = TLit(2.0, t)
            return TBin(op, self.texpr(t, d - 1), b, t)
        if c < 0.33:
            return TUn('~', self.texpr(t, d - 1), t)
        if c < 0.45:
            # shift; the count is a literal or an Int expression kept >= 0
            w = INTS[t][0]
            if r.random() < 0.6:
                k = TLit(r.choice([0, 1, 2, 3, w - 1, w, w + 1, r.randint(0, w + 3)]), t)
                if not 0 <= k.v <= lo_hi(t)[1]:
                    k = TLit(1, t)
            else:
                kt = r.choice([x for x in self.types if x not in FLOATS])
                mask = 127 if lo_hi(kt)[1] >= 127 else 7
                k = TBin('&', self.texpr(kt, d - 1), TLit(mask, kt), kt)
            a = self.texpr(t, d - 1)
            if is_const(a):
                a = self.opaque(t)      # a literal shifted keeps type Int
            return TBin(r.choice(['<<', '>>']), a, k, t)
        if c < 0.55:
            # division by an operand made odd, so never zero
            b = TBin('|', self.texpr(t, d - 1), TLit(1, t), t)
            if r.random() < 0.5:
                b = self.lit(t)
                if b.v == 0:
                    b = TLit(3, t)
            return TBin(r.choice(['/', '%']), self.texpr(t, d - 1), b, t)
        op = r.choice(['+', '-', '*', '&', '|', '^', '+', '-'])
        return TBin(op, self.texpr(t, d - 1), self.texpr(t, d - 1), t)

    def tcmp(self, d):
        t = self.r.choice(self.types)
        return TBin(self.r.choice(CMP), self.texpr(t, d), self.texpr(t, d), 'Bool')

    def bool_expr(self, d, ctx):
        if self.r.random() < 0.4:
            return self.tcmp(max(d, 1))
        return super().bool_expr(d, ctx)

    def expr_of(self, typ, d, ctx):
        if typ in NUMS and typ != 'Int':
            return self.texpr(typ, d)
        return super().expr_of(typ, d, ctx)

    # ---- statements
    def extra_weights(self, ctx, depth):
        w = [('tdecl', 5), ('tprint', 0 if ctx.pure else 4)]
        if any(self.names(t, True) for t in self.types):
            w.append(('tassign', 8))
        if depth > 0:
            w.append(('tloop', 2))
        if self.nfuncs and not ctx.pure and not ctx.in_defer:
            w.append(('tvcall', 1))
        return w

    def s_tdecl(self, ctx, depth):
        r = self.r
        t = r.choice(self.types)
        name = self.fresh('n')
        mut = r.random() < 0.6
        e = self.texpr(t, 3) if r.random() < 0.8 else self.lit(t)
        if t in ('Int', 'Float') and (isinstance(e, TLit) or r.random() < 0.6):
            ann = None if r.random() < 0.7 else t
        elif self.typed_expr(e) and r.random() < 0.5:
            ann = None
        else:
            ann = t
        uid = self.bind(name, t, mut)
        return [Decl(name, e, mut, ann=ann, uid=uid)]

    def typed_expr(self, e):
        """True when e's type does not come from its context (a literal
        operand takes its partner's type)."""
        if isinstance(e, TLit):
            return False
        if isinstance(e, TBin):
            if e.op in ('<<', '>>'):
                return self.typed_expr(e.a)
            return self.typed_expr(e.a) or self.typed_expr(e.b)
        if isinstance(e, TUn):
            return self.typed_expr(e.a)
        return True

    def s_tassign(self, ctx, depth):
        r = self.r
        cands = [(n, t) for t in self.types for n in self.names(t, True)
                 if not (ctx.pure and n in self.globals)]
        if not cands:
            return [self.print_stmt(ctx)]
        n, t = r.choice(cands)
        if t in FLOATS:
            op = r.choice(['=', '+=', '-=', '*=', '/=', '+='])
        else:
            op = r.choice(['=', '+=', '-=', '*=', '/=', '&=', '|=', '^=', '<<=', '>>=', '+='])
        if op in ('/=',):
            e = self.lit(t)
            if e.v == 0:
                e = TLit(2.0 if t in FLOATS else 3, t)
        elif op in ('<<=', '>>='):
            k = r.randint(0, INTS[t][0] + 2)
            e = TLit(min(k, lo_hi(t)[1]), t)
        else:
            e = self.texpr(t, 2)
        return [TAssign(n, op, e, t, self.uid(n))]

    def s_tprint(self, ctx, depth):
        r = self.r
        parts = [self.next_tag()]
        for _ in range(r.randint(1, 3)):
            parts.append(' ')
            if r.random() < 0.15:
                parts.append(FShow(self.tcmp(1), 'Bool'))
            else:
                t = r.choice(self.types)
                e = self.texpr(t, r.randint(0, 3))
                if isinstance(e, TLit):
                    e = self.opaque(t)      # a literal alone has type Int
                parts.append(FShow(e, t))
        return [Print(Interp(parts))]

    def s_tloop(self, ctx, depth):
        """for i in range(k) { x op= T(i) ... } on a numeric var."""
        r = self.r
        cands = [(n, t) for t in self.types for n in self.names(t, True)
                 if not (ctx.pure and n in self.globals)]
        if not cands:
            return self.s_tdecl(ctx, depth)
        n, t = r.choice(cands)
        i = self.fresh('i')
        lc = ctx.sub(loop=ctx.loop + 1, in_defer=False)
        self.push()
        iu = self.bind(i, 'Int', False)
        iv = Var(i, iu)
        step = TConv(t, iv, 'Int') if t != 'Int' else iv
        op = r.choice(['+=', '-=', '*=', '^=', '+='] if t not in FLOATS else ['+=', '-=', '*='])
        if op == '*=' and t not in FLOATS:
            step = TBin('|', step, TLit(1, t), t)
        if op == '*=' and t in FLOATS:
            step = TLit(r.choice([1.5, 0.5, 2.0, 1.1]), t)
        body = [TAssign(n, op, step, t, self.uid(n))]
        for _ in range(r.randint(0, 2)):
            body.extend(self.stmt(lc, max(depth - 1, 0)))
        self.pop()
        return [ForRange(i, Lit(r.randint(1, 9)), Block(body), uid=iu)]

    def s_tvcall(self, ctx, depth):
        fs = [fd for fd, pt, rt in self.nfuncs if any(p.mut for p in fd.params)]
        if not fs:
            return [self.print_stmt(ctx)]
        fd = self.r.choice(fs)
        args = self.call_args(fd, ctx)
        if args is None:
            return [self.print_stmt(ctx)]
        return [ExprStmt(Call(fd, args))]

    # ---- top level
    def gen_top(self):
        r = self.r
        for i in range(r.randint(1, 4)):
            pt = [r.choice(self.types) for _ in range(r.randint(1, 3))]
            rt = r.choice(self.types)
            mut = r.random() < 0.35
            params = [Param(f'a{j}', t, mut and j == 0) for j, t in enumerate(pt)]
            fd = FuncDef(f'nf{i}', params, None if mut else rt, None)
            fd.rec = False
            ctx = Ctx(fd)
            ctx.pure = not mut
            ctx.no_rec = True
            self.push()
            for p in params:
                self.scopes[-1][p.name] = (p.typ, p.mut, p.name)
            stmts = []
            for _ in range(r.randint(0, 3)):
                stmts.extend(self.stmt(ctx, 1))
            if mut:
                stmts.append(TAssign('a0', '=', self.texpr(pt[0], 2), pt[0], 'a0'))
            else:
                stmts.append(Return(self.texpr(rt, 3)))
            self.pop()
            fd.body = Block(stmts)
            self.funcs.append((fd, {'pure': not mut, 'varp': mut, 'rec': False}))
            if not mut:
                self.nfuncs.append((fd, pt, rt))
            else:
                self.nfuncs.append((fd, pt, None))

    def param_types(self):
        return ['Int', 'Int', 'Bool'] + [e.name for e in self.enums]

    # ---- error programs
    def bad_stmt(self):
        """Lines the manual rejects, and the codes it gives for them."""
        r = self.r
        k = r.choice(['mixw', 'intfloat', 'litfit', 'convfit', 'bitfloat', 'boolconv',
                      'mixf', 'compound', 'mixcmp', 'widen'])
        x, y, z = self.fresh('bad'), self.fresh('bad'), self.fresh('bad')
        if k == 'mixcmp':
            a, b = r.sample([t for t in INTS if t != 'Int64'], 2)
            if {a, b} == {'UInt', 'UInt64'}:
                b = 'Int32'
            op = r.choice(['==', '!=', '<', '>='])
            return [f'val {x}: {a} = 1', f'val {y}: {b} = 2', f'println("{{{x} {op} {y}}}")'], {'E0202'}
        if k == 'widen':
            a, b = r.sample(['Int', 'Int32', 'Int8', 'UInt8', 'Int16', 'UInt32'], 2)
            return [f'val {x}: {a} = 1', f'val {y}: {b} = {x}', f'println("{{{y}}}")'], {'E0202'}
        if k in ('mixw', 'compound'):
            a, b = r.sample([t for t in INTS if t != 'Int64'], 2)
            if {a, b} == {'UInt', 'UInt64'}:
                b = 'Int8'
            if k == 'mixw':
                op = r.choice(['+', '-', '*', '&', '|', '/'])
                return [f'val {x}: {a} = 1', f'val {y}: {b} = 2', f'val {z} = {x} {op} {y}',
                        f'println("{{{z}}}")'], {'E0202'}
            op = r.choice(['+=', '-=', '*=', '&='])
            return [f'var {x}: {a} = 1', f'val {y}: {b} = 2', f'{x} {op} {y}',
                    f'println("{{{x}}}")'], {'E0202'}
        if k == 'intfloat':
            op = r.choice(['+', '-', '*', '/'])
            a, b = ('Int', 'Float') if r.random() < 0.5 else ('Float', 'Int')
            va = '3' if a == 'Int' else '3.5'
            vb = '2' if b == 'Int' else '2.5'
            return [f'val {x}: {a} = {va}', f'val {y}: {b} = {vb}', f'val {z} = {x} {op} {y}',
                    f'println("{{{z}}}")'], {'E0222'}
        if k == 'litfit':
            t = r.choice([q for q in INTS if INTS[q][0] < 64])
            lo, hi = lo_hi(t)
            v = r.choice([hi + 1, hi + r.randint(2, 1000), lo - 1])
            if r.random() < 0.5:
                return [f'val {x}: {t} = {v}', f'println("{{{x}}}")'], {'E0292'}
            return [f'var {x}: {t} = 1', f'{x} += {v}' if v >= 0 else f'{x} += ({v})',
                    f'println("{{{x}}}")'], {'E0292'}
        if k == 'convfit':
            t = r.choice([q for q in INTS if INTS[q][0] < 64])
            lo, hi = lo_hi(t)
            return [f'val {x} = {t}({hi + r.randint(1, 50)})', f'println("{{{x}}}")'], {'E0311'}
        if k == 'bitfloat':
            op = r.choice(['&', '|', '^'])
            return [f'val {x} = 2.5', f'val {y} = 1.5', f'val {z} = {x} {op} {y}',
                    f'println("{{{z}}}")'], {'E0233'}
        if k == 'boolconv':
            return [f'val {x} = {r.randint(0, 3)}', f'val {y} = Bool({x})',
                    f'println("{{{y}}}")'], {'E0310'}
        a, b = ('Float32', 'Float') if r.random() < 0.5 else ('Float', 'Float32')
        return [f'val {x}: {a} = 1.5', f'val {y}: {b} = 2.5', f'val {z} = {x} + {y}',
                f'println("{{{z}}}")'], {'E0202'}

    def top_lines(self):
        return ['func pk(n: Int) -> Int {', '    return n', '}', '',
                'func pkf(x: Float) -> Float {', '    return x', '}', '']

    def program(self):
        super().program()
        if self.r.random() < 0.12:
            lines, codes = self.bad_stmt()
            pos = self.r.randint(0, len(self.mainblock.stmts))
            self.mainblock.stmts.insert(pos, RawStmt(lines))
            self.bad = codes

    def model(self):
        if self.bad:
            return Expected([], errors=self.bad)
        return Expected(super().model())


class RawStmt(Stmt):
    """Source lines the model does not run."""

    def __init__(self, text):
        self.text = text

    def lines(self, ind):
        return [ind + t for t in self.text]

    def run(self, m, f):
        pass


def generate(rng):
    g = NumGen(rng)
    g.program()
    return g
