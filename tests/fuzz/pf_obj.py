"""Object and ownership programs: objects with fields and methods (readonly
and mutating), copy and drop blocks that print, copies on assignment and on
`get`, moves of temporaries, lists of objects, `rc` handles (in bindings,
lists and fields) and `weak rc` upgrades, `heap` with `defer free`, and
defer, mixed with the control flow of pf_stmt.

The model follows manual 6.1, 6.3, 6.7 and these rules the manual leaves
implicit, checked against the compiler before the generator relied on them:
  - a parameter passed by value is lent: no copy block, no drop in the callee
    (the manual says objects are copied when passed by value; the compiler
    lends them, which differs only in copy and drop hooks, so the generator
    does not pass objects with hooks to `val` parameters)... see the
    `peek_*` helpers: they are called on bindings and temporaries, and only
    the temporary's own drop is observable;
  - a `var` parameter is the caller's value;
  - returning a local moves it; `pop`/`remove` move the element out;
  - `x = y` copies y, then drops the old x;
  - an explicit `init` storing an object parameter into a field stores a
    copy, and the temporary argument is dropped after the construction;
    a positional construction moves its arguments into the fields;
  - `for e in xs` over objects lends each element (no copy, no drop);
  - a list drops its elements in index order;
  - a temporary read in an interpolation is dropped right after the read.
"""
from pf_lang import *
from pf_stmt import Gen, Ctx


# ---------------------------------------------------------------- values

class TypeDef:
    def __init__(self, name, fields, has_copy, has_drop, init, tag):
        self.name = name
        self.fields = fields        # [(name, kind)], kind: id int leaf rc
        self.has_copy = has_copy
        self.has_drop = has_drop
        self.init = init            # explicit init (else positional)
        self.tag = tag              # prefix in the trace

    def has(self, kind):
        return any(k == kind for _, k in self.fields)


class OV:
    __slots__ = ('t', 'f')

    def __init__(self, t, f):
        self.t, self.f = t, f


class RcBox:
    __slots__ = ('obj', 'strong', 'alive')

    def __init__(self, obj):
        self.obj, self.strong, self.alive = obj, 1, True


class RcH:
    __slots__ = ('box',)

    def __init__(self, box):
        self.box = box


class WeakH:
    __slots__ = ('box',)

    def __init__(self, box):
        self.box = box


def retain(h):
    h.box.strong += 1
    return RcH(h.box)


def ocopy(m, v):
    if isinstance(v, OV):
        f = {}
        for n, k in v.t.fields:
            x = v.f[n]
            f[n] = ocopy(m, x) if k in ('leaf', 'rc') else x
        nv = OV(v.t, f)
        if v.t.has_copy:
            m.emit(f'copy {v.t.tag}{v.f["id"]}')
        return nv
    if isinstance(v, RcH):
        return retain(v)
    if isinstance(v, WeakH):
        return WeakH(v.box)
    if isinstance(v, ListV):
        return ListV([ocopy(m, x) for x in v.items])
    return v


def odestroy(m, v):
    if isinstance(v, OV):
        if v.t.has_drop:
            if v.t.has('int'):
                m.emit(f'drop {v.t.tag}{v.f["id"]} {v.f["v"]}')
            else:
                m.emit(f'drop {v.t.tag}{v.f["id"]}')
        for n, k in reversed(v.t.fields):
            if k in ('leaf', 'rc'):
                odestroy(m, v.f[n])
    elif isinstance(v, RcH):
        b = v.box
        b.strong -= 1
        if b.strong == 0:
            b.alive = False
            odestroy(m, b.obj)
    elif isinstance(v, ListV):
        items = v.items
        v.items = []
        for x in items:
            odestroy(m, x)


def deref(v):
    """The object a stack binding, heap binding or rc handle reaches."""
    if isinstance(v, RcH):
        return v.box.obj
    return v


# ------------------------------------------------------------ statements

class ODecl(Stmt):
    """A binding that owns its value: dropped at scope exit (heap: freed
    by its `defer free`)."""

    def __init__(self, name, uid, mut, init, ann=None, heap=False):
        self.name, self.uid, self.mut, self.init, self.ann, self.heap = name, uid, mut, init, ann, heap

    def lines(self, ind):
        kw = 'var' if self.mut else 'val'
        ann = f': {self.ann}' if self.ann else ''
        return [f'{ind}{kw} {self.name}{ann} = {self.init.src()}']

    def run(self, m, f):
        c = Cell(self.init.make(m, f))
        f.vars[self.uid] = c
        if not self.heap:
            f.drops.append(lambda m, c=c: odestroy(m, c.v))


