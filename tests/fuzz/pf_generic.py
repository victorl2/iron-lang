"""Generic programs (manual 2.8, 5.6, 9.10): generic objects (`Cell[T]`
with an init and methods, the positional `Pair[A, B]`, `Stack[T]` over a
list, `Counter[K: Hashable]` over a map), generic enums with payloads
(`Opt[T]`, `Res[T, E]`) matched with bindings, generic functions with and
without constraints (`ident`, `pick`, `first_or`, `unwrap_or`, `swap`,
`show`, `distinct`, `tally`, `count_of`, `find_idx`, `apply` and `fold`
with lambdas, `res_text`), instantiated with Int, String, Bool, an object
key (`Pt impl Hashable`), other instances (`Cell[Pair[Int, String]]`,
`Opt[Cell[Int]]`), lists, `rc Pt` and `rc Cell[Int]`; type arguments
inferred from arguments and written out (`Cell[Int](3)`, `Stack[Pt]()`);
Map and Set with generic keys and values. A `Seeded` constraint supplies a
static function returning `Self`, called as `T.seed(n)` from generic code.
Mixed with the control flow of pf_stmt.

The model follows manual 2.6 / 6.1 (objects are values: binding, passing,
storing and returning copy them; rc handles share one object) and 3.3
(text forms: an object through its to_string, an enum by its variant
name). The generator keeps to the instantiations the compiler accepts
and reports where it does not (see the notes on readonly results and
lists as type arguments in the report)."""
from pf_lang import *
from pf_lang import _cmp
from pf_stmt import Gen, Ctx


# ---------------------------------------------------------------- values

class PtV:
    __slots__ = ('x', 'y')

    def __init__(self, x, y):
        self.x, self.y = x, y

    def __eq__(self, o):
        return isinstance(o, PtV) and (self.x, self.y) == (o.x, o.y)

    def __hash__(self):
        return hash((self.x, self.y))


class CellV:
    __slots__ = ('v',)

    def __init__(self, v):
        self.v = v


class PairV:
    __slots__ = ('a', 'b')

    def __init__(self, a, b):
        self.a, self.b = a, b


class EV:
    __slots__ = ('var', 'p')

    def __init__(self, var, p=None):
        self.var, self.p = var, p


class StackV:
    __slots__ = ('items',)

    def __init__(self, items):
        self.items = items


class CounterV:
    __slots__ = ('d',)

    def __init__(self, d):
        self.d = d


class RcV:
    __slots__ = ('obj',)

    def __init__(self, obj):
        self.obj = obj


_COPY_BUDGET = [0]


def vcopy(v):
    # a program whose values keep doubling (a list pushed copies of
    # itself in a loop) is dropped, not modelled for ever
    _COPY_BUDGET[0] += 1
    if _COPY_BUDGET[0] > 300000:
        raise Bail('copy budget')
    if isinstance(v, CellV):
        return CellV(vcopy(v.v))
    if isinstance(v, PairV):
        return PairV(vcopy(v.a), vcopy(v.b))
    if isinstance(v, EV):
        return EV(v.var, vcopy(v.p))
    if isinstance(v, ListV):
        return ListV([vcopy(x) for x in v.items])
    if isinstance(v, StackV):
        return StackV([vcopy(x) for x in v.items])
    if isinstance(v, MapV):
        return MapV({k: vcopy(x) for k, x in v.d.items()})
    if isinstance(v, SetV):
        return SetV(set(v.s))
    if isinstance(v, CounterV):
        return CounterV(dict(v.d))
    return v         # Int, String, Bool, Pt (immutable), rc handles (shared)


def text(v):
    if isinstance(v, bool):
        return 'true' if v else 'false'
    if isinstance(v, (int, str)):
        return str(v)
    if isinstance(v, PtV):
        return f'Pt({v.x},{v.y})'
    if isinstance(v, CellV):
        return f'Cell({text(v.v)})'
    if isinstance(v, PairV):
        return f'({text(v.a)}, {text(v.b)})'
    if isinstance(v, EV):
        return v.var
    if isinstance(v, RcV):
        return text(v.obj)
    raise Bail(f'no text for {v!r}')


# ---------------------------------------------------------------- types
# A type is a tuple: ('Int',), ('Cell', T), ('Pair', A, B), ('Opt', T),
# ('Res', T, E), ('List', T), ('Stack', T), ('Map', K, V), ('Set', K),
# ('Counter', K), ('RcPt',), ('RcCell',) (rc Cell[Int]).

I, S, B, P = ('Int',), ('String',), ('Bool',), ('Pt',)
RCPT, RCCELL = ('RcPt',), ('RcCell',)
KEYS = (I, S, P)


