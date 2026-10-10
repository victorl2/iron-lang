"""Collection programs: lists ([Int], [String], [[Int]]) with push, pop,
insert, remove, index assignment, slicing, copy and take (manual 2.3, 6.8,
9.3); maps and sets (9.10); strings (slicing, len, concatenation,
interpolation, comparison and the 9.2 methods). Mixed with the control flow
of pf_stmt, and with functions that take lists by `val` and `var`."""
import re

from pf_lang import *
from pf_stmt import Gen, Ctx

S, LI, LS, LL = 'String', '[Int]', '[String]', '[[Int]]'
FA, BV = '[Int; 3]', '[Int; <=4]'   # a fixed array and a bounded vector: values
MII, MSI, SI, SS, MSL = ('Map[Int, Int]', 'Map[String, Int]', 'Set[Int]',
                         'Set[String]', 'Map[String, [Int]]')
LISTS = (LI, LS, LL)
MAPS = (MII, MSI, MSL)
SETS = (SI, SS)

PIECES = ['a', 'b', 'ab', 'ba', 'c', 'é', 'ö', '日', 'Z', ' ', ',', '-', 'aa',
          'x1', '42', '-7', ' 3 ', '\t', 'ñ', '', '{', 'Ü']
SEPS = [',', '-', 'a', ', ', 'ab']


def s_to_int(s):
    t = s.strip(' \t\n\r')
    if re.fullmatch(r'-?[0-9]{1,15}', t):
        return int(t)
    return 0


def m_(name, impl):
    """Method implementation table entry."""
    return name, impl


def pad_left(s, w, ch):
    return ch * (w - len(s)) + s if len(s) < w else s


def pad_right(s, w, ch):
    return s + ch * (w - len(s)) if len(s) < w else s


def substring(s, a, b):
    n = len(s)
    a, b = max(0, min(a, n)), max(0, min(b, n))
    return s[a:b] if a < b else ''


def list_insert(lst, i, v):
    if not 0 <= i <= len(lst.items):
        raise Bail('insert out of range')
    lst.items.insert(i, v)


def list_remove(lst, i):
    if not 0 <= i < len(lst.items):
        raise Bail('remove out of range')
    return lst.items.pop(i)


def list_pop(lst):
    if not lst.items:
        raise Bail('pop empty')
    return lst.items.pop()


def map_get(mp, k):
    if k not in mp.d:
        raise Bail('missing key')
    return copy_value(mp.d[k])


def take(c):
    if isinstance(c, ListV):
        out = ListV(c.items)
        c.items = []
        return out
    if isinstance(c, MapV):
        out = MapV(c.d)
        c.d = {}
        return out
    out = SetV(c.s)
    c.s = set()
    return out


def set_add(st, x):
    new = x not in st.s
    st.s.add(x)
    return new


def set_remove(st, x):
    had = x in st.s
    st.s.discard(x)
    return had


def map_remove(mp, k):
    had = k in mp.d
    mp.d.pop(k, None)
    return had


def check_sort(items):
    return sorted(items)


class ForMap(Stmt):
    """for (k, v) in m { acc += ... } or for x in set: order free bodies."""

    def __init__(self, names, uids, it, body):
        self.names, self.uids, self.it, self.body = names, uids, it, body

    def lines(self, ind):
        head = f'({self.names[0]}, {self.names[1]})' if len(self.names) == 2 else self.names[0]
        return [f'{ind}for {head} in {self.it.src()} {{'] + \
            self.body.lines(ind + '    ') + [f'{ind}}}']

    def run(self, m, f):
        c = self.it.ev(m, f)
        if isinstance(c, MapV):
            items = [(k, copy_value(v)) for k, v in c.d.items()]
        else:
            items = [(x,) for x in c.s]
        for it in items:
            m.tick()
            self.body.run(m, f, dict(zip(self.uids, it)))