class OAssign(Stmt):
    def __init__(self, name, uid, init):
        self.name, self.uid, self.init = name, uid, init

    def lines(self, ind):
        return [f'{ind}{self.name} = {self.init.src()}']

    def run(self, m, f):
        c = f.look(self.uid)
        new = self.init.make(m, f)
        old = c.v
        c.v = new
        odestroy(m, old)


class Free(Stmt):
    def __init__(self, name, uid):
        self.name, self.uid = name, uid

    def lines(self, ind):
        return [f'{ind}free {self.name}']

    def run(self, m, f):
        odestroy(m, f.look(self.uid).v)


class Place:
    """Where an object lives: a binding (stack, heap or rc handle), a list
    element, or an rc field of a holder."""

    def __init__(self, text, fn):
        self.text, self.fn = text, fn

    def get(self, m, f):
        return deref(self.fn(m, f))


def bind_place(name, uid):
    return Place(name, lambda m, f: f.look(uid).v)


def elem_place(lname, luid, k):
    def fn(m, f):
        items = f.look(luid).v.items
        if not 0 <= k < len(items):
            raise Bail('elem index')
        return items[k]
    return Place(f'{lname}[{k}]', fn)


class FieldGet(Expr):
    def __init__(self, place, field):
        self.place, self.field = place, field

    def src(self, parent=0):
        return f'{self.place.text}.{self.field}'

    def ev(self, m, f):
        o = self.place.get(m, f)
        if self.field == 'r.v':
            return deref(o.f['r']).f['v']
        if self.field == 'leaf.id':
            return o.f['leaf'].f['id']
        return o.f[self.field]


def get_value(o):
    if o.t.has('int'):
        return wrap(o.f['v'] * 3 + o.f['id'])
    return wrap(o.f['id'] * 2 + o.f['w'])


class GetCall(Expr):
    """x.get(): the readonly method of every object type."""

    def __init__(self, place):
        self.place = place

    def src(self, parent=0):
        return f'{self.place.text}.get()'

    def ev(self, m, f):
        return get_value(self.place.get(m, f))


class FieldSet(Stmt):
    def __init__(self, place, op, e):
        self.place, self.op, self.e = place, op, e

    def lines(self, ind):
        return [f'{ind}{self.place.text}.v {self.op} {self.e.src()}']

    def run(self, m, f):
        o = self.place.get(m, f)
        v = self.e.ev(m, f)
        o.f['v'] = v if self.op == '=' else Bin(self.op[:-1], Lit(o.f['v']), Lit(v)).ev(m, f)


class Bump(Stmt):
    """x.bump(k): the mutating method; prints and adds k to v."""

    def __init__(self, place, e):
        self.place, self.e = place, e

    def lines(self, ind):
        return [f'{ind}{self.place.text}.bump({self.e.src()})']

    def run(self, m, f):
        o = self.place.get(m, f)
        k = self.e.ev(m, f)
        m.emit(f'bump {o.t.tag}{o.f["id"]}')
        o.f['v'] = wrap(o.f['v'] + k)


class Poke(Stmt):
    """poke_T(x, k): a function with a `var` object parameter."""

    def __init__(self, t, name, uid, e):
        self.t, self.name, self.uid, self.e = t, name, uid, e

    def lines(self, ind):
        return [f'{ind}poke_{self.t.name}({self.name}, {self.e.src()})']

    def run(self, m, f):
        o = deref(f.look(self.uid).v)
        k = self.e.ev(m, f)
        o.f['v'] = wrap(o.f['v'] + k)
        m.emit(f'poke {o.t.tag}{o.f["id"]}')


class ListOp(Stmt):
    """A list method call statement with a Python implementation."""

    def __init__(self, text, fn):
        self.text, self.fn = text, fn

    def lines(self, ind):
        return [f'{ind}{self.text}']

    def run(self, m, f):
        self.fn(m, f)


class ObjForIn(Stmt):
    """for e in xs over objects: each element is lent to the body."""

    def __init__(self, name, uid, lname, luid, body):
        self.name, self.uid, self.lname, self.luid, self.body = name, uid, lname, luid, body

    def lines(self, ind):
        return [f'{ind}for {self.name} in {self.lname} {{'] + \
            self.body.lines(ind + '    ') + [f'{ind}}}']

    def run(self, m, f):
        items = list(f.look(self.luid).v.items)
        for x in items:
            m.tick()
            try:
                self.body.run(m, f, {self.uid: x})
            except BreakX:
                break
            except ContinueX:
                continue


# ------------------------------------------------------------ initializers

class Init:
    def src(self):
        raise NotImplementedError

    def make(self, m, f):
        raise NotImplementedError


