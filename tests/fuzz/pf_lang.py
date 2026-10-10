"""A small model of Iron used by the program fuzzers (prog_fuzz.py).

Each generator builds a program as a tree of the node classes below. Every
node can print itself as Iron source (`src`) and run itself in Python with
the semantics the manual (docs/language_definition.md) gives (`ev` for
expressions, `run` for statements), so a generated program carries its own
expected output: the lines its `println` calls produce.

Only constructs the generators emit are modelled, and each one is modelled
from the manual, not from the compiler.
"""

MASK = (1 << 64) - 1


def wrap(x):
    x &= MASK
    return x - (1 << 64) if x >> 63 else x


def tdiv(a, b):
    q = abs(a) // abs(b)
    return q if (a >= 0) == (b >= 0) else -q


class Bail(Exception):
    """The model hit something the generator should have prevented
    (step budget, a case the manual leaves open): drop the program."""


# ---------------------------------------------------------------- values

class EnumV:
    __slots__ = ('enum', 'variant', 'payload')

    def __init__(self, enum, variant, payload):
        self.enum, self.variant, self.payload = enum, variant, tuple(payload)

    def __eq__(self, o):
        return (isinstance(o, EnumV) and self.enum is o.enum and
                self.variant == o.variant and self.payload == o.payload)

    def __hash__(self):
        return hash((self.variant, self.payload))


class ListV:
    __slots__ = ('items',)

    def __init__(self, items):
        self.items = items


class MapV:
    __slots__ = ('d',)

    def __init__(self, d=None):
        self.d = {} if d is None else d


class SetV:
    __slots__ = ('s',)

    def __init__(self, s=None):
        self.s = set() if s is None else s


class Cell:
    __slots__ = ('v',)

    def __init__(self, v):
        self.v = v


def fmt(v):
    if isinstance(v, bool):
        return 'true' if v else 'false'
    if isinstance(v, int):
        return str(v)
    if isinstance(v, str):
        return v
    if isinstance(v, EnumV):
        return v.variant
    if v is None:
        return 'null'
    if hasattr(v, 'iron_text'):
        return v.iron_text()
    raise Bail(f'no text form for {v!r}')


def iron_str(s):
    """An Iron string literal for the Python string s."""
    out = []
    for ch in s:
        if ch == '\\':
            out.append('\\\\')
        elif ch == '"':
            out.append('\\"')
        elif ch == '{':
            out.append('\\{')
        elif ch == '}':
            out.append('\\}')
        elif ch == '\n':
            out.append('\\n')
        elif ch == '\t':
            out.append('\\t')
        else:
            out.append(ch)
    return '"' + ''.join(out) + '"'


# ------------------------------------------------------------ run state

class Frame:
    """Run-time scope chain: one dict of name -> Cell per block."""

    def __init__(self, parent=None, func=None):
        self.vars = {}
        self.parent = parent
        self.func = func

    def look(self, name):
        f = self
        while f is not None:
            c = f.vars.get(name)
            if c is not None:
                return c
            f = f.parent
        raise Bail(f'unbound {name}')


class Machine:
    def __init__(self, globals_frame=None, budget=400000, max_out=4000):
        self.out = []
        self.steps = 0
        self.budget = budget
        self.max_out = max_out
        self.globals = globals_frame or Frame()

    def tick(self, n=1):
        self.steps += n
        if self.steps > self.budget:
            raise Bail('step budget')

    partial = ''

    def emit(self, line):
        self.out.append(self.partial + line)
        self.partial = ''
        if len(self.out) > self.max_out:
            raise Bail('too much output')

    def emit_part(self, text):
        self.partial += text
        if len(self.partial) > 20000:
            raise Bail('long line')

    def lines(self):
        return self.out + ([self.partial] if self.partial else [])


class Closure:
    """A lambda value: parameters, body and the frame it was created in.
    Holding the frame (not a snapshot) models both capture rules of manual
    3.7: a captured `val` cannot change, and a captured `var` is a cell
    shared with the enclosing function."""

    def __init__(self, lam, frame):
        self.lam, self.frame = lam, frame

    def call(self, m, vals):
        m.tick()
        fr = Frame(self.frame, self.lam)
        fr.defers = []
        fr.drops = []
        for (n, t, u), v in zip(self.lam.params, vals):
            fr.vars[u] = Cell(v)
        try:
            try:
                for s in self.lam.body.stmts:
                    m.tick()
                    s.run(m, fr)
            finally:
                exit_frame(m, fr)
        except ReturnX as r:
            return r.v
        return None


