"""Pointer programs (manual 6.2, 6.4, 6.5): checked pointers `*P` and
`*var P` to bindings, object fields, list elements (as call arguments) and
fixed array elements, pointers to Int, nullable `?*P`, the automatic `&`
when a binding is passed to a pointer parameter, writes and mutating calls
through `*var` pointers, `heap` values with `defer free` and pointers to
them, and `Box` cells (`Box(v)`, `unwrap`, `is_null`, `free`, `Box.null()`)
with unchecked pointers into them. Objects with a drop block print when a
heap value or a box is released.

Some programs free a heap value and then read it through a pointer (or
free it twice): the manual defines that as a "stale pointer dereference"
abort, which the model predicts with the output before it. Others carry
one statement the manual rejects: `&` of a growable list element kept in a
binding (E0330), of a temporary (E0270), of an rc handle (E0296),
returning a pointer to a local (E0271), a checked pointer bound to an
unchecked one (E0289 / E0294), pointer arithmetic (E0268 / E0295), `free`
of a stack value (E0212 / E0274), and a field written through a read-only
pointer (E0234).

Two rules the manual leaves implicit, checked against the compiler:
  - a `Box` that is not freed is released (and its value dropped) when its
    block exits; `b.free()` releases it at once and a later exit does not;
  - a value in a box is dropped by `free` before the statement after it.
"""
from pf_lang import *
from pf_stmt import Gen, Ctx


class OV:
    """An object value: P (hp, tag), H (inner: P, n) or D (hp, tag)."""
    __slots__ = ('t', 'f')

    def __init__(self, t, f):
        self.t, self.f = t, f

    def iron_copy(self):
        return OV(self.t, {k: (v.iron_copy() if isinstance(v, OV) else v)
                           for k, v in self.f.items()})


class HeapV:
    """A heap binding's allocation."""

    def __init__(self, obj):
        self.obj, self.freed = obj, False


class BoxV:
    def __init__(self, obj, null=False):
        self.obj, self.freed, self.null = obj, False, null


class PtrV:
    """A checked pointer: get() is the place it points to."""

    def __init__(self, get, heap=None):
        self.get, self.heap = get, heap

    def deref(self):
        if self.heap is not None and self.heap.freed:
            raise PanicX('stale pointer dereference')
        return self.get()


STALE = 'stale pointer dereference'


def dropline(m, o):
    m.emit(f'drop D{o.f["tag"]} {o.f["hp"]}')


def deref(v):
    """The object a binding, heap binding, pointer or box pointer reaches."""
    if isinstance(v, PtrV):
        return v.deref()
    if isinstance(v, HeapV):
        if v.freed:
            raise PanicX(STALE)
        return v.obj
    return v


class Ev(Expr):
    """src text and a Python evaluator fn(m, f)."""

    def __init__(self, text, fn):
        self.text, self.fn = text, fn

    def src(self, parent=0):
        return self.text

    def ev(self, m, f):
        return self.fn(m, f)


class Do(Stmt):
    """One statement: source lines and a Python effect fn(m, f)."""

    def __init__(self, text, fn):
        self.text, self.fn = text, fn

    def lines(self, ind):
        return [ind + t for t in (self.text if isinstance(self.text, list) else [self.text])]

    def run(self, m, f):
        self.fn(m, f)


PROLOGUE = '''object P {
    var hp: Int
    var tag: Int
    init(hp: Int, tag: Int) {
        self.hp = hp
        self.tag = tag
    }
    func bump(k: Int) {
        self.hp += k
    }
    readonly func get() -> Int {
        return self.hp * 10 + self.tag
    }
}

object H {
    var inner: P
    var n: Int
    init(inner: P, n: Int) {
        self.inner = inner
        self.n = n
    }
}

object D {
    var hp: Int
    val tag: Int
    init(hp: Int, tag: Int) {
        self.hp = hp
        self.tag = tag
    }
    func bump(k: Int) {
        self.hp += k
    }
    drop {
        println("drop D{self.tag} {self.hp}")
    }
}

func rd(p: *P) -> Int {
    return p.get()
}

func wr(p: *var P, k: Int) {
    p.hp += k
    p.bump(1)
}

func opt(p: ?*P) -> Int {
    if p != null {
        return p.hp
    }
    return -1
}

func showi(p: *Int) -> String {
    return "<{p}>"
}

func rdd(p: *D) -> Int {
    return p.hp * 10 + p.tag
}

func wrd(p: *var D, k: Int) {
    p.hp += k
    p.bump(2)
}

func wru(p: *var unchecked D, k: Int) {
    p.hp += k
}
'''


