"""Pointer lifetimes (manual 6.1 to 6.8): programs that take pointers to
storage of every kind, keep them in the slots of a hub object that lives
for the whole program, and dereference them later, while the storage they
point at comes and goes:

  - locals of inner blocks, loop iterations, match-free if/else arms and
    function frames (calls, self recursion, recursion deeper than 1024
    frames), read in the frame that took them, in callees, after the block
    or the call ended, and from a spawned task (#471, #537, #538, #541)
  - heap values freed with `free` or `defer free` (#539, #540)
  - arena values across `save`, `restore` and `reset`, restores to save
    points a reset or an earlier restore invalidated (#432)
  - rc objects, kept alive by a list in the hub or released at the end of
    their binding's block (#540)
  - pointers to fields (`&x.v`), derived from another pointer (`&q.v`) or
    taken by a method from its receiver (`&self.v`, #542)
  - list element pointers handed to a call that keeps them (stale after the
    call) or that grows the list during the call (#543)
  - pointers handed on through a `?*T` parameter, made from a place
    reached through a block's own pointer (`&p.v`), or taken by a method
    called through a pointer parameter, each of which must end with the
    block that owns the storage
  - guards: objects with a `drop` block that reads a pointer. Locals are
    destroyed in reverse order of declaration after the block's defers, so
    a guard reads a local declared before it and panics on one declared
    after it
  - a `var` captured by a closure kept in the hub: it lives in a cell, and a
    pointer to it is valid while the closure (or the frame) holds the cell
  - a global, always valid.

The model computes, for every dereference, whether the storage is still
alive. The first dereference of dead storage ends the program with the
panic of its kind ("dangling stack pointer to frame", "stale pointer
dereference", "stale pointer dereference (arena)", "list changed while a
pointer to one of its elements is in use", "arena restored to a stale save
point"); every other dereference prints the value the storage holds.
"""
from pf_lang import Block, Stmt, Expected, Bail

NC = 4          # *Cell slots in the hub
NI = 3          # *Int slots in the hub
DEEP_SLOT = 0   # the slot the deep recursion uses

MSG = {
    'stack': 'dangling stack pointer to frame',
    'heap': 'stale pointer dereference',
    'rc': 'stale pointer dereference',
    'elem': 'stale pointer dereference',
    'arena': 'stale pointer dereference (arena)',
}


class Panic(Exception):
    def __init__(self, msg):
        super().__init__(msg)
        self.msg = msg


class Store:
    __slots__ = ('kind', 'alive', 'value', 'seq')

    def __init__(self, kind, value, seq=-1):
        self.kind, self.alive, self.value, self.seq = kind, True, value, seq


class Mark:
    __slots__ = ('seq', 'valid')

    def __init__(self, seq):
        self.seq, self.valid = seq, True


class Scope:
    def __init__(self, parent):
        self.parent = parent
        self.names = {}       # name -> sid / Mark
        self.defers = []      # `defer` statements, run first, in reverse
        self.on_exit = []     # destruction of the locals, in reverse

    def lookup(self, name):
        s = self
        while s is not None:
            if name in s.names:
                return s.names[name]
            s = s.parent
        raise Bail(f'unbound {name}')


class Model:
    def __init__(self, gen):
        self.g = gen
        self.out = []
        self.stores = {}
        self.next = 0
        self.cslot = [None] * NC
        self.islot = [None] * NI
        self.fslot = None         # the storage the hub's closure reads, or None
        self.seq = 0
        self.marks = []
        self.rc = {}
        self.cell_rc = {}
        self.gcell = None         # (storage, slot) of the var the hub's g closure captured
        self.steps = 0
        self.xs = list(gen.xs_init)
        self.glob = self.new('global', gen.gval)

    def new(self, kind, value):
        self.next += 1
        st = Store(kind, value)
        if kind == 'arena':
            st.seq = self.seq
            self.seq += 1
        self.stores[self.next] = st
        return self.next

    def tick(self):
        self.steps += 1
        if self.steps > 200000:
            raise Bail('too many steps')

    def deref(self, sid):
        st = self.stores[sid]
        if not st.alive:
            raise Panic(MSG[st.kind])
        return st

    def kill(self, sid):
        self.stores[sid].alive = False

    def run_scope(self, stmts, scope):
        try:
            for s in stmts:
                self.tick()
                s.run(self, scope)
        finally:
            pass
        for fn in reversed(scope.defers):
            fn()
        for fn in reversed(scope.on_exit):
            fn()