def ts(t):
    k = t[0]
    if k in ('Int', 'String', 'Bool', 'Pt'):
        return k
    if k == 'RcPt':
        return 'rc Pt'
    if k == 'RcCell':
        return 'rc Cell[Int]'
    if k == 'List':
        return f'[{ts(t[1])}]'
    return f'{k}[' + ', '.join(ts(a) for a in t[1:]) + ']'


def showable(t):
    k = t[0]
    if k in ('Int', 'String', 'Bool', 'Pt', 'RcPt', 'RcCell', 'Opt', 'Res'):
        return True
    if k in ('Cell', 'Pair'):
        return all(showable(a) for a in t[1:])
    return False


def ro_ok(t):
    """A value a readonly method may return (Cell.get): no rc, no list
    (E0280, not in the manual)."""
    k = t[0]
    if k in ('Int', 'String', 'Bool', 'Pt'):
        return True
    if k in ('Cell', 'Pair', 'Opt', 'Res'):
        return all(ro_ok(a) for a in t[1:])
    return False


PROLOGUE = '''interface Seeded {
    static func seed(n: Int) -> Self
}

object Pt impl Hashable, Seeded {
    val x: Int
    val y: Int
    static func seed(n: Int) -> Self {
        return Pt(n, n + 1)
    }
    pure func hash() -> Int {
        return self.x * 31 + self.y
    }
    pure func equals(other: Hashable) -> Bool {
        if other is Pt {
            return self.x == other.x and self.y == other.y
        }
        return false
    }
    readonly func to_string() -> String {
        return "Pt({self.x},{self.y})"
    }
    readonly func with[T](x: T) -> Pair[Pt, T] {
        return Pair(self, x)
    }
}

object Cell[T] {
    var v: T
    init(v: T) {
        self.v = v
    }
    readonly func get() -> T {
        return self.v
    }
    func set(x: T) {
        self.v = x
    }
    readonly func to_string() -> String {
        return "Cell({self.v})"
    }
    readonly func pair_with[U](u: U) -> Pair[T, U] {
        return Pair(self.v, u)
    }
}

object Pair[A, B] {
    val first: A
    val second: B
    readonly func to_string() -> String {
        return "({self.first}, {self.second})"
    }
}

object Stack[T] {
    var items: [T]
    init() {
        self.items = []
    }
    func push(x: T) {
        self.items.push(x)
    }
    func pop() -> T {
        return self.items.pop()
    }
    readonly func size() -> Int {
        return len(self.items)
    }
}

object Counter[K: Hashable] {
    var m: Map[K, Int]
    init() {
        self.m = Map[K, Int]()
    }
    func add(k: K) {
        self.m.put(k, self.m.get_or(k, 0) + 1)
    }
    readonly func count(k: K) -> Int {
        return self.m.get_or(k, 0)
    }
    readonly func kinds() -> Int {
        return self.m.len()
    }
}

enum Opt[T] {
    Some(T),
    None,
}

enum Res[T, E] {
    Ok(T),
    Err(E),
}

func ident[T](x: T) -> T {
    return x
}

func seeded[T: Seeded](n: Int) -> T {
    return T.seed(n)
}

func pick[T](c: Bool, a: T, b: T) -> T {
    if c {
        return a
    }
    return b
}

func first_or[T](xs: [T], d: T) -> T {
    if len(xs) == 0 {
        return d
    }
    return xs[0]
}

func show[T](x: T) -> String {
    return "<{x}>"
}

func swap[A, B](p: Pair[A, B]) -> Pair[B, A] {
    return Pair(p.second, p.first)
}

func distinct[K: Hashable](xs: [K]) -> Int {
    var s = Set[K]()
    for x in xs {
        s.add(x)
    }
    return s.len()
}

func tally[K: Hashable](xs: [K]) -> Map[K, Int] {
    var m = Map[K, Int]()
    for x in xs {
        m.put(x, m.get_or(x, 0) + 1)
    }
    return m
}

func count_of[K: Hashable](xs: [K], k: K) -> Int {
    val m = tally(xs)
    return m.get_or(k, 0)
}

func unwrap_or[T](o: Opt[T], d: T) -> T {
    match o {
        Opt.Some(v) -> return v
        Opt.None -> return d
    }
    return d
}

func find_idx[K: Hashable](xs: [K], k: K) -> Opt[Int] {
    var s = Set[K]()
    s.add(k)
    for i in range(len(xs)) {
        if s.has(xs[i]) {
            return Opt.Some(i)
        }
    }
    return Opt.None
}

func apply[T](f: func(T) -> T, x: T, n: Int) -> T {
    var r = x
    for i in range(n) {
        r = f(r)
    }
    return r
}

func fold[T](xs: [T], start: T, f: func(T, T) -> T) -> T {
    var acc = start
    for x in xs {
        acc = f(acc, x)
    }
    return acc
}

func res_text[T, E](r: Res[T, E]) -> String {
    match r {
        Res.Ok(v) -> return "ok {v}"
        Res.Err(e) -> return "err {e}"
    }
    return ""
}
'''