def get_val(o):
    return o.f['hp'] * 10 + o.f['tag']


class PtrGen(Gen):
    NFUNCS = (0, 3)
    NMAIN = (10, 26)

    def __init__(self, rng):
        super().__init__(rng)
        self.depth = {}        # uid -> scope depth it was declared at
        self.tags = 0
        self.stale = None      # set when the program ends in a stale read
        self.bad = None

    def bind2(self, name, typ, mut=False):
        uid = self.bind(name, typ, mut)
        self.depth[uid] = len(self.scopes)
        return uid

    def newtag(self):
        self.tags += 1
        return self.tags

    def objs(self, typ, mut=None):
        return [(n, u) for n, (t, mu, u) in self.visible().items()
                if t == typ and (mut is None or mu == mut)]

    def iexpr(self, ctx):
        return self.int_expr(1, ctx.sub(pure=True, no_rec=True))

    # ------------------------------------------------------------ weights
    def extra_weights(self, ctx, depth):
        if ctx.fn is not None:
            return []
        w = [('newp', 4), ('newh', 1), ('newptr', 3), ('pcall', 4), ('pprint', 4),
             ('newheap', 1 if not ctx.in_defer else 0), ('newbox', 1 if not ctx.in_defer else 0),
             ('pint', 1), ('fixed', 1), ('list', 1), ('nullable', 1)]
        if self.objs('*var P') or self.objs('*var D'):
            w.append(('ptrwrite', 5))
        if self.objs('P', True) or self.objs('H', True):
            w.append(('objwrite', 3))
        return w

    # ------------------------------------------------------------ objects
    def s_newp(self, ctx, depth):
        r = self.r
        name = self.fresh('a')
        hp, t = self.iexpr(ctx), self.newtag()
        mut = r.random() < 0.75
        others = self.objs('P')
        if others and r.random() < 0.25:
            # a copy of another object
            on, ou = r.choice(others)
            uid = self.bind2(name, 'P', mut)
            return [Do(f'{"var" if mut else "val"} {name} = {on}',
                       lambda m, f, ou=ou, uid=uid: f.vars.__setitem__(
                           uid, Cell(f.look(ou).v.iron_copy())))]
        uid = self.bind2(name, 'P', mut)
        return [Do(f'{"var" if mut else "val"} {name} = P({hp.src()}, {t})',
                   lambda m, f, uid=uid, hp=hp, t=t: f.vars.__setitem__(
                       uid, Cell(OV('P', {'hp': hp.ev(m, f), 'tag': t}))))]

    def s_newh(self, ctx, depth):
        name = self.fresh('h')
        hp, n, t = self.iexpr(ctx), self.iexpr(ctx), self.newtag()
        uid = self.bind2(name, 'H', True)
        return [Do(f'var {name} = H(P({hp.src()}, {t}), {n.src()})',
                   lambda m, f, uid=uid, hp=hp, n=n, t=t: f.vars.__setitem__(
                       uid, Cell(OV('H', {'inner': OV('P', {'hp': hp.ev(m, f), 'tag': t}),
                                          'n': n.ev(m, f)}))))]

    def s_objwrite(self, ctx, depth):
        r = self.r
        cands = self.objs('P', True) + self.objs('H', True)
        n, u = r.choice(cands)
        e = self.iexpr(ctx)
        t = self.visible()[n][0]
        c = r.random()
        if t == 'H':
            if c < 0.3:
                tg = self.newtag()
                return [Do(f'{n}.inner = P({e.src()}, {tg})',
                           lambda m, f: f.look(u).v.f.__setitem__(
                               'inner', OV('P', {'hp': e.ev(m, f), 'tag': tg})))]
            if c < 0.6:
                return [Do(f'{n}.inner.bump({e.src()})',
                           lambda m, f: _add(f.look(u).v.f['inner'], e.ev(m, f)))]
            return [Do(f'{n}.inner.hp = {e.src()}',
                       lambda m, f: f.look(u).v.f['inner'].f.__setitem__('hp', e.ev(m, f)))]
        if c < 0.25:
            tg = self.newtag()
            return [Do(f'{n} = P({e.src()}, {tg})',
                       lambda m, f: setattr(f.look(u), 'v', OV('P', {'hp': e.ev(m, f), 'tag': tg})))]
        if c < 0.6:
            return [Do(f'{n}.bump({e.src()})', lambda m, f: _add(f.look(u).v, e.ev(m, f)))]
        return [Do(f'{n}.hp += {e.src()}', lambda m, f: _add(f.look(u).v, e.ev(m, f)))]

    # ------------------------------------------------------------ pointers
    def place(self, want_var):
        """(source text of `&place`, getter, heap, decl depth) of an object
        P to point at; want_var: the place must be writable."""
        r = self.r
        cands = []
        for n, u in self.objs('P', True if want_var else None):
            cands.append((f'&{n}', (lambda u: lambda m, f: f.look(u).v)(u), None,
                          self.depth[u], n))
        for n, u in self.objs('H', True):
            cands.append((f'&{n}.inner', (lambda u: lambda m, f: f.look(u).v.f['inner'])(u),
                          None, self.depth[u], n))
        for n, u in self.objs('[P; 2]', True if want_var else None):
            k = r.randint(0, 1)
            cands.append((f'&{n}[{k}]', (lambda u, k: lambda m, f: f.look(u).v.items[k])(u, k),
                          None, self.depth[u], n))
        if not cands:
            return None
        return r.choice(cands)

    def s_newptr(self, ctx, depth):
        r = self.r
        want_var = r.random() < 0.6
        pl = self.place(want_var)
        if pl is None:
            return self.s_newp(ctx, depth)
        text, get, heap, d, root = pl
        name = self.fresh('p')
        typ = '*var P' if self.visible()[root][1] else '*P'
        ann = '' if r.random() < 0.7 else f': {typ}'
        if not want_var and r.random() < 0.3:
            typ, ann = '*P', ': *P'       # a writable place seen read-only
        uid = self.bind2(name, typ, False)
        self.depth[uid] = d
        return [Do(f'val {name}{ann} = {text}',
                   lambda m, f, uid=uid: f.vars.__setitem__(uid, Cell(PtrV(lambda: get(m, f)))))]

    def s_ptrwrite(self, ctx, depth):
        r = self.r
        cands = [(n, u, 'P') for n, u in self.objs('*var P')] + \
                [(n, u, 'D') for n, u in self.objs('*var D')]
        n, u, t = r.choice(cands)
        e = self.iexpr(ctx)
        c = r.random()
        if c < 0.35:
            return [Do(f'{n}.hp = {e.src()}',
                       lambda m, f: f.look(u).v.deref().f.__setitem__('hp', e.ev(m, f)))]
        if c < 0.6:
            return [Do(f'{n}.bump({e.src()})',
                       lambda m, f: _add(f.look(u).v.deref(), e.ev(m, f)))]
        if c < 0.8:
            return [Do(f'{n}.hp += {e.src()}',
                       lambda m, f: _add(f.look(u).v.deref(), e.ev(m, f)))]
        if t == 'P':
            return [Do(f'wr({n}, {e.src()})', lambda m, f: _wr(f.look(u).v.deref(), e.ev(m, f)))]
        return [Do(f'wrd({n}, {e.src()})', lambda m, f: _wrd(f.look(u).v.deref(), e.ev(m, f)))]

    def s_pcall(self, ctx, depth):
        """wr / rd / opt on a place, a pointer binding or a binding (the
        address is taken for the call)."""
        r = self.r
        e = self.iexpr(ctx)
        c = r.random()
        if c < 0.45:
            pl = self.place(True)
            if pl is None:
                return self.s_newp(ctx, depth)
            text, get, heap, d, root = pl
            if r.random() < 0.3 and text.startswith('&') and '.' not in text and '[' not in text:
                text = text[1:]       # automatic &
            ls = self.objs('[P]', True)
            if ls and r.random() < 0.3:
                ln, lu = r.choice(ls)
                k = r.randint(0, 1)
                text = f'&{ln}[{k}]'
                get = (lambda lu, k: lambda m, f: f.look(lu).v.items[k])(lu, k)
            return [Do(f'wr({text}, {e.src()})', lambda m, f: _wr(get(m, f), e.ev(m, f)))]
        tag = self.next_tag()
        srcs = []
        for n, u in self.objs('*P') + self.objs('*var P'):
            srcs.append((n, (lambda u: lambda m, f: f.look(u).v.deref())(u)))
        pl = self.place(False)
        if pl:
            srcs.append((pl[0], (lambda g: lambda m, f: g(m, f))(pl[1])))
            if '.' not in pl[0] and '[' not in pl[0]:
                srcs.append((pl[0][1:], (lambda g: lambda m, f: g(m, f))(pl[1])))
        for n, u in self.objs('[P]'):
            k = r.randint(0, 1)
            srcs.append((f'&{n}[{k}]', (lambda u, k: lambda m, f: f.look(u).v.items[k])(u, k)))
        if not srcs:
            return self.s_newp(ctx, depth)
        s, g = r.choice(srcs)
        if c >= 0.75 and not s.startswith('&') and s not in [n for n, u in
                                                             self.objs('*P') + self.objs('*var P')]:
            s = '&' + s       # the automatic & is for a `*T` parameter (manual 6.4)
        if c < 0.75:
            return [Do(f'println("{tag} {{rd({s})}}")',
                       lambda m, f: m.emit(f'{tag} {get_val(g(m, f))}'))]
        return [Do(f'println("{tag} {{opt({s})}}")',
                   lambda m, f: m.emit(f'{tag} {g(m, f).f["hp"]}'))]

    def s_pprint(self, ctx, depth):
        r = self.r
        tag = self.next_tag()
        parts = []
        fns = []
        for n, u in r.sample(self.objs('*P') + self.objs('*var P'),
                             min(2, len(self.objs('*P') + self.objs('*var P')))):
            if r.random() < 0.5:
                parts.append(f'{{{n}.hp}}')
                fns.append((lambda u: lambda m, f: str(f.look(u).v.deref().f['hp']))(u))
            else:
                parts.append(f'{{{n}.get()}}')
                fns.append((lambda u: lambda m, f: str(get_val(f.look(u).v.deref())))(u))
        for n, u in r.sample(self.objs('P'), min(2, len(self.objs('P')))):
            parts.append(f'{{{n}.hp}}')
            fns.append((lambda u: lambda m, f: str(f.look(u).v.f['hp']))(u))
        for n, u in self.objs('H')[:1]:
            parts.append(f'{{{n}.inner.hp}}')
            fns.append((lambda u: lambda m, f: str(f.look(u).v.f['inner'].f['hp']))(u))
        for n, u in self.objs('*Int')[:2]:
            parts.append(f'{{{n}}} {{showi({n})}}')
            fns.append((lambda u: lambda m, f: '%d <%d>' % ((f.look(u).v.deref(),) * 2))(u))
        for n, u in self.objs('*var D') + self.objs('*D'):
            parts.append(f'{{rdd({n})}}')
            fns.append((lambda u: lambda m, f: str(get_val(f.look(u).v.deref())))(u))
            break
        for n, u in self.objs('Box'):
            parts.append(f'{{{n}.is_null()}}')
            fns.append((lambda u: lambda m, f: fmt(f.look(u).v.freed or f.look(u).v.null))(u))
            break
        if not parts:
            return [self.print_stmt(ctx)]
        return [Do(f'println("{tag} ' + ' '.join(parts) + '")',
                   lambda m, f: m.emit(f'{tag} ' + ' '.join(fn(m, f) for fn in fns)))]

    def s_pint(self, ctx, depth):
        ints = [(n, u) for n, u in self.objs('Int', True) if n not in self.globals]
        if not ints:
            return self.s_newp(ctx, depth)
        n, u = self.r.choice(ints)
        name = self.fresh('q')
        uid = self.bind2(name, '*Int', False)
        self.depth[uid] = self.depth.get(u, 0)
        return [Do(f'val {name} = &{n}',
                   lambda m, f: f.vars.__setitem__(uid, Cell(PtrV(lambda: f.look(u).v))))]

    def s_fixed(self, ctx, depth):
        name = self.fresh('fa')
        a, b = self.iexpr(ctx), self.iexpr(ctx)
        t1, t2 = self.newtag(), self.newtag()
        uid = self.bind2(name, '[P; 2]', True)
        return [Do(f'var {name}: [P; 2] = [P({a.src()}, {t1}), P({b.src()}, {t2})]',
                   lambda m, f: f.vars.__setitem__(uid, Cell(ListV(
                       [OV('P', {'hp': a.ev(m, f), 'tag': t1}),
                        OV('P', {'hp': b.ev(m, f), 'tag': t2})]))))]

    def s_list(self, ctx, depth):
        name = self.fresh('xs')
        a, b = self.iexpr(ctx), self.iexpr(ctx)
        t1, t2 = self.newtag(), self.newtag()
        uid = self.bind2(name, '[P]', True)
        return [Do(f'var {name} = [P({a.src()}, {t1}), P({b.src()}, {t2})]',
                   lambda m, f: f.vars.__setitem__(uid, Cell(ListV(
                       [OV('P', {'hp': a.ev(m, f), 'tag': t1}),
                        OV('P', {'hp': b.ev(m, f), 'tag': t2})]))))]

    def s_nullable(self, ctx, depth):
        r = self.r
        name = self.fresh('o')
        tag = self.next_tag()
        uid = self.bind2(name, '?*P', True)
        out = [Do(f'var {name}: ?*P = null', lambda m, f: f.vars.__setitem__(uid, Cell(None))),
               Do(f'println("{tag} {{opt({name})}}")', lambda m, f: m.emit(f'{tag} -1'))]
        pl = self.place(False)
        if pl and pl[3] <= self.depth[uid]:
            text, get, heap, d, root = pl
            tag2 = self.next_tag()
            out.append(Do(f'{name} = {text}',
                          lambda m, f: setattr(f.look(uid), 'v', PtrV(lambda: get(m, f)))))
            out.append(Do([f'if {name} != null {{', f'    println("{tag2} {{{name}.hp}}")', '}'],
                          lambda m, f: m.emit(f'{tag2} {f.look(uid).v.deref().f["hp"]}')))
            out.append(Do(f'println("{tag2} {{opt({name})}}")',
                          lambda m, f: m.emit(f'{tag2} {f.look(uid).v.deref().f["hp"]}')))
        return out

    # ------------------------------------------------------------ heap and Box
    def s_newheap(self, ctx, depth):
        r = self.r
        name = self.fresh('hd')
        hp, t = self.iexpr(ctx), self.newtag()
        uid = self.bind2(name, 'heapD', True)
        out = [Do(f'var {name} = heap D({hp.src()}, {t})',
                  lambda m, f: f.vars.__setitem__(uid, Cell(HeapV(OV('D', {'hp': hp.ev(m, f),
                                                                            'tag': t})))))]
        out.append(Defer(Do(f'free {name}', lambda m, f: _free(m, f.look(uid).v)), single=True))
        e = self.iexpr(ctx)
        out.append(Do(f'wrd(&{name}, {e.src()})',
                      lambda m, f: _wrd(deref(f.look(uid).v), e.ev(m, f))))
        if r.random() < 0.7:
            pn = self.fresh('pd')
            pu = self.bind2(pn, '*var D', False)
            out.append(Do(f'val {pn} = &{name}',
                          lambda m, f: f.vars.__setitem__(
                              pu, Cell(PtrV(lambda: f.look(uid).v.obj, f.look(uid).v)))))
        return out

    def s_newbox(self, ctx, depth):
        r = self.r
        name = self.fresh('b')
        if r.random() < 0.25:
            # a box of an Int: its pointer prints the value
            e = self.iexpr(ctx)
            pn = self.fresh('bp')
            tag = self.next_tag()
            return [Do([f'val {name} = Box({e.src()})', f'val {pn} = {name}.unwrap()',
                        f'println("{tag} {{{pn}}} {{{name}.is_null()}}")'],
                       lambda m, f: m.emit(f'{tag} {e.ev(m, f)} false'))]
        if r.random() < 0.15:
            tag = self.next_tag()
            return [Do([f'val {name}: Box[D] = Box.null()', f'println("{tag} {{{name}.is_null()}}")'],
                       lambda m, f: m.emit(f'{tag} true'))]
        hp, t = self.iexpr(ctx), self.newtag()
        uid = self.bind2(name, 'Box', False)

        def make(m, f):
            b = BoxV(OV('D', {'hp': hp.ev(m, f), 'tag': t}))
            f.vars[uid] = Cell(b)
            f.drops.append(lambda m, b=b: _box_drop(m, b))
        out = [Do(f'val {name} = Box(D({hp.src()}, {t}))', make)]
        un = self.fresh('u')
        uu = self.bind2(un, '*var unchecked D', False)
        ann = ': *var unchecked D' if r.random() < 0.4 else ''
        out.append(Do(f'val {un}{ann} = {name}.unwrap()',
                      lambda m, f: f.vars.__setitem__(uu, Cell(f.look(uid).v.obj))))
        tag = self.next_tag()
        for _ in range(r.randint(1, 3)):
            e = self.iexpr(ctx)
            c = r.random()
            if c < 0.3:
                out.append(Do(f'{un}.hp = {e.src()}',
                              lambda m, f, e=e: f.look(uu).v.f.__setitem__('hp', e.ev(m, f))))
            elif c < 0.6:
                out.append(Do(f'{un}.bump({e.src()})',
                              lambda m, f, e=e: _add(f.look(uu).v, e.ev(m, f))))
            else:
                out.append(Do(f'wru({un}, {e.src()})',
                              lambda m, f, e=e: _add(f.look(uu).v, e.ev(m, f))))
        out.append(Do(f'println("{tag} {{{un}.hp}} {{{name}.is_null()}}")',
                      lambda m, f: m.emit(f'{tag} {f.look(uu).v.f["hp"]} false')))
        if r.random() < 0.5:
            tag2 = self.next_tag()
            out.append(Do([f'{name}.free()', f'println("{tag2} {{{name}.is_null()}}")'],
                          lambda m, f: (_box_free(m, f.look(uid).v), m.emit(f'{tag2} true'))))
            # the unchecked pointer dangles now: nothing reads it again
            self.scopes[-1].pop(un, None)
        return out

    # ------------------------------------------------------------ program
    def gen_top(self):
        pass

    def top_lines(self):
        return PROLOGUE.splitlines() + ['']

    def param_types(self):
        return ['Int', 'Int', 'Bool']

    def program(self):
        super().program()
        r = self.r
        c = r.random()
        if c < 0.12:
            self.add_stale()
        elif c < 0.24:
            self.add_bad()

    def add_stale(self):
        r = self.r
        name, pn = self.fresh('hz'), self.fresh('pz')
        hp, t = r.randint(1, 50), self.newtag()
        k = r.random()
        if k < 0.4:
            text = [f'var {name} = heap D({hp}, {t})', f'val {pn} = &{name}', f'free {name}',
                    f'println("{{{pn}.hp}}")']
        elif k < 0.7:
            text = [f'var {name} = heap D({hp}, {t})', f'free {name}', f'println("{{{name}.hp}}")']
        else:
            text = [f'var {name} = heap D({hp}, {t})', f'val {pn} = &{name}', f'free {name}',
                    f'println("{{rdd({pn})}}")']

        def run(m, f):
            m.emit(f'drop D{t} {hp}')
            p = PanicX(STALE)
            p.lines = m.lines()
            raise p
        pos = r.randint(0, len(self.mainblock.stmts))
        self.mainblock.stmts.insert(pos, Do(text, run))
        self.stale = STALE

    def add_bad(self):
        r = self.r
        k = r.choice(['listelem', 'temp', 'rc', 'escape', 'unchecked', 'arith', 'freestack',
                      'rowrite', 'rowrite_opt'])
        x, y = self.fresh('bad'), self.fresh('bad')
        lines, codes, top = {
            'listelem': ([f'var {x} = [P(1, 1), P(2, 2)]', f'val {y} = &{x}[0]',
                          f'println("{{{y}.hp}}")'], {'E0330'}, []),
            'temp': ([f'val {y} = &P(1, 2)', f'println("{{{y}.hp}}")'], {'E0270'}, []),
            'rc': ([f'val {x} = rc P(1, 2)', f'val {y} = &{x}', f'println("{{{y}.hp}}")'],
                   {'E0296'}, []),
            'escape': ([f'println("{{bad_escape().hp}}")'], {'E0271'},
                       ['func bad_escape() -> *P {', '    val local = P(1, 2)',
                        '    return &local', '}', '']),
            'unchecked': ([f'val {x} = Box(D(1, 99))', f'val {y}: *D = {x}.unwrap()',
                           f'println("{{{y}.hp}}")'], {'E0289', 'E0294'}, []),
            'arith': ([f'var {x} = P(1, 2)', f'val {y} = &{x}', f'val z{y} = {y} + 1'],
                      {'E0268', 'E0295'}, []),
            'freestack': ([f'var {x} = P(1, 2)', f'free {x}'], {'E0212', 'E0274'}, []),
            'rowrite': ([f'var {x} = P(1, 2)', f'var {y}: *P = &{x}', f'{y}.hp = 5'],
                        {'E0234'}, []),
            'rowrite_opt': ([f'val {x} = P(1, 2)', f'var {y}: ?*P = &{x}',
                             f'if {y} != null {{', f'    {y}.hp = 5', '}'], {'E0234'}, []),
        }[k]
        pos = r.randint(0, len(self.mainblock.stmts))
        self.mainblock.stmts.insert(pos, Do(lines, lambda m, f: None))
        self.bad_top = top
        self.bad = codes

    def render(self):
        src = super().render()
        if self.bad and self.bad_top:
            src = src.replace('func main() {', '\n'.join(self.bad_top) + '\nfunc main() {', 1)
        return src

    def model(self):
        if self.bad:
            return Expected([], errors=self.bad)
        gf = Frame()
        for g, v in self.gl:
            gf.vars[g] = Cell(v)
        m = Machine(gf)
        try:
            self.mainblock.run(m, Frame(gf, None))
        except PanicX as p:
            # the abort skips every defer and drop still pending
            return Expected(p.lines, panic=p.msg)
        return Expected(m.lines())


def _add(o, k):
    o.f['hp'] = wrap(o.f['hp'] + k)


def _wr(o, k):
    o.f['hp'] = wrap(o.f['hp'] + k + 1)


def _wrd(o, k):
    o.f['hp'] = wrap(o.f['hp'] + k + 2)


def _free(m, h):
    if h.freed:
        raise PanicX(STALE)
    h.freed = True
    dropline(m, h.obj)


def _box_free(m, b):
    if not b.freed:
        b.freed = True
        dropline(m, b.obj)


def _box_drop(m, b):
    if not b.freed:
        b.freed = True
        dropline(m, b.obj)


def generate(rng):
    g = PtrGen(rng)
    g.program()
    return g