class BreakX(Exception):
    pass


class ContinueX(Exception):
    pass


class ReturnX(Exception):
    def __init__(self, v):
        self.v = v


# ----------------------------------------------------------- expressions

PREC = {'or': 2, 'and': 3, '|': 4, '^': 5, '&': 6, '==': 7, '!=': 7,
        '<': 8, '>': 8, '<=': 8, '>=': 8, '<<': 9, '>>': 9,
        '+': 10, '-': 10, '*': 11, '/': 11, '%': 11}
UNARY = 12
POSTFIX = 13


class Expr:
    def src(self, parent=0):
        raise NotImplementedError

    def ev(self, m, f):
        raise NotImplementedError


class Lit(Expr):
    def __init__(self, v):
        self.v = v

    def src(self, parent=0):
        v = self.v
        if isinstance(v, bool):
            return 'true' if v else 'false'
        if isinstance(v, int):
            return str(v) if v >= 0 else f'({v})'
        if isinstance(v, str):
            return iron_str(v)
        raise ValueError(v)

    def ev(self, m, f):
        return self.v


class Var(Expr):
    """A binding reference. uid identifies the binding lexically, so a
    later shadowing declaration does not change what this reads."""

    def __init__(self, name, uid=None):
        self.name = name
        self.uid = uid or name

    def src(self, parent=0):
        return self.name

    def ev(self, m, f):
        return f.look(self.uid).v


class Raw(Expr):
    """Source text with a Python evaluator, for one-off forms."""

    def __init__(self, text, fn, prec=POSTFIX):
        self.text, self.fn, self.prec = text, fn, prec

    def src(self, parent=0):
        return f'({self.text})' if self.prec < parent else self.text

    def ev(self, m, f):
        return self.fn(m, f)


def _cmp(op, a, b):
    if op == '==':
        return a == b
    if op == '!=':
        return a != b
    if isinstance(a, str):
        # code point order
        pass
    return {'<': a < b, '>': a > b, '<=': a <= b, '>=': a >= b}[op]


class Bin(Expr):
    def __init__(self, op, a, b):
        self.op, self.a, self.b = op, a, b

    def src(self, parent=0):
        p = PREC[self.op]
        s = f'{self.a.src(p)} {self.op} {self.b.src(p + 1)}'
        return f'({s})' if p < parent else s

    def ev(self, m, f):
        op = self.op
        if op == 'and':
            return bool(self.a.ev(m, f)) and bool(self.b.ev(m, f))
        if op == 'or':
            return bool(self.a.ev(m, f)) or bool(self.b.ev(m, f))
        a = self.a.ev(m, f)
        b = self.b.ev(m, f)
        if op in ('==', '!=', '<', '>', '<=', '>='):
            return _cmp(op, a, b)
        if isinstance(a, str):
            assert op == '+'
            return a + b
        if op == '+':
            return wrap(a + b)
        if op == '-':
            return wrap(a - b)
        if op == '*':
            return wrap(a * b)
        if op == '/':
            if b == 0:
                raise Bail('div0')
            return wrap(tdiv(a, b))
        if op == '%':
            if b == 0:
                raise Bail('div0')
            return wrap(a - tdiv(a, b) * b)
        if op == '&':
            return a & b
        if op == '|':
            return a | b
        if op == '^':
            return a ^ b
        if op == '<<':
            return wrap(a << b) if b < 64 else 0
        if op == '>>':
            return a >> b if b < 64 else (0 if a >= 0 else -1)
        raise ValueError(op)


class Un(Expr):
    def __init__(self, op, a):
        self.op, self.a = op, a

    def src(self, parent=0):
        inner = self.a.src(UNARY)
        if self.op == '-' and inner.startswith('-'):
            inner = f'({inner})'
        s = ('not ' + inner) if self.op == 'not' else self.op + inner
        return f'({s})' if parent > UNARY else s

    def ev(self, m, f):
        v = self.a.ev(m, f)
        if self.op == 'not':
            return not v
        if self.op == '-':
            return wrap(-v)
        if self.op == '~':
            return wrap(~v)
        raise ValueError(self.op)