def ind_lines(stmts, ind):
    out = []
    for s in stmts:
        out.extend(s.lines(ind))
    return out


class Seq(Block):
    """A plain list of statements of the enclosing block."""

    def run(self, m, sc):
        for s in self.stmts:
            m.tick()
            s.run(m, sc)


# ---------------------------------------------------------------- statements

class DeclStack(Stmt):
    def __init__(self, name, v):
        self.name, self.v = name, v

    def lines(self, ind):
        return [f'{ind}var {self.name} = Cell({self.v})']

    def run(self, m, sc):
        sid = m.new('stack', self.v)
        sc.names[self.name] = sid
        sc.on_exit.append(lambda: m.kill(sid))


class DeclHeap(Stmt):
    def __init__(self, name, v, defer):
        self.name, self.v, self.defer = name, v, defer

    def lines(self, ind):
        out = [f'{ind}var {self.name} = heap Cell({self.v})']
        if self.defer:
            out.append(f'{ind}defer free {self.name}')
        return out

    def run(self, m, sc):
        sid = m.new('heap', self.v)
        sc.names[self.name] = sid
        if self.defer:
            sc.defers.append(lambda: m.kill(sid))


class FreeHeap(Stmt):
    def __init__(self, name):
        self.name = name

    def lines(self, ind):
        return [f'{ind}free {self.name}']

    def run(self, m, sc):
        sid = sc.lookup(self.name)
        st = m.stores[sid]
        if not st.alive:
            raise Panic(MSG['heap'])      # a double free
        st.alive = False


class DeclArena(Stmt):
    def __init__(self, name, v):
        self.name, self.v = name, v

    def lines(self, ind):
        return [f'{ind}var {self.name} = heap(in: ar) Cell({self.v})']

    def run(self, m, sc):
        sc.names[self.name] = m.new('arena', self.v)


class DeclRc(Stmt):
    def __init__(self, name, v, keep):
        self.name, self.v, self.keep = name, v, keep

    def lines(self, ind):
        out = [f'{ind}val {self.name} = rc Cell({self.v})']
        if self.keep:
            out.append(f'{ind}h.keep.push({self.name})')
        return out

    def run(self, m, sc):
        sid = m.new('rc', self.v)
        sc.names[self.name] = sid
        m.rc[sid] = 2 if self.keep else 1

        def release():
            m.rc[sid] -= 1
            if m.rc[sid] == 0:
                m.kill(sid)
        sc.on_exit.append(release)


def target_sid(m, sc, name):
    return m.glob if name == 'gcell' else sc.lookup(name)


class TakeC(Stmt):
    def __init__(self, slot, name):
        self.slot, self.name = slot, name

    def lines(self, ind):
        return [f'{ind}h.c{self.slot} = &{self.name}']

    def run(self, m, sc):
        m.cslot[self.slot] = target_sid(m, sc, self.name)


class TakeI(Stmt):
    def __init__(self, slot, name):
        self.slot, self.name = slot, name

    def lines(self, ind):
        return [f'{ind}h.i{self.slot} = &{self.name}.v']

    def run(self, m, sc):
        sid = target_sid(m, sc, self.name)
        if m.stores[sid].kind in ('heap', 'arena'):
            m.deref(sid)       # a field access through the handle
        m.islot[self.slot] = sid


class Derive(Stmt):
    """&q.v for q read out of a *Cell slot: a field access through q (so
    q is checked) giving a pointer to the same storage (#539)."""

    def __init__(self, islot, cslot):
        self.islot, self.cslot = islot, cslot

    def lines(self, ind):
        return [f'{ind}{{', f'{ind}    val q = h.c{self.cslot}', f'{ind}    if q != null {{',
                f'{ind}        h.i{self.islot} = &q.v', f'{ind}    }}', f'{ind}}}']

    def run(self, m, sc):
        sid = m.cslot[self.cslot]
        if sid is not None:
            m.deref(sid)
            m.islot[self.islot] = sid


