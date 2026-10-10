"""Closure programs: lambdas capturing `val` and `var` bindings (manual 3.7),
closures returned from functions (counters, adders, compose), stored in
bindings, lists and object fields and called later, recursive lambdas
through a `var`, closures sharing an `rc [Int]`, and closures created in
loops, mixed with the control flow of pf_stmt.

The model holds the defining frame in each closure (pf_lang.Closure), which
is the manual's rule: a captured `val` cannot change, and a captured `var`
is one cell shared by the frame and every closure that captured it."""
from pf_lang import *
from pf_stmt import Gen, Ctx

F1 = 'func(Int) -> Int'
F0 = 'func() -> Int'
F2 = 'func(Int, Int) -> Int'
LF1 = '[func(Int) -> Int]'
RCL = 'rc [Int]'


class LamFn:
    """Stands in for a FuncDef while a lambda body is generated."""

    def __init__(self, ret):
        self.ret = ret
        self.rec = False


HELPERS = '''func make_adder(n: Int) -> func(Int) -> Int {
    return func(x: Int) -> Int { return x + n }
}

func make_counter(start: Int) -> func() -> Int {
    var c = start
    return func() -> Int {
        c += 1
        return c
    }
}

func make_scaled(k: Int) -> func(Int) -> Int {
    val base = k * 2
    var calls = 0
    return func(x: Int) -> Int {
        calls += 1
        return x * base + calls
    }
}

func compose(f: func(Int) -> Int, g: func(Int) -> Int) -> func(Int) -> Int {
    return func(x: Int) -> Int { return f(g(x)) }
}

func apply(f: func(Int) -> Int, x: Int) -> Int {
    return f(x)
}

func twice(f: func(Int) -> Int, x: Int) -> Int {
    return f(f(x))
}

object Btn {
    val id: Int
    val on: func(Int) -> Int
}
'''


def _adder(n):
    lam = Lambda([('x', 'Int', 'x')], 'Int', Block([Return(Bin('+', Var('x'), Var('n')))]))
    fr = Frame()
    fr.vars['n'] = Cell(n)
    return Closure(lam, fr)


def _counter(start):
    lam = Lambda([], 'Int', Block([Assign('c', '+=', Lit(1)), Return(Var('c'))]))
    fr = Frame()
    fr.vars['c'] = Cell(start)
    return Closure(lam, fr)


def _scaled(k):
    lam = Lambda([('x', 'Int', 'x')], 'Int',
                 Block([Assign('calls', '+=', Lit(1)),
                        Return(Bin('+', Bin('*', Var('x'), Var('base')), Var('calls')))]))
    fr = Frame()
    fr.vars['base'] = Cell(wrap(k * 2))
    fr.vars['calls'] = Cell(0)
    return Closure(lam, fr)


class PyFn:
    """A closure-like value implemented in Python (compose)."""

    def __init__(self, fn):
        self.fn = fn

    def call(self, m, vals):
        m.tick()
        return self.fn(m, vals)


def _compose(f, g):
    return PyFn(lambda m, vals: f.call(m, [g.call(m, vals)]))


class BtnV:
    def __init__(self, id_, on):
        self.id, self.on = id_, on