class CollGen(Gen):
    NFUNCS = (0, 4)
    NMAIN = (10, 30)

    def __init__(self, rng):
        super().__init__(rng)
        self.frozen = set()     # collections iterated by an enclosing loop

    # ------------------------------------------------------------ scope
    def names(self, typ, mut=None):
        ns = super().names(typ, mut)
        if mut:
            ns = [n for n in ns if n not in self.frozen]
        return ns

    def param_types(self):
        return ['Int', 'Int', 'Bool', S, S, LI, LI, LS]

    def top_lines(self):
        return ['func num(n: Int) -> Int {', '    return n', '}', '']

    def call_args(self, fd, ctx):
        """A list passed to a var parameter may not be passed again in the
        same call (E0328): the other parameter gets a literal instead."""
        args = super().call_args(fd, ctx)
        if args is None:
            return None
        var_uids = {a.uid for p, a in zip(fd.params, args)
                    if p.mut and p.typ.startswith('[') and isinstance(a, Var)}
        for i, (p, a) in enumerate(zip(fd.params, args)):
            if not p.mut and isinstance(a, Var) and a.uid in var_uids and p.typ in (LI, LS):
                args[i] = self.list_lit(p.typ, ctx)
        return args

    # ------------------------------------------------------ expressions
    def str_lit(self):
        r = self.r
        return Lit(''.join(r.choice(PIECES) for _ in range(r.randint(0, 4))))

    def str_expr(self, d, ctx):
        r = self.r
        vs = self.names(S)
        if d <= 0 or r.random() < 0.3:
            if vs and r.random() < 0.65:
                return self.ref(r.choice(vs))
            return self.str_lit()
        c = r.random()
        a = lambda: self.str_expr(d - 1, ctx)
        i = lambda: self.int_expr(1, ctx)
        if c < 0.18:
            return Bin('+', a(), a())
        if c < 0.3:
            parts = []
            for _ in range(r.randint(1, 3)):
                parts.append(r.choice(PIECES))
                parts.append(r.choice([a(), i(), self.bool_expr(0, ctx)]))
            return Interp(parts)
        if c < 0.36:
            return MethodCall(a(), 'upper', [], lambda m, f, s, x: s.upper())
        if c < 0.40:
            return MethodCall(a(), 'lower', [], lambda m, f, s, x: s.lower())
        if c < 0.44:
            return MethodCall(a(), 'trim', [], lambda m, f, s, x: s.strip(' \t\n\r'))
        if c < 0.50:
            old = Lit(r.choice([p for p in PIECES if p]))
            return MethodCall(a(), 'replace', [old, self.str_lit()],
                              lambda m, f, s, x: s.replace(x[0], x[1]))
        if c < 0.54:
            return MethodCall(a(), 'repeat', [Lit(r.randint(0, 3))],
                              lambda m, f, s, x: s * max(0, x[0]))
        if c < 0.64:
            lo = r.randint(-1, 5)
            hi = r.choice([None, r.randint(-1, 7)])
            # A constant slice that is negative or reversed is a compile
            # error (E0313): those bounds go through num() so they reach
            # the clamping at run time.
            bad = lo < 0 or (hi is not None and (hi < 0 or hi < lo))
            wrapn = (lambda k: Builtin('num', [Lit(k)], lambda v: v)) if bad else Lit
            return Slice(a(), wrapn(lo), wrapn(hi) if hi is not None else None)
        if c < 0.68:
            return MethodCall(a(), 'substring', [Lit(r.randint(0, 4)), Lit(r.randint(0, 6))],
                              lambda m, f, s, x: substring(s, x[0], x[1]))
        if c < 0.74:
            return Index(a(), Lit(r.randint(-1, 5)))
        if c < 0.77:
            return MethodCall(a(), 'char_at', [Lit(r.randint(-1, 5))],
                              lambda m, f, s, x: s[x[0]] if 0 <= x[0] < len(s) else '')
        if c < 0.82:
            ls = self.names(LS)
            if ls:
                return MethodCall(Lit(r.choice(SEPS)), 'join', [self.ref(r.choice(ls))],
                                  lambda m, f, s, x: s.join(x[0].items))
        if c < 0.86:
            return MethodCall(Paren(i()), 'to_string', [], lambda m, f, s, x: str(s))
        if c < 0.92:
            w = r.randint(0, 6)
            ch = Lit(r.choice(['*', '0', 'é', ' ']))
            if r.random() < 0.5:
                return MethodCall(a(), 'pad_left', [Lit(w), ch],
                                  lambda m, f, s, x: pad_left(s, x[0], x[1]))
            return MethodCall(a(), 'pad_right', [Lit(w), ch],
                              lambda m, f, s, x: pad_right(s, x[0], x[1]))
        return Bin('+', a(), self.str_lit())

    def int_leaf(self):
        r = self.r
        if r.random() < 0.25:
            e = self.coll_int()
            if e is not None:
                return e
        return super().int_leaf()

    def coll_int(self):
        r = self.r
        ctx = Ctx(None)
        c = r.random()
        if c < 0.25:
            ls = [n for t in LISTS for n in self.names(t)]
            if ls:
                return Builtin('len', [self.ref(r.choice(ls))], lambda x: len(x.items))
        if c < 0.45:
            s = self.str_expr(1, ctx)
            k = r.random()
            if k < 0.35:
                return MethodCall(s, 'len', [], lambda m, f, s, x: len(s))
            if k < 0.45:
                return MethodCall(s, 'byte_len', [], lambda m, f, s, x: len(s.encode()))
            if k < 0.6:
                return MethodCall(s, 'index_of', [Lit(r.choice([p for p in PIECES if p]))],
                                  lambda m, f, s, x: s.find(x[0]))
            if k < 0.7:
                return MethodCall(s, 'rindex_of', [Lit(r.choice([p for p in PIECES if p]))],
                                  lambda m, f, s, x: s.rfind(x[0]))
            if k < 0.8:
                return MethodCall(s, 'count', [Lit(r.choice([p for p in PIECES if p]))],
                                  lambda m, f, s, x: s.count(x[0]))
            if k < 0.9:
                return MethodCall(s, 'to_int', [], lambda m, f, s, x: s_to_int(s))
            return Builtin('len', [s], lambda x: len(x))
        if c < 0.6:
            ms = [n for t in MAPS + SETS for n in self.names(t)]
            if ms:
                return MethodCall(self.ref(r.choice(ms)), 'len', [],
                                  lambda m, f, c, x: len(c.d) if isinstance(c, MapV) else len(c.s))
        if c < 0.72:
            ms = self.names(MII)
            if ms:
                return MethodCall(self.ref(r.choice(ms)), 'get_or',
                                  [Lit(r.randint(-2, 6)), Lit(r.randint(-9, 9))],
                                  lambda m, f, c, x: c.d.get(x[0], x[1]))
        if c < 0.8:
            ms = self.names(MSI)
            if ms:
                return MethodCall(self.ref(r.choice(ms)), 'get_or',
                                  [self.key_str(), Lit(r.randint(-9, 9))],
                                  lambda m, f, c, x: c.d.get(x[0], x[1]))
        if c < 0.9:
            ls = self.names(LI)
            if ls:
                return MethodCall(self.ref(r.choice(ls)), 'sum', [],
                                  lambda m, f, c, x: wrap(sum(c.items)))
        return None

    def key_str(self):
        return Lit(self.r.choice(['a', 'b', 'é', 'ab', '', 'k1', '日']))

    def bool_expr(self, d, ctx):
        r = self.r
        if r.random() < 0.3:
            c = r.random()
            if c < 0.3:
                op = r.choice(['==', '!=', '<', '>', '<=', '>='])
                return Bin(op, self.str_expr(1, ctx), self.str_expr(1, ctx))
            if c < 0.5:
                s = self.str_expr(1, ctx)
                name = r.choice(['contains', 'starts_with', 'ends_with'])
                fn = {'contains': lambda m, f, s, x: x[0] in s,
                      'starts_with': lambda m, f, s, x: s.startswith(x[0]),
                      'ends_with': lambda m, f, s, x: s.endswith(x[0])}[name]
                return MethodCall(s, name, [self.str_expr(0, ctx)], fn)
            if c < 0.65:
                ls = self.names(LI)
                if ls:
                    return MethodCall(self.ref(r.choice(ls)), 'contains', [self.int_expr(1, ctx)],
                                      lambda m, f, c, x: x[0] in c.items)
            if c < 0.75:
                ls = self.names(LS)
                if ls:
                    return MethodCall(self.ref(r.choice(ls)), 'contains', [self.str_expr(0, ctx)],
                                      lambda m, f, c, x: x[0] in c.items)
            if c < 0.9:
                for t, kf in ((MII, lambda: Lit(r.randint(-2, 6))), (SI, lambda: Lit(r.randint(-2, 6))),
                              (MSI, self.key_str), (SS, self.key_str), (MSL, self.key_str)):
                    ms = self.names(t)
                    if ms and r.random() < 0.5:
                        return MethodCall(self.ref(r.choice(ms)), 'has', [kf()],
                                          lambda m, f, c, x: x[0] in (c.d if isinstance(c, MapV) else c.s))
        return super().bool_expr(d, ctx)

    def expr_of(self, typ, d, ctx):
        r = self.r
        if typ == S:
            return self.str_expr(d, ctx)
        if typ in (LI, LS):
            ns = self.names(typ)
            if ns and r.random() < 0.8:
                return self.ref(r.choice(ns))
            return self.list_lit(typ, ctx)
        return super().expr_of(typ, d, ctx)

    def list_lit(self, typ, ctx):
        n = self.r.randint(1, 4)
        if typ == LI:
            items = [self.int_expr(1, ctx) for _ in range(n)]
        else:
            items = [self.str_expr(1, ctx) for _ in range(n)]
        return Raw('[' + ', '.join(i.src() for i in items) + ']',
                   lambda m, f, items=items: ListV([i.ev(m, f) for i in items]))

    # ------------------------------------------------------- statements
    def print_stmt(self, ctx):
        r = self.r
        parts = [self.next_tag()]
        pool = self.names('Int') + self.names('Bool') + self.names(S)
        for _ in range(r.randint(1, 3)):
            parts.append(' ')
            c = r.random()
            if pool and c < 0.4:
                parts.append(self.ref(r.choice(pool)))
            elif c < 0.6:
                parts.append(self.str_expr(2, ctx))
            elif c < 0.85:
                parts.append(self.int_expr(2, ctx))
            else:
                parts.append(self.bool_expr(1, ctx))
        return Print(Interp(parts))

    def extra_weights(self, ctx, depth):
        def has(*ts, mut=None):
            return any(self.names(t, mut) for t in ts)
        w = [('ldecl', 4), ('sdecl', 3),
             ('lop', 7 if has(LI, LS, LL, mut=True) else 0),
             ('lread', 3 if has(LI, LS, LL) and not ctx.pure else 0),
             ('sassign', 2 if has(S, mut=True) else 0),
             ('mdecl', 2), ('mop', 5 if has(*(MAPS + SETS), mut=True) else 0),
             ('mread', 3 if has(*(MAPS + SETS)) and not ctx.pure else 0),
             ('adecl', 2), ('aop', 4 if has(FA, BV, mut=True) else 0),
             ('aread', 2 if has(FA, BV) and not ctx.pure else 0)]
        if depth > 0:
            w += [('liter', 2 if has(LI, LS) else 0), ('miter', 1 if has(MII, MSI, SI, SS) else 0),
                  ('lfunc', 2 if has(LI) else 0)]
        return w

    def new_name(self, typ, mut):
        name = self.fresh('c' if typ not in (S,) else 's')
        return name, self.bind(name, typ, mut)

    def s_sdecl(self, ctx, depth):
        mut = self.r.random() < 0.5
        e = self.str_expr(3, ctx)
        name, uid = self.new_name(S, mut)
        return [Decl(name, e, mut, uid=uid)]

    def s_sassign(self, ctx, depth):
        t = self.r.choice(self.names(S, True))
        if self.r.random() < 0.5:
            return [Assign(t, '+=', self.str_expr(2, ctx), uid=self.uid(t))]
        return [Assign(t, '=', self.str_expr(2, ctx), uid=self.uid(t))]

    def s_ldecl(self, ctx, depth):
        r = self.r
        typ = r.choice([LI, LI, LI, LS, LS, LL])
        mut = r.random() < 0.75
        c = r.random()
        src_lists = [n for n in self.names(typ)]
        ann = None
        if c < 0.25:
            e = self.list_lit(typ, ctx) if typ != LL else Raw('[[1, 2], [3]]', lambda m, f: ListV([ListV([1, 2]), ListV([3])]))
        elif c < 0.4:
            e = Raw('[]', lambda m, f: ListV([]))
            ann = typ
        elif c < 0.55 and src_lists:
            e = MethodCall(self.ref(r.choice(src_lists)), 'copy', [], lambda m, f, c, x: copy_value(c))
        elif c < 0.65 and self.names(typ, True):
            e = MethodCall(self.ref(r.choice(self.names(typ, True))), 'take', [], lambda m, f, c, x: take(c))
        elif c < 0.75 and src_lists:
            src = self.ref(r.choice(src_lists))
            a = r.randint(0, 3)
            b = a + r.randint(0, 3)
            ln = Builtin('len', [src], lambda x: len(x.items))
            lo = Builtin('min', [Lit(a), ln], min)
            hi = Builtin('min', [Lit(b), Builtin('len', [src], lambda x: len(x.items))], min)
            e = Slice(src, lo, hi if r.random() < 0.7 else None)
        elif c < 0.82 and typ == LI:
            e = Builtin('fill', [Lit(r.randint(0, 4)), self.int_expr(1, ctx)],
                        lambda n, v: ListV([v] * max(0, n)))
        elif typ == LI and self.names(LI) and c < 0.92:
            e = self.list_hof(ctx, LI)
        elif typ == LS and c < 0.92:
            k = r.random()
            if k < 0.4 and self.names(LI):
                e = self.list_hof(ctx, LS)
            elif k < 0.7:
                sep = r.choice(SEPS)
                e = MethodCall(self.str_expr(2, ctx), 'split', [Lit(sep)],
                               lambda m, f, s, x: ListV(s.split(x[0])))
            else:
                e = MethodCall(self.str_expr(2, ctx), 'chars', [], lambda m, f, s, x: ListV(list(s)))
        else:
            e = Raw('[]', lambda m, f: ListV([]))
            ann = typ
        name, uid = self.new_name(typ, mut)
        return [Decl(name, e, mut, ann=ann, uid=uid)]

    def lam(self, params, ret, body_fn):
        """Build a lambda whose body is made by body_fn() with params bound."""
        self.push()
        ps = []
        for n, t in params:
            nm = self.fresh(n)
            ps.append((nm, t, self.bind(nm, t, False)))
        body = body_fn([Var(nm, u) for nm, t, u in ps])
        self.pop()
        return Lambda(ps, ret, Block(body))

    def list_hof(self, ctx, out):
        r = self.r
        src = self.ref(r.choice(self.names(LI)))
        lc = ctx.sub(loop=0, in_defer=False, pure=True, no_rec=True)
        if out == LS:
            fn = self.lam([('x', 'Int')], S, lambda ps: [Return(Interp([r.choice(PIECES), ps[0]]))])
            return MethodCall(src, 'map', [fn], lambda m, f, c, x: ListV([x[0].call(m, [v]) for v in c.items]))
        k = r.random()
        if k < 0.4:
            fn = self.lam([('x', 'Int')], 'Int', lambda ps: [Return(self.int_expr(2, lc))])
            return MethodCall(src, 'map', [fn], lambda m, f, c, x: ListV([x[0].call(m, [v]) for v in c.items]))
        if k < 0.8:
            fn = self.lam([('x', 'Int')], 'Bool', lambda ps: [Return(self.bool_expr(2, lc))])
            return MethodCall(src, 'filter', [fn],
                              lambda m, f, c, x: ListV([v for v in c.items if x[0].call(m, [v])]))
        fn = self.lam([('x', 'Int')], 'Int', lambda ps: [Return(self.int_expr(2, lc))])
        mapped = MethodCall(src, 'map', [fn], lambda m, f, c, x: ListV([x[0].call(m, [v]) for v in c.items]))
        fn2 = self.lam([('y', 'Int')], 'Bool', lambda ps: [Return(self.bool_expr(1, lc))])
        return MethodCall(mapped, 'filter', [fn2],
                          lambda m, f, c, x: ListV([v for v in c.items if x[0].call(m, [v])]))

    def guard_len(self, lst, k, body):
        """if len(lst) > k { body }"""
        return If([(Bin('>', Builtin('len', [lst], lambda x: len(x.items)), Lit(k)), Block(body))])

    def elem(self, typ, ctx):
        if typ == LI:
            return self.int_expr(2, ctx)
        if typ == LS:
            return self.str_expr(2, ctx)
        ls = self.names(LI)
        if ls and self.r.random() < 0.6:
            return MethodCall(self.ref(self.r.choice(ls)), 'copy', [], lambda m, f, c, x: copy_value(c))
        return self.list_lit(LI, ctx)

    def s_lop(self, ctx, depth):
        r = self.r
        ns = [(n, t) for t in LISTS for n in self.names(t, True)]
        name, typ = r.choice(ns)
        L = lambda: self.ref(name)
        k = r.randint(0, 3)
        c = r.random()
        if c < 0.3:
            return [ExprStmt(MethodCall(L(), 'push', [self.elem(typ, ctx)],
                                        lambda m, f, c, x: c.items.append(x[0])))]
        if c < 0.38:
            e = MethodCall(L(), 'pop', [], lambda m, f, c, x: list_pop(c))
            if typ == LL or ctx.pure:
                body = [ExprStmt(e)]
            else:
                body = [Print(Interp([self.next_tag() + ' pop ', e]))]
            return [self.guard_len(L(), 0, body)]
        if c < 0.46:
            return [self.guard_len(L(), k, [ExprStmt(MethodCall(L(), 'insert', [Lit(k), self.elem(typ, ctx)],
                                                                lambda m, f, c, x: list_insert(c, x[0], x[1])))])]
        if c < 0.52:
            e = MethodCall(L(), 'remove', [Lit(k)], lambda m, f, c, x: list_remove(c, x[0]))
            if typ == LL or ctx.pure:
                body = [ExprStmt(e)]
            else:
                body = [Print(Interp([self.next_tag() + ' rm ', e]))]
            return [self.guard_len(L(), k, body)]
        if c < 0.64 and typ != LL:
            op = '=' if typ == LS else r.choice(['=', '=', '+=', '-=', '^='])
            return [self.guard_len(L(), k, [IndexAssign(Index(L(), Lit(k)), op, self.elem(typ, ctx))])]
        if c < 0.68 and typ != LL:
            return [self.guard_len(L(), k, [ExprStmt(MethodCall(L(), 'set', [Lit(k), self.elem(typ, ctx)],
                                                                lambda m, f, c, x: c.items.__setitem__(x[0], x[1])))])]
        if c < 0.74:
            return [ExprStmt(MethodCall(L(), 'reverse', [], lambda m, f, c, x: c.items.reverse()))]
        if c < 0.8 and typ != LL:
            return [ExprStmt(MethodCall(L(), 'sort', [], lambda m, f, c, x: c.items.sort()))]
        if c < 0.83:
            return [ExprStmt(MethodCall(L(), 'clear', [], lambda m, f, c, x: c.items.clear()))]
        if typ == LL:
            # grid[i].push(v), grid[i][j] = v
            j = r.randint(0, 2)
            inner = Index(L(), Lit(k))
            if r.random() < 0.5:
                return [self.guard_len(L(), k, [ExprStmt(MethodCall(inner, 'push', [self.int_expr(2, ctx)],
                                                                    lambda m, f, c, x: c.items.append(x[0])))])]
            inner2 = Index(L(), Lit(k))
            body = [If([(Bin('>', Builtin('len', [inner2], lambda x: len(x.items)), Lit(j)),
                         Block([IndexAssign(Index(Index(L(), Lit(k)), Lit(j)), '=', self.int_expr(2, ctx))]))])]
            return [self.guard_len(L(), k, body)]
        return [ExprStmt(MethodCall(L(), 'push', [self.elem(typ, ctx)],
                                    lambda m, f, c, x: c.items.append(x[0])))]

    def s_lread(self, ctx, depth):
        r = self.r
        ns = [(n, t) for t in LISTS for n in self.names(t)]
        name, typ = r.choice(ns)
        L = lambda: self.ref(name)
        tag = self.next_tag()
        if typ == LL:
            k, j = r.randint(0, 2), r.randint(0, 2)
            inner = Index(L(), Lit(k))
            body = [Print(Interp([tag + ' ', Builtin('len', [inner], lambda x: len(x.items))])),
                    If([(Bin('>', Builtin('len', [Index(L(), Lit(k))], lambda x: len(x.items)), Lit(j)),
                         Block([Print(Interp([tag + 'e ', Index(Index(L(), Lit(k)), Lit(j))]))]))])]
            return [Print(Interp([tag + 'n ', Builtin('len', [L()], lambda x: len(x.items))])),
                    self.guard_len(L(), k, body)]
        c = r.random()
        if c < 0.5:
            # print every element
            x = self.fresh('x')
            self.push()
            xu = self.bind(x, 'Int' if typ == LI else S)
            self.pop()
            return [PrintPart(Interp([tag + ':'])),
                    ForIn(x, L(), Block([PrintPart(Interp([' ', Var(x, xu)]))]), uid=xu),
                    Print(Interp([]))]
        k = r.randint(0, 3)
        if c < 0.8:
            return [self.guard_len(L(), k, [Print(Interp([tag + ' ', Index(L(), Lit(k))]))])]
        return [self.guard_len(L(), k, [Print(Interp([tag + ' ', MethodCall(L(), 'get', [Lit(k)],
                                                                          lambda m, f, c, x: c.items[x[0]])]))])]

    def s_liter(self, ctx, depth):
        """for x in xs { ... } with xs left alone by the body."""
        r = self.r
        ns = [(n, t) for t in (LI, LS) for n in self.names(t)]
        name, typ = r.choice(ns)
        x = self.fresh('x')
        lc = ctx.sub(loop=ctx.loop + 1, in_defer=False)
        self.frozen.add(name)
        self.push()
        xu = self.bind(x, 'Int' if typ == LI else S)
        body = self.block(lc, r.randint(1, 3), depth - 1)
        self.pop()
        self.frozen.discard(name)
        return [ForIn(x, self.ref(name), body, uid=xu)]

    def s_lfunc(self, ctx, depth):
        """forEach with a lambda that writes a captured var."""
        r = self.r
        src = self.ref(r.choice(self.names(LI)))
        acc = self.fresh('v')
        accu = self.bind(acc, 'Int', True)
        lc = ctx.sub(loop=0, in_defer=False, pure=True, no_rec=True)
        fn = self.lam([('x', 'Int')], None,
                      lambda ps: [Assign(acc, r.choice(['+=', '^=', '-=']), self.int_expr(2, lc), uid=accu)])
        out = [Decl(acc, Lit(r.randint(0, 5)), True, uid=accu),
               ExprStmt(MethodCall(src, 'forEach', [fn], lambda m, f, c, x: [x[0].call(m, [v]) for v in list(c.items)]))]
        if not ctx.pure:
            out.append(Print(Interp([self.next_tag() + ' ', Var(acc, accu)])))
        return out

    # maps and sets
    def key_for(self, typ):
        if typ in (MII, SI):
            return Lit(self.r.randint(-2, 6))
        return self.key_str()

    def s_mdecl(self, ctx, depth):
        r = self.r
        typ = r.choice([MII, MII, MSI, SI, SS, MSL])
        srcs = self.names(typ)
        c = r.random()
        if c < 0.15 and srcs:
            e = MethodCall(self.ref(r.choice(srcs)), 'copy', [], lambda m, f, c, x: copy_value(c))
        elif c < 0.22 and self.names(typ, True):
            e = MethodCall(self.ref(r.choice(self.names(typ, True))), 'take', [], lambda m, f, c, x: take(c))
        else:
            e = Raw(f'{typ}()', (lambda m, f: SetV()) if typ in SETS else (lambda m, f: MapV()))
        name, uid = self.new_name(typ, True)
        return [Decl(name, e, True, uid=uid)]

    def s_mop(self, ctx, depth):
        r = self.r
        ns = [(n, t) for t in MAPS + SETS for n in self.names(t, True)]
        name, typ = r.choice(ns)
        M = lambda: self.ref(name)
        c = r.random()
        k = self.key_for(typ)
        quiet = ctx.pure
        if typ in SETS:
            if c < 0.6:
                e = MethodCall(M(), 'add', [k], lambda m, f, c, x: set_add(c, x[0]))
            elif c < 0.9:
                e = MethodCall(M(), 'remove', [k], lambda m, f, c, x: set_remove(c, x[0]))
            else:
                return [ExprStmt(MethodCall(M(), 'clear', [], lambda m, f, c, x: c.s.clear()))]
            if quiet or r.random() < 0.5:
                return [ExprStmt(e)]
            return [Print(Interp([self.next_tag() + ' ', e]))]
        if c < 0.6:
            if typ == MSL:
                v = self.elem(LL, ctx)
            else:
                v = self.int_expr(2, ctx)
            return [ExprStmt(MethodCall(M(), 'put', [k, v], lambda m, f, c, x: c.d.__setitem__(x[0], x[1])))]
        if c < 0.75 and typ != MSL:
            # read-modify-write
            k2 = Lit(k.v)
            v = Bin('+', MethodCall(M(), 'get_or', [k2, Lit(0)], lambda m, f, c, x: c.d.get(x[0], x[1])),
                    self.int_expr(1, ctx))
            return [ExprStmt(MethodCall(M(), 'put', [k, v], lambda m, f, c, x: c.d.__setitem__(x[0], x[1])))]
        if c < 0.92:
            e = MethodCall(M(), 'remove', [k], lambda m, f, c, x: map_remove(c, x[0]))
            if quiet or r.random() < 0.5:
                return [ExprStmt(e)]
            return [Print(Interp([self.next_tag() + ' ', e]))]
        return [ExprStmt(MethodCall(M(), 'clear', [], lambda m, f, c, x: c.d.clear()))]

    def s_mread(self, ctx, depth):
        r = self.r
        ns = [(n, t) for t in MAPS + SETS for n in self.names(t)]
        name, typ = r.choice(ns)
        M = lambda: self.ref(name)
        tag = self.next_tag()
        c = r.random()
        if c < 0.45:
            # sorted keys (or values of a set)
            kt = 'Int' if typ in (MII, SI) else S
            meth = 'values' if typ in SETS else 'keys'
            ks, ku = self.new_name('[Int]' if kt == 'Int' else LS, True)
            self.scopes[-1].pop(ks)   # not visible to later statements
            x = self.fresh('x')
            self.push()
            xu = self.bind(x, kt)
            self.pop()
            fn = (lambda m, f, c, a: ListV(list(c.s))) if typ in SETS else (lambda m, f, c, a: ListV(list(c.d.keys())))
            body = [PrintPart(Interp([' ', Var(x, xu)]))]
            if typ in (MII, MSI) and r.random() < 0.6:
                body = [PrintPart(Interp([' ', Var(x, xu), '=', MethodCall(M(), 'get', [Var(x, xu)],
                                                                            lambda m, f, c, a: map_get(c, a[0]))]))]
            if typ == MSL:
                body = [PrintPart(Interp([' ', Var(x, xu), '#', MethodCall(MethodCall(M(), 'get', [Var(x, xu)],
                                                                                    lambda m, f, c, a: map_get(c, a[0])),
                                                                         'len', [], lambda m, f, c, a: len(c.items))]))]
            return [BareBlock(Block([
                Decl(ks, MethodCall(M(), meth, [], fn), True, uid=ku),
                ExprStmt(MethodCall(Var(ks, ku), 'sort', [], lambda m, f, c, a: c.items.sort())),
                PrintPart(Interp([tag + ':'])),
                ForIn(x, Var(ks, ku), Block(body), uid=xu),
                Print(Interp([]))]))]
        if c < 0.7 and typ in MAPS:
            k = self.key_for(typ)
            k2 = Lit(k.v)
            if typ == MSL:
                g = MethodCall(MethodCall(M(), 'get', [k2], lambda m, f, c, a: map_get(c, a[0])), 'len', [],
                               lambda m, f, c, a: len(c.items))
            else:
                g = MethodCall(M(), 'get', [k2], lambda m, f, c, a: map_get(c, a[0]))
            return [If([(MethodCall(M(), 'has', [k], lambda m, f, c, a: a[0] in c.d),
                         Block([Print(Interp([tag + ' ', g]))]))],
                       Block([Print(Interp([tag + ' none']))]))]
        e = MethodCall(M(), 'len', [], lambda m, f, c, a: len(c.d) if isinstance(c, MapV) else len(c.s))
        return [Print(Interp([tag + ' ', e]))]

    def s_miter(self, ctx, depth):
        r = self.r
        ns = [(n, t) for t in (MII, MSI, SI, SS) for n in self.names(t)]
        name, typ = r.choice(ns)
        acc = self.fresh('v')
        accu = self.bind(acc, 'Int', True)
        self.frozen.add(name)
        self.push()
        if typ in MAPS:
            kn, vn = self.fresh('k'), self.fresh('x')
            ku = self.bind(kn, 'Int' if typ == MII else S)
            vu = self.bind(vn, 'Int')
            kexpr = Var(kn, ku) if typ == MII else MethodCall(Var(kn, ku), 'len', [], lambda m, f, s, a: len(s))
            rhs = Bin('+', Bin('*', kexpr, Lit(r.randint(1, 9))), Var(vn, vu))
            names, uids = [kn, vn], [ku, vu]
        else:
            kn = self.fresh('k')
            ku = self.bind(kn, 'Int' if typ == SI else S)
            kexpr = Var(kn, ku) if typ == SI else MethodCall(Var(kn, ku), 'byte_len', [],
                                                              lambda m, f, s, a: len(s.encode()))
            rhs = Bin('*', kexpr, Lit(r.randint(1, 9)))
            names, uids = [kn], [ku]
        self.pop()
        self.frozen.discard(name)
        out = [Decl(acc, Lit(0), True, uid=accu),
               ForMap(names, uids, self.ref(name), Block([Assign(acc, '+=', rhs, uid=accu)]))]
        if not ctx.pure:
            out.append(Print(Interp([self.next_tag() + ' ', Var(acc, accu)])))
        return out