class ReadC(Stmt):
    def __init__(self, slot, tag):
        self.slot, self.tag = slot, tag

    def lines(self, ind):
        return [f'{ind}{{', f'{ind}    val q = h.c{self.slot}', f'{ind}    if q != null {{',
                f'{ind}        println("{self.tag} {{q.v}}")', f'{ind}    }}', f'{ind}}}']

    def run(self, m, sc):
        sid = m.cslot[self.slot]
        if sid is not None:
            m.out.append(f'{self.tag} {m.deref(sid).value}')


class ReadI(Stmt):
    def __init__(self, slot, tag):
        self.slot, self.tag = slot, tag

    def lines(self, ind):
        return [f'{ind}{{', f'{ind}    val q = h.i{self.slot}', f'{ind}    if q != null {{',
                f'{ind}        println("{self.tag} {{q}}")', f'{ind}    }}', f'{ind}}}']

    def run(self, m, sc):
        sid = m.islot[self.slot]
        if sid is not None:
            m.out.append(f'{self.tag} {m.deref(sid).value}')


class ReadBind(Stmt):
    def __init__(self, name, tag):
        self.name, self.tag = name, tag

    def lines(self, ind):
        return [f'{ind}println("{self.tag} {{{self.name}.v}}")']

    def run(self, m, sc):
        m.out.append(f'{self.tag} {m.deref(target_sid(m, sc, self.name)).value}')


class Write(Stmt):
    def __init__(self, name, v):
        self.name, self.v = name, v

    def lines(self, ind):
        return [f'{ind}{self.name}.v = {self.v}']

    def run(self, m, sc):
        m.deref(target_sid(m, sc, self.name)).value = self.v


class Pin(Stmt):
    """A method storing &self.v into an *Int slot: the receiver's
    generation (#542). The receiver is dereferenced at the call."""

    def __init__(self, name, slot):
        self.name, self.slot = name, slot

    def lines(self, ind):
        return [f'{ind}{self.name}.pin(h, {self.slot})']

    def run(self, m, sc):
        sid = target_sid(m, sc, self.name)
        m.deref(sid)
        m.islot[self.slot] = sid


class KeepN(Stmt):
    """keepn(h, &x): the pointer goes through a `?*Cell` parameter into a
    hub slot."""

    def __init__(self, slot, name):
        self.slot, self.name = slot, name

    def lines(self, ind):
        return [f'{ind}keepn{self.slot}(h, &{self.name})']

    def run(self, m, sc):
        m.cslot[self.slot] = target_sid(m, sc, self.name)


class ViaField(Stmt):
    """A pointer to a binding kept in a val of the block, and the address of
    a field reached through it stored in the hub: the derived pointer has
    the binding's lifetime."""

    def __init__(self, slot, name, pname):
        self.slot, self.name, self.pname = slot, name, pname

    def lines(self, ind):
        return [f'{ind}val {self.pname} = &{self.name}', f'{ind}h.i{self.slot} = &{self.pname}.v']

    def run(self, m, sc):
        sid = target_sid(m, sc, self.name)
        m.deref(sid)
        m.islot[self.slot] = sid


class PinVia(Stmt):
    """pinvia(&x, h, k): a function that calls the &self.v method through
    its pointer parameter."""

    def __init__(self, name, slot):
        self.name, self.slot = name, slot

    def lines(self, ind):
        return [f'{ind}pinvia(&{self.name}, h, {self.slot})']

    def run(self, m, sc):
        sid = target_sid(m, sc, self.name)
        m.deref(sid)
        m.islot[self.slot] = sid


class DeclGuard(Stmt):
    """A local with a drop block that reads the pointer it holds, if any."""

    def __init__(self, name, tag):
        self.name, self.tag = name, tag

    def lines(self, ind):
        return [f'{ind}var {self.name} = Guard("{self.tag}")']

    def run(self, m, sc):
        g = {'sid': None}
        sc.names[self.name] = g

        def drop():
            if g['sid'] is not None:
                m.out.append(f'{self.tag} {m.deref(g["sid"]).value}')
        sc.on_exit.append(drop)


class SetGuard(Stmt):
    def __init__(self, gname, name):
        self.gname, self.name = gname, name

    def lines(self, ind):
        return [f'{ind}{self.gname}.p = &{self.name}']

    def run(self, m, sc):
        g = sc.lookup(self.gname)
        if not isinstance(g, dict):
            raise Bail('not a guard')
        g['sid'] = target_sid(m, sc, self.name)