class Paren(Expr):
    def __init__(self, a):
        self.a = a

    def src(self, parent=0):
        return f'({self.a.src()})'

    def ev(self, m, f):
        return self.a.ev(m, f)


class Interp(Expr):
    """An interpolated string literal: parts are str or Expr."""

    def __init__(self, parts):
        self.parts = parts

    def src(self, parent=0):
        out = []
        for p in self.parts:
            if isinstance(p, str):
                out.append(iron_str(p)[1:-1])
            else:
                out.append('{' + p.src() + '}')
        return '"' + ''.join(out) + '"'

    def ev(self, m, f):
        return ''.join(p if isinstance(p, str) else fmt(p.ev(m, f))
                       for p in self.parts)


class Call(Expr):
    """Call of a generated top-level function (a FuncDef)."""

    def __init__(self, fn, args):
        self.fn, self.args = fn, args

    def src(self, parent=0):
        return f'{self.fn.name}({", ".join(a.src() for a in self.args)})'

    def ev(self, m, f):
        return self.fn.call(m, f, self.args)


class EnumCon(Expr):
    def __init__(self, enum, variant, args):
        self.enum, self.variant, self.args = enum, variant, args

    def src(self, parent=0):
        s = f'{self.enum.name}.{self.variant}'
        if self.args or self.enum.arity(self.variant):
            s += '(' + ', '.join(a.src() for a in self.args) + ')'
        return s

    def ev(self, m, f):
        return EnumV(self.enum, self.variant, [a.ev(m, f) for a in self.args])


class MethodCall(Expr):
    """recv.name(args) with a Python implementation fn(m, f, recv_value,
    arg_values, recv_cell)."""

    def __init__(self, recv, name, args, fn):
        self.recv, self.name, self.args, self.fn = recv, name, args, fn

    def src(self, parent=0):
        return f'{self.recv.src(POSTFIX)}.{self.name}({", ".join(a.src() for a in self.args)})'

    def ev(self, m, f):
        rv = self.recv.ev(m, f)
        avs = [a.ev(m, f) for a in self.args]
        return self.fn(m, f, rv, avs)


class Builtin(Expr):
    """name(args) for a built-in function, with a Python implementation."""

    def __init__(self, name, args, fn):
        self.name, self.args, self.fn = name, args, fn

    def src(self, parent=0):
        return f'{self.name}({", ".join(a.src() for a in self.args)})'

    def ev(self, m, f):
        return self.fn(*[a.ev(m, f) for a in self.args])


class Lambda(Expr):
    """func(params) [-> R] { body }; params are (name, type, uid)."""

    def __init__(self, params, ret, body, ind='    '):
        self.params, self.ret, self.body, self.ind = params, ret, body, ind

    def src(self, parent=0):
        ps = ', '.join(f'{n}: {t}' for n, t, _ in self.params)
        r = f' -> {self.ret}' if self.ret else ''
        inner = self.body.lines(self.ind + '    ')
        if not inner:
            return f'func({ps}){r} {{ }}'
        return f'func({ps}){r} {{\n' + '\n'.join(inner) + f'\n{self.ind}}}'

    def ev(self, m, f):
        return Closure(self, f)


class CallValue(Expr):
    """Call of a function value: f(args), fs[i](args), make()(args)."""

    def __init__(self, callee, args):
        self.callee, self.args = callee, args

    def src(self, parent=0):
        return f'{self.callee.src(POSTFIX)}({", ".join(a.src() for a in self.args)})'

    def ev(self, m, f):
        c = self.callee.ev(m, f)
        vals = [a.ev(m, f) for a in self.args]
        return c.call(m, vals)


class Index(Expr):
    def __init__(self, a, i):
        self.a, self.i = a, i

    def src(self, parent=0):
        return f'{self.a.src(POSTFIX)}[{self.i.src()}]'

    def ev(self, m, f):
        a = self.a.ev(m, f)
        i = self.i.ev(m, f)
        if isinstance(a, str):
            return a[i] if 0 <= i < len(a) else ''
        if not 0 <= i < len(a.items):
            raise Bail('index out of range')
        return a.items[i]