class ArrV:
    """A fixed array or bounded vector: a value, copied on assignment."""
    __slots__ = ('items', 'cap')

    def __init__(self, items, cap):
        self.items, self.cap = items, cap


def arr_copy(v):
    return ArrV(list(v.items), v.cap)


def _adecl(self, ctx, depth):
    r = self.r
    typ = r.choice([FA, BV])
    srcs = self.names(typ)
    if srcs and r.random() < 0.4:
        n = r.choice(srcs)
        u = self.uid(n)
        e = Raw(n, lambda m, f, u=u: arr_copy(f.look(u).v))
        ann = None
    elif typ == FA:
        items = [self.int_expr(1, ctx) for _ in range(3)]
        e = Raw('[' + ', '.join(i.src() for i in items) + ']',
                lambda m, f, items=items: ArrV([i.ev(m, f) for i in items], 3))
        ann = typ
    else:
        items = [self.int_expr(1, ctx) for _ in range(r.randint(0, 3))]
        e = Raw('[' + ', '.join(i.src() for i in items) + ']',
                lambda m, f, items=items: ArrV([i.ev(m, f) for i in items], 4))
        ann = typ
    name, uid = self.new_name(typ, r.random() < 0.8)
    return [Decl(name, e, self.visible()[name][1], ann=ann, uid=uid)]