def construct(m, t, vals):
    """Run construction: explicit init copies an object argument into its
    field and drops the temporary argument after; positional moves it."""
    f = {}
    temps = []
    for (n, k), v in zip(t.fields, vals):
        if k == 'leaf' and t.init:
            f[n] = ocopy(m, v)
            temps.append(v)
        else:
            f[n] = v
    o = OV(t, f)
    for v in temps:
        odestroy(m, v)
    return o


class New(Init):
    """T(args); args: list of Expr for int fields, ('leaf', id) for a Leaf
    temporary, ('rc', name, uid) for an rc binding."""

    def __init__(self, t, args, prefix=''):
        self.t, self.args, self.prefix = t, args, prefix

    def src(self):
        parts = []
        for a in self.args:
            if isinstance(a, tuple) and a[0] == 'leaf':
                parts.append(f'Leaf({a[1]})')
            elif isinstance(a, tuple) and a[0] == 'rc':
                parts.append(a[1])
            else:
                parts.append(a.src())
        return f'{self.prefix}{self.t.name}({", ".join(parts)})'

    def make(self, m, f):
        vals = []
        for a in self.args:
            if isinstance(a, tuple) and a[0] == 'leaf':
                vals.append(OV(LEAF, {'id': a[1]}))
            elif isinstance(a, tuple) and a[0] == 'rc':
                vals.append(retain(f.look(a[2]).v))
            else:
                vals.append(a.ev(m, f))
        o = construct(m, self.t, vals)
        if self.prefix == 'rc ':
            return RcH(RcBox(o))
        return o


class CopyOf(Init):
    def __init__(self, name, uid, explicit=False):
        self.name, self.uid, self.explicit = name, uid, explicit

    def src(self):
        return f'{self.name}.copy()' if self.explicit else self.name

    def make(self, m, f):
        return ocopy(m, f.look(self.uid).v)


class Make(Init):
    """make_T(id, v): constructs inside a function and returns it (a move)."""

    def __init__(self, t, idv, e):
        self.t, self.idv, self.e = t, idv, e

    def src(self):
        return f'make_{self.t.name}({self.idv}, {self.e.src()})'

    def make(self, m, f):
        v = self.e.ev(m, f)
        vals = [self.idv, wrap(v * 2)]
        if self.t.has('leaf'):
            vals.append(OV(LEAF, {'id': self.idv + 100}))
        o = construct(m, self.t, vals)
        m.emit(f'make {self.t.tag}{self.idv}')
        return o


class ListTake(Init):
    def __init__(self, lname, luid, how):
        self.lname, self.luid, self.how = lname, luid, how

    def src(self):
        return f'{self.lname}.{self.how}'

    def make(self, m, f):
        lst = f.look(self.luid).v
        if self.how == 'take()':
            out = ListV(lst.items)
            lst.items = []
            return out
        if self.how == 'copy()':
            return ocopy(m, lst)
        if self.how == 'pop()':
            if not lst.items:
                raise Bail('pop')
            return lst.items.pop()
        if self.how.startswith('get('):
            k = int(self.how[4:-1])
            return ocopy(m, lst.items[k])
        if self.how.startswith('remove('):
            k = int(self.how[7:-1])
            return lst.items.pop(k)
        raise ValueError(self.how)


class Simple(Init):
    def __init__(self, text, fn):
        self.text, self.fn = text, fn

    def src(self):
        return self.text

    def make(self, m, f):
        return self.fn(m, f)


class TempRead(Expr):
    """A read off a temporary, dropped right after the read."""

    def __init__(self, init, how):
        self.init, self.how = init, how

    def src(self, parent=0):
        if self.how == 'peek':
            return f'peek_{self.init.t.name}({self.init.src()})'
        return f'{self.init.src()}.{self.how}'

    def ev(self, m, f):
        o = self.init.make(m, f)
        if self.how == 'v':
            r = o.f['v']
        elif self.how == 'get()':
            r = get_value(o)
        else:
            r = wrap(o.f['v'] + o.f['id'] * 10)
        odestroy(m, o)
        return r


class PeekCall(Expr):
    def __init__(self, t, place):
        self.t, self.place = t, place

    def src(self, parent=0):
        return f'peek_{self.t.name}({self.place.text})'

    def ev(self, m, f):
        o = self.place.get(m, f)
        return wrap(o.f['v'] + o.f['id'] * 10)


LEAF = TypeDef('Leaf', [('id', 'id')], False, True, False, 'L')

# area() differs per implementor so a dispatch to the wrong one shows
AREA_K = {'Ta': 1000, 'Tb': 2000}


def area_of(o):
    return wrap(o.f['v'] * 5 + o.f['id'] + AREA_K[o.t.name])