class CellVar(Stmt):
    """A var captured by a closure stored in the hub lives in a cell shared
    with the closure; the closure also stores a pointer to it in a slot. The
    cell lives while the frame (until the block ends) or the hub's closure
    holds it."""

    def __init__(self, name, v, slot):
        self.name, self.v, self.slot = name, v, slot

    def lines(self, ind):
        return [f'{ind}var {self.name} = Cell({self.v})',
                f'{ind}h.g = func() {{', f'{ind}    {self.name}.v += 1',
                f'{ind}    h.c{self.slot} = &{self.name}', f'{ind}}}']

    def run(self, m, sc):
        sid = m.new('heap', self.v)       # stale, not dangling, once the cell is gone
        sc.names[self.name] = sid
        old = m.gcell

        def frame_release():
            m.cell_rc[sid] -= 1
            if m.cell_rc[sid] == 0:
                m.kill(sid)
        m.cell_rc[sid] = 2                # the frame and the hub's closure
        m.gcell = (sid, self.slot)
        if old is not None:               # the closure this one replaces lets go
            m.cell_rc[old[0]] -= 1
            if m.cell_rc[old[0]] == 0:
                m.kill(old[0])
        sc.on_exit.append(frame_release)


class CallG(Stmt):
    def lines(self, ind):
        return [f'{ind}h.g()']

    def run(self, m, sc):
        if m.gcell is not None:
            sid, slot = m.gcell
            m.deref(sid).value += 1
            m.cslot[slot] = sid


class Nest(Stmt):
    def __init__(self, body):
        self.body = body

    def lines(self, ind):
        return [f'{ind}{{'] + self.body.lines(ind + '    ') + [f'{ind}}}']

    def run(self, m, sc):
        m.run_scope(self.body.stmts, Scope(sc))


class Loop(Stmt):
    def __init__(self, var, n, body):
        self.var, self.n, self.body = var, n, body

    def lines(self, ind):
        return ([f'{ind}for {self.var} in range({self.n}) {{'] + self.body.lines(ind + '    ') +
                [f'{ind}}}'])

    def run(self, m, sc):
        for _ in range(self.n):
            m.run_scope(self.body.stmts, Scope(sc))


class If(Stmt):
    def __init__(self, k, then, other):
        self.k, self.then, self.other = k, then, other

    def lines(self, ind):
        out = [f'{ind}if d > {self.k} {{'] + self.then.lines(ind + '    ')
        if self.other is not None:
            out += [f'{ind}}} else {{'] + self.other.lines(ind + '    ')
        return out + [f'{ind}}}']

    def run(self, m, sc):
        d = sc.lookup('d')
        if d > self.k:
            m.run_scope(self.then.stmts, Scope(sc))
        elif self.other is not None:
            m.run_scope(self.other.stmts, Scope(sc))


class Call(Stmt):
    def __init__(self, fidx, darg):
        self.fidx, self.darg = fidx, darg

    def lines(self, ind):
        return [f'{ind}f{self.fidx}(h, ar, {self.darg})']

    def run(self, m, sc):
        fn = m.g.fnmap.get(self.fidx)
        if fn is None:
            raise Bail('function removed')
        fn.call(m, self.darg)


class Recurse(Stmt):
    def __init__(self, fidx):
        self.fidx = fidx

    def lines(self, ind):
        return [f'{ind}if d > 0 {{', f'{ind}    f{self.fidx}(h, ar, d - 1)', f'{ind}}}']

    def run(self, m, sc):
        d = sc.lookup('d')
        if d > 0:
            m.g.fnmap[self.fidx].call(m, d - 1)


class ArenaSave(Stmt):
    def __init__(self, name):
        self.name = name

    def lines(self, ind):
        return [f'{ind}val {self.name} = ar.save()']

    def run(self, m, sc):
        mk = Mark(m.seq)
        m.marks.append(mk)
        sc.names[self.name] = mk