class Slice(Expr):
    def __init__(self, a, lo, hi):
        self.a, self.lo, self.hi = a, lo, hi

    def src(self, parent=0):
        hi = self.hi.src() if self.hi is not None else ''
        return f'{self.a.src(POSTFIX)}[{self.lo.src()}..{hi}]'

    def ev(self, m, f):
        a = self.a.ev(m, f)
        lo = self.lo.ev(m, f)
        if isinstance(a, str):
            n = len(a)
            hi = self.hi.ev(m, f) if self.hi is not None else n
            lo, hi = max(0, min(lo, n)), max(0, min(hi, n))
            return a[lo:hi] if lo < hi else ''
        n = len(a.items)
        hi = self.hi.ev(m, f) if self.hi is not None else n
        if not (0 <= lo <= hi <= n):
            raise Bail('list slice out of range')
        return ListV([copy_value(x) for x in a.items[lo:hi]])


def copy_value(v):
    """Deep copy as `copy()` does for collections (manual 6.8, 9.10)."""
    if isinstance(v, ListV):
        return ListV([copy_value(x) for x in v.items])
    if isinstance(v, MapV):
        return MapV({k: copy_value(x) for k, x in v.d.items()})
    if isinstance(v, SetV):
        return SetV(set(v.s))
    if hasattr(v, 'iron_copy'):
        return v.iron_copy()
    return v


# ------------------------------------------------------------ statements

class Stmt:
    def lines(self, ind):
        raise NotImplementedError

    def run(self, m, f):
        raise NotImplementedError


def block_lines(stmts, ind):
    out = []
    for s in stmts:
        out.extend(s.lines(ind))
    return out


class Block:
    """A list of statements run in a fresh scope, with defers."""

    def __init__(self, stmts=None):
        self.stmts = stmts if stmts is not None else []

    def lines(self, ind):
        return block_lines(self.stmts, ind)

    def run(self, m, f, pre=None):
        fr = Frame(f, f.func)
        if pre:
            for k, v in pre.items():
                fr.vars[k] = Cell(v)
        fr.defers = []
        fr.drops = []
        try:
            for s in self.stmts:
                m.tick()
                s.run(m, fr)
        finally:
            exit_frame(m, fr)


def exit_frame(m, fr):
    """Block exit: defers in reverse registration order, then the values
    the block owns in reverse declaration order (manual 4.9, 6.1)."""
    for d in reversed(fr.defers):
        d.run(m, fr)
    for hook in reversed(fr.drops):
        hook(m)


class Decl(Stmt):
    def __init__(self, name, expr, mut=False, ann=None, uid=None):
        self.name, self.expr, self.mut, self.ann = name, expr, mut, ann
        self.uid = uid or name

    def lines(self, ind):
        kw = 'var' if self.mut else 'val'
        ann = f': {self.ann}' if self.ann else ''
        return [f'{ind}{kw} {self.name}{ann} = {self.expr.src()}']

    def run(self, m, f):
        f.vars[self.uid] = Cell(self.expr.ev(m, f))


class Assign(Stmt):
    def __init__(self, target, op, expr, uid=None):
        self.target, self.op, self.expr = target, op, expr
        self.uid = uid or target

    def lines(self, ind):
        return [f'{ind}{self.target} {self.op} {self.expr.src()}']

    def run(self, m, f):
        c = f.look(self.uid)
        v = self.expr.ev(m, f)
        if self.op == '=':
            c.v = v
        else:
            c.v = Bin(self.op[:-1], Lit(c.v), Lit(v)).ev(m, f)


class IndexAssign(Stmt):
    """xs[i] op= e on a list."""

    def __init__(self, target, op, expr):
        self.target, self.op, self.expr = target, op, expr

    def lines(self, ind):
        return [f'{ind}{self.target.src()} {self.op} {self.expr.src()}']

    def run(self, m, f):
        lst = self.target.a.ev(m, f)
        i = self.target.i.ev(m, f)
        v = self.expr.ev(m, f)
        if not 0 <= i < len(lst.items):
            raise Bail('index out of range')
        if self.op == '=':
            lst.items[i] = v
        else:
            lst.items[i] = Bin(self.op[:-1], Lit(lst.items[i]), Lit(v)).ev(m, f)


class Print(Stmt):
    def __init__(self, interp):
        self.e = interp

    def lines(self, ind):
        return [f'{ind}println({self.e.src()})']

    def run(self, m, f):
        m.emit(self.e.ev(m, f))


class PrintPart(Stmt):
    """print(...) without a newline."""

    def __init__(self, interp):
        self.e = interp

    def lines(self, ind):
        return [f'{ind}print({self.e.src()})']

    def run(self, m, f):
        m.emit_part(self.e.ev(m, f))