class AreaCall(Expr):
    """s.area() through an interface binding (or element)."""

    def __init__(self, place):
        self.place = place

    def src(self, parent=0):
        return f'{self.place.text}.area()'

    def ev(self, m, f):
        return area_of(self.place.get(m, f))


class IsTest(Expr):
    def __init__(self, place, tname):
        self.place, self.tname = place, tname

    def src(self, parent=0):
        return f'{self.place.text} is {self.tname}'

    def ev(self, m, f):
        return self.place.get(m, f).t.name == self.tname


class TypeMatch(Stmt):
    """match s { Ta(x) -> println(x.v) Tb(y) -> println(y.id) }: the arm
    binding views the object (no copy, no drop)."""

    def __init__(self, place, tag, names):
        self.place, self.tag, self.names = place, tag, names

    def lines(self, ind):
        a, b = self.names
        return [f'{ind}match {self.place.text} {{',
                f'{ind}    Ta({a}) -> println("{self.tag} A {{{a}.v}} {{{a}.get()}}")',
                f'{ind}    Tb({b}) -> println("{self.tag} B {{{b}.id}}")',
                f'{ind}}}']

    def run(self, m, f):
        o = self.place.get(m, f)
        if o.t.name == 'Ta':
            m.emit(f'{self.tag} A {o.f["v"]} {get_value(o)}')
        else:
            m.emit(f'{self.tag} B {o.f["id"]}')


# ------------------------------------------------------------- generator