def m_unwrap_or(o, d):
    return vcopy(o.p) if o.var == 'Some' else d


def m_find_idx(xs, k):
    for i, x in enumerate(xs.items):
        if x == k:
            return EV('Some', i)
    return EV('None')


def m_tally(xs):
    d = {}
    for x in xs.items:
        d[x] = d.get(x, 0) + 1
    return MapV(d)


def m_res_text(r):
    return ('ok ' if r.var == 'Ok' else 'err ') + text(r.p)


class Ev(Expr):
    def __init__(self, text_, fn):
        self.text, self.fn = text_, fn

    def src(self, parent=0):
        return self.text

    def ev(self, m, f):
        return self.fn(m, f)


class Do(Stmt):
    def __init__(self, text_, fn):
        self.text, self.fn = text_, fn

    def lines(self, ind):
        return [ind + t for t in (self.text if isinstance(self.text, list) else [self.text])]

    def run(self, m, f):
        self.fn(m, f)


def ev_all(es, m, f):
    return [e.ev(m, f) for e in es]


class GenGen(Gen):
    NFUNCS = (0, 2)
    NMAIN = (10, 24)

    def __init__(self, rng):
        super().__init__(rng)
        self.tmap = {}         # type source text -> tuple

    # ------------------------------------------------------------ types
    def rtype(self, d=2, show=False, ro=False, key=False, top=False):
        """A type; Res only at the top (its error type comes from a
        binding's annotation, see s_gdecl)."""
        r = self.r
        if key:
            return r.choice(KEYS)
        c = r.random()
        if 0.82 <= c < 0.88 and not top:
            c = 0.7
        if d <= 0 or c < 0.45:
            t = r.choice([I, I, S, S, B, P])
        elif c < 0.6:
            t = ('Cell', self.rtype(d - 1, show, True))
        elif c < 0.72:
            t = ('Pair', self.rtype(d - 1, show, ro), self.rtype(d - 1, show, ro))
        elif c < 0.82:
            t = ('Opt', self.rtype(d - 1, show, ro))
        elif c < 0.88:
            t = ('Res', self.rtype(d - 1, show, ro), self.rtype(d - 1, show, ro))
        elif c < 0.93 and not ro:
            t = RCPT
        elif not ro:
            t = RCCELL
        else:
            t = I
        return t

    def bind_t(self, name, t, mut):
        s = ts(t)
        self.tmap[s] = t
        return self.bind(name, s, mut)

    def vars_of(self, t, mut=None):
        return self.names(ts(t), mut)

    def vars_where(self, pred, mut=None):
        out = []
        for n, (s, mu, u) in self.visible().items():
            t = self.tmap.get(s)
            if t is not None and pred(t) and (mut is None or mu == mut):
                out.append((n, t, u))
        return out

    # ------------------------------------------------------------ expressions
    def gref(self, name):
        u = self.uid(name)
        return Ev(name, lambda m, f: f.look(u).v)

    def ilit(self):
        return self.r.randint(-9, 30)

    def gexpr(self, t, d):
        """An expression of type t. Values are copied by the consumers."""
        r = self.r
        k = t[0]
        vs = self.vars_of(t)
        if vs and (d <= 0 or r.random() < 0.35):
            return self.gref(r.choice(vs))
        if d <= 0:
            return self.leaf(t, 0)
        c = r.random()
        if c < 0.12:
            e = self.gexpr(t, d - 1)
            return Ev(f'ident({e.src()})', lambda m, f: vcopy(e.ev(m, f)))
        if c < 0.2:
            cb, a, b = self.bexpr(1), self.gexpr(t, d - 1), self.gexpr(t, d - 1)
            return Ev(f'pick({cb.src()}, {a.src()}, {b.src()})',
                      lambda m, f: vcopy(a.ev(m, f) if cb.ev(m, f) else b.ev(m, f)))
        if c < 0.26 and k not in ('RcCell',):
            xs, dflt = self.lexpr(t, d - 1), self.gexpr(t, d - 1)
            return Ev(f'first_or({xs.src()}, {dflt.src()})',
                      lambda m, f: vcopy((lambda l, dv: l.items[0] if l.items else dv)(
                          xs.ev(m, f), dflt.ev(m, f))))
        if c < 0.32:
            o, dflt = self.gexpr(('Opt', t), d - 1), self.gexpr(t, d - 1)
            return Ev(f'unwrap_or({o.src()}, {dflt.src()})',
                      lambda m, f: m_unwrap_or(o.ev(m, f), dflt.ev(m, f)))
        if c < 0.4:
            got = self.from_container(t, d)
            if got is not None:
                return got
        return self.leaf(t, d)

    def from_container(self, t, d):
        """t read out of a Cell, a Pair or an rc Cell in scope."""
        r = self.r
        cs = self.vars_where(lambda x: x[0] == 'Cell' and x[1] == t)
        ps = self.vars_where(lambda x: x[0] == 'Pair' and t in x[1:])
        opts = []
        for n, ct, u in cs:
            opts.append(Ev(f'{n}.get()' if r.random() < 0.7 else f'{n}.v',
                           (lambda u: lambda m, f: vcopy(f.look(u).v.v))(u)))
        for n, pt, u in ps:
            if pt[1] == t:
                opts.append(Ev(f'{n}.first', (lambda u: lambda m, f: vcopy(f.look(u).v.a))(u)))
            if pt[2] == t:
                opts.append(Ev(f'{n}.second', (lambda u: lambda m, f: vcopy(f.look(u).v.b))(u)))
        if t == I:
            for n, _, u in self.vars_where(lambda x: x == RCCELL):
                opts.append(Ev(f'{n}.get()', (lambda u: lambda m, f: f.look(u).v.obj.v)(u)))
        return r.choice(opts) if opts else None

    def leaf(self, t, d=1):
        r = self.r
        k = t[0]
        if k == 'Int':
            return self.iexpr(d)
        if k == 'String':
            c = r.random()
            if c < 0.4 or d <= 0:
                s = r.choice(['a', 'b', 'xy', '', 'é', 'Z z'])
                return Ev(iron_str(s), lambda m, f: s)
            if c < 0.6:
                t2 = self.rtype(1, show=True)
                if showable(t2):
                    e = self.gexpr(t2, d - 1)
                    return Ev(f'show({e.src()})', lambda m, f: '<' + text(e.ev(m, f)) + '>')
            rs = self.vars_where(lambda x: x[0] == 'Res')
            if c < 0.7 and rs:
                n, rt, u = r.choice(rs)
                return Ev(f'res_text({n})', lambda m, f: m_res_text(f.look(u).v))
            if c < 0.8:
                xs, st = self.lexpr(S, d - 1), self.gexpr(S, d - 1)
                return Ev(f'fold({xs.src()}, {st.src()}, func(a: String, b: String) -> String '
                          f'{{ return a + b }})',
                          lambda m, f: _short(st.ev(m, f) + ''.join(xs.ev(m, f).items)))
            a, b = self.gexpr(S, d - 1), self.gexpr(S, d - 1)
            return Ev(f'({a.src()} + {b.src()})', lambda m, f: _short(a.ev(m, f) + b.ev(m, f)))
        if k == 'Bool':
            return self.bexpr(d)
        if k == 'Pt':
            a = self.iexpr(min(d, 1))
            if r.random() < 0.3:
                return Ev(f'seeded[Pt]({a.src()})',
                          lambda m, f: (lambda x: PtV(x, wrap(x + 1)))(a.ev(m, f)))
            b = self.iexpr(min(d, 1))
            return Ev(f'Pt({a.src()}, {b.src()})', lambda m, f: PtV(a.ev(m, f), b.ev(m, f)))
        if k == 'Cell':
            e = self.gexpr(t[1], d - 1)
            ctor = 'Cell' if r.random() < 0.6 else f'Cell[{ts(t[1])}]'
            return Ev(f'{ctor}({e.src()})', lambda m, f: CellV(vcopy(e.ev(m, f))))
        if k == 'Pair':
            # generic methods: Cell[A].pair_with(b), Pt.with(b)
            if d > 0 and r.random() < 0.3 and ro_ok(t[2]):
                cs = self.vars_where(lambda x: x == ('Cell', t[1]))
                if cs:
                    n, _, u = r.choice(cs)
                    b = self.gexpr(t[2], d - 1)
                    return Ev(f'{n}.pair_with({b.src()})',
                              lambda m, f: PairV(vcopy(f.look(u).v.v), vcopy(b.ev(m, f))))
                if t[1] == P:
                    a, b = self.gexpr(P, d - 1), self.gexpr(t[2], d - 1)
                    return Ev(f'{a.src(POSTFIX)}.with({b.src()})',
                              lambda m, f: PairV(a.ev(m, f), vcopy(b.ev(m, f))))
            if d > 0 and r.random() < 0.2:
                e = self.gexpr(('Pair', t[2], t[1]), d - 1)
                return Ev(f'swap({e.src()})',
                          lambda m, f: (lambda p: PairV(vcopy(p.b), vcopy(p.a)))(e.ev(m, f)))
            a, b = self.gexpr(t[1], d - 1), self.gexpr(t[2], d - 1)
            return Ev(f'Pair({a.src()}, {b.src()})',
                      lambda m, f: PairV(vcopy(a.ev(m, f)), vcopy(b.ev(m, f))))
        if k == 'Opt':
            if t[1] == I and d > 0 and r.random() < 0.3:
                kt = r.choice(KEYS)
                xs, kk = self.lexpr(kt, d - 1), self.gexpr(kt, d - 1)
                return Ev(f'find_idx({xs.src()}, {kk.src()})',
                          lambda m, f: m_find_idx(xs.ev(m, f), kk.ev(m, f)))
            e = self.gexpr(t[1], d - 1)
            return Ev(f'Opt.Some({e.src()})', lambda m, f: EV('Some', vcopy(e.ev(m, f))))
        if k == 'Res':
            # only where a binding names both type arguments (see s_gdecl)
            vs = self.vars_of(t)
            if vs:
                return self.gref(r.choice(vs))
            raise Bail('Res needs a binding')
        if k == 'List':
            return self.lexpr(t[1], d)
        if k == 'RcPt':
            a, b = self.iexpr(0), self.iexpr(0)
            return Ev(f'rc Pt({a.src()}, {b.src()})', lambda m, f: RcV(PtV(a.ev(m, f), b.ev(m, f))))
        if k == 'RcCell':
            e = self.iexpr(0)
            return Ev(f'rc Cell({e.src()})', lambda m, f: RcV(CellV(e.ev(m, f))))
        raise ValueError(t)

    def lexpr(self, et, d):
        """A fresh list of et (never a list binding: E0328)."""
        r = self.r
        lv = self.vars_of(('List', et))
        if lv and r.random() < 0.4:
            n = r.choice(lv)
            u = self.uid(n)
            return Ev(f'{n}.copy()', lambda m, f: vcopy(f.look(u).v))
        items = [self.gexpr(et, max(d - 1, 0)) for _ in range(r.randint(1, 3))]
        return Ev('[' + ', '.join(i.src() for i in items) + ']',
                  lambda m, f: ListV([vcopy(i.ev(m, f)) for i in items]))

    def iexpr(self, d):
        r = self.r
        ctx = Ctx(None).sub(pure=True, no_rec=True)
        c = r.random()
        if d <= 0 or c < 0.3:
            ie = self.int_expr(1, ctx)
            return Ev(ie.src(UNARY), lambda m, f: ie.ev(m, f))
        if c < 0.4:
            kt = r.choice(KEYS)
            xs = self.lexpr(kt, d - 1)
            return Ev(f'distinct({xs.src()})', lambda m, f: len(set(xs.ev(m, f).items)))
        if c < 0.5:
            kt = r.choice(KEYS)
            xs, kk = self.lexpr(kt, d - 1), self.gexpr(kt, d - 1)
            return Ev(f'count_of({xs.src()}, {kk.src()})',
                      lambda m, f: xs.ev(m, f).items.count(kk.ev(m, f)))
        if c < 0.58:
            x, n = self.iexpr(d - 1), r.randint(0, 4)
            k = r.choice([2, 3, -1])
            return Ev(f'apply(func(v: Int) -> Int {{ return v * {k} + 1 }}, {x.src()}, {n})',
                      lambda m, f: _apply(x.ev(m, f), n, k))
        if c < 0.66:
            xs, st = self.lexpr(I, d - 1), self.iexpr(d - 1)
            return Ev(f'fold({xs.src()}, {st.src()}, func(a: Int, b: Int) -> Int '
                      f'{{ return a - b }})',
                      lambda m, f: _fold_sub(st.ev(m, f), xs.ev(m, f).items))
        if c < 0.74:
            sized = self.vars_where(lambda t: t[0] in ('List', 'Stack', 'Map', 'Set', 'Counter'))
            if sized:
                n, t, u = r.choice(sized)
                if t[0] == 'List':
                    return Ev(f'len({n})', lambda m, f: len(f.look(u).v.items))
                if t[0] == 'Stack':
                    return Ev(f'{n}.size()', lambda m, f: len(f.look(u).v.items))
                if t[0] == 'Counter':
                    kk = self.gexpr(t[1], d - 1)
                    return Ev(f'{n}.count({kk.src()})',
                              lambda m, f: f.look(u).v.d.get(kk.ev(m, f), 0))
                if t[0] == 'Map' and t[2] == I and r.random() < 0.6:
                    kk = self.gexpr(t[1], d - 1)
                    dv = self.iexpr(0)
                    return Ev(f'{n}.get_or({kk.src()}, {dv.src()})',
                              lambda m, f: f.look(u).v.d.get(kk.ev(m, f), dv.ev(m, f)))
                return Ev(f'{n}.len()', lambda m, f: _clen(f.look(u).v))
        if c < 0.8:
            kt = r.choice(KEYS)
            xs, kk = self.lexpr(kt, d - 1), self.gexpr(kt, d - 1)
            return Ev(f'tally({xs.src()}).get_or({kk.src()}, 0)',
                      lambda m, f: xs.ev(m, f).items.count(kk.ev(m, f)))
        got = self.from_container(I, d)
        if got is not None:
            return got
        a, b = self.iexpr(d - 1), self.iexpr(d - 1)
        op = r.choice(['+', '-', '*'])
        return Ev(f'({a.src()} {op} {b.src()})',
                  lambda m, f: wrap({'+': lambda x, y: x + y, '-': lambda x, y: x - y,
                                     '*': lambda x, y: x * y}[op](a.ev(m, f), b.ev(m, f))))

    def bexpr(self, d):
        r = self.r
        c = r.random()
        if c < 0.3:
            sets = self.vars_where(lambda t: t[0] in ('Set', 'Map'))
            if sets:
                n, t, u = r.choice(sets)
                kk = self.gexpr(t[1], max(d - 1, 0))
                return Ev(f'{n}.has({kk.src()})',
                          lambda m, f: kk.ev(m, f) in _keys(f.look(u).v))
        a, b = self.iexpr(max(d - 1, 0)), self.iexpr(max(d - 1, 0))
        op = r.choice(['==', '!=', '<', '>='])
        return Ev(f'{a.src()} {op} {b.src()}', lambda m, f: _cmp(op, a.ev(m, f), b.ev(m, f)))

    def bool_expr(self, d, ctx):
        if self.r.random() < 0.3:
            e = self.bexpr(1)
            return Ev(f'({e.src()})', e.fn)
        return super().bool_expr(d, ctx)

    # ------------------------------------------------------------ statements
    def extra_weights(self, ctx, depth):
        if ctx.fn is not None:
            return []
        w = [('gdecl', 6), ('gprint', 0 if ctx.pure else 5), ('gcoll', 2)]
        if self.vars_where(lambda t: t[0] == 'Cell', True) or self.vars_where(lambda t: t == RCCELL):
            w.append(('gset', 3))
        if self.vars_where(lambda t: t[0] in ('List', 'Stack', 'Map', 'Set', 'Counter'), True):
            w.append(('gmut', 5))
        if self.vars_where(lambda t: t[0] in ('Opt', 'Res')) and depth > 0:
            w.append(('gmatch', 2))
        return w

    def s_gdecl(self, ctx, depth):
        r = self.r
        t = self.rtype(2, top=True)
        name = self.fresh('q')
        mut = r.random() < 0.5
        k = t[0]
        if k in ('Opt', 'Res') and (k == 'Res' or r.random() < 0.3):
            # a generic enum value whose type comes from the annotation
            if k == 'Opt' and t[1][0] == 'Opt' and r.random() < 0.5:
                # the inner value's type comes from the annotation too
                e = Ev('Opt.Some(Opt.None)', lambda m, f: EV('Some', EV('None')))
            elif k == 'Opt':
                e = Ev('Opt.None', lambda m, f: EV('None'))
            elif r.random() < 0.5:
                pe = self.gexpr(t[1], 1)
                e = Ev(f'Res.Ok({pe.src()})', lambda m, f: EV('Ok', vcopy(pe.ev(m, f))))
            else:
                pe = self.gexpr(t[2], 1)
                e = Ev(f'Res.Err({pe.src()})', lambda m, f: EV('Err', vcopy(pe.ev(m, f))))
            uid = self.bind_t(name, t, mut)
            return [Do(f'{"var" if mut else "val"} {name}: {ts(t)} = {e.src()}',
                       lambda m, f: f.vars.__setitem__(uid, Cell(vcopy(e.ev(m, f)))))]
        e = self.gexpr(t, 2)
        ann = f': {ts(t)}' if r.random() < 0.25 else ''
        uid = self.bind_t(name, t, mut)
        return [Do(f'{"var" if mut else "val"} {name}{ann} = {e.src()}',
                   lambda m, f: f.vars.__setitem__(uid, Cell(vcopy(e.ev(m, f)))))]

    def s_gcoll(self, ctx, depth):
        """A list, Stack, Map, Set or Counter binding."""
        r = self.r
        name = self.fresh('c')
        c = r.random()
        if c < 0.25:
            et = self.rtype(1)
            e = self.lexpr(et, 1)
            t = ('List', et)
            uid = self.bind_t(name, t, True)
            return [Do(f'var {name} = {e.src()}',
                       lambda m, f: f.vars.__setitem__(uid, Cell(e.ev(m, f))))]
        if c < 0.45:
            et = self.rtype(1)
            t = ('Stack', et)
            uid = self.bind_t(name, t, True)
            return [Do(f'var {name} = Stack[{ts(et)}]()',
                       lambda m, f: f.vars.__setitem__(uid, Cell(StackV([]))))]
        if c < 0.65:
            kt, vt = r.choice(KEYS), self.rtype(1)
            t = ('Map', kt, vt)
            uid = self.bind_t(name, t, True)
            return [Do(f'var {name} = Map[{ts(kt)}, {ts(vt)}]()',
                       lambda m, f: f.vars.__setitem__(uid, Cell(MapV())))]
        if c < 0.75:
            kt = r.choice(KEYS)
            xs = self.lexpr(kt, 1)
            t = ('Map', kt, I)
            uid = self.bind_t(name, t, r.random() < 0.5)
            mut = self.visible()[name][1]
            return [Do(f'{"var" if mut else "val"} {name} = tally({xs.src()})',
                       lambda m, f: f.vars.__setitem__(uid, Cell(m_tally(xs.ev(m, f)))))]
        if c < 0.88:
            kt = r.choice(KEYS)
            t = ('Set', kt)
            uid = self.bind_t(name, t, True)
            return [Do(f'var {name} = Set[{ts(kt)}]()',
                       lambda m, f: f.vars.__setitem__(uid, Cell(SetV())))]
        kt = r.choice(KEYS)
        t = ('Counter', kt)
        uid = self.bind_t(name, t, True)
        ctor = f'Counter[{ts(kt)}]()'
        return [Do(f'var {name} = {ctor}',
                   lambda m, f: f.vars.__setitem__(uid, Cell(CounterV({}))))]

    def s_gset(self, ctx, depth):
        r = self.r
        cands = self.vars_where(lambda t: t[0] == 'Cell', True) + \
            self.vars_where(lambda t: t == RCCELL)
        n, t, u = r.choice(cands)
        if t == RCCELL:
            e = self.iexpr(1)
            return [Do(f'{n}.set({e.src()})',
                       lambda m, f: setattr(f.look(u).v.obj, 'v', e.ev(m, f)))]
        e = self.gexpr(t[1], 2)
        if r.random() < 0.3:
            ne = self.gexpr(t, 2)
            return [Do(f'{n} = {ne.src()}',
                       lambda m, f: setattr(f.look(u), 'v', vcopy(ne.ev(m, f))))]
        return [Do(f'{n}.set({e.src()})', lambda m, f: setattr(f.look(u).v, 'v', vcopy(e.ev(m, f))))]

    def s_gmut(self, ctx, depth):
        r = self.r
        n, t, u = r.choice(self.vars_where(
            lambda t: t[0] in ('List', 'Stack', 'Map', 'Set', 'Counter'), True))
        k = t[0]
        tag = self.next_tag()
        if k == 'List':
            e = self.gexpr(t[1], 2)
            return [Do(f'{n}.push({e.src()})',
                       lambda m, f: f.look(u).v.items.append(vcopy(e.ev(m, f))))]
        if k == 'Stack':
            if r.random() < 0.6 or not showable(t[1]):
                e = self.gexpr(t[1], 2)
                return [Do(f'{n}.push({e.src()})',
                           lambda m, f: f.look(u).v.items.append(vcopy(e.ev(m, f))))]

            def pop(m, f):
                st = f.look(u).v
                if st.items:
                    m.emit(f'{tag} <{text(st.items.pop())}>')
            return [Do([f'if {n}.size() > 0 {{', f'    println("{tag} {{show({n}.pop())}}")', '}'],
                       pop)]
        if k == 'Map':
            kk, vv = self.gexpr(t[1], 1), self.gexpr(t[2], 2)
            if r.random() < 0.25:
                return [Do(f'println("{tag} {{{n}.remove({kk.src()})}}")',
                           lambda m, f: m.emit(f'{tag} ' + fmt(
                               f.look(u).v.d.pop(kk.ev(m, f), _MISSING) is not _MISSING)))]
            return [Do(f'{n}.put({kk.src()}, {vv.src()})',
                       lambda m, f: (lambda kv, vvv: f.look(u).v.d.__setitem__(kv, vvv))(
                           kk.ev(m, f), vcopy(vv.ev(m, f))))]
        if k == 'Counter':
            kk = self.gexpr(t[1], 1)
            return [Do(f'{n}.add({kk.src()})',
                       lambda m, f: (lambda d, kv: d.__setitem__(kv, d.get(kv, 0) + 1))(
                           f.look(u).v.d, kk.ev(m, f)))]
        kk = self.gexpr(t[1], 1)
        return [Do(f'println("{tag} {{{n}.add({kk.src()})}}")',
                   lambda m, f: m.emit(f'{tag} ' + fmt(_set_add(f.look(u).v, kk.ev(m, f)))))]

    def s_gprint(self, ctx, depth):
        r = self.r
        tag = self.next_tag()
        parts = []
        for _ in range(r.randint(1, 3)):
            t = self.rtype(2)
            if not showable(t) or t[0] == 'Res' and not self.vars_of(t):
                t = I
            e = self.gexpr(t, 2)
            if r.random() < 0.4:
                parts.append((f'{{show({e.src()})}}', (lambda e: lambda m, f: '<' + text(e.ev(m, f)) + '>')(e)))
            else:
                parts.append((f'{{{e.src()}}}', (lambda e: lambda m, f: text(e.ev(m, f)))(e)))
        return [Do(f'println("{tag} ' + ' '.join(p for p, _ in parts) + '")',
                   lambda m, f: m.emit(f'{tag} ' + ' '.join(fn(m, f) for _, fn in parts)))]

    def s_gmatch(self, ctx, depth):
        r = self.r
        n, t, u = r.choice(self.vars_where(lambda t: t[0] in ('Opt', 'Res')))
        tag = self.next_tag()
        b = self.fresh('m')
        if t[0] == 'Opt':
            pt = t[1]
            body = f'println("{tag} some {{show({b})}}")' if showable(pt) else f'println("{tag} some")'
            lines = [f'match {n} {{', f'    Opt.Some({b}) -> {body}', f'    Opt.None -> println("{tag} none")',
                     '}']

            def run(m, f):
                v = f.look(u).v
                if v.var == 'Some':
                    m.emit(f'{tag} some <{text(v.p)}>' if showable(pt) else f'{tag} some')
                else:
                    m.emit(f'{tag} none')
            return [Do(lines, run)]
        lines = [f'match {n} {{', f'    Res.Ok({b}) -> println("{tag} ok {{show({b})}}")',
                 f'    Res.Err({b}) -> println("{tag} err {{show({b})}}")', '}']

        def run2(m, f):
            v = f.look(u).v
            m.emit(f'{tag} {"ok" if v.var == "Ok" else "err"} <{text(v.p)}>')
        if not (showable(t[1]) and showable(t[2])):
            return [self.print_stmt(ctx)]
        return [Do(lines, run2)]

    # ------------------------------------------------------------ program
    def gen_enums(self):
        super().gen_enums()
        # Tok's `Pair` variant would make an unqualified `Pair(a, b)`
        # pattern ambiguous with the object Pair (manual 4.7)
        for e in self.enums:
            e.variants = [('Two' if n == 'Pair' else n, ps, v) for n, ps, v in e.variants]

    def top_lines(self):
        return PROLOGUE.splitlines() + ['']

    def model(self):
        _COPY_BUDGET[0] = 0
        return super().model()

    def param_types(self):
        return ['Int', 'Int', 'Bool']


_MISSING = object()


def _short(t):
    """A string the model builds; a program whose strings keep doubling
    is dropped."""
    if len(t) > 20000:
        raise Bail('long string')
    return t


def _apply(x, n, k):
    for _ in range(n):
        x = wrap(x * k + 1)
    return x


def _fold_sub(acc, items):
    for x in items:
        acc = wrap(acc - x)
    return acc


def _clen(v):
    if isinstance(v, MapV):
        return len(v.d)
    if isinstance(v, SetV):
        return len(v.s)
    return len(v.items)


def _keys(v):
    return v.d if isinstance(v, MapV) else v.s


def _set_add(s, k):
    new = k not in s.s
    s.s.add(k)
    return new


def generate(rng):
    _COPY_BUDGET[0] = 0
    g = GenGen(rng)
    g.program()
    return g