def _aop(self, ctx, depth):
    r = self.r
    cands = [(n, t) for t in (FA, BV) for n in self.names(t, True)]
    n, typ = r.choice(cands)
    u = self.uid(n)
    A = lambda: Var(n, u)
    c = r.random()
    if c < 0.4:
        # a constant index past a fixed array's end is a compile error (E0312)
        k = r.randint(0, 2 if typ == FA else 3)
        st = IndexAssign(Index(A(), Lit(k)), r.choice(['=', '+=', '^=']), self.int_expr(2, ctx))
        return [st] if typ == FA else [self.guard_len(A(), k, [st])]
    if c < 0.6:
        others = [x for x in self.names(typ) if x != n]
        if others:
            o = r.choice(others)
            ou = self.uid(o)
            return [Assign(n, '=', Raw(o, lambda m, f, ou=ou: arr_copy(f.look(ou).v)), uid=u)]
    if typ == BV and c < 0.8:
        body = [ExprStmt(MethodCall(A(), 'push', [self.int_expr(1, ctx)],
                                    lambda m, f, c, x: c.items.append(x[0])))]
        return [If([(Bin('<', Builtin('len', [A()], lambda x: len(x.items)), Lit(4)), Block(body))])]
    if typ == BV:
        return [self.guard_len(A(), 0, [ExprStmt(MethodCall(A(), 'pop', [],
                                                            lambda m, f, c, x: c.items.pop()))])]
    k = r.randint(0, 2)
    return [IndexAssign(Index(A(), Lit(k)), '=', self.int_expr(1, ctx))]


def _aread(self, ctx, depth):
    r = self.r
    n = r.choice([x for t in (FA, BV) for x in self.names(t)])
    u = self.uid(n)
    tag = self.next_tag()
    x = self.fresh('x')
    self.push()
    xu = self.bind(x, 'Int')
    self.pop()
    return [PrintPart(Interp([tag + ' ', Builtin('len', [Var(n, u)], lambda a: len(a.items)), ':'])),
            ForIn(x, Var(n, u), Block([PrintPart(Interp([' ', Var(x, xu)]))]), uid=xu),
            Print(Interp([]))]


CollGen.s_adecl = _adecl
CollGen.s_aop = _aop
CollGen.s_aread = _aread


def generate(rng):
    g = CollGen(rng)
    g.program()
    return g