class ObjGen(Gen):
    NFUNCS = (0, 3)
    NMAIN = (10, 26)

    def __init__(self, rng):
        super().__init__(rng)
        self.next_id = 0

    def oid(self):
        self.next_id += 1
        return self.next_id

    def gen_top(self):
        r = self.r
        self.types = []
        ta = [('id', 'id'), ('v', 'int')]
        if r.random() < 0.6:
            ta.append(('leaf', 'leaf'))
        self.ta = TypeDef('Ta', ta, r.random() < 0.6, True, True, 'A')
        self.tb = TypeDef('Tb', [('id', 'id'), ('v', 'int')], r.random() < 0.5,
                          r.random() < 0.85, True, 'B')
        self.tp = TypeDef('P', [('id', 'id'), ('w', 'id'), ('leaf', 'leaf')], False,
                          r.random() < 0.8, False, 'P')
        self.th = TypeDef('Holder', [('id', 'id'), ('r', 'rc')], False, True, False, 'H')
        self.stack_types = [self.ta, self.tb, self.tp]

    def top_lines(self):
        out = ['object Leaf {', '    val id: Int', '    drop {',
               '        println("drop L{self.id}")', '    }', '}', '']
        out += ['interface Shape {', '    readonly func area() -> Int', '}', '']
        for t in [self.ta, self.tb, self.tp, self.th]:
            out.append(f'object {t.name}{" impl Shape" if t in (self.ta, self.tb) else ""} {{')
            for n, k in t.fields:
                typ = {'id': 'Int', 'int': 'Int', 'leaf': 'Leaf', 'rc': 'rc Ta'}[k]
                out.append(f'    {"var" if k == "int" else "val"} {n}: {typ}')
            if t.init:
                ps = ', '.join(f'{n}: {"Leaf" if k == "leaf" else "Int"}' for n, k in t.fields)
                out.append(f'    init({ps}) {{')
                for n, k in t.fields:
                    out.append(f'        self.{n} = {n}')
                out.append('    }')
            if t.has('int'):
                out += ['    func bump(k: Int) {',
                        f'        println("bump {t.tag}{{self.id}}")',
                        '        self.v += k', '    }',
                        '    readonly func get() -> Int {',
                        '        return self.v * 3 + self.id', '    }',
                        '    readonly func area() -> Int {',
                        f'        return self.v * 5 + self.id + {AREA_K[t.name]}', '    }']
            elif t is self.tp:
                out += ['    readonly func get() -> Int {',
                        '        return self.id * 2 + self.w', '    }']
            if t.has_copy:
                out += ['    copy {', f'        println("copy {t.tag}{{self.id}}")', '    }']
            if t.has_drop:
                if t.has('int'):
                    out += ['    drop {', f'        println("drop {t.tag}{{self.id}} {{self.v}}")', '    }']
                else:
                    out += ['    drop {', f'        println("drop {t.tag}{{self.id}}")', '    }']
            out += ['}', '']
        for t in (self.ta, self.tb):
            args = f'id, v * 2' + (', Leaf(id + 100)' if t.has('leaf') else '')
            out += [f'func make_{t.name}(id: Int, v: Int) -> {t.name} {{',
                    f'    val t = {t.name}({args})',
                    f'    println("make {t.tag}{{id}}")',
                    '    return t', '}', '',
                    f'func peek_{t.name}(t: {t.name}) -> Int {{',
                    '    return t.v + t.id * 10', '}', '',
                    f'func poke_{t.name}(var t: {t.name}, k: Int) {{',
                    '    t.v += k',
                    f'    println("poke {t.tag}{{t.id}}")', '}', '']
        return out

    # ---------------------------------------------------------- places
    def obj_places(self, mutable=None, with_int=False):
        """(Place, TypeDef, writable) for every visible object."""
        out = []
        for t in self.stack_types:
            for n in self.names(t.name):
                mu = self.visible()[n][1]
                out.append((bind_place(n, self.uid(n)), t, mu))
            for n in self.names('heap ' + t.name):
                mu = self.visible()[n][1]
                out.append((bind_place(n, self.uid(n)), t, mu))
        for t in (self.ta, self.tb):
            for n in self.names('rc ' + t.name):
                out.append((bind_place(n, self.uid(n)), t, True))
        if with_int:
            out = [p for p in out if p[1].has('int')]
        if mutable:
            out = [p for p in out if p[2]]
        return out

    def new_init(self, t, ctx, prefix=''):
        r = self.r
        args = []
        for n, k in t.fields:
            if k == 'id':
                args.append(Lit(self.oid() if n == 'id' else r.randint(0, 9)))
            elif k == 'int':
                args.append(self.int_expr(1, ctx))
            elif k == 'leaf':
                args.append(('leaf', self.oid()))
            elif k == 'rc':
                rs = self.names('rc Ta')
                args.append(('rc', rs[0], self.uid(rs[0])) if rs else None)
        if any(a is None for a in args):
            return None
        return New(t, args, prefix)

    def int_leaf(self):
        r = self.r
        if r.random() < 0.3:
            ps = self.obj_places()
            if ps:
                p, t, _ = r.choice(ps)
                c = r.random()
                if t.has('int') and c < 0.5:
                    return FieldGet(p, 'v')
                if c < 0.75:
                    return GetCall(p)
                if t.has('leaf'):
                    return FieldGet(p, 'leaf.id')
                return FieldGet(p, 'id')
            hs = self.names('Holder')
            if hs and r.random() < 0.5:
                return FieldGet(bind_place(hs[0], self.uid(hs[0])), 'r.v')
            ls = [n for t in ('[Ta]', '[Tb]', '[rc Ta]') for n in self.names(t)]
            if ls:
                n = r.choice(ls)
                return Builtin('len', [self.ref(n)], lambda x: len(x.items))
        return super().int_leaf()

    def print_stmt(self, ctx):
        r = self.r
        parts = [self.next_tag()]
        for _ in range(r.randint(1, 3)):
            parts.append(' ')
            c = r.random()
            ps = self.obj_places()
            if ps and c < 0.6:
                p, t, _ = r.choice(ps)
                k = r.random()
                if t.has('int') and k < 0.5:
                    parts.append(FieldGet(p, 'v'))
                elif k < 0.8:
                    parts.append(GetCall(p))
                elif t in (self.ta, self.tb) and (p.text in self.names(t.name)):
                    parts.append(PeekCall(t, p))
                else:
                    parts.append(FieldGet(p, 'id'))
            else:
                parts.append(self.int_expr(2, ctx))
        return Print(Interp(parts))

    # ------------------------------------------------------ statements
    def extra_weights(self, ctx, depth):
        if ctx.pure or ctx.in_defer:
            return []
        def has(*ts, mut=None):
            return any(self.names(t, mut) for t in ts)
        w = [('onew', 6), ('ocopy', 2 if self.obj_places() else 0),
             ('oassign', 3 if any(self.names(t.name, True) for t in self.stack_types) else 0),
             ('omut', 5 if self.obj_places(mutable=True, with_int=True) else 0),
             ('otemp', 2), ('olist', 2), ('olop', 6 if has('[Ta]', '[Tb]', '[rc Ta]', mut=True) else 0),
             ('orc', 3), ('oweak', 2 if has('weak rc Ta') else 0),
             ('oheap', 1), ('oholder', 1 if has('rc Ta') else 0),
             ('inew', 3), ('iuse', 3 if has('Shape') else 0),
             ('iassign', 2 if has('Shape', mut=True) else 0),
             ('ilist', 1), ('ilop', 3 if has('[Shape]', mut=True) else 0)]
        if depth > 0:
            w += [('oiter', 2 if has('[Ta]', '[Tb]') else 0), ('oscope', 2)]
        return w

    def decl(self, typ, mut, init, ann=None, heap=False):
        name = self.fresh('o')
        uid = self.bind(name, typ, mut)
        return name, uid, ODecl(name, uid, mut, init, ann, heap)

    def s_onew(self, ctx, depth):
        r = self.r
        t = r.choice(self.stack_types)
        c = r.random()
        if t in (self.ta, self.tb) and c < 0.3:
            init = Make(t, self.oid(), self.int_expr(1, ctx))
        else:
            init = self.new_init(t, ctx)
        mut = t.has('int') and r.random() < 0.7
        return [self.decl(t.name, mut, init)[2]]

    def s_ocopy(self, ctx, depth):
        r = self.r
        cands = [(n, t) for t in self.stack_types for n in self.names(t.name)]
        if not cands:
            return self.s_onew(ctx, depth)
        n, t = r.choice(cands)
        mut = t.has('int') and r.random() < 0.6
        return [self.decl(t.name, mut, CopyOf(n, self.uid(n), r.random() < 0.3))[2]]

    def s_oassign(self, ctx, depth):
        r = self.r
        cands = [(n, t) for t in self.stack_types for n in self.names(t.name, True)]
        n, t = r.choice(cands)
        srcs = [s for s in self.names(t.name) if s != n]
        if srcs and r.random() < 0.5:
            init = CopyOf(r.choice(srcs), None)
            init.uid = self.uid(init.name)
        elif r.random() < 0.3 and t in (self.ta, self.tb):
            init = Make(t, self.oid(), self.int_expr(1, ctx))
        else:
            init = self.new_init(t, ctx)
        return [OAssign(n, self.uid(n), init)]

    def s_omut(self, ctx, depth):
        r = self.r
        p, t, _ = r.choice(self.obj_places(mutable=True, with_int=True))
        c = r.random()
        if c < 0.4:
            return [FieldSet(p, r.choice(['=', '+=', '-=', '^=']), self.int_expr(2, ctx))]
        if c < 0.75:
            return [Bump(p, self.int_expr(1, ctx))]
        stack = [n for n in self.names(t.name, True)]
        if stack:
            n = r.choice(stack)
            return [Poke(t, n, self.uid(n), self.int_expr(1, ctx))]
        return [Bump(p, self.int_expr(1, ctx))]

    def shape_init(self, ctx):
        """A value for an interface binding or element: a new object (moved),
        a made one (moved), or a copy of an object or interface binding."""
        r = self.r
        c = r.random()
        srcs = [n for t in ('Ta', 'Tb', 'Shape') for n in self.names(t)]
        if srcs and c < 0.35:
            n = r.choice(srcs)
            return CopyOf(n, self.uid(n))
        t = r.choice([self.ta, self.tb])
        if c < 0.55:
            return Make(t, self.oid(), self.int_expr(1, ctx))
        return self.new_init(t, ctx)

    def s_inew(self, ctx, depth):
        mut = self.r.random() < 0.6
        return [self.decl('Shape', mut, self.shape_init(ctx), ann='Shape')[2]]

    def s_iassign(self, ctx, depth):
        n = self.r.choice(self.names('Shape', True))
        init = self.shape_init(ctx)
        if isinstance(init, CopyOf) and init.name == n:
            init = self.new_init(self.tb, ctx)
        return [OAssign(n, self.uid(n), init)]

    def s_iuse(self, ctx, depth):
        r = self.r
        n = r.choice(self.names('Shape'))
        p = bind_place(n, self.uid(n))
        c = r.random()
        if c < 0.45:
            return [Print(Interp([self.next_tag() + ' ', AreaCall(p)]))]
        if c < 0.7:
            return [Print(Interp([self.next_tag() + ' ', IsTest(p, r.choice(['Ta', 'Tb']))]))]
        return [TypeMatch(p, self.next_tag(), (self.fresh('x'), self.fresh('y')))]

    def s_ilist(self, ctx, depth):
        return [self.decl('[Shape]', True, Simple('[]', lambda m, f: ListV([])), ann='[Shape]')[2]]

    def s_ilop(self, ctx, depth):
        r = self.r
        n = r.choice(self.names('[Shape]', True))
        u = self.uid(n)
        L = lambda m, f: f.look(u).v
        c = r.random()
        if c < 0.5:
            ei = self.shape_init(ctx)
            return [ListOp(f'{n}.push({ei.src()})', lambda m, f, ei=ei: L(m, f).items.append(ei.make(m, f)))]
        if c < 0.65:
            def pop(m, f):
                odestroy(m, L(m, f).items.pop())
            return [self.guard(n, u, 0, [ListOp(f'{n}.pop()', pop)])]
        if c < 0.85:
            k = r.randint(0, 2)
            return [self.guard(n, u, k, [Print(Interp([self.next_tag() + ' ', AreaCall(elem_place(n, u, k))]))])]
        # every element, lent to the loop
        e = self.fresh('e')
        self.push()
        eu = self.bind(e, 'Shape#lent', False)
        self.pop()
        tag = self.next_tag()
        return [ObjForIn(e, eu, n, u, Block([Print(Interp([tag + ' ', AreaCall(bind_place(e, eu))]))]))]

    def s_otemp(self, ctx, depth):
        r = self.r
        t = r.choice([self.ta, self.tb])
        c = r.random()
        if c < 0.3:
            e = TempRead(Make(t, self.oid(), self.int_expr(1, ctx)), r.choice(['v', 'get()']))
        elif c < 0.6:
            e = TempRead(self.new_init(t, ctx), 'peek')
        else:
            e = TempRead(self.new_init(t, ctx), r.choice(['v', 'get()']))
        return [Print(Interp([self.next_tag() + ' ', e]))]

    def s_olist(self, ctx, depth):
        r = self.r
        typ = r.choice(['[Ta]', '[Tb]', '[rc Ta]'])
        srcs = self.names(typ)
        c = r.random()
        if srcs and c < 0.25:
            n = r.choice(srcs)
            init = ListTake(n, self.uid(n), 'copy()')
        elif self.names(typ, True) and c < 0.4:
            n = r.choice(self.names(typ, True))
            init = ListTake(n, self.uid(n), 'take()')
        else:
            init = Simple('[]', lambda m, f: ListV([]))
        return [self.decl(typ, True, init, ann=typ if isinstance(init, Simple) else None)[2]]

    def guard(self, lname, luid, k, body):
        return If([(Bin('>', Builtin('len', [Var(lname, luid)], lambda x: len(x.items)), Lit(k)),
                    Block(body))])

    def s_olop(self, ctx, depth):
        r = self.r
        cands = [(n, t) for t in ('[Ta]', '[Tb]', '[rc Ta]') for n in self.names(t, True)]
        n, typ = r.choice(cands)
        u = self.uid(n)
        k = r.randint(0, 2)
        isrc = typ == '[rc Ta]'
        et = {'[Ta]': self.ta, '[Tb]': self.tb, '[rc Ta]': self.ta}[typ]
        L = lambda m, f: f.look(u).v
        c = r.random()

        def elem_init():
            if isrc:
                hs = self.names('rc Ta')
                if hs and r.random() < 0.6:
                    h = r.choice(hs)
                    return Simple(h, lambda m, f, hu=self.uid(h): retain(f.look(hu).v))
                return self.new_init(self.ta, ctx, 'rc ')
            srcs = self.names(et.name)
            if srcs and r.random() < 0.4:
                s = r.choice(srcs)
                return CopyOf(s, self.uid(s))
            if r.random() < 0.3:
                return Make(et, self.oid(), self.int_expr(1, ctx))
            return self.new_init(et, ctx)

        if c < 0.3:
            ei = elem_init()
            return [ListOp(f'{n}.push({ei.src()})',
                           lambda m, f, ei=ei: L(m, f).items.append(ei.make(m, f)))]
        if c < 0.4:
            def pop(m, f):
                odestroy(m, L(m, f).items.pop())
            return [self.guard(n, u, 0, [ListOp(f'{n}.pop()', pop)])]
        if c < 0.48:
            def rem(m, f, k=k):
                odestroy(m, L(m, f).items.pop(k))
            return [self.guard(n, u, k, [ListOp(f'{n}.remove({k})', rem)])]
        if c < 0.56:
            # bind the popped / removed / copied element in a block of its own
            how = r.choice(['pop()', f'remove({k})', f'get({k})'] if not isrc else ['pop()', f'remove({k})'])
            self.push()
            name, uid, d = self.decl(typ[1:-1], False, ListTake(n, u, how))
            body = [d, self.print_stmt(ctx)]
            self.pop()
            return [self.guard(n, u, k if how != 'pop()' else 0, body)]
        if c < 0.63:
            ei = elem_init()
            def ins(m, f, ei=ei, k=k):
                L(m, f).items.insert(k, ei.make(m, f))
            return [self.guard(n, u, k, [ListOp(f'{n}.insert({k}, {ei.src()})', ins)])]
        if c < 0.7:
            ei = elem_init()
            def setel(m, f, ei=ei, k=k):
                new = ei.make(m, f)
                lst = L(m, f).items
                old = lst[k]
                lst[k] = new
                odestroy(m, old)
            return [self.guard(n, u, k, [ListOp(f'{n}[{k}] = {ei.src()}', setel)])]
        if c < 0.82:
            p = elem_place(n, u, k)
            if r.random() < 0.5:
                st = FieldSet(p, r.choice(['=', '+=']), self.int_expr(1, ctx))
            else:
                st = Bump(p, self.int_expr(1, ctx))
            return [self.guard(n, u, k, [st])]
        if c < 0.88:
            def clear(m, f):
                odestroy(m, L(m, f))
            return [ListOp(f'{n}.clear()', clear)]
        if c < 0.93:
            return [ListOp(f'{n}.reverse()', lambda m, f: L(m, f).items.reverse())]
        p = elem_place(n, u, k)
        return [self.guard(n, u, k, [Print(Interp([self.next_tag() + ' ', FieldGet(p, 'v'), ' ',
                                                   GetCall(p)]))])]

    def s_orc(self, ctx, depth):
        r = self.r
        hs = self.names('rc Ta')
        c = r.random()
        if hs and c < 0.35:
            h = r.choice(hs)
            hu = self.uid(h)
            init = Simple(h, lambda m, f, hu=hu: retain(f.look(hu).v))
        else:
            init = self.new_init(self.ta, ctx, 'rc ')
        out = [self.decl('rc Ta', r.random() < 0.3, init)[2]]
        if r.random() < 0.4:
            # a weak handle to it, declared in this scope or reassigned
            name, uid, d = out[0].name, out[0].uid, None
            ws = self.names('weak rc Ta', True)
            if ws and r.random() < 0.6:
                w = r.choice(ws)
                out.append(OAssign(w, self.uid(w), Simple(f'{name}.downgrade()',
                                                         lambda m, f, uid=uid: WeakH(f.look(uid).v.box))))
            else:
                out.append(self.decl('weak rc Ta', True, Simple(f'{name}.downgrade()',
                                                               lambda m, f, uid=uid: WeakH(f.look(uid).v.box)))[2])
        return out

    def s_oweak(self, ctx, depth):
        r = self.r
        w = r.choice(self.names('weak rc Ta'))
        wu = self.uid(w)
        self.push()
        name, uid, d = self.decl('rc Ta?', False,
                                 Simple(f'{w}.upgrade()', lambda m, f: (
                                     retain(RcH(f.look(wu).v.box)) if f.look(wu).v.box and
                                     f.look(wu).v.box.alive else None)))
        tag = self.next_tag()
        p = bind_place(name, uid)
        body = If([(Raw(f'{name} != null', lambda m, f: f.look(uid).v is not None, 7),
                    Block([Print(Interp([tag + ' alive ', FieldGet(p, 'v')]))]))],
                  Block([Print(Interp([tag + ' gone']))]))
        self.pop()
        return [BareBlock(Block([d, body]))]

    def s_oheap(self, ctx, depth):
        t = self.r.choice([self.ta, self.tb])
        init = self.new_init(t, ctx, 'heap ')
        init.prefix = 'heap '
        name, uid, d = self.decl('heap ' + t.name, self.r.random() < 0.7, Simple(init.src(), lambda m, f, i=init: _heap_make(i, m, f)), heap=True)
        return [d, Defer(Free(name, uid), single=True)]

    def s_oholder(self, ctx, depth):
        init = self.new_init(self.th, ctx)
        return [self.decl('Holder', False, init)[2]]

    def s_oiter(self, ctx, depth):
        r = self.r
        cands = [(n, t) for t in ('[Ta]', '[Tb]') for n in self.names(t)]
        n, typ = r.choice(cands)
        et = self.ta if typ == '[Ta]' else self.tb
        e = self.fresh('e')
        lc = ctx.sub(loop=ctx.loop + 1, in_defer=False)
        self.frozen = getattr(self, 'frozen', set())
        self.frozen.add(n)
        self.push()
        eu = self.bind(e, et.name, False)
        body = self.block(lc, r.randint(1, 2), depth - 1)
        self.pop()
        self.frozen.discard(n)
        return [ObjForIn(e, eu, n, self.uid(n), body)]

    def names(self, typ, mut=None):
        ns = super().names(typ, mut)
        if mut:
            fr = getattr(self, 'frozen', set())
            ns = [x for x in ns if x not in fr]
        return ns

    def s_oscope(self, ctx, depth):
        """A block whose objects die at its end, with a weak handle that
        outlives the rc it points to."""
        r = self.r
        out = []
        if r.random() < 0.5 and not self.names('weak rc Ta'):
            out.append(self.decl('weak rc Ta', True, Simple('weak rc null', lambda m, f: WeakH(None)),
                                 ann='weak rc Ta')[2])
        out.append(BareBlock(self.block(ctx, r.randint(2, 4), depth - 1)))
        return out


def _heap_make(init, m, f):
    o = New(init.t, init.args).make(m, f)
    return o


def generate(rng):
    g = ObjGen(rng)
    g.program()
    return g