class ClosureGen(Gen):
    NFUNCS = (0, 3)
    NMAIN = (10, 26)

    def __init__(self, rng):
        super().__init__(rng)
        self.no_lists = 0
        self.lam_depth = 0

    def top_lines(self):
        return HELPERS.splitlines() + ['']

    def names(self, typ, mut=None):
        if self.no_lists and typ.startswith('['):
            return []
        ns = super().names(typ, mut)
        if self.lam_depth:
            # a var parameter is written back to the caller: lambdas leave it alone
            vis = self.visible()
            ns = [n for n in ns if not (vis[n][1] and vis[n][2] == n and n not in self.globals)]
        return ns

    # ------------------------------------------------------ expressions
    def lam_body(self, ctx, params, ret):
        """Statements of a lambda body; params is [(name, type)]."""
        r = self.r
        lc = Ctx(LamFn(ret))
        lc.pure = ctx.pure
        lc.no_rec = True
        self.push()
        ps = []
        for n, t in params:
            nm = self.fresh(n)
            ps.append((nm, t, self.bind(nm, t, False)))
        body = []
        self.lam_depth += 1
        self.no_lists += 1
        for _ in range(r.randint(0, 2 if self.lam_depth < 2 else 1)):
            body.extend(self.stmt(lc, 1 if self.lam_depth < 2 else 0))
        if ret:
            body.append(Return(self.int_expr(2, lc)))
        self.no_lists -= 1
        self.lam_depth -= 1
        self.pop()
        return ps, body

    def lam_expr(self, ctx, typ):
        params = {F1: [('x', 'Int')], F0: [], F2: [('x', 'Int'), ('y', 'Int')]}[typ]
        ps, body = self.lam_body(ctx, params, 'Int')
        return Lambda(ps, 'Int', Block(body))

    def fn_value(self, ctx, typ):
        """An expression of function type typ (no side effects)."""
        r = self.r
        vs = self.names(typ)
        c = r.random()
        if vs and c < 0.4:
            return self.ref(r.choice(vs))
        if typ == F1 and c < 0.55:
            k = self.int_expr(1, ctx)
            return Builtin('make_adder', [k], _adder)
        if typ == F1 and c < 0.65:
            k = self.int_expr(1, ctx)
            return Builtin('make_scaled', [k], _scaled)
        if typ == F0 and c < 0.6:
            k = self.int_expr(1, ctx)
            return Builtin('make_counter', [k], _counter)
        if typ == F1 and c < 0.72 and len(vs) >= 1:
            a = self.ref(r.choice(vs))
            b = self.ref(r.choice(vs))
            return Builtin('compose', [a, b], _compose)
        return self.lam_expr(ctx, typ)

    def call_expr(self, ctx):
        """One call of a closure (it may print or write captured vars)."""
        r = self.r
        opts = []
        for typ in (F1, F0, F2):
            for n in self.names(typ):
                opts.append(('var', n, typ))
        for n in self.names(LF1):
            opts.append(('list', n, LF1))
        for n in self.names('Btn'):
            opts.append(('btn', n, 'Btn'))
        if not opts:
            return None
        kind, n, typ = r.choice(opts)
        arg = lambda: self.int_expr(1, ctx)
        if kind == 'var':
            if typ == F1 and r.random() < 0.3:
                return HelperCall(r.choice(['apply', 'twice']), self.ref(n), arg())
            args = {F1: 1, F0: 0, F2: 2}[typ]
            return CallValue(self.ref(n), [arg() for _ in range(args)])
        if kind == 'list':
            return None   # list calls are guarded statements
        return BtnCall(self.ref(n), arg())

    # ------------------------------------------------------- statements
    def extra_weights(self, ctx, depth):
        if ctx.in_defer:
            return []
        has = lambda *ts: any(self.names(t) for t in ts)
        w = [('cdecl', 6 if self.lam_depth < 2 else 0),
             ('ccall', 0 if ctx.pure else (6 if has(F1, F0, F2, 'Btn') else 0)),
             ('cassign', 2 if any(self.names(t, True) for t in (F1, F0, F2)) else 0)]
        if not ctx.pure and not self.no_lists:
            w += [('clist', 2), ('clop', 4 if has(LF1) else 0), ('cbtn', 2 if has(F1) else 0),
                  ('crec', 1), ('crc', 1)]
        return w

    def s_cdecl(self, ctx, depth):
        r = self.r
        typ = r.choice([F1, F1, F0, F2])
        e = self.fn_value(ctx, typ)
        mut = r.random() < 0.3
        name = self.fresh('cl')
        uid = self.bind(name, typ, mut)
        ann = typ if isinstance(e, Lambda) and r.random() < 0.2 else None
        return [Decl(name, e, mut, ann=ann, uid=uid)]

    def s_cassign(self, ctx, depth):
        r = self.r
        cands = [(n, t) for t in (F1, F0, F2) for n in self.names(t, True)]
        n, t = r.choice(cands)
        return [Assign(n, '=', self.fn_value(ctx, t), uid=self.uid(n))]

    def s_ccall(self, ctx, depth):
        r = self.r
        e = self.call_expr(ctx)
        if e is None:
            return [self.print_stmt(ctx)]
        c = r.random()
        if c < 0.4:
            return [Print(Interp([self.next_tag() + ' ', e]))]
        if c < 0.7:
            name = self.fresh()
            uid = self.bind(name, 'Int', r.random() < 0.3)
            return [Decl(name, e, self.visible()[name][1], uid=uid)]
        return [ExprStmt(e)]

    def s_clist(self, ctx, depth):
        name = self.fresh('fs')
        uid = self.bind(name, LF1, True)
        return [Decl(name, Raw('[]', lambda m, f: ListV([])), True, ann=LF1, uid=uid)]

    def s_clop(self, ctx, depth):
        r = self.r
        n = r.choice(self.names(LF1))
        u = self.uid(n)
        L = lambda: Var(n, u)
        c = r.random()
        if c < 0.45:
            e = self.fn_value(ctx, F1)
            return [ExprStmt(MethodCall(L(), 'push', [e], lambda m, f, c, x: c.items.append(x[0])))]
        if c < 0.75:
            k = r.randint(0, 2)
            call = CallValue(Index(L(), Lit(k)), [self.int_expr(1, ctx)])
            return [If([(Bin('>', Builtin('len', [L()], lambda x: len(x.items)), Lit(k)),
                         Block([Print(Interp([self.next_tag() + ' ', call]))]))])]
        if c < 0.85:
            return [If([(Bin('>', Builtin('len', [L()], lambda x: len(x.items)), Lit(0)),
                         Block([ExprStmt(MethodCall(L(), 'pop', [], lambda m, f, c, x: c.items.pop()))]))])]
        # call every stored closure in order
        h = self.fresh('h')
        self.push()
        hu = self.bind(h, F1)
        self.pop()
        arg = self.int_expr(1, ctx)
        tag = self.next_tag()
        return [ForIn(h, L(), Block([Print(Interp([tag + ' ', CallValue(Var(h, hu), [arg])]))]), uid=hu)]

    def s_cbtn(self, ctx, depth):
        idv = self.r.randint(0, 99)
        fv = self.fn_value(ctx, F1)
        name = self.fresh('b')
        uid = self.bind(name, 'Btn', False)
        return [Decl(name, Raw(f'Btn({idv}, {fv.src()})',
                                lambda m, f, idv=idv, fv=fv: BtnV(idv, fv.ev(m, f))), False, uid=uid)]

    def s_crec(self, ctx, depth):
        """var f: func(Int) -> Int = ...; f = func(n) { ... f(n - 1) ... }"""
        r = self.r
        name = self.fresh('rf')
        uid = self.bind(name, F1, True)
        self.push()
        pn = self.fresh('n')
        pu = self.bind(pn, 'Int')
        k = r.randint(1, 3)
        base = self.int_expr(1, Ctx(None)) if r.random() < 0.5 else Lit(r.randint(0, 5))
        body = Block([
            If([(Bin('<=', Var(pn, pu), Lit(0)), Block([Return(base)]))]),
            Return(Bin('+', Bin('*', Var(pn, pu), Lit(k)),
                       CallValue(Var(name, uid), [Bin('-', Var(pn, pu), Lit(1))])))])
        self.pop()
        lam = Lambda([(pn, 'Int', pu)], 'Int', body)
        zn = self.fresh('z')
        init = Lambda([(zn, 'Int', zn + '#p')], 'Int', Block([Return(Lit(0))]))
        arg = r.randint(0, 6)
        return [Decl(name, init, True, ann=F1, uid=uid),
                Assign(name, '=', lam, uid=uid),
                Print(Interp([self.next_tag() + ' ', CallValue(Var(name, uid), [Lit(arg)])]))]

    def s_crc(self, ctx, depth):
        """A closure that grows an rc [Int] shared with the frame."""
        r = self.r
        sn = self.fresh('sh')
        su = self.bind(sn, RCL, False)
        fn = self.fresh('cl')
        self.push()
        xn = self.fresh('x')
        xu = self.bind(xn, 'Int')
        self.pop()
        lam = Lambda([(xn, 'Int', xu)], None,
                     Block([ExprStmt(MethodCall(Var(sn, su), 'push', [Bin('*', Var(xn, xu), Lit(2))],
                                                lambda m, f, c, x: c.items.append(x[0])))]))
        fu = self.bind(fn, 'func(Int)', False)
        out = [Decl(sn, Raw(f'rc [{r.randint(0, 9)}]', None), False, ann=RCL, uid=su),
               Decl(fn, lam, False, uid=fu)]
        v0 = int(out[0].expr.text[4:-1])
        out[0].expr.fn = lambda m, f, v0=v0: ListV([v0])
        for _ in range(r.randint(1, 3)):
            out.append(ExprStmt(CallValue(Var(fn, fu), [self.int_expr(1, ctx)])))
        out.append(Print(Interp([self.next_tag() + ' ', Builtin('len', [Var(sn, su)], lambda x: len(x.items)),
                                 ' ', Index(Var(sn, su), Lit(0))])))
        return out


class HelperCall(Expr):
    """apply(f, x) / twice(f, x)."""

    def __init__(self, name, f, x):
        self.name, self.f, self.x = name, f, x

    def src(self, parent=0):
        return f'{self.name}({self.f.src()}, {self.x.src()})'

    def ev(self, m, f):
        fn = self.f.ev(m, f)
        x = self.x.ev(m, f)
        m.tick()
        if self.name == 'apply':
            return fn.call(m, [x])
        return fn.call(m, [fn.call(m, [x])])


class BtnCall(Expr):
    def __init__(self, b, x):
        self.b, self.x = b, x

    def src(self, parent=0):
        return f'{self.b.src()}.on({self.x.src()})'

    def ev(self, m, f):
        b = self.b.ev(m, f)
        return b.on.call(m, [self.x.ev(m, f)])


def generate(rng):
    g = ClosureGen(rng)
    g.program()
    return g