class ArenaRestore(Stmt):
    def __init__(self, name):
        self.name = name

    def lines(self, ind):
        return [f'{ind}ar.restore({self.name})']

    def run(self, m, sc):
        mk = sc.lookup(self.name)
        if not isinstance(mk, Mark):
            raise Bail('not a mark')
        if not mk.valid:
            raise Panic('arena restored to a stale save point')
        for st in m.stores.values():
            if st.kind == 'arena' and st.seq >= mk.seq:
                st.alive = False
        for o in m.marks:
            if o.seq > mk.seq:
                o.valid = False


class ArenaReset(Stmt):
    def lines(self, ind):
        return [f'{ind}ar.reset()']

    def run(self, m, sc):
        for st in m.stores.values():
            if st.kind == 'arena':
                st.alive = False
        for o in m.marks:
            o.valid = False


class ElemStash(Stmt):
    """stash(&xs[j], h): the callee keeps the element pointer, which is
    only valid during the call (#543)."""

    def __init__(self, j, slot):
        self.j, self.slot = j, slot

    def lines(self, ind):
        return [f'{ind}stash{self.slot}(&xs[{self.j}], h)']

    def run(self, m, sc):
        sid = m.new('elem', m.xs[self.j])
        m.cslot[self.slot] = sid
        m.kill(sid)


class ElemWrite(Stmt):
    def __init__(self, j, v):
        self.j, self.v = j, v

    def lines(self, ind):
        return [f'{ind}xs[{self.j}].v = {self.v}']

    def run(self, m, sc):
        m.xs[self.j] = self.v


class ElemGrow(Stmt):
    """grow(&xs[j], g): a closure the callee runs grows the list the
    pointer points into while the call runs: the borrow panics at the growth
    (#543). (`grow(&xs[j], xs)` with a `var` list parameter is E0330.)"""

    def __init__(self, j):
        self.j = j

    def lines(self, ind):
        return [f'{ind}grow(&xs[{self.j}], func() {{',
                f'{ind}    for k in range(1000) {{',
                f'{ind}        xs.push(Cell(k))',
                f'{ind}    }}',
                f'{ind}}})']

    def run(self, m, sc):
        raise Panic('list changed while a pointer to one of its elements is in use')


class SpawnRead(Stmt):
    """A task reads a slot and is awaited at once: a stack pointer read on
    another thread is checked against its owner's scopes (#538)."""

    def __init__(self, slot, tag):
        self.slot, self.tag = slot, tag

    def lines(self, ind):
        t = self.tag
        return [f'{ind}{{', f'{ind}    val t = spawn("{t}") {{', f'{ind}        val q = h.c{self.slot}',
                f'{ind}        if q != null {{', f'{ind}            return q.v', f'{ind}        }}',
                f'{ind}        return -1', f'{ind}    }}', f'{ind}    println("{t} {{await t}}")',
                f'{ind}}}']

    def run(self, m, sc):
        sid = m.cslot[self.slot]
        v = -1 if sid is None else m.deref(sid).value
        m.out.append(f'{self.tag} {v}')


class CaptureC(Stmt):
    """A closure kept in the hub captures a pointer to a binding and reads
    through it when called, possibly after the binding's block ended."""

    def __init__(self, name, pname):
        self.name, self.pname = name, pname

    def lines(self, ind):
        return [f'{ind}val {self.pname} = &{self.name}',
                f'{ind}h.f = func() -> Int {{ return {self.pname}.v }}']

    def run(self, m, sc):
        m.fslot = target_sid(m, sc, self.name)


class CallF(Stmt):
    def __init__(self, tag):
        self.tag = tag

    def lines(self, ind):
        return [f'{ind}println("{self.tag} {{h.f()}}")']

    def run(self, m, sc):
        v = -1 if m.fslot is None else m.deref(m.fslot).value
        m.out.append(f'{self.tag} {v}')


class WithReset(Stmt):
    """A method of a binding runs a lambda that resets the arena: an arena
    receiver is pinned for the call, so the reset panics (#650); any other
    receiver goes on, and the reset takes effect."""

    def __init__(self, name):
        self.name = name

    def lines(self, ind):
        return [f'{ind}{self.name}.with(func() {{', f'{ind}    ar.reset()', f'{ind}}})']

    def run(self, m, sc):
        sid = target_sid(m, sc, self.name)
        st = m.deref(sid)
        if st.kind == 'arena':
            raise Panic('an arena value was reset while one of its methods is running')
        ArenaReset().run(m, sc)
        st.value += 1


