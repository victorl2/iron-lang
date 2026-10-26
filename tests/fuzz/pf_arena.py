"""Arena programs (manual 6.6): `heap(in: a) T(...)`, `in a { heap T(...) }`
blocks (nested, innermost wins, arenas declared inside them), lists and
strings made inside `in` blocks (ordinary values, freed as usual), field
reads, writes and method calls on arena objects, objects with plain fields
and with fields that own memory outside the arena (String, list, rc,
closure: W0605, leaked by definition) or a drop block (never run), arenas
passed to functions, `a.save()` / `a.restore(m)` / `a.reset()`, `a.used()`
and `a.capacity()`, `heap Arena(n)` freed with `free`, arenas in loops,
`Arena.threadsafe` shared by a parallel for and a spawn, and a stale access
after reset or restore, which must stop the program with the stale pointer
error; mixed with the control flow of pf_stmt.

Rules the model follows (manual 6.6 and 6.2):
  - an arena value is never freed alone; reset frees all of them, restore
    the ones allocated after the mark; using one afterwards is a stale
    pointer error (only generated as the program's last statement, right
    after the reset or restore, before anything else is allocated there);
  - the drop block of an object allocated in an arena never runs, and an
    rc handle stored in one is never released (its target's drop never
    runs either);
  - used() is 0 after reset, back to its value at save() after restore, and
    grows with every allocation; the model compares two readings only when
    one sequence of allocations is a prefix of the other;
  - capacity() is the size the arena was made with.
"""
import random
from pf_lang import *
from pf_stmt import Gen, Ctx
from pf_conc import MutexV, ThreadV, LockDecl, GuardGet, GuardSet, PFor, SpawnDecl, Await

CAP = 1 << 20

HELPERS = '''object Pt {
    var x: Int
    var y: Int
    init(x: Int, y: Int) {
        self.x = x
        self.y = y
    }
    readonly func sum() -> Int {
        return self.x + self.y
    }
    func bump(d: Int) {
        self.x = self.x + d
    }
}

object Seg {
    val a: Pt
    val b: Pt
    readonly func span() -> Int {
        return (self.b.x - self.a.x) + (self.b.y - self.a.y)
    }
}

object Tag {
    val name: String
    val n: Int
}

object Bag {
    var items: [Int]
    val k: Int
    init(items: [Int], k: Int) {
        self.items = items.copy()
        self.k = k
    }
}

object Res {
    val id: Int
    drop {
        println("drop res {self.id}")
    }
}

object Hold {
    val r: rc Res
    val n: Int
}

object Cb {
    val f: func(Int) -> Int
    val n: Int
}

object Noisy {
    val id: Int
    drop {
        println("drop noisy {self.id}")
    }
}

func psum(p: Pt) -> Int {
    return p.sum()
}

func fill_pts(a: Arena, n: Int) -> Int {
    var t = 0
    for i in range(n) {
        val p = heap(in: a) Pt(i, n)
        t += p.sum()
    }
    return t
}

func inner_in(a: Arena, k: Int) -> Int {
    var t = 0
    in a {
        val p = heap Pt(k, k + 1)
        val s = heap Seg(Pt(k, 0), Pt(0, k))
        t = p.sum() * 2 + s.span()
    }
    return t
}

func roll(a: Arena, k: Int) -> Int {
    val m = a.save()
    val p = heap(in: a) Pt(k, k)
    val q = heap(in: a) Pt(p.x + 1, p.y * 2)
    val t = q.sum() - p.sum()
    a.restore(m)
    return t
}

func scratch(n: Int) -> Int {
    val a = Arena(4096)
    var t = 0
    for i in range(n) {
        val p = heap(in: a) Pt(i, i * i)
        t += p.sum()
        a.reset()
    }
    return t + a.used()
}
'''

# owning fields leak in an arena; Noisy has a drop block
OWNING = ('Tag', 'Bag', 'Hold', 'Cb')
DROPS = OWNING + ('Noisy',)
TYPES = ('Pt', 'Pt', 'Pt', 'Seg', 'Tag', 'Bag', 'Hold', 'Cb', 'Noisy')


# ---------------------------------------------------------------- values

class ArenaV:
    def __init__(self, cap, ordered=True):
        self.cap = cap
        self.seq = []          # allocation tags since the last reset
        self.objs = []         # live arena objects, in allocation order
        self.alive = True
        self.ordered = ordered  # False: threads allocate in any order

    def alloc(self, o, tag):
        o.arena, o.idx, o.dead = self, len(self.seq), False
        self.seq.append(tag)
        self.objs.append(o)
        return o

    def reset(self):
        for o in self.objs:
            o.dead = True
        self.seq, self.objs = [], []

    def restore(self, mark):
        for o in self.objs:
            if o.idx >= mark.n:
                o.dead = True
        self.objs = [o for o in self.objs if not o.dead]
        del self.seq[mark.n:]