class ExprStmt(Stmt):
    def __init__(self, e):
        self.e = e

    def lines(self, ind):
        return [f'{ind}{self.e.src()}']

    def run(self, m, f):
        self.e.ev(m, f)


class If(Stmt):
    def __init__(self, arms, els=None):
        self.arms, self.els = arms, els

    def lines(self, ind):
        out = []
        for i, (c, b) in enumerate(self.arms):
            kw = 'if' if i == 0 else '} elif'
            out.append(f'{ind}{kw} {c.src()} {{' if i == 0 else f'{ind}}} elif {c.src()} {{')
            out.extend(b.lines(ind + '    '))
        if self.els is not None:
            out.append(f'{ind}}} else {{')
            out.extend(self.els.lines(ind + '    '))
        out.append(f'{ind}}}')
        return out

    def run(self, m, f):
        for c, b in self.arms:
            if c.ev(m, f):
                b.run(m, f)
                return
        if self.els is not None:
            self.els.run(m, f)


class While(Stmt):
    """while cond { body }; the generator guarantees termination."""

    def __init__(self, cond, body):
        self.cond, self.body = cond, body

    def lines(self, ind):
        return [f'{ind}while {self.cond.src()} {{'] + \
            self.body.lines(ind + '    ') + [f'{ind}}}']

    def run(self, m, f):
        while self.cond.ev(m, f):
            m.tick()
            try:
                self.body.run(m, f)
            except BreakX:
                break
            except ContinueX:
                continue


class ForRange(Stmt):
    def __init__(self, name, n, body, uid=None):
        self.name, self.n, self.body = name, n, body
        self.uid = uid or name

    def lines(self, ind):
        return [f'{ind}for {self.name} in range({self.n.src()}) {{'] + \
            self.body.lines(ind + '    ') + [f'{ind}}}']

    def run(self, m, f):
        n = self.n.ev(m, f)
        for i in range(max(0, n)):
            m.tick()
            try:
                self.body.run(m, f, {self.uid: i})
            except BreakX:
                break
            except ContinueX:
                continue


class ForIn(Stmt):
    """for x in <list / string expression>."""

    def __init__(self, name, it, body, uid=None):
        self.name, self.it, self.body = name, it, body
        self.uid = uid or name

    def lines(self, ind):
        return [f'{ind}for {self.name} in {self.it.src()} {{'] + \
            self.body.lines(ind + '    ') + [f'{ind}}}']

    def run(self, m, f):
        v = self.it.ev(m, f)
        items = list(v) if isinstance(v, str) else [copy_value(x) for x in v.items]
        for x in items:
            m.tick()
            try:
                self.body.run(m, f, {self.uid: x})
            except BreakX:
                break
            except ContinueX:
                continue


class Break(Stmt):
    def lines(self, ind):
        return [f'{ind}break']

    def run(self, m, f):
        raise BreakX()


class Continue(Stmt):
    def lines(self, ind):
        return [f'{ind}continue']

    def run(self, m, f):
        raise ContinueX()


class Return(Stmt):
    def __init__(self, e=None):
        self.e = e

    def lines(self, ind):
        return [f'{ind}return' + (f' {self.e.src()}' if self.e is not None else '')]

    def run(self, m, f):
        raise ReturnX(self.e.ev(m, f) if self.e is not None else None)


class BareBlock(Stmt):
    def __init__(self, body):
        self.body = body

    def lines(self, ind):
        return [f'{ind}{{'] + self.body.lines(ind + '    ') + [f'{ind}}}']

    def run(self, m, f):
        self.body.run(m, f)


class Defer(Stmt):
    """defer stmt / defer { block }: runs at exit of the enclosing block."""

    def __init__(self, body, single=False):
        self.body, self.single = body, single

    def lines(self, ind):
        if self.single:
            ls = self.body.lines('')
            assert len(ls) == 1
            return [f'{ind}defer {ls[0]}']
        return [f'{ind}defer {{'] + self.body.lines(ind + '    ') + [f'{ind}}}']

    def run(self, m, f):
        f.defers.append(_DeferRun(self.body))


class _DeferRun:
    def __init__(self, body):
        self.body = body

    def run(self, m, f):
        if isinstance(self.body, Block):
            self.body.run(m, f)
        else:
            Block([self.body]).run(m, f)