class DeepCall(Stmt):
    """deep(h, n, at, rd): frame `at` (counted down from n) stores &c into
    the deep slot, frame `rd` reads it, then every frame returns."""

    def __init__(self, n, at, rd, tag):
        self.n, self.at, self.rd, self.tag = n, at, rd, tag

    def lines(self, ind):
        return [f'{ind}deep(h, {self.n}, {self.at}, {self.rd}, "{self.tag}")']

    def run(self, m, sc):
        frames = []
        for d in range(self.n, -1, -1):
            m.tick()
            sid = m.new('stack', d)
            frames.append(sid)
            if d == self.at:
                m.cslot[DEEP_SLOT] = sid
            if d == self.rd:
                s = m.cslot[DEEP_SLOT]
                if s is not None:
                    m.out.append(f'{self.tag} {m.deref(s).value}')
        for sid in frames:
            m.kill(sid)


# ----------------------------------------------------------------- functions

class Fn:
    def __init__(self, idx):
        self.idx = idx
        self.body = Block([])

    def lines(self):
        return ([f'func f{self.idx}(h: *var Hub, ar: Arena, d: Int) {{'] +
                self.body.lines('    ') + ['}'])

    def call(self, m, d):
        if m.depth > 64:
            raise Bail('deep')
        m.depth += 1
        sc = Scope(None)
        sc.names['d'] = d
        try:
            m.run_scope(self.body.stmts, sc)
        finally:
            m.depth -= 1


DECLS = '''object Cell {
    var v: Int
    init(v: Int) {
        self.v = v
    }
    func pin(h: *var Hub, k: Int) {
%PIN%
    }
    func with(f: func()) {
        f()
        self.v += 1
    }
}

object Hub {
%SLOTS%
    var keep: [rc Cell]
    var f: func() -> Int
    var g: func()
    init() {
%INIT%
        self.keep = []
        self.f = func() -> Int { return -1 }
        self.g = func() {}
    }
}

object Guard {
    var p: ?*Cell
    val tag: String
    init(tag: String) {
        self.p = null
        self.tag = tag
    }
    drop {
        val q = self.p
        if q != null {
            println("{self.tag} {q.v}")
        }
    }
}

func pinvia(p: *var Cell, h: *var Hub, k: Int) {
    p.pin(h, k)
}

func grow(p: *Cell, g: func()) {
    g()
    println("grown {p.v}")
}

func deep(h: *var Hub, d: Int, at: Int, rd: Int, tag: String) {
    var c = Cell(d)
    if d == at {
        h.c%DEEP% = &c
    }
    if d == rd {
        val q = h.c%DEEP%
        if q != null {
            println("{tag} {q.v}")
        }
    }
    if d > 0 {
        deep(h, d - 1, at, rd, tag)
    }
}
'''


