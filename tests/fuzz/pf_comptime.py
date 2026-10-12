"""Compile-time evaluation programs (manual 8): top-level functions over Int
arithmetic, Bool logic, lists built with `fill` and index assignment (also
lists of lists), `len`, `range`, `for` over a list, object construction,
string literal results and recursion, each evaluated with `comptime` (in a
global `val` or a local) and called again at run time; both results are
printed and must be what the model computes. Some programs instead call,
at compile time, a function that uses a construct section 8 leaves out (a
method call, field access, string concatenation or interpolation, a
lambda, match, an enum value, heap or rc, a global, break) and must fail
with E0231; others run past the step limit (E0230), divide by zero or shift
by a negative count at compile time (E0232). Bitwise operators, shifts and
conversions are evaluated at compile time (#658).

The model is pf_lang's: Int arithmetic wraps (manual 2.1; compile-time
evaluation is the same function, so it wraps too), division truncates."""
from pf_lang import *
from pf_stmt import Scoped


class ObjVal:
    __slots__ = ('x', 'y')

    def __init__(self, x, y):
        self.x, self.y = x, y

    def iron_copy(self):
        return ObjVal(self.x, self.y)


def fill(n, v):
    return ListV([copy_value(v) for _ in range(max(0, n))])


def show(v):
    if isinstance(v, ListV):
        return '[' + ','.join(show(x) for x in v.items) + ']'
    if isinstance(v, ObjVal):
        return f'Pt({v.x},{v.y})'
    return fmt(v)


class Show(Expr):
    """show(e) / show2(e) / "{p.x},{p.y}": a result's text."""

    def __init__(self, e, kind):
        self.e, self.kind = e, kind

    def src(self, parent=0):
        if self.kind == 'list':
            return f'show({self.e.src()})'
        if self.kind == 'grid':
            return f'show2({self.e.src()})'
        if self.kind == 'pt':
            return f'showpt({self.e.src()})'
        return self.e.src(parent)

    def ev(self, m, f):
        return show(self.e.ev(m, f))


class GridAssign(Stmt):
    """g[i][j] op= e."""

    def __init__(self, g, i, j, op, e):
        self.g, self.i, self.j, self.op, self.e = g, i, j, op, e

    def lines(self, ind):
        return [f'{ind}{self.g.src()}[{self.i.src()}][{self.j.src()}] {self.op} {self.e.src()}']

    def run(self, m, f):
        row = self.g.ev(m, f).items[self.i.ev(m, f)]
        j = self.j.ev(m, f)
        v = self.e.ev(m, f)
        row.items[j] = v if self.op == '=' else Bin(self.op[:-1], Lit(row.items[j]), Lit(v)).ev(m, f)


RET = ('Int', 'Int', 'Int', 'Bool', 'list', 'grid', 'pt', 'str')
TYPES = {'Int': 'Int', 'Bool': 'Bool', 'list': '[Int]', 'grid': '[[Int]]', 'pt': 'Pt',
         'str': 'String'}