class MarkV:
    def __init__(self, a):
        self.a, self.n = a, len(a.seq)


class UsedV:
    def __init__(self, a):
        self.a, self.seq = a, tuple(a.seq)


def used_cmp(op, a, snap):
    """a.used() op snap, decided from the allocation sequences."""
    if snap.a is not a or not a.ordered:
        raise Bail('used of another arena')
    cur = tuple(a.seq)
    old = snap.seq
    if cur == old:
        c = 0
    elif cur[:len(old)] == old:
        c = 1
    elif old[:len(cur)] == cur:
        c = -1
    else:
        raise Bail('used not comparable')
    return {'==': c == 0, '>': c > 0, '<': c < 0, '>=': c >= 0}[op]


class OV:
    """An object value: type name and fields. Arena objects also carry
    their arena, index and dead flag."""

    def __init__(self, t, **f):
        self.t, self.f = t, f
        self.arena, self.dead = None, False


def live(o):
    if o.dead:
        raise Bail('stale arena value used')
    return o


class ResV:
    def __init__(self, i):
        self.id = i


class RcRes:
    """An rc Res handle; the drop prints when the last handle goes and no
    arena object holds it."""

    def __init__(self, i):
        self.box = ResV(i)
        self.held = False


# ---------------------------------------------------------------- nodes

class Text(Expr):
    """Source text and an evaluator."""

    def __init__(self, text, fn):
        self.text, self.fn = text, fn

    def src(self, parent=0):
        return self.text

    def ev(self, m, f):
        return self.fn(m, f)


class Stmt1(Stmt):
    """One source line and an action."""

    def __init__(self, text, fn):
        self.text, self.fn = text, fn

    def lines(self, ind):
        return [ind + self.text]

    def run(self, m, f):
        self.fn(m, f)


class InArena(Stmt):
    def __init__(self, aname, auid, body):
        self.aname, self.auid, self.body = aname, auid, body

    def lines(self, ind):
        return [f'{ind}in {self.aname} {{'] + self.body.lines(ind + '    ') + [f'{ind}}}']

    def run(self, m, f):
        m.arena_stack.append(f.look(self.auid).v)
        try:
            self.body.run(m, f)
        finally:
            m.arena_stack.pop()


class Alloc(Expr):
    """heap(in: a) T(args) / heap T(args) in an `in` block (aname None)."""

    def __init__(self, t, args, aname=None, auid=None, skip=False):
        self.t, self.args, self.aname, self.auid, self.skip = t, args, aname, auid, skip

    def src(self, parent=0):
        opts = []
        if self.aname:
            opts.append(f'in: {self.aname}')
        if self.skip:
            opts.append('allow_drop_skip: true')
        head = f'heap({", ".join(opts)})' if opts else 'heap'
        return f'{head} {self.t}({", ".join(a.src() for a in self.args)})'

    def ev(self, m, f):
        vals = [a.ev(m, f) for a in self.args]
        a = f.look(self.auid).v if self.aname else m.arena_stack[-1]
        return a.alloc(make_obj(self.t, vals), self.t)


def make_obj(t, vals):
    if t == 'Pt':
        return OV(t, x=vals[0], y=vals[1])
    if t == 'Seg':
        return OV(t, a=vals[0], b=vals[1])
    if t == 'Tag':
        return OV(t, name=vals[0], n=vals[1])
    if t == 'Bag':
        return OV(t, items=ListV(list(vals[0].items)), k=vals[1])
    if t == 'Hold':
        vals[0].held = True
        return OV(t, r=vals[0], n=vals[1])
    if t == 'Cb':
        return OV(t, f=vals[0], n=vals[1])
    if t == 'Noisy':
        return OV(t, id=vals[0])
    raise ValueError(t)


class Get(Expr):
    """A read through an arena object binding: path of fields / a method."""

    def __init__(self, name, uid, path, text, fn):
        self.name, self.uid, self.path, self.text, self.fn = name, uid, path, text, fn

    def src(self, parent=0):
        return self.name + self.text

    def ev(self, m, f):
        return self.fn(live(f.look(self.uid).v), m)


def pt_new(x, y):
    return OV('Pt', x=x, y=y)


# --------------------------------------------------------------- the gen

