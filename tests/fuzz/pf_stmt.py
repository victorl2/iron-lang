"""Statement and control-flow programs: var mutation, if/elif/else, while
and for loops with break/continue, early returns, match on Int and on
enums (unit and payload variants), functions with val and var parameters,
recursion, globals, shadowing and defer."""
from pf_lang import *


class Ctx:
    def __init__(self, fn=None):
        self.fn = fn            # FuncDef being generated, None in main
        self.loop = 0           # loop nesting
        self.in_defer = False
        self.pure = False       # no prints, no global writes, no impure calls
        self.no_rec = False     # no calls of recursive functions

    def sub(self, **kw):
        c = Ctx(self.fn)
        c.loop, c.in_defer, c.pure, c.no_rec = self.loop, self.in_defer, self.pure, self.no_rec
        for k, v in kw.items():
            setattr(c, k, v)
        return c


class Scoped:
    """Name scopes shared by the generators: name -> (type, mutable, uid)."""

    def __init__(self, rng):
        self.r = rng
        self.n = 0
        self.u = 0
        self.tag = 0
        self.scopes = []

    def fresh(self, p='v'):
        self.n += 1
        return f'{p}{self.n}'

    def push(self):
        self.scopes.append({})

    def pop(self):
        self.scopes.pop()

    def bind(self, name, typ, mut=False):
        self.u += 1
        uid = f'{name}#{self.u}'
        self.scopes[-1][name] = (typ, mut, uid)
        return uid

    def visible(self):
        seen = {}
        for s in self.scopes:
            seen.update(s)
        return seen

    def names(self, typ, mut=None):
        return [n for n, (t, mu, _) in self.visible().items()
                if t == typ and (mut is None or mu == mut)]

    def ref(self, name):
        return Var(name, self.visible()[name][2])

    def uid(self, name):
        return self.visible()[name][2]

    def next_tag(self):
        self.tag += 1
        return f't{self.tag}'

    def pick(self, weights):
        tot = sum(x for _, x in weights)
        k = self.r.random() * tot
        for kind, x in weights:
            k -= x
            if k < 0:
                return kind
        return weights[-1][0]