class CtGen(Scoped):
    def __init__(self, rng):
        super().__init__(rng)
        self.funcs = []          # (FuncDef, kind, meta)
        self.sites = []          # (global name or None, call Expr, kind)
        self.globals_ = []       # (name, Expr) global vals initialized with comptime
        self.lists = {}          # uid -> length (lists in scope)
        self.grids = {}          # uid -> (rows, cols)
        self.bad = None
        self.params = set()      # list parameters (returning one is E0328)
        self.extra = []          # extra top-level lines (an enum, a global)

    # ------------------------------------------------------------ ints
    def ileaf(self):
        r = self.r
        vs = self.names('Int')
        c = r.random()
        if vs and c < 0.55:
            return self.ref(r.choice(vs))
        if c < 0.57:
            return Lit(r.choice([4000000000000000000, 3037000500, -9223372036854775807,
                                 9223372036854775807, 1 << 40]))
        return Lit(r.randint(-20, 50))

    def iexpr(self, d):
        r = self.r
        if d <= 0 or r.random() < 0.3:
            return self.ileaf()
        c = r.random()
        if c < 0.08:
            return Un('-', self.iexpr(d - 1))
        if c < 0.18:
            return Bin(r.choice(['/', '%']), self.iexpr(d - 1), Lit(r.choice([2, 3, 5, 7, -3, -1])))
        if c < 0.26:
            lst = [(n, u) for n, (t, mu, u) in self.visible().items() if u in self.lists]
            if lst:
                n, u = r.choice(lst)
                if r.random() < 0.3:
                    return Builtin('len', [Var(n, u)], lambda v: len(v.items))
                return Index(Var(n, u), self.index_into(self.lists[u]))
        if c < 0.32:
            gs = [(n, u) for n, (t, mu, u) in self.visible().items() if u in self.grids]
            if gs:
                n, u = r.choice(gs)
                rows, cols = self.grids[u]
                return Index(Index(Var(n, u), self.index_into(rows)), self.index_into(cols))
        if c < 0.42:
            fs = [fd for fd, k, meta in self.funcs if k == 'Int' and not meta.get('bad')]
            if fs:
                fd = r.choice(fs)
                return Call(fd, self.args_for(fd))
        if c < 0.46:
            fs = [fd for fd, k, meta in self.funcs if k == 'list' and not meta.get('bad')]
            if fs:
                fd = r.choice(fs)
                return Builtin('len', [Call(fd, self.args_for(fd))], lambda v: len(v.items))
        op = r.choice(['+', '-', '*', '+', '-'])
        return Bin(op, self.iexpr(d - 1), self.iexpr(d - 1))

    def index_into(self, n):
        """An index in 0 .. n-1."""
        r = self.r
        if r.random() < 0.5:
            return Lit(r.randrange(n))
        e = self.iexpr(1)
        # ((e % n) + n) % n
        return Bin('%', Paren(Bin('+', Bin('%', e, Lit(n)), Lit(n))), Lit(n))

    def bexpr(self, d):
        r = self.r
        c = r.random()
        if d <= 0 or c < 0.5:
            bs = self.names('Bool')
            if bs and r.random() < 0.3:
                return self.ref(r.choice(bs))
            return Bin(r.choice(['==', '!=', '<', '>', '<=', '>=']), self.iexpr(1), self.iexpr(1))
        if c < 0.65:
            return Un('not', Paren(self.bexpr(d - 1)))
        if c < 0.75:
            fs = [fd for fd, k, meta in self.funcs if k == 'Bool' and not meta.get('bad')]
            if fs:
                fd = r.choice(fs)
                return Call(fd, self.args_for(fd))
        return Bin(r.choice(['and', 'or']), self.bexpr(d - 1), self.bexpr(d - 1))

    def args_for(self, fd):
        out = []
        for i, p in enumerate(fd.params):
            if i == 0 and getattr(fd, 'rec', False):
                out.append(Lit(self.r.randint(0, 6)))
            elif p.typ == '[Int]':
                out.append(self.list_arg())
            else:
                out.append(self.iexpr(1))
        return out

    def list_arg(self):
        r = self.r
        fs = [fd for fd, k, meta in self.funcs if k == 'list' and not meta.get('bad')]
        if fs and r.random() < 0.4:
            fd = r.choice(fs)
            return Call(fd, self.args_for(fd))
        items = [Lit(r.randint(-9, 30)) for _ in range(r.randint(1, 5))]
        return Raw('[' + ', '.join(i.src() for i in items) + ']',
                   lambda m, f, items=items: ListV([i.v for i in items]))

    # ------------------------------------------------------------ statements
    def block(self, ctx, n, depth):
        self.push()
        out = []
        for _ in range(n):
            out.extend(self.stmt(ctx, depth))
        self.pop()
        return Block(out)

    def stmt(self, ctx, depth):
        r = self.r
        w = [('decl', 5), ('assign', 6 if self.names('Int', True) else 0)]
        lists = [(n, u) for n, (t, mu, u) in self.visible().items() if u in self.lists and mu]
        grids = [(n, u) for n, (t, mu, u) in self.visible().items() if u in self.grids and mu]
        if lists:
            w.append(('lset', 5))
        if grids:
            w.append(('gset', 5))
        if depth > 0:
            w += [('if', 4), ('while', 2), ('for', 3), ('forin', 1 if lists else 0)]
        if ctx['ret'] is not None and depth < 2:
            w.append(('ret', 1))
        k = self.pick(w)
        return getattr(self, 's_' + k)(ctx, depth, lists, grids)

    def s_decl(self, ctx, depth, lists, grids):
        r = self.r
        name = self.fresh('v')
        c = r.random()
        if c < 0.12:
            n = r.randint(1, 6)
            e = Builtin('fill', [Lit(n), self.iexpr(1)], fill)
            uid = self.bind(name, '[Int]', True)
            self.lists[uid] = n
            return [Decl(name, e, True, uid=uid)]
        if c < 0.17:
            rows, cols = r.randint(1, 4), r.randint(1, 4)
            inner = Builtin('fill', [Lit(cols), self.iexpr(0)], fill)
            e = Builtin('fill', [Lit(rows), inner], fill)
            uid = self.bind(name, '[[Int]]', True)
            self.grids[uid] = (rows, cols)
            return [Decl(name, e, True, uid=uid)]
        if c < 0.22:
            items = [self.iexpr(1) for _ in range(r.randint(1, 4))]
            e = Raw('[' + ', '.join(i.src() for i in items) + ']',
                    lambda m, f, items=items: ListV([i.ev(m, f) for i in items]))
            uid = self.bind(name, '[Int]', True)
            self.lists[uid] = len(items)
            return [Decl(name, e, True, uid=uid)]
        if c < 0.32:
            e = self.bexpr(2)
            mut = r.random() < 0.3
            uid = self.bind(name, 'Bool', mut)
            return [Decl(name, e, mut, uid=uid)]
        mut = r.random() < 0.6
        e = self.iexpr(3)
        uid = self.bind(name, 'Int', mut)
        if mut and r.random() < 0.15:
            # declared without a value, assigned on the next line
            return [DeclNoInit(name, uid), Assign(name, '=', e, uid=uid)]
        return [Decl(name, e, mut, uid=uid)]

    def s_assign(self, ctx, depth, lists, grids):
        r = self.r
        n = r.choice(self.names('Int', True))
        op = r.choice(['=', '+=', '-=', '*=', '/=', '+='])
        e = Lit(r.choice([2, 3, -2, 7])) if op == '/=' else self.iexpr(2)
        return [Assign(n, op, e, uid=self.uid(n))]

    def s_lset(self, ctx, depth, lists, grids):
        n, u = self.r.choice(lists)
        op = self.r.choice(['=', '+=', '-=', '*='])
        return [IndexAssign(Index(Var(n, u), self.index_into(self.lists[u])), op, self.iexpr(2))]

    def s_gset(self, ctx, depth, lists, grids):
        n, u = self.r.choice(grids)
        rows, cols = self.grids[u]
        op = self.r.choice(['=', '+=', '-='])
        return [GridAssign(Var(n, u), self.index_into(rows), self.index_into(cols), op,
                           self.iexpr(2))]

    def s_if(self, ctx, depth, lists, grids):
        r = self.r
        arms = [(self.bexpr(2), self.block(ctx, r.randint(1, 3), depth - 1))
                for _ in range(r.choice([1, 1, 2]))]
        els = self.block(ctx, r.randint(1, 2), depth - 1) if r.random() < 0.5 else None
        return [If(arms, els)]

    def s_while(self, ctx, depth, lists, grids):
        r = self.r
        cnt = self.fresh('w')
        uid = self.bind(cnt, 'Int', False)
        lim = r.randint(0, 6)
        self.push()
        body = [Assign(cnt, '+=', Lit(1), uid=uid)]
        for _ in range(r.randint(1, 3)):
            body.extend(self.stmt(ctx, depth - 1))
        self.pop()
        return [Decl(cnt, Lit(0), True, uid=uid), While(Bin('<', Var(cnt, uid), Lit(lim)),
                                                         Block(body))]

    def s_for(self, ctx, depth, lists, grids):
        r = self.r
        name = self.fresh('i')
        n = Lit(r.randint(0, 6)) if r.random() < 0.7 else Bin('%', self.iexpr(1), Lit(5))
        self.push()
        uid = self.bind(name, 'Int', False)
        body = self.block(ctx, r.randint(1, 3), depth - 1)
        self.pop()
        return [ForRange(name, n, body, uid=uid)]

    def s_forin(self, ctx, depth, lists, grids):
        r = self.r
        ln, lu = r.choice(lists)
        name = self.fresh('x')
        self.push()
        uid = self.bind(name, 'Int', False)
        # the body reads the list but does not write it
        saved = {u: self.lists.pop(u) for n2, u in lists}
        body = self.block(ctx, r.randint(1, 2), depth - 1)
        self.lists.update(saved)
        self.pop()
        return [ForIn(name, Var(ln, lu), body, uid=uid)]

    def s_ret(self, ctx, depth, lists, grids):
        return [If([(self.bexpr(1), Block([Return(self.ret_expr(ctx))]))])]

    def ret_expr(self, ctx):
        k = ctx['ret']
        r = self.r
        if k == 'Int':
            return self.iexpr(3)
        if k == 'Bool':
            return self.bexpr(2)
        if k == 'str':
            return Lit(r.choice(['small', 'big', '', 'x y', 'tab\there', 'é']))
        if k == 'pt':
            a, b = self.iexpr(2), self.iexpr(2)
            return Raw(f'Pt({a.src()}, {b.src()})',
                       lambda m, f, a=a, b=b: ObjVal(a.ev(m, f), b.ev(m, f)))
        if k == 'list':
            ls = [(n, u) for n, (t, mu, u) in self.visible().items()
                  if u in self.lists and u not in self.params]
            if ls and r.random() < 0.8:
                n, u = r.choice(ls)
                return Var(n, u)
            items = [self.iexpr(1) for _ in range(r.randint(1, 3))]
            return Raw('[' + ', '.join(i.src() for i in items) + ']',
                       lambda m, f, items=items: ListV([i.ev(m, f) for i in items]))
        gs = [(n, u) for n, (t, mu, u) in self.visible().items() if u in self.grids]
        if gs:
            n, u = r.choice(gs)
            return Var(n, u)
        return Builtin('fill', [Lit(2), Builtin('fill', [Lit(2), self.iexpr(1)], fill)], fill)

    # ------------------------------------------------------------ functions
    def gen_func(self, i):
        r = self.r
        kind = r.choice(RET)
        rec = kind == 'Int' and r.random() < 0.3
        params = []
        if rec:
            params.append(Param('n', 'Int'))
        for j in range(r.randint(0 if not rec else 0, 2)):
            params.append(Param(f'a{j}', '[Int]' if r.random() < 0.15 else 'Int'))
        fd = FuncDef(f'f{i}', params, TYPES[kind], None)
        fd.rec = rec
        ctx = {'ret': kind, 'fn': fd}
        self.push()
        for p in params:
            self.scopes[-1][p.name] = (p.typ, False, p.name)
            if p.typ == '[Int]':
                self.lists[p.name] = 1     # at least one element: index 0 only
                self.params.add(p.name)
        stmts = []
        if rec:
            stmts.append(If([(Bin('<=', self.ref('n'), Lit(0)),
                              Block([Return(self.iexpr(1))]))]))
        for _ in range(r.randint(1, 5)):
            stmts.extend(self.stmt(ctx, 2))
        if rec:
            t = self.fresh('v')
            args = [Bin('-', self.ref('n'), Lit(1))] + \
                [self.list_arg() if p.typ == '[Int]' else self.iexpr(1) for p in params[1:]]
            tu = self.bind(t, 'Int', False)
            stmts.append(Decl(t, Call(fd, args), uid=tu))
            stmts.append(Return(Bin(r.choice(['+', '-', '*']), Var(t, tu), self.iexpr(1))))
        else:
            stmts.append(Return(self.ret_expr(ctx)))
        self.pop()
        for p in params:
            self.lists.pop(p.name, None)
        self.params = set()
        fd.body = Block(stmts)
        self.funcs.append((fd, kind, {}))

    def program(self):
        r = self.r
        self.push()
        for i in range(r.randint(2, 6)):
            self.gen_func(i)
        main = []
        for k in range(r.randint(3, 8)):
            fd, kind, _ = r.choice(self.funcs)
            self.push()
            args = self.args_for(fd)
            if fd.rec and r.random() < 0.4:
                args[0] = Lit(r.choice([30, 60, 120, 200]))
            self.pop()
            call = Call(fd, args)
            if r.random() < 0.7:
                g = f'C{k}'
                self.globals_.append((g, call))
                ct = Var(g)
            else:
                name = self.fresh('c')
                main.append(Decl(name, Raw(f'comptime {call.src()}',
                                           lambda m, f, c=call: c.ev(m, f), UNARY)))
                ct = Var(name)
            main.append(Print(Interp([f'c{k} ', Show(ct, kind), ' ', Show(call, kind)])))
        if r.random() < 0.4:
            self.push()
            e = self.iexpr(3)
            self.pop()
            main.append(Print(Interp(['e ', Raw(f'comptime ({e.src()})',
                                                lambda m, f, e=e: e.ev(m, f), UNARY)])))
        self.pop()
        self.mainblock = Block(main)
        if r.random() < 0.15:
            self.make_bad()

    # ------------------------------------------------------------ boundary
    def make_bad(self):
        """Replace the program with one whose compile-time call reaches a
        construct section 8 rules out."""
        r = self.r
        k = r.choice(['method', 'field', 'concat', 'interp', 'match', 'lambda',
                      'enum', 'heap', 'rc', 'global', 'break', 'steps', 'div0',
                      'negshift'])
        body = {
            'method': ['var xs = fill(2, 0)', 'xs.push(n)', 'return len(xs)'],
            'field': ['val p = Pt(n, 2)', 'return p.x'],
            'concat': ['val s = "a" + "b"', 'return len(s) + n'],
            'interp': ['val s = "n={n}"', 'return len(s)'],
            'negshift': ['return 1 << (n - 9)'],
            'match': ['match n {', '    1 -> return 5', '    else -> return 6', '}', 'return 0'],
            'lambda': ['val f = func(x: Int) -> Int { return x + 1 }', 'return f(n)'],
            'enum': ['val c = Shade.Dark', 'if c == Shade.Dark {', '    return 1', '}',
                     'return n'],
            'heap': ['val h = heap Pt(n, 1)', 'defer free h', 'return n'],
            'rc': ['val h = rc Pt(n, 1)', 'return n'],
            'global': ['return n + BASE'],
            'break': ['var s = 0', 'for i in range(10) {', '    if i == n {', '        break',
                      '    }', '    s += i', '}', 'return s'],
            'steps': ['var i = 0', 'while i < 2000000 {', '    i += 1', '}', 'return i + n'],
            'div0': ['val z = n - n', 'return 10 / z'],
        }[k]
        codes = {'steps': {'E0230'}, 'div0': {'E0232'}, 'negshift': {'E0232'}}.get(k, {'E0231'})
        self.extra = ['enum Shade {', '    Light,', '    Dark,', '}', '', 'val BASE = 7', '']
        lines = ['func bad(n: Int) -> Int {'] + ['    ' + l for l in body] + ['}', '']
        via = r.random() < 0.4
        if via:
            lines += ['func via(n: Int) -> Int {', '    return bad(n + 1) * 2', '}', '']
        self.bad_lines = lines
        target = 'via' if via else 'bad'
        call = f'comptime {target}({r.randint(0, 3)})'
        self.bad_site = call
        self.bad = codes

    # ------------------------------------------------------------ output
    def render(self):
        lines = ['object Pt {', '    val x: Int', '    val y: Int', '}', '',
                 'func show(xs: [Int]) -> String {', '    var s = "["',
                 '    for i in range(len(xs)) {', '        if i > 0 {', '            s += ","',
                 '        }', '        s += "{xs[i]}"', '    }', '    return s + "]"', '}', '',
                 'func show2(g: [[Int]]) -> String {', '    var s = "["',
                 '    for i in range(len(g)) {', '        if i > 0 {', '            s += ","',
                 '        }', '        s += show(g[i])', '    }', '    return s + "]"', '}', '',
                 'func showpt(p: Pt) -> String {', '    return "Pt({p.x},{p.y})"', '}', '']
        lines += self.extra
        if self.bad:
            lines += self.bad_lines
        for fd, _, _ in self.funcs:
            lines += fd.lines() + ['']
        for g, call in self.globals_:
            lines.append(f'val {g} = comptime {call.src()}')
        if self.bad:
            lines.append(f'val BAD = {self.bad_site}')
        lines += ['', 'func main() {'] + self.mainblock.lines('    ')
        if self.bad:
            lines.append('    println("{BAD}")')
        lines.append('}')
        return '\n'.join(lines) + '\n'

    def model(self):
        if self.bad:
            return Expected([], errors=self.bad)
        gf = Frame()
        m = Machine(gf, budget=300000)
        for g, call in self.globals_:
            gf.vars[g] = Cell(call.ev(m, Frame(gf, None)))
        self.mainblock.run(m, Frame(gf, None))
        return Expected(m.lines())


class DeclNoInit(Stmt):
    """var x: Int, assigned by the next statement (manual 4.2)."""

    def __init__(self, name, uid):
        self.name, self.uid = name, uid

    def lines(self, ind):
        return [f'{ind}var {self.name}: Int']

    def run(self, m, f):
        f.vars[self.uid] = Cell(None)


def generate(rng):
    g = CtGen(rng)
    g.program()
    g.model()
    return g