class MatchInt(Stmt):
    """match e { k -> body ... else -> body }; arms: (pattern_src, value, Block)."""

    def __init__(self, e, arms, els):
        self.e, self.arms, self.els = e, arms, els

    def lines(self, ind):
        out = [f'{ind}match {self.e.src()} {{']
        for psrc, _, b in self.arms:
            out.extend(arm_lines(psrc, b, ind + '    '))
        out.extend(arm_lines('else', self.els, ind + '    '))
        out.append(f'{ind}}}')
        return out

    def run(self, m, f):
        v = self.e.ev(m, f)
        for _, k, b in self.arms:
            if v == k:
                b.run(m, f)
                return
        self.els.run(m, f)


def arm_lines(pat, b, ind):
    # A one-statement arm is written without braces when that statement is
    # a single line, otherwise as a block.
    if getattr(b, 'single', False) and len(b.stmts) == 1:
        ls = b.stmts[0].lines('')
        if len(ls) == 1:
            return [f'{ind}{pat} -> {ls[0]}']
    return [f'{ind}{pat} -> {{'] + b.lines(ind + '    ') + [f'{ind}}}']


class MatchEnum(Stmt):
    """arms: (variant, [binding names or '_'], Block); els may be None."""

    def __init__(self, e, enum, arms, els, qualify=True):
        self.e, self.enum, self.arms, self.els, self.qualify = e, enum, arms, els, qualify

    def lines(self, ind):
        out = [f'{ind}match {self.e.src()} {{']
        for var, binds, b in self.arms:
            p = f'{self.enum.name}.{var}' if self.qualify else var
            if self.enum.arity(var):
                p += '(' + ', '.join(n for n, _ in binds) + ')'
            out.extend(arm_lines(p, b, ind + '    '))
        if self.els is not None:
            out.extend(arm_lines('else', self.els, ind + '    '))
        out.append(f'{ind}}}')
        return out

    def run(self, m, f):
        v = self.e.ev(m, f)
        for var, binds, b in self.arms:
            if v.variant == var:
                pre = {u: x for (n, u), x in zip(binds, v.payload) if n != '_'}
                b.run(m, f, pre)
                return
        if self.els is not None:
            self.els.run(m, f)


# ------------------------------------------------------------ declarations

class EnumDef:
    def __init__(self, name, variants):
        # variants: list of (name, [payload type names], explicit value or None)
        self.name, self.variants = name, variants

    def arity(self, v):
        for n, ps, _ in self.variants:
            if n == v:
                return len(ps)
        raise KeyError(v)

    def payload_types(self, v):
        for n, ps, _ in self.variants:
            if n == v:
                return ps
        raise KeyError(v)

    def lines(self):
        out = [f'enum {self.name} {{']
        for n, ps, val in self.variants:
            if ps:
                out.append(f'    {n}({", ".join(ps)}),')
            elif val is not None:
                out.append(f'    {n} = {val},')
            else:
                out.append(f'    {n},')
        out.append('}')
        return out


class Param:
    def __init__(self, name, typ, mut=False):
        self.name, self.typ, self.mut = name, typ, mut


class FuncDef:
    def __init__(self, name, params, ret, body):
        self.name, self.params, self.ret, self.body = name, params, ret, body

    def lines(self):
        ps = ', '.join(('var ' if p.mut else '') + f'{p.name}: {p.typ}'
                       for p in self.params)
        r = f' -> {self.ret}' if self.ret else ''
        return [f'func {self.name}({ps}){r} {{'] + self.body.lines('    ') + ['}']

    def call(self, m, f, args):
        vals = [a.ev(m, f) for a in args]
        m.tick()
        root = Frame(m.globals, self)
        root.defers = []
        root.drops = []
        cells = {}
        for p, v in zip(self.params, vals):
            c = Cell(copy_param(v, p))
            root.vars[p.name] = c
            cells[p.name] = c
        ret = None
        try:
            try:
                for s in self.body.stmts:
                    m.tick()
                    s.run(m, root)
            finally:
                exit_frame(m, root)
        except ReturnX as r:
            ret = r.v
        # var parameters write their final value back (manual 5.2)
        for p, a in zip(self.params, args):
            if p.mut and not p.typ.startswith('['):
                f.look(a.uid).v = cells[p.name].v
        return ret


def copy_param(v, p):
    return v