class Gen(Scoped):
    def __init__(self, rng):
        super().__init__(rng)
        self.funcs = []         # (FuncDef, meta)
        self.globals = []       # names of global Int vars
        self.enums = []

    # ------------------------------------------------------- expressions
    def int_leaf(self):
        vs = self.names('Int')
        if vs and self.r.random() < 0.7:
            return self.ref(self.r.choice(vs))
        return Lit(self.r.randint(-9, 20))

    def int_expr(self, d, ctx):
        r = self.r
        if d <= 0 or r.random() < 0.3:
            return self.int_leaf()
        c = r.random()
        if c < 0.08:
            return Un('-', self.int_expr(d - 1, ctx))
        if c < 0.18:
            k = r.choice([2, 3, 5, 7, -3, 4])
            return Bin(r.choice(['/', '%']), self.int_expr(d - 1, ctx), Lit(k))
        if c < 0.30:
            fs = [fd for fd, meta in self.funcs if meta['pure'] and fd.ret == 'Int'
                  and not (meta['rec'] and (ctx.loop or ctx.no_rec))]
            if fs:
                fd = r.choice(fs)
                return Call(fd, self.args_for(fd, d - 1, ctx))
        if c < 0.36:
            return Bin('*', self.int_expr(d - 1, ctx),
                       Bin('&', self.int_expr(d - 1, ctx), Lit(7)))
        op = r.choice(['+', '-', '+', '&', '|', '^'])
        return Bin(op, self.int_expr(d - 1, ctx), self.int_expr(d - 1, ctx))

    def args_for(self, fd, d, ctx):
        out = []
        for i, p in enumerate(fd.params):
            if i == 0 and fd.rec:
                out.append(Lit(self.r.randint(0, 4)))
            else:
                out.append(self.expr_of(p.typ, d, ctx.sub(no_rec=True)))
        return out

    def expr_of(self, typ, d, ctx):
        if typ == 'Int':
            return self.int_expr(d, ctx)
        if typ == 'Bool':
            return self.bool_expr(d, ctx)
        for e in self.enums:
            if e.name == typ:
                return self.enum_expr(e, d, ctx)
        raise ValueError(typ)

    def enum_expr(self, e, d, ctx):
        vs = self.names(e.name)
        if vs and self.r.random() < 0.5:
            return self.ref(self.r.choice(vs))
        n, ps, _ = self.r.choice(e.variants)
        return EnumCon(e, n, [self.expr_of(t, max(d - 1, 0), ctx) for t in ps])

    def bool_expr(self, d, ctx):
        r = self.r
        c = r.random()
        bs = self.names('Bool')
        if bs and c < 0.15:
            return self.ref(r.choice(bs))
        if d <= 0 or c < 0.55:
            ens = [e for e in self.enums if self.names(e.name)]
            if ens and r.random() < 0.2:
                e = r.choice(ens)
                return Bin(r.choice(['==', '!=']), self.ref(r.choice(self.names(e.name))),
                           self.enum_expr(e, 1, ctx))
            op = r.choice(['==', '!=', '<', '>', '<=', '>='])
            return Bin(op, self.int_expr(max(d - 1, 1), ctx), self.int_expr(max(d - 1, 0), ctx))
        if c < 0.68:
            return Un('not', self.bool_expr(d - 1, ctx))
        return Bin(r.choice(['and', 'or']), self.bool_expr(d - 1, ctx), self.bool_expr(d - 1, ctx))

    # -------------------------------------------------------- statements
    def print_stmt(self, ctx):
        parts = [self.next_tag()]
        pool = self.names('Int') + self.names('Bool')
        for e in self.enums:
            pool += self.names(e.name)
        for _ in range(self.r.randint(1, 3)):
            parts.append(' ')
            if pool and self.r.random() < 0.6:
                parts.append(self.ref(self.r.choice(pool)))
            elif self.r.random() < 0.7:
                parts.append(self.int_expr(2, ctx))
            else:
                parts.append(self.bool_expr(1, ctx))
        return Print(Interp(parts))

    def block(self, ctx, n, depth, tail=None, pre=()):
        self.push()
        for name, typ in pre:
            self.bind(name, typ)
        stmts = []
        for _ in range(n):
            stmts.extend(self.stmt(ctx, depth))
        if tail:
            # generated last, in the block's scope (a declaration in the
            # block may shadow a name the exit statement uses)
            t = self.exit_stmt(ctx)
            if t is not None:
                stmts.append(t)
        self.pop()
        return Block(stmts)

    def exit_stmt(self, ctx):
        """A statement that leaves the block (last in an if body)."""
        opts = []
        if ctx.loop and not ctx.in_defer:
            opts += ['break', 'continue']
        if ctx.fn is not None and not ctx.in_defer:
            opts.append('return')
        if not opts:
            return None
        k = self.r.choice(opts)
        if k == 'break':
            return Break()
        if k == 'continue':
            return Continue()
        if ctx.fn.ret:
            return Return(self.expr_of(ctx.fn.ret, 2, ctx))
        return Return()

    def mutable_ints(self, ctx):
        ms = self.names('Int', True)
        if ctx.pure:
            ms = [n for n in ms if n not in self.globals]
        return ms

    def impure_calls(self):
        return [fd for fd, meta in self.funcs if not meta['pure'] or meta['varp']]

    def stmt(self, ctx, depth):
        w = [('decl', 5), ('assign', 6 if self.mutable_ints(ctx) else 0),
             ('print', 0 if ctx.pure else 4)]
        if depth > 0:
            w += [('if', 4), ('while', 2), ('for', 2), ('mint', 2),
                  ('menum', 2 if self.enums else 0), ('block', 1),
                  ('defer', 0 if (ctx.in_defer or ctx.pure) else 1)]
        if self.impure_calls() and not ctx.pure and not ctx.in_defer:
            w.append(('call', 2))
        w += self.extra_weights(ctx, depth)
        return getattr(self, 's_' + self.pick(w))(ctx, depth)

    def extra_weights(self, ctx, depth):
        return []

    def s_decl(self, ctx, depth):
        r = self.r
        name = self.fresh()
        # occasionally shadow a name of an enclosing scope (manual 4.1)
        outer = [n for s in self.scopes[:-1] for n in s if n not in self.scopes[-1]
                 and not n.startswith('g')]
        if outer and r.random() < 0.12:
            name = r.choice(outer)
        c = r.random()
        mut = r.random() < 0.5
        if c < 0.65 or not self.enums:
            e = self.int_expr(3, ctx)
            typ = 'Int'
            if not ctx.pure and not ctx.in_defer and r.random() < 0.25:
                # value of an impure call, var arguments written back
                calls = [fd for fd, meta in self.funcs if fd.ret == 'Int' and not meta['rec']]
                if calls:
                    fd = r.choice(calls)
                    args = self.call_args(fd, ctx)
                    if args is not None:
                        e = Call(fd, args)
        elif c < 0.8:
            e = self.bool_expr(2, ctx)
            typ = 'Bool'
        else:
            en = r.choice(self.enums)
            e = self.enum_expr(en, 2, ctx)
            typ = en.name
            mut = False
        uid = self.bind(name, typ, mut)
        return [Decl(name, e, mut, uid=uid)]

    def call_args(self, fd, ctx):
        args = []
        used = set()
        for p in fd.params:
            if p.mut:
                cands = [n for n in self.names(p.typ, True)
                         if n not in used and n not in self.globals]
                if not cands:
                    return None
                n = self.r.choice(cands)
                used.add(n)
                args.append(self.ref(n))
            else:
                args.append(self.expr_of(p.typ, 2, ctx))
        return args

    def s_call(self, ctx, depth):
        fd = self.r.choice(self.impure_calls())
        args = self.call_args(fd, ctx)
        if args is None:
            return [self.print_stmt(ctx)]
        return [ExprStmt(Call(fd, args))]

    def s_assign(self, ctx, depth):
        r = self.r
        t = r.choice(self.mutable_ints(ctx))
        op = r.choice(['=', '=', '+=', '-=', '*=', '&=', '|=', '^=', '/='])
        if op == '/=':
            e = Lit(r.choice([2, 3, -2, 5]))
        elif op == '*=':
            e = Bin('&', self.int_expr(1, ctx), Lit(3))
        else:
            e = self.int_expr(2, ctx)
        return [Assign(t, op, e, uid=self.uid(t))]

    def s_print(self, ctx, depth):
        return [self.print_stmt(ctx)]

    def s_if(self, ctx, depth):
        r = self.r
        arms = []
        for i in range(r.choice([1, 1, 2, 3])):
            cond = self.bool_expr(2, ctx)
            tail = r.random() < 0.3
            arms.append((cond, self.block(ctx, r.randint(1, 3), depth - 1, tail)))
        els = None
        if r.random() < 0.5:
            tail = r.random() < 0.2
            els = self.block(ctx, r.randint(1, 3), depth - 1, tail)
        return [If(arms, els)]

    def s_while(self, ctx, depth):
        r = self.r
        cnt = self.fresh('w')
        lim = r.randint(0, 5 if ctx.loop == 0 else 3)
        uid = self.bind(cnt, 'Int', False)   # the body must not assign the counter
        decl = Decl(cnt, Lit(0), True, uid=uid)
        lc = ctx.sub(loop=ctx.loop + 1, in_defer=False)
        cv = Var(cnt, uid)
        cond = Bin('<', cv, Lit(lim))
        if r.random() < 0.3:
            extra = self.bool_expr(1, ctx)
            if r.random() < 0.5:
                cond = Bin('and', cond, extra)
            else:
                # an `or` could keep the loop going: keep the counter bound
                cond = Bin('and', Paren(Bin('or', cond, extra)), Bin('<', cv, Lit(lim + 1)))
        self.push()
        stmts = [Assign(cnt, '+=', Lit(1), uid=uid)]
        for _ in range(r.randint(1, 4)):
            stmts.extend(self.stmt(lc, depth - 1))
        if r.random() < 0.3:
            t = self.exit_stmt(lc)
            stmts.append(If([(self.bool_expr(1, lc), Block([t]))]))
        self.pop()
        return [decl, While(cond, Block(stmts))]

    def s_for(self, ctx, depth):
        r = self.r
        name = self.fresh('i')
        lc = ctx.sub(loop=ctx.loop + 1, in_defer=False)
        self.push()
        uid = self.bind(name, 'Int', False)
        if r.random() < 0.5:
            n = Lit(r.randint(0, 5 if ctx.loop == 0 else 3))
            body = self.block(lc, r.randint(1, 4), depth - 1)
            self.pop()
            return [ForRange(name, n, body, uid=uid)]
        self.pop()
        items = [self.int_expr(1, ctx) for _ in range(r.randint(1, 4))]
        self.push()
        uid = self.bind(name, 'Int', False)
        body = self.block(lc, r.randint(1, 4), depth - 1)
        self.pop()
        lst = Raw('[' + ', '.join(i.src() for i in items) + ']',
                  lambda m, f, items=items: ListV([i.ev(m, f) for i in items]))
        return [ForIn(name, lst, body, uid=uid)]

    def s_mint(self, ctx, depth):
        r = self.r
        subj = self.int_expr(2, ctx)
        if r.random() < 0.5:
            subj = Bin('%', subj, Lit(r.choice([3, 4, 5])))
        vals = r.sample(range(-4, 8), r.randint(1, 4))
        arms = []
        for v in vals:
            if v >= 2 and r.random() < 0.15:
                psrc = f'1 + {v - 1}'            # constant expression (#344)
            else:
                psrc = str(v)
            b = self.block(ctx, r.randint(1, 2), depth - 1)
            b.single = True
            arms.append([psrc, v, b])
        # a pattern starting with `-` continues a one-line arm before it
        # (manual 1.6): those arms are written as blocks.
        for i in range(1, len(arms)):
            if arms[i][0].startswith('-'):
                arms[i - 1][2].single = False
        els = self.block(ctx, r.randint(1, 2), depth - 1)
        els.single = True
        return [MatchInt(subj, [tuple(a) for a in arms], els)]

    def s_menum(self, ctx, depth):
        r = self.r
        en = r.choice(self.enums)
        vs = self.names(en.name)
        subj = self.ref(r.choice(vs)) if vs and r.random() < 0.7 else self.enum_expr(en, 2, ctx)
        variants = [v[0] for v in en.variants]
        cover = variants[:] if r.random() < 0.5 else r.sample(variants, r.randint(1, len(variants)))
        r.shuffle(cover)
        arms = []
        for v in cover:
            binds = []
            self.push()
            for t in en.payload_types(v):
                if r.random() < 0.2:
                    binds.append(('_', None))
                else:
                    b = self.fresh('p')
                    binds.append((b, self.bind(b, t, False)))
            blk = self.block(ctx, r.randint(1, 3), depth - 1)
            self.pop()
            blk.single = True
            arms.append((v, binds, blk))
        els = None
        if len(cover) < len(variants):
            els = self.block(ctx, r.randint(1, 2), depth - 1)
            els.single = True
        return [MatchEnum(subj, en, arms, els, qualify=r.random() < 0.8)]

    def s_block(self, ctx, depth):
        return [BareBlock(self.block(ctx, self.r.randint(1, 4), depth - 1))]

    def s_defer(self, ctx, depth):
        dc = ctx.sub(in_defer=True, no_rec=True)
        # In a function body a deferred block only prints: whether `return e`
        # reads its value before or after the defers is not stated.
        if ctx.fn is not None or self.r.random() < 0.6:
            return [Defer(self.print_stmt(dc), single=True)]
        b = self.block(dc, self.r.randint(1, 3), min(depth - 1, 1))
        return [Defer(b)]

    # -------------------------------------------------------- top level
    def gen_enums(self):
        r = self.r
        e0 = []
        val = None
        for n in ['Red', 'Green', 'Blue', 'Gray'][:r.randint(2, 4)]:
            if r.random() < 0.3:
                val = r.randint(0, 9) if val is None else val + r.randint(1, 4)
                e0.append((n, [], val))
            else:
                val = 0 if val is None else val + 1
                e0.append((n, [], None))
        self.enums.append(EnumDef('Color', e0))
        e1 = [('Num', ['Int'], None), ('Pair', ['Int', 'Int'], None), ('Empty', [], None)]
        if r.random() < 0.5:
            e1.append(('Flag', ['Bool'], None))
        if r.random() < 0.5:
            e1.append(('Tint', ['Color'], None))
        r.shuffle(e1)
        self.enums.append(EnumDef('Tok', e1))

    def gen_func(self, i):
        r = self.r
        kind = r.random()
        rec = kind < 0.2
        pure = rec or kind < 0.55
        params = []
        if rec:
            params.append(Param('n', 'Int'))
        for j in range(r.randint(1 if rec else 0, 3)):
            t = r.choice(self.param_types())
            mut = (not rec and (t == 'Int' or t.startswith('['))
                   and r.random() < (0.5 if not pure else 0.25))
            params.append(Param(f'a{j}', t, mut))
        varp = any(p.mut for p in params)
        if varp:
            pure = False
        ret = 'Int' if (pure or r.random() < 0.7) else None
        fd = FuncDef(f'f{i}', params, ret, None)
        fd.rec = rec
        ctx = Ctx(fd)
        ctx.pure = pure
        ctx.no_rec = True
        self.push()
        for p in params:
            self.scopes[-1][p.name] = (p.typ, p.mut, p.name)
        meta = {'pure': pure, 'varp': varp, 'rec': rec}
        stmts = []
        if rec:
            stmts.append(If([(Bin('<=', Var('n'), Lit(0)),
                              Block([Return(self.int_expr(2, ctx))]))]))
            nn = self.fresh()
            uid = self.bind(nn, 'Int', False)
            stmts.append(Decl(nn, Bin('-', Var('n'), Lit(r.choice([1, 1, 2]))), uid=uid))
            # recursive calls go through nn so the depth always shrinks
            for _ in range(r.randint(1, 2)):
                t = self.fresh()
                args = [Var(nn, uid)] + [self.expr_of(p.typ, 1, ctx) for p in params[1:]]
                tu = self.bind(t, 'Int', False)
                stmts.append(Decl(t, Call(fd, args), uid=tu))
        for _ in range(r.randint(1, 5)):
            stmts.extend(self.stmt(ctx, 2))
        if ret:
            stmts.append(Return(self.int_expr(3, ctx)))
        self.pop()
        fd.body = Block(stmts)
        self.funcs.append((fd, meta))

    def program(self):
        r = self.r
        self.gen_enums()
        gl = []
        self.push()
        for i in range(r.randint(0, 2)):
            g = f'g{i}'
            gl.append((g, r.randint(-5, 5)))
            self.globals.append(g)
            self.scopes[-1][g] = ('Int', True, g)
        self.gen_top()
        for i in range(r.randint(*self.NFUNCS)):
            self.gen_func(i)
        ctx = Ctx(None)
        self.push()
        main = []
        for _ in range(r.randint(*self.NMAIN)):
            main.extend(self.stmt(ctx, 3))
        for g, _ in gl:
            main.append(Print(Interp([f'{g}=', Var(g)])))
        self.pop()
        self.pop()
        self.gl = gl
        self.mainblock = Block(main)
        return self.render(), self.model()

    def render(self):
        lines = []
        for e in self.enums:
            lines += e.lines() + ['']
        lines += self.top_lines()
        for g, v in self.gl:
            lines.append(f'var {g} = {Lit(v).src()}')
        if self.gl:
            lines.append('')
        for fd, _ in self.funcs:
            lines += fd.lines() + ['']
        lines += ['func main() {'] + self.mainblock.lines('    ') + ['}']
        return '\n'.join(lines) + '\n'

    def model(self):
        gf = Frame()
        for g, v in self.gl:
            gf.vars[g] = Cell(v)
        m = Machine(gf)
        self.mainblock.run(m, Frame(gf, None))
        return m.lines()

    NFUNCS = (1, 6)
    NMAIN = (8, 25)

    def gen_top(self):
        pass

    def param_types(self):
        return ['Int', 'Int', 'Int', 'Bool'] + [e.name for e in self.enums]

    def top_lines(self):
        return []


def generate(rng):
    g = Gen(rng)
    g.program()
    return g