class PtrLifeGen:
    def __init__(self, rng):
        self.r = rng
        self.n = 0
        self.funcs = []
        self.fnmap = {}
        self.mainblock = Block([])
        self.gval = rng.randint(1, 99)
        self.xs_init = [rng.randint(1, 99) for _ in range(rng.randint(2, 5))]
        self.d0 = rng.randint(0, 3)
        self.panic_budget = 1

    def fresh(self, p):
        self.n += 1
        return f'{p}{self.n}'

    # --------------------------------------------------------- generation
    def block(self, ctx, depth, budget):
        """ctx: dict with 'vis' (list of (name, kind, mutable)), 'marks',
        'fidx' (None for main), 'loop'. Returns a list of statements."""
        r = self.r
        out = []
        vis = list(ctx['vis'])
        marks = list(ctx['marks'])
        guards = list(ctx.get('guards', []))
        ctx = {**ctx, 'guards': guards}
        pending_free = []
        n = r.randint(2, budget)
        for _ in range(n):
            x = r.random()
            y = r.random()
            addressable = [v for v in vis if v[1] != 'rc'] + [('gcell', 'global')]
            if y < 0.03:
                out.append(KeepN(r.randrange(NC), r.choice(addressable)[0]))
                continue
            elif y < 0.06:
                out.append(ViaField(r.randrange(NI), r.choice(addressable)[0], self.fresh('p')))
                continue
            elif y < 0.09:
                out.append(PinVia(r.choice(addressable)[0], r.randrange(NI)))
                continue
            elif y < 0.12:
                nm = self.fresh('g')
                out.append(DeclGuard(nm, self.fresh('gd')))
                guards.append(nm)
                continue
            elif y < 0.16 and guards:
                out.append(SetGuard(r.choice(guards), r.choice(addressable)[0]))
                continue
            elif y < 0.175:
                nm = self.fresh('cv')
                out.append(CellVar(nm, r.randint(1, 999), r.randrange(NC)))
                vis.append((nm, 'cell'))
                continue
            elif y < 0.19:
                out.append(CallG())
                continue
            if x < 0.11:
                nm = self.fresh('c')
                out.append(DeclStack(nm, r.randint(1, 999)))
                vis.append((nm, 'stack'))
            elif x < 0.17:
                nm = self.fresh('h')
                dfr = r.random() < 0.6
                out.append(DeclHeap(nm, r.randint(1, 999), dfr))
                vis.append((nm, 'heap'))
                if not dfr:
                    pending_free.append(nm)
            elif x < 0.22:
                nm = self.fresh('a')
                out.append(DeclArena(nm, r.randint(1, 999)))
                vis.append((nm, 'arena'))
            elif x < 0.26:
                nm = self.fresh('r')
                out.append(DeclRc(nm, r.randint(1, 999), r.random() < 0.5))
                vis.append((nm, 'rc'))
            elif x < 0.36 and vis:
                # `&` of an rc handle is E0296; its field (&r.v) is fine
                nm = r.choice([v for v in vis if v[1] != 'rc'] + [('gcell', 'global')])[0]
                out.append(TakeC(r.randrange(NC), nm))
            elif x < 0.42 and vis:
                nm = r.choice(vis + [('gcell', 'global')])[0]
                out.append(TakeI(r.randrange(NI), nm))
            elif x < 0.45:
                out.append(Derive(r.randrange(NI), r.randrange(NC)))
            elif x < 0.56:
                out.append(ReadC(r.randrange(NC), self.fresh('rc')))
            elif x < 0.61:
                out.append(ReadI(r.randrange(NI), self.fresh('ri')))
            elif x < 0.64 and vis:
                out.append(ReadBind(r.choice(vis)[0], self.fresh('rb')))
            elif x < 0.67 and vis:
                out.append(Write(r.choice(vis + [('gcell', 'global')])[0], r.randint(1, 999)))
            elif x < 0.70 and vis:
                out.append(Pin(r.choice(vis + [('gcell', 'global')])[0], r.randrange(NI)))
            elif x < 0.73 and pending_free:
                out.append(FreeHeap(pending_free.pop(0)))
            elif x < 0.77 and depth < 3:
                out.append(Nest(Block(self.block({**ctx, 'vis': vis, 'marks': marks, 'guards': guards},
                                                 depth + 1, max(2, budget - 2)))))
            elif x < 0.80 and depth < 3:
                lv = self.fresh('i')
                out.append(Loop(lv, r.randint(1, 3),
                                Block(self.block({**ctx, 'vis': vis, 'marks': marks, 'guards': guards},
                                                 depth + 1, max(2, budget - 2)))))
            elif x < 0.83 and depth < 3:
                t = Block(self.block({**ctx, 'vis': vis, 'marks': marks, 'guards': guards}, depth + 1,
                                     max(2, budget - 3)))
                o = (Block(self.block({**ctx, 'vis': vis, 'marks': marks, 'guards': guards}, depth + 1,
                                      max(2, budget - 3))) if r.random() < 0.5 else None)
                out.append(If(r.randint(-1, 2), t, o))
            elif x < 0.88 and ctx['callees']:
                out.append(Call(r.choice(ctx['callees']), r.randint(0, 3)))
            elif x < 0.90 and ctx['fidx'] is not None and ctx['recursive']:
                out.append(Recurse(ctx['fidx']))
            elif x < 0.92:
                nm = self.fresh('m')
                out.append(ArenaSave(nm))
                marks.append(nm)
            elif x < 0.94 and marks:
                out.append(ArenaRestore(r.choice(marks)))
            elif x < 0.95:
                out.append(ArenaReset())
            elif x < 0.965 and ctx['fidx'] is None and depth == 0:
                out.append(ElemStash(r.randrange(len(self.xs_init)), r.randrange(NC)))
            elif x < 0.975 and ctx['fidx'] is None and depth == 0:
                out.append(ElemWrite(r.randrange(len(self.xs_init)), r.randint(1, 999)))
            elif x < 0.985:
                out.append(SpawnRead(r.randrange(NC), self.fresh('s')))
            elif x < 0.9875 and vis:
                out.append(CaptureC(r.choice([v for v in vis if v[1] != 'rc'] +
                                             [('gcell', 'global')])[0], self.fresh('p')))
            elif x < 0.989:
                out.append(CallF(self.fresh('f')))
            elif x < 0.9895 and vis:
                out.append(WithReset(r.choice(vis)[0]))
            elif x < 0.99 and ctx['fidx'] is None and depth == 0 and self.panic_budget:
                self.panic_budget -= 1
                out.append(ElemGrow(r.randrange(len(self.xs_init))))
            elif ctx['fidx'] is None and depth == 0:
                n = r.choice([3, 20, 1030, 1100])
                at = r.randint(0, n)
                rd = r.choice([at, r.randint(0, n)])
                out.append(DeepCall(n, at, rd, self.fresh('dp')))
        # an explicit free for every heap binding that has no defer
        for nm in pending_free:
            out.append(FreeHeap(nm))
        return out

    def generate(self):
        r = self.r
        nf = r.randint(1, 4)
        for k in range(nf, 0, -1):
            fn = Fn(k)
            callees = [j for j in range(k + 1, nf + 1)]
            ctx = {'vis': [], 'marks': [], 'fidx': k, 'callees': callees,
                   'recursive': r.random() < 0.4}
            fn.body.stmts = self.block(ctx, 1, 7)
            self.fnmap[k] = fn
            self.funcs.insert(0, (fn,))
        ctx = {'vis': [], 'marks': [], 'fidx': None, 'callees': list(range(1, nf + 1)),
               'recursive': False}
        self.mainblock.stmts = self.block(ctx, 0, 14)
        return self

    # ---------------------------------------------------------- output
    def render(self):
        pin = []
        for k in range(NI):
            kw = 'if' if k == 0 else '} elif'
            pin += [f'        {kw} k == {k} {{', f'            h.i{k} = &self.v']
        pin.append('        }')
        slots = ([f'    var c{k}: ?*Cell' for k in range(NC)] +
                 [f'    var i{k}: ?*Int' for k in range(NI)])
        init = ([f'        self.c{k} = null' for k in range(NC)] +
                [f'        self.i{k} = null' for k in range(NI)])
        text = (DECLS.replace('%PIN%', '\n'.join(pin)).replace('%SLOTS%', '\n'.join(slots))
                .replace('%INIT%', '\n'.join(init)).replace('%DEEP%', str(DEEP_SLOT)))
        lines = text.rstrip('\n').split('\n') + ['', f'var gcell = Cell({self.gval})', '']
        for k in range(NC):
            lines += [f'func stash{k}(p: *Cell, h: *var Hub) {{', f'    h.c{k} = p', '}', '']
            lines += [f'func keepn{k}(h: *var Hub, p: ?*Cell) {{', f'    h.c{k} = p', '}', '']
        for (fn,) in self.funcs:
            lines += fn.lines() + ['']
        xs = ', '.join(f'Cell({v})' for v in self.xs_init)
        lines += ['func main() {', '    var hub = Hub()', '    val h = &hub',
                  '    val ar = Arena(4194304)', f'    var xs = [{xs}]', f'    val d = {self.d0}']
        lines += self.mainblock.lines('    ') + ['}']
        return '\n'.join(lines) + '\n'

    def model(self):
        m = Model(self)
        m.depth = 0
        self.fnmap = {fn.idx: fn for (fn,) in self.funcs}
        sc = Scope(None)
        sc.names['d'] = self.d0
        try:
            m.run_scope(self.mainblock.stmts, sc)
        except Panic as p:
            e = Expected(m.out, panic=p.msg)
            e.leaks_ok = True
            return e
        return Expected(m.out)


def generate(rng):
    g = PtrLifeGen(rng).generate()
    g.lines_cache = g.model()
    return g