class ArenaGen(Gen):
    NFUNCS = (0, 2)
    NMAIN = (6, 16)

    def __init__(self, rng):
        super().__init__(rng)
        self.meta = {}        # uid -> dict: kind, arena uid, order, type
        self.dead = set()     # uids of bindings no longer usable
        self.order = 0
        self.in_stack = []    # arena uids of the enclosing `in` blocks
        self.leaks_ok = False
        self.expect_panic = None
        self.fresh_kill = {}  # arena uid -> uids killed with nothing allocated since
        self.in_func = False
        self.no_reads = 0     # generating a defer body: no arena reads
        # owning fields leak by definition and turn the leak checkers off:
        # only some programs allocate them
        self.owning = rng.random() < 0.4
        self.loop = 0         # loop nesting of the statement being generated

    def top_lines(self):
        return HELPERS.splitlines() + ['']

    def names(self, typ, mut=None):
        return [n for n in super().names(typ, mut) if self.uid(n) not in self.dead]

    def model(self):
        m = ArenaMachine(Frame())
        m.rng = random.Random(7)
        m.arena_stack = []
        self.expect_panic = None
        try:
            self.mainblock.run(m, Frame(m.globals, None))
        except StopProgram:
            # the program stops with the stale pointer error; nothing
            # (defers, drops) runs after it
            self.expect_panic = 'stale pointer'
        return m.lines()

    def program(self):
        r = self.r
        self.gen_enums()
        self.push()
        for i in range(r.randint(*self.NFUNCS)):
            self.gen_func(i)
        ctx = Ctx(None)
        self.push()
        main = []
        for _ in range(r.randint(*self.NMAIN)):
            main.extend(self.stmt(ctx, 3))
        if r.random() < 0.2:
            main.extend(self.stale_access())
        elif r.random() < 0.2:
            main.extend(self.stale_tail(ctx))
        self.pop()
        self.pop()
        self.gl = []
        self.mainblock = Block(main)

    def gen_func(self, i):
        self.in_func = True
        super().gen_func(i)
        self.in_func = False

    # ---------------------------------------------------- helpers
    def arenas(self, any_kind=False):
        """Visible arena names (not threadsafe ones unless any_kind);
        an `Arena` parameter is one too, but is never reset here."""
        out = []
        for t in ('arena', 'Arena') + (('arena_ts',) if any_kind else ()):
            out += self.names(t)
        return out

    def expr_of(self, typ, d, ctx):
        if typ == 'Arena':
            ar = self.arenas()
            if ar and self.r.random() < 0.8:
                n = self.r.choice(ar)
                return self.ref(n)
            return Text('Arena(65536)', lambda m, f: ArenaV(65536))
        return super().expr_of(typ, d, ctx)

    def objs(self, t=None, mut=None):
        out = []
        for typ in ([t] if t else set(TYPES)):
            out += self.names('ao:' + typ, mut)
        return out

    def bind_obj(self, name, t, mut, auid):
        uid = self.bind(name, 'ao:' + t, mut)
        self.order += 1
        self.meta[uid] = {'arena': auid, 'order': self.order, 'type': t}
        self.fresh_kill.pop(auid, None)
        return uid

    def ctor_args(self, t, ctx):
        ie = lambda: self.int_expr(1, ctx)
        if t == 'Pt':
            x, y = ie(), ie()
            return [x, y]
        if t == 'Seg':
            a, b, c, d = ie(), ie(), ie(), ie()
            return [Text(f'Pt({a.src()}, {b.src()})', lambda m, f, a=a, b=b: pt_new(a.ev(m, f), b.ev(m, f))),
                    Text(f'Pt({c.src()}, {d.src()})', lambda m, f, c=c, d=d: pt_new(c.ev(m, f), d.ev(m, f)))]
        if t == 'Tag':
            e = ie()
            return [Interp(['tag', e]) if self.r.random() < 0.6 else Lit('t'), ie()]
        if t == 'Bag':
            items = [ie() for _ in range(self.r.randint(1, 3))]
            return [Text('[' + ', '.join(i.src() for i in items) + ']',
                         lambda m, f, items=items: ListV([i.ev(m, f) for i in items])), ie()]
        if t == 'Hold':
            k = self.r.randint(1, 99)
            return [Text(f'rc Res({k})', lambda m, f, k=k: RcRes(k)), ie()]
        if t == 'Cb':
            k = ie()
            lam = Lambda([('x', 'Int', 'x#cb')], 'Int', Block([Return(Bin('+', Var('x', 'x#cb'), Lit(3)))]))
            return [Text(lam.src(), lambda m, f: lam.ev(m, f)), k]
        if t == 'Noisy':
            return [Lit(self.r.randint(0, 99))]
        raise ValueError(t)

    def reads(self, name, t):
        """Expressions reading an arena object binding of type t (Int)."""
        uid = self.uid(name)
        G = lambda text, fn: Get(name, uid, None, text, fn)
        if t == 'Pt':
            return [G('.x', lambda o, m: o.f['x']), G('.y', lambda o, m: o.f['y']),
                    G('.sum()', lambda o, m: wrap(o.f['x'] + o.f['y']))]
        if t == 'Seg':
            return [G('.a.x', lambda o, m: o.f['a'].f['x']), G('.b.y', lambda o, m: o.f['b'].f['y']),
                    G('.span()', lambda o, m: wrap(wrap(o.f['b'].f['x'] - o.f['a'].f['x']) +
                                                     wrap(o.f['b'].f['y'] - o.f['a'].f['y'])))]
        if t == 'Tag':
            return [G('.n', lambda o, m: o.f['n']), G('.name.len()', lambda o, m: len(o.f['name']))]
        if t == 'Bag':
            return [G('.k', lambda o, m: o.f['k']), G('.items.len()', lambda o, m: len(o.f['items'].items)),
                    G('.items[0]', lambda o, m: o.f['items'].items[0])]
        if t == 'Hold':
            return [G('.n', lambda o, m: o.f['n']), G('.r.id', lambda o, m: o.f['r'].box.id)]
        if t == 'Cb':
            return [G('.n', lambda o, m: o.f['n']), G('.f(4)', lambda o, m: o.f['f'].call(m, [4]))]
        if t == 'Noisy':
            return [G('.id', lambda o, m: o.f['id'])]
        raise ValueError(t)

    def obj_type(self, name):
        return self.visible()[name][0][3:]

    def int_leaf(self):
        os_ = self.objs() if not self.no_reads else []
        if os_ and self.r.random() < 0.25:
            n = self.r.choice(os_)
            return self.r.choice(self.reads(n, self.obj_type(n)))
        return super().int_leaf()

    def kill(self, auid, pred):
        """Bindings of arena auid for which pred(meta) holds go stale."""
        killed = []
        for u, mt in self.meta.items():
            if mt.get('arena') == auid and u not in self.dead and pred(mt):
                self.dead.add(u)
                if mt.get('kind') != 'mark' and mt.get('kind') != 'used':
                    killed.append(u)
        if killed:
            self.fresh_kill[auid] = killed

    # ---------------------------------------------------- weights
    def s_defer(self, ctx, depth):
        self.no_reads += 1
        out = super().s_defer(ctx, depth)
        self.no_reads -= 1
        return out

    def stmt(self, ctx, depth):
        self.loop = ctx.loop
        return super().stmt(ctx, depth)

    def resettable(self, ctx):
        """Arenas this code may reset or restore: declared here (not a
        parameter), outside any loop that does not also contain this
        statement (a later iteration would read what the reset freed)."""
        return [a for a in self.arenas() if self.visible()[a][0] == 'arena' and
                self.meta[self.uid(a)].get('loop', 0) == ctx.loop]

    def extra_weights(self, ctx, depth):
        if ctx.in_defer:
            return []
        ar = self.arenas()
        mine = self.resettable(ctx)
        w = [('arena', 3 if len(ar) < 2 else 0.5),
             ('alloc', 5 if ar else 0),
             ('use', 0 if ctx.pure else (4 if self.objs() else 0)),
             ('mutate', 2 if self.objs('Pt', True) or self.objs('Bag', True) else 0),
             ('save', 1.5 if mine else 0),
             ('restore', 2 if self.restorable(ctx) else 0),
             ('reset', 1 if mine else 0),
             ('used', 0 if ctx.pure else (1.5 if ar else 0)),
             ('acall', 1.5 if ar else 0),
             ('stackdrop', 0 if ctx.pure or self.in_stack else 1),
             ('inlist', 1)]
        if depth > 0:
            w += [('inblk', 3 if ar else 0), ('harena', 0 if self.in_stack else 0.5),
                  ('tsa', 0 if ctx.pure else 0.5)]
        return w

    def param_types(self):
        return super().param_types() + ['Arena']

    def gen_func_hook(self):
        pass

    # ---------------------------------------------------- statements
    def s_arena(self, ctx, depth):
        name = self.fresh('ar')
        uid = self.bind(name, 'arena', False)
        cap = self.r.choice([CAP, CAP, 65536])
        self.meta[uid] = {'kind': 'arena', 'cap': cap, 'loop': ctx.loop}
        return [Decl(name, Text(f'Arena({cap})', lambda m, f, cap=cap: ArenaV(cap)), uid=uid)]

    def s_harena(self, ctx, depth):
        """heap Arena(n), freed by a defer at the end of the block."""
        name = self.fresh('ha')
        uid = self.bind(name, 'arena', False)
        self.meta[uid] = {'kind': 'arena', 'cap': CAP, 'loop': ctx.loop}
        d = Decl(name, Text(f'heap Arena({CAP})', lambda m, f: ArenaV(CAP)), uid=uid)
        if ctx.in_defer or ctx.fn is not None and False:
            return [d]
        fr = Defer(Stmt1(f'free {name}', lambda m, f, uid=uid: None), single=True)
        return [d, fr]

    def choose_type(self):
        t = self.r.choice(TYPES)
        if t == 'Hold' and self.in_stack:
            t = 'Tag'   # rc inside an `in` block is E0301
        if t in OWNING and not self.owning:
            t = self.r.choice(['Pt', 'Seg'])
        if t in OWNING:
            self.leaks_ok = True
        return t

    def s_alloc(self, ctx, depth):
        r = self.r
        a = r.choice(self.arenas())
        au = self.uid(a)
        t = self.choose_type()
        args = self.ctor_args(t, ctx)
        skip = t in DROPS and r.random() < 0.6
        name = self.fresh('o')
        mut = t in ('Pt', 'Bag') and r.random() < 0.5
        e = Alloc(t, args, a, au, skip)
        uid = self.bind_obj(name, t, mut, au)
        return [Decl(name, e, mut, uid=uid)]

    def s_use(self, ctx, depth):
        r = self.r
        n = r.choice(self.objs())
        rs = self.reads(n, self.obj_type(n))
        parts = [self.next_tag()]
        for e in r.sample(rs, r.randint(1, len(rs))):
            parts += [' ', e]
        if self.obj_type(n) == 'Pt' and r.random() < 0.3:
            uid = self.uid(n)
            parts += [' ', Text(f'psum({n})', lambda m, f, uid=uid: wrap(live(f.look(uid).v).f['x'] +
                                                                           f.look(uid).v.f['y']))]
        return [Print(Interp(parts))]

    def s_mutate(self, ctx, depth):
        r = self.r
        cands = [(n, 'Pt') for n in self.objs('Pt', True)] + [(n, 'Bag') for n in self.objs('Bag', True)]
        n, t = r.choice(cands)
        uid = self.uid(n)
        e = self.int_expr(2, ctx)
        if t == 'Bag':
            def push(m, f, uid=uid, e=e):
                v = e.ev(m, f)
                live(f.look(uid).v).f['items'].items.append(v)
            return [Stmt1(f'{n}.items.push({e.src()})', push)]
        c = r.random()
        if c < 0.4:
            def setx(m, f, uid=uid, e=e):
                v = e.ev(m, f)
                live(f.look(uid).v).f['x'] = v
            return [Stmt1(f'{n}.x = {e.src()}', setx)]
        if c < 0.6:
            def addy(m, f, uid=uid, e=e):
                v = e.ev(m, f)
                o = live(f.look(uid).v)
                o.f['y'] = wrap(o.f['y'] + v)
            return [Stmt1(f'{n}.y += {e.src()}', addy)]

        def bump(m, f, uid=uid, e=e):
            v = e.ev(m, f)
            o = live(f.look(uid).v)
            o.f['x'] = wrap(o.f['x'] + v)
        return [Stmt1(f'{n}.bump({e.src()})', bump)]

    def restorable(self, ctx):
        out = []
        ok = set(self.uid(a) for a in self.resettable(ctx))
        for mk in self.names('mark'):
            mt = self.meta[self.uid(mk)]
            a = mt['aname']
            if (mt['arena'] in ok and mt['loop'] == ctx.loop and a in self.visible() and
                    self.uid(a) == mt['arena']):
                out.append(mk)
        return out

    def s_save(self, ctx, depth, arena=None):
        # (a save point is restored on the arena it was taken on: a save
        # point of another arena is invalid, manual 6.6)
        a = arena if arena is not None else self.r.choice(self.resettable(ctx))
        au = self.uid(a)
        name = self.fresh('mk')
        uid = self.bind(name, 'mark', False)
        self.order += 1
        self.meta[uid] = {'kind': 'mark', 'arena': au, 'order': self.order, 'aname': a,
                          'loop': ctx.loop}
        return [Decl(name, Text(f'{a}.save()', lambda m, f, au=au: MarkV(f.look(au).v)), uid=uid)]

    def s_restore(self, ctx, depth):
        mk = self.r.choice(self.restorable(ctx))
        mu = self.uid(mk)
        mt = self.meta[mu]
        au, a = mt['arena'], mt['aname']
        order = mt['order']
        self.kill(au, lambda x: x.get('order', 0) > order)

        def run(m, f, au=au, mu=mu):
            f.look(au).v.restore(f.look(mu).v)
        return [Stmt1(f'{a}.restore({mk})', run)]

    def s_reset(self, ctx, depth):
        a = self.r.choice(self.resettable(ctx))
        au = self.uid(a)
        self.kill(au, lambda x: True)
        return [Stmt1(f'{a}.reset()', lambda m, f, au=au: f.look(au).v.reset())]

    def s_used(self, ctx, depth):
        r = self.r
        a = r.choice(self.arenas())
        au = self.uid(a)
        c = r.random()
        if c < 0.3:
            snap = self.fresh('u')
            su = self.bind(snap, 'used', False)
            self.meta[su] = {'kind': 'used', 'arena': au}
            return [Decl(snap, Text(f'{a}.used()', lambda m, f, au=au: UsedV(f.look(au).v)), uid=su)]
        snaps = [s for s in self.names('used') if self.meta[self.uid(s)]['arena'] == au]
        if snaps and c < 0.7:
            s = r.choice(snaps)
            su = self.uid(s)
            op = r.choice(['==', '>', '>=', '<'])
            e = Text(f'{a}.used() {op} {s}', lambda m, f, au=au, su=su, op=op:
                     used_cmp(op, f.look(au).v, f.look(su).v))
        elif c < 0.85:
            e = Text(f'{a}.used() > 0', lambda m, f, au=au: len(f.look(au).v.seq) > 0)
        else:
            e = Text(f'{a}.capacity()', lambda m, f, au=au: f.look(au).v.cap)
        return [Print(Interp([self.next_tag() + ' ', e]))]

    def s_acall(self, ctx, depth):
        """A helper taking the arena: fill / inner_in / roll / scratch."""
        r = self.r
        a = r.choice(self.arenas())
        au = self.uid(a)
        k = self.int_expr(1, ctx)
        kind = r.choice(['fill', 'inner_in', 'roll', 'scratch'])

        if kind == 'fill':
            n = r.randint(0, 6)

            def fn(m, f, au=au, n=n):
                ar = f.look(au).v
                t = 0
                for i in range(n):
                    ar.alloc(pt_new(i, n), 'Pt')
                    t = wrap(t + i + n)
                return t
            text = f"fill_pts({a}, {n})"
            self.fresh_kill.pop(au, None)
        elif kind == 'inner_in':
            def fn(m, f, au=au, k=k):
                ar = f.look(au).v
                kv = k.ev(m, f)
                ar.alloc(pt_new(kv, wrap(kv + 1)), 'Pt')
                ar.alloc(OV('Seg', a=pt_new(kv, 0), b=pt_new(0, kv)), 'Seg')
                return wrap(wrap(wrap(kv + wrap(kv + 1)) * 2) + wrap(wrap(0 - kv) + kv))
            text = f'inner_in({a}, {k.src()})'
            self.fresh_kill.pop(au, None)
        elif kind == 'roll':
            def fn(m, f, au=au, k=k):
                ar = f.look(au).v
                kv = k.ev(m, f)
                mk = MarkV(ar)
                p = ar.alloc(pt_new(kv, kv), 'Pt')
                q = ar.alloc(pt_new(wrap(kv + 1), wrap(kv * 2)), 'Pt')
                t = wrap(wrap(q.f['x'] + q.f['y']) - wrap(p.f['x'] + p.f['y']))
                ar.restore(mk)
                return t
            text = f'roll({a}, {k.src()})'
        else:
            n = r.randint(0, 5)

            def fn(m, f, n=n):
                t = 0
                for i in range(n):
                    t = wrap(t + i + i * i)
                return t
            text = f'scratch({n})'
        name = self.fresh()
        uid = self.bind(name, 'Int', False)
        return [Decl(name, Text(text, fn), uid=uid)]

    def s_stackdrop(self, ctx, depth):
        """Drop blocks of values outside an arena still run; an rc Res held
        by an arena object is never released."""
        r = self.r
        k = r.randint(0, 99)
        c = r.random()
        ar = self.arenas()
        if c < 0.4 and ar and self.owning:
            a = r.choice(ar)
            au = self.uid(a)
            self.leaks_ok = True
            rn, hn = self.fresh('r'), self.fresh('h')
            ru, hu = rn + '#r', hn + '#h'
            n = self.int_expr(1, ctx)
            alloc = Alloc('Hold', [Text(rn, lambda m, f, ru=ru: f.look(ru).v), n], a, au, r.random() < 0.5)
            body = [Decl(rn, Text(f'rc Res({k})', lambda m, f, k=k: RcRes(k)), uid=ru),
                    Decl(hn, alloc, uid=hu),
                    Print(Interp([self.next_tag() + ' ', Text(f'{hn}.r.id', lambda m, f, hu=hu: f.look(hu).v.f['r'].box.id),
                                  ' ', Text(f'{hn}.n', lambda m, f, hu=hu: f.look(hu).v.f['n'])]))]
            self.fresh_kill.pop(au, None)
            return [BareBlock(Block(body))]
        rn = self.fresh('r')
        ru = rn + '#r'
        if c < 0.7:
            def decl(m, f, ru=ru, k=k):
                h = RcRes(k)
                f.vars[ru] = Cell(h)
                f.drops.append(lambda m, h=h: None if h.held else m.emit(f'drop res {h.box.id}'))
            body = [Stmt1(f'val {rn} = rc Res({k})', decl),
                    Print(Interp([self.next_tag() + ' ', Text(f'{rn}.id', lambda m, f, ru=ru: f.look(ru).v.box.id)]))]
            return [BareBlock(Block(body))]

        def decl2(m, f, ru=ru, k=k):
            f.vars[ru] = Cell(k)
            f.drops.append(lambda m, k=k: m.emit(f'drop noisy {k}'))
        body = [Stmt1(f'val {rn} = Noisy({k})', decl2),
                Print(Interp([self.next_tag() + ' ', Text(f'{rn}.id', lambda m, f, ru=ru: f.look(ru).v)]))]
        return [BareBlock(Block(body))]

    def s_inlist(self, ctx, depth):
        """A list and a string made (inside or outside an `in` block):
        ordinary values, owned and freed as usual."""
        r = self.r
        ln = self.fresh('ls')
        lu = ln + '#ls'
        items = [self.int_expr(1, ctx) for _ in range(r.randint(1, 3))]
        e = self.int_expr(1, ctx)
        sn = self.fresh('st')
        su = sn + '#st'
        se = self.int_expr(1, ctx)
        out = [Decl(ln, Text('[' + ', '.join(i.src() for i in items) + ']',
                             lambda m, f, items=items: ListV([i.ev(m, f) for i in items])), True, uid=lu),
               Stmt1(f'{ln}.push({e.src()})', lambda m, f, lu=lu, e=e: f.look(lu).v.items.append(e.ev(m, f))),
               Decl(sn, Bin('+', Lit('s'), Interp(['-', se, '-'])), uid=su)]
        if not ctx.pure:
            out.append(Print(Interp([self.next_tag() + ' ',
                                     Text(f'{ln}.len()', lambda m, f, lu=lu: len(f.look(lu).v.items)), ' ',
                                     Text(f'{ln}[{len(items)}]', lambda m, f, lu=lu, n=len(items): f.look(lu).v.items[n]),
                                     ' ', Var(sn, su)])))
        return [BareBlock(Block(out))]

    def s_inblk(self, ctx, depth):
        """in a { ... }: bare heap allocations use the innermost arena."""
        r = self.r
        a = r.choice(self.arenas())
        au = self.uid(a)
        self.in_stack.append(au)
        self.push()
        stmts = []
        if r.random() < 0.2:
            # an arena declared inside the block, used by a nested `in`
            stmts.extend(self.s_arena(ctx, depth))
        for _ in range(r.randint(1, 4)):
            c = r.random()
            if c < 0.45:
                t = self.choose_type()
                if t == 'Noisy' or t in OWNING:
                    t = 'Pt' if t == 'Noisy' else t
                name = self.fresh('o')
                mut = t in ('Pt', 'Bag') and r.random() < 0.5
                cur = self.in_stack[-1]
                args = self.ctor_args(t, ctx)
                uid = self.bind_obj(name, t, mut, cur)
                stmts.append(Decl(name, Alloc(t, args), mut, uid=uid))
            elif c < 0.6 and depth > 1:
                inner = [x for x in self.arenas() if self.uid(x) != au]
                if inner:
                    stmts.extend(self.s_inblk_with(ctx, depth - 1, r.choice(inner)))
            else:
                stmts.extend(self.stmt(ctx, max(depth - 1, 0)))
        self.pop()
        self.in_stack.pop()
        return [InArena(a, au, Block(stmts))]

    def s_inblk_with(self, ctx, depth, a):
        au = self.uid(a)
        self.in_stack.append(au)
        self.push()
        stmts = []
        for _ in range(r_range(self.r, 1, 3)):
            if self.r.random() < 0.5:
                name = self.fresh('o')
                args = self.ctor_args("Pt", ctx)
                uid = self.bind_obj(name, 'Pt', False, au)
                stmts.append(Decl(name, Alloc('Pt', args), uid=uid))
            else:
                stmts.extend(self.stmt(ctx, max(depth - 1, 0)))
        self.pop()
        self.in_stack.pop()
        return [InArena(a, au, Block(stmts))]

    def s_tsa(self, ctx, depth):
        """Arena.threadsafe shared by a parallel for and a spawn."""
        r = self.r
        ta = self.fresh('ta')
        tu = self.bind(ta, 'arena_ts', False)
        self.meta[tu] = {'kind': 'arena'}
        mn = self.fresh('m')
        mu = mn + '#m'
        i = self.fresh('i')
        iu = i + '#i'
        k = r.randint(0, 9)
        n = r.randint(0, 12)
        z, g = self.fresh('z'), self.fresh('g')
        zu, gu = z + '#z', g + '#g'
        body = Block([Decl(z, Alloc('Pt', [Var(i, iu), Lit(k)], ta, tu), uid=zu),
                      BareBlock(Block([LockDecl(g, gu, mn, mu),
                                       GuardSet(g, gu, Bin('+', GuardGet(g, gu),
                                                           Text(f'{z}.sum()', lambda m, f, zu=zu:
                                                                wrap(f.look(zu).v.f['x'] + f.look(zu).v.f['y']))))]))])
        h = self.fresh('h')
        hu = h + '#h'
        q = self.fresh('q')
        qu = q + '#q'
        sbody = Block([Decl(q, Alloc('Pt', [Lit(k), Lit(n)], ta, tu), uid=qu),
                       Return(Text(f'{q}.sum()', lambda m, f, qu=qu: wrap(f.look(qu).v.f['x'] + f.look(qu).v.f['y'])))])
        g2 = self.fresh('g')
        g2u = g2 + '#g'
        out = [Decl(ta, Text(f'Arena.threadsafe({CAP})', lambda m, f: ArenaV(CAP, ordered=False)), uid=tu),
               Decl(mn, Text('Mutex(0)', lambda m, f: MutexV(0)), uid=mu),
               SpawnDecl(h, hu, self.next_tag(), sbody),
               PFor(i, iu, Lit(n), body),
               Print(Interp([self.next_tag() + ' ', Await(h, hu)])),
               BareBlock(Block([LockDecl(g2, g2u, mn, mu), Print(Interp([self.next_tag() + ' ', GuardGet(g2, g2u)]))])),
               Print(Interp([self.next_tag() + ' ', Text(f'{ta}.used() > 0', lambda m, f, tu=tu: len(f.look(tu).v.seq) > 0)]))]
        return out

    def stale_tail(self, ctx):
        """The program's last statements: allocate, reset or restore past
        the allocation, read it (a stale pointer error)."""
        r = self.r
        ar = [a for a in self.resettable(ctx)]
        out = []
        if not ar or r.random() < 0.3:
            out.extend(self.s_arena(ctx, 0))
            ar = [out[-1].name]
        a = r.choice(ar)
        au = self.uid(a)
        mk = None
        if r.random() < 0.5:
            out.extend(self.s_save(ctx, 0, a))
            mk = out[-1].name
        t = r.choice(['Pt', 'Pt', 'Seg'])
        args = self.ctor_args(t, ctx)
        name = self.fresh('o')
        mut = t == 'Pt' and r.random() < 0.5
        uid = self.bind_obj(name, t, mut, au)
        out.append(Decl(name, Alloc(t, args, a, au), mut, uid=uid))
        out.append(Print(Interp([self.next_tag() + ' ', self.reads(name, t)[0]])))
        if mk is not None:
            mu = self.uid(mk)
            out.append(Stmt1(f'{a}.restore({mk})', lambda m, f, au=au, mu=mu:
                             f.look(au).v.restore(f.look(mu).v)))
        else:
            out.append(Stmt1(f'{a}.reset()', lambda m, f, au=au: f.look(au).v.reset()))
        e = self.reads(name, t)[r.randint(0, 1)]
        out.append(Print(Interp([self.next_tag() + ' ', StaleGet(name, uid, e.text, e.fn)])))
        return out

    def stale_access(self):
        """The last statement: read an arena object killed by the latest
        reset or restore of its arena, with nothing allocated there since."""
        vis = self.visible()
        for au, killed in self.fresh_kill.items():
            names = [n for n, (t, mu, u) in vis.items() if u in killed and t.startswith('ao:')]
            if names:
                n = self.r.choice(names)
                t = vis[n][0][3:]
                e = self.reads(n, t)[0]
                uid = self.uid(n)
                return [Print(Interp([self.next_tag() + ' ', StaleGet(n, uid, e.text, e.fn)]))]
        return []


def _noop(m, f):
    pass


class StopProgram(Exception):
    pass


class ArenaMachine(Machine):
    stopped = False

    def emit(self, line):
        if not self.stopped:
            super().emit(line)


class StaleGet(Expr):
    """The deliberate stale read: the program stops here if the object is
    dead (its arena was reset or restored past it), else reads it."""

    def __init__(self, name, uid, text, fn):
        self.name, self.uid, self.text, self.fn = name, uid, text, fn

    def src(self, parent=0):
        return self.name + self.text

    def ev(self, m, f):
        o = f.look(self.uid).v
        if o.dead:
            m.stopped = True
            raise StopProgram()
        return self.fn(o, m)


def r_range(r, a, b):
    return r.randint(a, b)


class _Model(ArenaGen):
    pass


def generate(rng):
    g = ArenaGen(rng)
    g.program()
    return g
