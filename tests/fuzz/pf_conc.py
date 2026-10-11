"""Concurrency programs (manual 7): spawn and await (Int, String, object,
list and Bool results, captured `val`s, spawns nested in spawns and in
parallel for bodies), fire-and-forget spawns, Mutex updates and reads,
RWLock, channels (producers to main, main to consumers, fan-in, fan-out,
pipelines, a channel used as a buffer by one thread, parallel for bodies
sending), parallel for over ranges and list bindings (nested, inside spawns
and inside functions called several times, which reuses the worker pool),
`rc [Int]` slots written by one iteration or one thread each, and an `rc`
object read from many threads; mixed with the control flow of pf_stmt.

Only orders the manual fixes reach the output: a thread never prints, a
Mutex is only read once every thread that updates it was awaited (or its
parallel for ended), threads update it with one commutative operation, a
channel's items are printed in order only when one thread sends them and
one receives them, and everything else is aggregated order-independently
(a sum, or a list sorted before it is printed).

The model runs each spawn body on a Python thread, with a bounded queue for
each channel and a lock for each Mutex, so blocking sends and receives
behave as in the program; a parallel for runs its iterations in a shuffled
order, so an order dependence the generator let through shows up as a
mismatch."""
import queue
import random
import threading
from pf_lang import *
from pf_stmt import Gen, Ctx

threading.stack_size(64 * 1024 * 1024)
TIMEOUT = 30

HELPERS = '''object P {
    val a: Int
    val s: String
}

func work(n: Int) -> Int {
    var t = 0
    for i in range(n & 15) {
        t += i * n + 1
    }
    return t
}

func label(n: Int) -> String {
    return "w{n}"
}
'''


def _work(n):
    t = 0
    for i in range(n & 15):
        t = wrap(t + wrap(i * n) + 1)
    return t


class PV:
    __slots__ = ('a', 's')

    def __init__(self, a, s):
        self.a, self.s = a, s


class MutexV:
    def __init__(self, v):
        self.value = v
        self.lock = threading.Lock()


class ChanV:
    def __init__(self, cap):
        self.q = queue.Queue(maxsize=cap)


class ThreadV(threading.Thread):
    """A spawned body running in the model."""

    def __init__(self, body, m, f):
        super().__init__(daemon=True)
        self.body, self.m, self.f = body, m, f
        self.result, self.exc = None, None

    def run(self):
        fr = Frame(self.f, 'spawn')
        fr.defers, fr.drops = [], []
        try:
            try:
                for s in self.body.stmts:
                    self.m.tick()
                    s.run(self.m, fr)
            finally:
                exit_frame(self.m, fr)
        except ReturnX as r:
            self.result = r.v
        except BaseException as e:   # noqa: B036 (re-raised at the await)
            self.exc = e


class ConcMachine(Machine):
    def emit(self, line):
        if threading.current_thread() is not threading.main_thread():
            raise Bail('print from a thread')
        super().emit(line)


# ------------------------------------------------------------ nodes

class SpawnDecl(Stmt):
    """val h = spawn("label") { body }, or the bare statement (fire)."""

    def __init__(self, name, uid, label, body, fire=False):
        self.name, self.uid, self.label, self.body, self.fire = name, uid, label, body, fire

    def lines(self, ind):
        head = f'spawn("{self.label}") {{' if self.fire else \
            f'val {self.name} = spawn("{self.label}") {{'
        return [ind + head] + self.body.lines(ind + '    ') + [ind + '}']

    def run(self, m, f):
        th = ThreadV(self.body, m, f)
        th.start()
        if not self.fire:
            f.vars[self.uid] = Cell(th)


class Await(Expr):
    def __init__(self, name, uid):
        self.name, self.uid = name, uid

    def src(self, parent=0):
        s = f'await {self.name}'
        return f'({s})' if parent > 0 else s

    def ev(self, m, f):
        th = f.look(self.uid).v
        th.join(TIMEOUT * 4)
        if th.is_alive():
            raise Bail('model: thread did not finish')
        if th.exc is not None:
            raise th.exc
        return th.result


class LockDecl(Stmt):
    """val g = m.lock() / rw.read() / rw.write(): held to the end of the block."""

    def __init__(self, g, guid, mname, muid, method='lock'):
        self.g, self.guid, self.mname, self.muid, self.method = g, guid, mname, muid, method

    def lines(self, ind):
        return [f'{ind}val {self.g} = {self.mname}.{self.method}()']

    def run(self, m, f):
        mv = f.look(self.muid).v
        if not mv.lock.acquire(timeout=TIMEOUT):
            raise Bail('model: lock wait')
        f.vars[self.guid] = Cell(mv)
        f.drops.append(lambda m, mv=mv: mv.lock.release())


class GuardGet(Expr):
    def __init__(self, g, guid):
        self.g, self.guid = g, guid

    def src(self, parent=0):
        return f'{self.g}.get()'

    def ev(self, m, f):
        return f.look(self.guid).v.value


class GuardSet(Stmt):
    def __init__(self, g, guid, e):
        self.g, self.guid, self.e = g, guid, e

    def lines(self, ind):
        return [f'{ind}{self.g}.set({self.e.src()})']

    def run(self, m, f):
        v = self.e.ev(m, f)
        f.look(self.guid).v.value = v


class PFor(Stmt):
    """for x in range(n) parallel { } / for x in xs parallel { }."""

    def __init__(self, name, uid, it, body, is_range=True):
        self.name, self.uid, self.it, self.body, self.is_range = name, uid, it, body, is_range

    def lines(self, ind):
        it = f'range({self.it.src()})' if self.is_range else self.it.src()
        return [f'{ind}for {self.name} in {it} parallel {{'] + \
            self.body.lines(ind + '    ') + [f'{ind}}}']

    def run(self, m, f):
        v = self.it.ev(m, f)
        items = list(range(max(0, v))) if self.is_range else list(v.items)
        m.rng.shuffle(items)
        for x in items:
            m.tick()
            self.body.run(m, f, {self.uid: x})


class ChanSend(Stmt):
    def __init__(self, ch, uid, e):
        self.ch, self.uid, self.e = ch, uid, e

    def lines(self, ind):
        return [f'{ind}{self.ch}.send({self.e.src()})']

    def run(self, m, f):
        v = self.e.ev(m, f)
        try:
            f.look(self.uid).v.q.put(v, timeout=TIMEOUT)
        except queue.Full:
            raise Bail('model: send blocked')


class ChanRecv(Expr):
    def __init__(self, ch, uid):
        self.ch, self.uid = ch, uid

    def src(self, parent=0):
        return f'{self.ch}.recv()'

    def ev(self, m, f):
        try:
            return f.look(self.uid).v.q.get(timeout=TIMEOUT)
        except queue.Empty:
            raise Bail('model: recv blocked')


class Field(Expr):
    """e.field on a P (or an rc P)."""

    def __init__(self, e, field):
        self.e, self.field = e, field

    def src(self, parent=0):
        return f'{self.e.src(POSTFIX)}.{self.field}'

    def ev(self, m, f):
        return getattr(self.e.ev(m, f), self.field)


class StrLen(Expr):
    def __init__(self, e):
        self.e = e

    def src(self, parent=0):
        return f'{self.e.src(POSTFIX)}.len()'

    def ev(self, m, f):
        return len(self.e.ev(m, f))


class ListLit(Expr):
    def __init__(self, items, prefix=''):
        self.items, self.prefix = items, prefix

    def src(self, parent=0):
        return self.prefix + '[' + ', '.join(i.src() for i in self.items) + ']'

    def ev(self, m, f):
        return ListV([i.ev(m, f) for i in self.items])


class PNew(Expr):
    def __init__(self, a, s, prefix=''):
        self.a, self.s, self.prefix = a, s, prefix

    def src(self, parent=0):
        return f'{self.prefix}P({self.a.src()}, {self.s.src()})'

    def ev(self, m, f):
        return PV(self.a.ev(m, f), self.s.ev(m, f))


class New(Expr):
    """Mutex(e) / RWLock(e) / Channel[T](cap)."""

    def __init__(self, text, arg, fn):
        self.text, self.arg, self.fn = text, arg, fn

    def src(self, parent=0):
        return f'{self.text}({self.arg.src()})'

    def ev(self, m, f):
        return self.fn(self.arg.ev(m, f))


def op_apply(op, a, b):
    return Bin(op, Lit(a), Lit(b)).ev(None, None)


# ---------------------------------------------------------- generator

class ConcGen(Gen):
    NFUNCS = (0, 2)
    NMAIN = (4, 10)

    def __init__(self, rng):
        super().__init__(rng)
        self.floors = []      # [(kind, scope index)] of thread bodies being generated
        self.pending = []     # [{'uid', 'touches'}]: spawned, not yet awaited
        self.collect = []     # touch sets of the spawn bodies being generated
        self.res = {}         # mutex uid -> commutative op
        self.quiet = 0        # generating a body that must have no effects
        self.heavy = 0        # inside a loop of many iterations: no threads

    def top_lines(self):
        return HELPERS.splitlines() + ['']

    def program(self):
        # no globals: a thread reading one while main writes it would race
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
        self.pop()
        self.pop()
        self.gl = []
        self.mainblock = Block(main)
        # a Bail here drops the program (make_gen tries again)
        self.lines_cache = self.model()

    def model(self):
        m = ConcMachine(Frame())
        m.rng = random.Random(7)
        self.mainblock.run(m, Frame(m.globals, None))
        return m.lines()

    # --------------------------------------------------- scope rules
    def entries(self):
        seen = {}
        for idx, s in enumerate(self.scopes):
            for n, (t, mu, u) in s.items():
                seen[n] = (t, mu, u, idx)
        return seen

    def names(self, typ, mut=None):
        """A thread body reads `val`s of the enclosing code (a parallel
        for body also reads `var`s: the enclosing code is waiting) and
        writes only its own bindings."""
        top = self.floors[-1][1] if self.floors else 0
        out = []
        for n, (t, mu, u, idx) in self.entries().items():
            if t != typ:
                continue
            if any(idx < fi and kind == 'spawn' and mu for kind, fi in self.floors):
                continue
            if mut is True and not (mu and idx >= top):
                continue
            if mut is False and mu:
                continue
            out.append(n)
        return out

    def scope_of(self, uid):
        for idx, s in enumerate(self.scopes):
            for n, (t, mu, u) in s.items():
                if u == uid:
                    return idx
        return -1

    def printing(self, ctx):
        return not ctx.pure and not self.floors and not ctx.in_defer

    def noexit(self, ctx):
        """Statements between a spawn and its await: an exit would skip the
        await, so break / continue / return only reach loops inside them."""
        c = ctx.sub(loop=0)
        c.fn = None
        return c

    def body_ctx(self):
        c = Ctx(None)
        c.pure = True
        return c

    def can_read(self, uid):
        top = self.floors[-1][1] if self.floors else 0
        return self.scope_of(uid) >= top and not any(uid in p['touches'] for p in self.pending)

    def thread_ok(self):
        return len(self.floors) < 2 and not self.heavy

    # ------------------------------------------------------ weights
    PLAIN = ('Int', 'Bool', 'Color', 'Tok')

    def s_decl(self, ctx, depth):
        """A shadowing declaration never hides a Mutex, channel or handle
        that generated code refers to by name."""
        out = super().s_decl(ctx, depth)
        d = out[0]
        outer = [s[d.name] for s in self.scopes[:-1] if d.name in s]
        if outer and outer[-1][0] not in self.PLAIN:
            typ, mu, _ = self.scopes[-1].pop(d.name)
            d.name = self.fresh()
            d.uid = self.bind(d.name, typ, mu)
        return out

    def extra_weights(self, ctx, depth):
        if self.quiet:
            return []
        ms = self.names('mutex')
        if ctx.in_defer:
            # a deferred block runs at every exit of its block: tasks in it
            # are emitted once per exit
            w = [('mupd', 2 if ms else 0)]
            if self.thread_ok() and depth > 0:
                w += [('spawn', 3), ('pfor', 3)]
            return w
        w = [('mutex', 2 if not ms else 0.5),
             ('mupd', 4 if ms else 0),
             ('mread', 2 if any(self.can_read(self.uid(n)) for n in ms) else 0),
             ('rcslots', 1.5), ('chan', 3), ('rwlock', 1), ('rcshare', 1)]
        if self.thread_ok() and depth > 0:
            w += [('spawn', 5), ('pfor', 4), ('fire', 0.3), ('plist', 1.5)]
        return w

    # ------------------------------------------------------ helpers
    def bind_hidden(self, name, typ):
        return self.bind(name, typ, False)

    def guard(self, prefix='g'):
        g = self.fresh(prefix)
        return g, g + '#guard'

    def touch(self, uid):
        for c in self.collect:
            c.add(uid)

    def int_e(self, ctx, d=2):
        return self.int_expr(d, ctx)

    def mupd(self, ctx, mname):
        """{ val g = m.lock(); g.set(g.get() OP e) } with the mutex's op."""
        r = self.r
        muid = self.uid(mname)
        op = self.res[muid]
        e = self.int_e(ctx)
        if op == '+' and r.random() < 0.3:
            e = Builtin('work', [Bin('&', e, Lit(31))], _work)
        self.touch(muid)
        g, gu = self.guard()
        if r.random() < 0.5:
            body = [LockDecl(g, gu, mname, muid), GuardSet(g, gu, Bin(op, GuardGet(g, gu), e))]
        else:
            cur = self.fresh('cur')
            cu = cur + '#cur'
            body = [LockDecl(g, gu, mname, muid), Decl(cur, GuardGet(g, gu), ann='Int', uid=cu),
                    GuardSet(g, gu, Bin(op, Var(cur, cu), e))]
        return BareBlock(Block(body))

    def read_mutex(self, ctx, mname):
        """Print the value, or copy it into a fresh var Int."""
        muid = self.uid(mname)
        g, gu = self.guard()
        if self.printing(ctx) and self.r.random() < 0.7:
            return [BareBlock(Block([LockDecl(g, gu, mname, muid),
                                     Print(Interp([self.next_tag() + ' ', GuardGet(g, gu)]))]))]
        v = self.fresh()
        vu = self.bind(v, 'Int', True)
        return [Decl(v, Lit(0), True, uid=vu),
                BareBlock(Block([LockDecl(g, gu, mname, muid), Assign(v, '=', GuardGet(g, gu), uid=vu)]))]

    def filler(self, ctx, depth, lo=0, hi=2):
        c = self.noexit(ctx)
        out = []
        for _ in range(self.r.randint(lo, hi)):
            out.extend(self.stmt(c, max(depth - 1, 0)))
        return out

    def agg_int(self, typ, e):
        """An Int from a received or awaited value of type typ."""
        if typ == 'Int':
            return e
        if typ == 'String':
            return StrLen(e)
        return Bin('+', Field(e, 'a'), StrLen(Field(e, 's')))

    def item(self, typ, ctx):
        if typ == 'Int':
            return self.int_e(ctx)
        if typ == 'String':
            if self.r.random() < 0.3:
                return Builtin('label', [self.int_e(ctx, 1)], lambda x: f'w{x}')
            return Interp([self.r.choice(['a', 'q', 'zz']), self.int_e(ctx, 1)])
        return PNew(self.int_e(ctx, 1), Interp(['p', self.int_e(ctx, 1)]))

    def show(self, typ, e):
        """Interpolation parts printing a value of type typ."""
        if typ == 'P':
            return [Field(e, 'a'), ' ', Field(e, 's')]
        return [e]

    # ------------------------------------------------------- spawn
    def spawn_body(self, ctx, depth, rtype=None):
        r = self.r
        self.push()
        self.floors.append(('spawn', len(self.scopes) - 1))
        touches = set()
        self.collect.append(touches)
        bc = self.body_ctx()
        stmts = []
        for _ in range(r.randint(0, 2)):
            stmts.extend(self.stmt(bc, max(min(depth, 2) - 1, 0)))
        ms = self.names('mutex')
        if ms and r.random() < 0.5:
            stmts.append(self.mupd(bc, r.choice(ms)))
        if rtype is None:
            rtype = r.choice(['Int'] * 6 + ['String', 'P', 'list', 'Bool'])
        if rtype == 'Int':
            ret = self.int_e(bc, 3)
        elif rtype == 'Bool':
            ret = self.bool_expr(2, bc)
        elif rtype == 'String':
            ret = self.item('String', bc)
        elif rtype == 'P':
            ret = self.item('P', bc)
        else:
            ret = ListLit([self.int_e(bc, 1) for _ in range(r.randint(1, 4))])
        if rtype == 'Int' and r.random() < 0.1:
            # the value returned from both arms of an if
            stmts.append(If([(self.bool_expr(1, bc), Block([Return(ret)]))],
                            Block([Return(self.int_e(bc, 2))])))
        else:
            stmts.append(Return(ret))
        self.collect.pop()
        self.floors.pop()
        self.pop()
        return Block(stmts), rtype, touches

    def await_stmts(self, ctx, h, hu, rtype):
        r = self.r
        aw = Await(h, hu)
        if rtype == 'Int':
            c = r.random()
            ms = self.names('Int', True)
            if self.printing(ctx) and c < 0.35:
                return [Print(Interp([self.next_tag() + ' ', aw]))]
            if ms and c < 0.55:
                t = r.choice(ms)
                return [Assign(t, '+=', aw, uid=self.uid(t))]
            v = self.fresh('r')
            return [Decl(v, aw, uid=self.bind(v, 'Int'))]
        v = self.fresh('r')
        vu = self.bind_hidden(v, 'res')
        out = [Decl(v, aw, uid=vu)]
        rv = Var(v, vu)
        if rtype == 'list':
            lenv = Builtin('len', [rv], lambda x: len(x.items))
            if self.printing(ctx):
                tag = self.next_tag()
                out.append(Print(Interp([tag + ' ', lenv])))
                x = self.fresh('x')
                xu = x + '#x'
                out.append(ForIn(x, rv, Block([Print(Interp([tag + ' ', Var(x, xu)]))]), uid=xu))
            else:
                n = self.fresh()
                out.append(Decl(n, lenv, uid=self.bind(n, 'Int')))
            return out
        if self.printing(ctx):
            if rtype == 'Bool':
                out.append(Print(Interp([self.next_tag() + ' ', rv])))
            else:
                out.append(Print(Interp([self.next_tag() + ' '] + self.show(rtype, rv))))
        elif rtype != 'Bool':
            n = self.fresh()
            out.append(Decl(n, self.agg_int(rtype, rv), uid=self.bind(n, 'Int')))
        return out

    def s_spawn(self, ctx, depth):
        r = self.r
        out = []
        hs = []
        for _ in range(r.randint(1, 3)):
            body, rtype, touches = self.spawn_body(ctx, depth)
            h = self.fresh('h')
            hu = self.bind_hidden(h, 'handle')
            out.append(SpawnDecl(h, hu, self.next_tag(), body))
            p = {'uid': hu, 'touches': touches}
            self.pending.append(p)
            hs.append((h, hu, rtype, p))
            out.extend(self.filler(ctx, depth, 0, 1))
        r.shuffle(hs)
        for h, hu, rtype, p in hs:
            out.extend(self.filler(ctx, depth, 0, 1))
            self.pending.remove(p)
            out.extend(self.await_stmts(ctx, h, hu, rtype))
        return out

    def s_fire(self, ctx, depth):
        self.push()
        self.floors.append(('spawn', len(self.scopes) - 1))
        bc = self.body_ctx()
        self.quiet += 1
        stmts = self.stmt(bc, 0) if self.r.random() < 0.5 else []
        self.quiet -= 1
        if self.r.random() < 0.4:
            # a String result nobody awaits: the thread drops it
            stmts.append(Return(Interp(['fire and forget result ', self.int_e(bc)])))
        else:
            stmts.append(Return(self.int_e(bc)))
        self.floors.pop()
        self.pop()
        return [SpawnDecl(None, None, self.next_tag(), Block(stmts), fire=True)]

    # ------------------------------------------------------- mutex
    def new_mutex(self, ctx):
        r = self.r
        name = self.fresh('m')
        op = r.choice(['+', '+', '+', '^', '|', '&'])
        uid = self.bind(name, 'mutex', False)
        self.res[uid] = op
        init = self.int_e(ctx, 1) if r.random() < 0.6 else Lit(-1 if op == '&' else 0)
        return name, [Decl(name, New('Mutex', init, MutexV), uid=uid)]

    def s_mutex(self, ctx, depth):
        return self.new_mutex(ctx)[1]

    def s_mupd(self, ctx, depth):
        return [self.mupd(ctx, self.r.choice(self.names('mutex')))]

    def s_mread(self, ctx, depth):
        r = self.r
        ms = [n for n in self.names('mutex') if self.can_read(self.uid(n))]
        n = r.choice(ms)
        if r.random() < 0.2:
            # replace the value: nothing else may be updating it
            g, gu = self.guard()
            return [BareBlock(Block([LockDecl(g, gu, n, self.uid(n)),
                                     GuardSet(g, gu, self.int_e(ctx))]))]
        return self.read_mutex(ctx, n)

    # -------------------------------------------------- parallel for
    def pfor_body(self, ctx, depth, var, typ, effects):
        """Body statements; effects(bc, var_name, var_uid) adds the writes."""
        r = self.r
        self.push()
        self.floors.append(('pfor', len(self.scopes) - 1))
        vu = self.bind(var, typ, False)
        bc = self.body_ctx()
        stmts = []
        for _ in range(r.randint(0, 2)):
            stmts.extend(self.stmt(bc, max(min(depth, 2) - 1, 0)))
        stmts.extend(effects(bc, vu))
        if self.thread_ok() and r.random() < 0.15:
            stmts.extend(self.s_spawn(bc, 1))
        self.floors.pop()
        self.pop()
        return vu, Block(stmts)

    def pfor_count(self, ctx):
        r = self.r
        c = r.random()
        if c < 0.12 and not self.floors:
            return Lit(r.randint(100, 400))
        if c < 0.3:
            return Bin('&', self.int_e(ctx, 1), Lit(7))
        return Lit(r.randint(0, 10))

    def s_pfor(self, ctx, depth):
        r = self.r
        out = []
        ms = self.names('mutex')
        if ms and r.random() < 0.6:
            mname = r.choice(ms)
        else:
            mname, decl = self.new_mutex(ctx)
            out.extend(decl)
        var = self.fresh('i')

        def effects(bc, vu):
            es = [self.mupd(bc, mname)]
            if r.random() < 0.3:
                es.append(self.mupd(bc, r.choice(self.names('mutex'))))
            if self.thread_ok() and r.random() < 0.15:
                es.extend(self.s_pfor(bc, 1))
            return es
        n = self.pfor_count(ctx)
        big = isinstance(n, Lit) and n.v > 20
        self.heavy += big
        vu, body = self.pfor_body(ctx, depth, var, 'Int', effects)
        self.heavy -= big
        out.append(PFor(var, vu, n, body))
        muid = self.uid(mname)
        if self.can_read(muid) and r.random() < 0.6:
            out.extend(self.read_mutex(ctx, mname))
        return out

    def s_plist(self, ctx, depth):
        """for x in xs parallel over a list binding of Int, String or P."""
        r = self.r
        typ = r.choice(['Int', 'Int', 'String', 'P'])
        xs = self.fresh('xs')
        xsu = self.bind(xs, '[' + typ + ']', False)
        n = r.randint(0, 8) if r.random() < 0.85 else r.randint(30, 120)
        items = [self.item(typ, ctx) for _ in range(min(n, 8))]
        if n > 8:
            items = [self.item(typ, ctx) if k < 4 else
                     (Lit(k * 3 - 50) if typ == 'Int' else
                      Lit(f's{k}') if typ == 'String' else PNew(Lit(k), Lit(f'p{k}')))
                     for k in range(n)]
        out = [Decl(xs, ListLit(items), ann=None if items else "[" + typ + "]", uid=xsu)]
        mname, decl = self.new_mutex(ctx)
        self.res[self.uid(mname)] = '+'
        out.extend(decl)
        x = self.fresh('x')

        def effects(bc, vu):
            xv = Var(x, vu)
            e = self.agg_int(typ, xv)
            if typ == 'Int':
                e = Bin('+', Bin('*', xv, self.int_e(bc, 1)), Lit(1))
            muid = self.uid(mname)
            self.touch(muid)
            g, gu = self.guard()
            return [BareBlock(Block([LockDecl(g, gu, mname, muid),
                                     GuardSet(g, gu, Bin('+', GuardGet(g, gu), e))]))]
        vu, body = self.pfor_body(ctx, depth, x, typ, effects)
        out.append(PFor(x, vu, Var(xs, xsu), body, is_range=False))
        out.extend(self.read_mutex(ctx, mname))
        return out

    def s_rcslots(self, ctx, depth):
        """An rc [Int] whose slot i only iteration i (or one thread) writes."""
        r = self.r
        n = r.randint(1, 10)
        rs = self.fresh('rs')
        ru = self.bind_hidden(rs, 'rclist')
        RS = lambda: Var(rs, ru)
        out = [Decl(rs, ListLit([Lit(0)] * n, prefix='rc '), uid=ru)]
        if r.random() < 0.6 or not self.thread_ok():
            var = self.fresh('i')
            self.push()
            self.floors.append(('pfor', len(self.scopes) - 1))
            vu = self.bind(var, 'Int', False)
            bc = self.body_ctx()
            stmts = []
            for _ in range(r.randint(0, 2)):
                stmts.extend(self.stmt(bc, 0))
            e = self.int_e(bc)
            if r.random() < 0.4:
                e = Bin('+', Index(RS(), Var(var, vu)), e)
            stmts.append(IndexAssign(Index(RS(), Var(var, vu)), '=', e))
            self.floors.pop()
            self.pop()
            out.append(PFor(var, vu, Lit(n), Block(stmts)))
        else:
            # two threads: one writes the even slots, the other the odd ones
            hs = []
            for parity in (0, 1):
                self.push()
                self.floors.append(('spawn', len(self.scopes) - 1))
                bc = self.body_ctx()
                j = self.fresh('j')
                ju = self.bind(j, 'Int', False)
                self.push()
                e = self.int_e(bc)
                self.pop()
                slot = Bin('+', Bin('*', Var(j, ju), Lit(2)), Lit(parity))
                cnt = (n - parity + 1) // 2
                body = Block([ForRange(j, Lit(cnt), Block([IndexAssign(Index(RS(), slot), '=', e)]), uid=ju),
                              Return(Lit(parity))])
                self.floors.pop()
                self.pop()
                h = self.fresh('h')
                hu = self.bind_hidden(h, 'handle')
                out.append(SpawnDecl(h, hu, self.next_tag(), body))
                hs.append((h, hu))
            out.extend(self.filler(ctx, depth, 0, 2))
            for h, hu in hs:
                z = self.fresh('z')
                out.append(Decl(z, Await(h, hu), uid=self.bind(z, 'Int')))
        if self.printing(ctx):
            out.append(Print(Interp([self.next_tag()] + [p for k in range(n) for p in (' ', Index(RS(), Lit(k)))])))
        else:
            s = self.fresh()
            su = self.bind(s, 'Int', True)
            out.append(Decl(s, Lit(0), True, uid=su))
            k = self.fresh('k')
            ku = k + '#k'
            out.append(ForRange(k, Lit(n), Block([Assign(s, '+=', Index(RS(), Var(k, ku)), uid=su)]), uid=ku))
        return out

    # ------------------------------------------------------ channels
    def s_chan(self, ctx, depth):
        r = self.r
        pats = ['buf', 'pfor']
        if self.thread_ok():
            pats += ['p2m', 'p2m', 'm2c', 'm2c', 'pipe', 'fan']
        pat = r.choice(pats)
        typ = r.choice(['Int', 'Int', 'String', 'P'])
        cap = r.randint(1, 4)
        ch = self.fresh('ch')
        cu = self.bind_hidden(ch, 'chan')
        decl = Decl(ch, New(f'Channel[{typ}]', Lit(cap), ChanV), uid=cu)
        return [decl] + getattr(self, 'chan_' + pat)(ctx, depth, ch, cu, typ, cap)

    def recv_decl(self, ch, cu, typ, ctx):
        v = self.fresh('v')
        vu = self.bind(v, typ if typ == 'Int' else 'res', False)
        return v, vu, Decl(v, ChanRecv(ch, cu), ann=typ, uid=vu)

    def consume(self, ctx, ch, cu, typ, count, ordered):
        """Receive `count` items in main; print them when the order is
        fixed, otherwise sum them (or sort them first)."""
        r = self.r
        out = []
        if count == 0:
            return out
        j = self.fresh('j')
        ju = j + '#j'
        self.push()
        v, vu, d = self.recv_decl(ch, cu, typ, ctx)
        self.pop()
        if ordered and self.printing(ctx) and r.random() < 0.7:
            tag = self.next_tag()
            out.append(ForRange(j, Lit(count), Block([d, Print(Interp([tag + ' '] + self.show(typ, Var(v, vu))))]), uid=ju))
            return out
        if self.printing(ctx) and r.random() < 0.4:
            got = self.fresh('got')
            gu = got + '#got'
            out.append(Decl(got, Raw('[]', lambda m, f: ListV([])), True, ann='[Int]', uid=gu))
            out.append(ForRange(j, Lit(count), Block([d, ExprStmt(MethodCall(
                Var(got, gu), 'push', [self.agg_int(typ, Var(v, vu))],
                lambda m, f, c, x: c.items.append(x[0])))]), uid=ju))
            out.append(ExprStmt(MethodCall(Var(got, gu), 'sort', [], lambda m, f, c, x: c.items.sort())))
            tag = self.next_tag()
            x = self.fresh('x')
            xu = x + '#x'
            out.append(ForIn(x, Var(got, gu), Block([Print(Interp([tag + ' ', Var(x, xu)]))]), uid=xu))
            return out
        acc = self.fresh('acc')
        au = self.bind(acc, 'Int', True)
        out.append(Decl(acc, Lit(0), True, uid=au))
        out.append(ForRange(j, Lit(count), Block([d, Assign(acc, '+=', self.agg_int(typ, Var(v, vu)), uid=au)]), uid=ju))
        if self.printing(ctx):
            out.append(Print(Interp([self.next_tag() + ' ', Var(acc, au)])))
        return out

    def producer(self, ctx, depth, ch, cu, typ, k):
        """A spawn sending k items, returning k."""
        r = self.r
        self.push()
        self.floors.append(('spawn', len(self.scopes) - 1))
        bc = self.body_ctx()
        stmts = []
        # no Mutex updates here: producers are not tracked as pending
        self.quiet += 1
        for _ in range(r.randint(0, 1)):
            stmts.extend(self.stmt(bc, 0))
        self.quiet -= 1
        i = self.fresh('i')
        self.push()
        iu = self.bind(i, 'Int', False)
        e = self.item(typ, bc)
        self.pop()
        stmts.append(ForRange(i, Lit(k), Block([ChanSend(ch, cu, e)]), uid=iu))
        stmts.append(Return(Lit(k)))
        self.floors.pop()
        self.pop()
        h = self.fresh('h')
        hu = self.bind_hidden(h, 'handle')
        return h, hu, SpawnDecl(h, hu, self.next_tag(), Block(stmts))

    def await_all(self, ctx, hs):
        out = []
        self.r.shuffle(hs)
        for h, hu in hs:
            z = self.fresh('z')
            out.append(Decl(z, Await(h, hu), uid=self.bind(z, 'Int')))
        return out

    def chan_p2m(self, ctx, depth, ch, cu, typ, cap):
        r = self.r
        np_ = r.choice([1, 1, 2, 3])
        out, hs, total = [], [], 0
        for _ in range(np_):
            k = r.randint(0, 6)
            h, hu, sd = self.producer(ctx, depth, ch, cu, typ, k)
            out.append(sd)
            hs.append((h, hu))
            total += k
        out.extend(self.filler(ctx, depth, 0, 2))
        # leftover items: with several producers which ones stay is not fixed
        left = r.randint(0, min(cap, total)) if np_ == 1 and r.random() < 0.3 else 0
        out.extend(self.consume(ctx, ch, cu, typ, total - left, np_ == 1))
        out.extend(self.await_all(ctx, hs))
        return out

    def consumer(self, ctx, ch, cu, typ, k, ordered):
        """A spawn receiving k items and returning a fold of them."""
        self.push()
        self.floors.append(('spawn', len(self.scopes) - 1))
        bc = self.body_ctx()
        s = self.fresh('s')
        su = self.bind(s, 'Int', True)
        j = self.fresh('j')
        self.push()
        ju = self.bind(j, 'Int', False)
        v, vu, d = self.recv_decl(ch, cu, typ, bc)
        self.pop()
        x = self.agg_int(typ, Var(v, vu))
        fold = Bin('+', Bin('*', Var(s, su), Lit(3)), x) if ordered else Bin('+', Var(s, su), x)
        stmts = [Decl(s, Lit(0), True, uid=su),
                 ForRange(j, Lit(k), Block([d, Assign(s, '=', fold, uid=su)]), uid=ju),
                 Return(Var(s, su))]
        self.floors.pop()
        self.pop()
        h = self.fresh('h')
        hu = self.bind_hidden(h, 'handle')
        return h, hu, SpawnDecl(h, hu, self.next_tag(), Block(stmts))

    def main_send(self, ctx, ch, cu, typ, k):
        i = self.fresh('i')
        self.push()
        iu = self.bind(i, 'Int', False)
        e = self.item(typ, ctx)
        self.pop()
        return ForRange(i, Lit(k), Block([ChanSend(ch, cu, e)]), uid=iu)

    def chan_m2c(self, ctx, depth, ch, cu, typ, cap):
        r = self.r
        k = r.randint(0, 8)
        h, hu, sd = self.consumer(ctx, ch, cu, typ, k, True)
        out = [sd]
        out.extend(self.filler(ctx, depth, 0, 1))
        out.append(self.main_send(ctx, ch, cu, typ, k))
        out.extend(self.filler(ctx, depth, 0, 1))
        out.extend(self.await_stmts(ctx, h, hu, 'Int'))
        return out

    def chan_fan(self, ctx, depth, ch, cu, typ, cap):
        """Several consumers: only the total of their sums is fixed."""
        r = self.r
        out, hs, total = [], [], 0
        for _ in range(r.randint(2, 3)):
            k = r.randint(0, 5)
            h, hu, sd = self.consumer(ctx, ch, cu, typ, k, False)
            out.append(sd)
            hs.append((h, hu))
            total += k
        out.append(self.main_send(ctx, ch, cu, typ, total))
        parts = []
        for h, hu in hs:
            z = self.fresh('z')
            zu = self.bind_hidden(z, 'partial')
            out.append(Decl(z, Await(h, hu), uid=zu))
            parts.append(Var(z, zu))
        tot = parts[0]
        for p in parts[1:]:
            tot = Bin('+', tot, p)
        t = self.fresh()
        out.append(Decl(t, tot, uid=self.bind(t, 'Int')))
        if self.printing(ctx):
            out.append(Print(Interp([self.next_tag() + ' ', Var(t, self.uid(t))])))
        return out

    def chan_pipe(self, ctx, depth, ch, cu, typ, cap):
        """producer -> stage -> main through a second Channel[Int]."""
        r = self.r
        k = r.randint(0, 6)
        ch2 = self.fresh('ch')
        cu2 = self.bind_hidden(ch2, 'chan')
        out = [Decl(ch2, New('Channel[Int]', Lit(r.randint(1, 3)), ChanV), uid=cu2)]
        h1, hu1, sd1 = self.producer(ctx, depth, ch, cu, typ, k)
        self.push()
        self.floors.append(('spawn', len(self.scopes) - 1))
        bc = self.body_ctx()
        j = self.fresh('j')
        self.push()
        ju = self.bind(j, 'Int', False)
        v, vu, d = self.recv_decl(ch, cu, typ, bc)
        e = Bin('+', self.agg_int(typ, Var(v, vu)), self.int_e(bc, 1))
        self.pop()
        body = Block([ForRange(j, Lit(k), Block([d, ChanSend(ch2, cu2, e)]), uid=ju), Return(Lit(k))])
        self.floors.pop()
        self.pop()
        h2 = self.fresh('h')
        hu2 = self.bind_hidden(h2, 'handle')
        out += [sd1, SpawnDecl(h2, hu2, self.next_tag(), body)]
        out.extend(self.filler(ctx, depth, 0, 1))
        out.extend(self.consume(ctx, ch2, cu2, 'Int', k, True))
        out.extend(self.await_all(ctx, [(h1, hu1), (h2, hu2)]))
        return out

    def chan_buf(self, ctx, depth, ch, cu, typ, cap):
        """One thread sends and receives: never more queued than cap."""
        r = self.r
        out = []
        queued = 0
        for _ in range(r.randint(1, 3)):
            k = r.randint(0, cap - queued)
            if k:
                out.append(self.main_send(ctx, ch, cu, typ, k))
                queued += k
            g = r.randint(0, queued)
            out.extend(self.consume(ctx, ch, cu, typ, g, True))
            queued -= g
        return out

    def chan_pfor(self, ctx, depth, ch, cu, typ, cap):
        """Each iteration sends one item; the channel holds them all."""
        r = self.r
        n = r.randint(0, cap)
        var = self.fresh('i')

        def effects(bc, vu):
            return [ChanSend(ch, cu, self.item(typ, bc))]
        vu, body = self.pfor_body(ctx, depth, var, 'Int', effects)
        out = [PFor(var, vu, Lit(n), body)]
        out.extend(self.consume(ctx, ch, cu, typ, n, False))
        return out

    # --------------------------------------------------- rwlock, rc
    def s_rwlock(self, ctx, depth):
        r = self.r
        rw = self.fresh('rw')
        ru = self.bind_hidden(rw, 'rwlock')
        out = [Decl(rw, New('RWLock', self.int_e(ctx, 1), MutexV), uid=ru)]
        if r.random() < 0.5:
            g, gu = self.guard('w')
            out.append(BareBlock(Block([LockDecl(g, gu, rw, ru, 'write'),
                                        GuardSet(g, gu, Bin('+', GuardGet(g, gu), self.int_e(ctx, 1)))])))
        if self.thread_ok():
            mname, decl = self.new_mutex(ctx)
            self.res[self.uid(mname)] = '+'
            out.extend(decl)
            muid = self.uid(mname)
            var = self.fresh('i')

            def effects(bc, vu):
                g, gu = self.guard('rd')
                g2, gu2 = self.guard()
                self.touch(muid)
                return [BareBlock(Block([LockDecl(g, gu, rw, ru, 'read'), LockDecl(g2, gu2, mname, muid),
                                         GuardSet(g2, gu2, Bin('+', GuardGet(g2, gu2),
                                                               Bin('*', GuardGet(g, gu), Var(var, vu))))]))]
            vu, body = self.pfor_body(ctx, depth, var, 'Int', effects)
            out.append(PFor(var, vu, self.pfor_count(ctx), body))
            out.extend(self.read_mutex(ctx, mname))
        g, gu = self.guard('rd')
        if self.printing(ctx):
            out.append(BareBlock(Block([LockDecl(g, gu, rw, ru, 'read'),
                                        Print(Interp([self.next_tag() + ' ', GuardGet(g, gu)]))])))
        return out

    def s_rcshare(self, ctx, depth):
        """An rc P read (and its handle copied) from several threads."""
        r = self.r
        ro = self.fresh('ro')
        ru = self.bind_hidden(ro, 'rcp')
        out = [Decl(ro, PNew(self.int_e(ctx, 1), Interp(['shared', self.int_e(ctx, 1)]), prefix='rc '), uid=ru)]
        if not self.thread_ok():
            return out
        if r.random() < 0.5:
            hs = []
            for _ in range(r.randint(1, 3)):
                self.push()
                self.floors.append(('spawn', len(self.scopes) - 1))
                bc = self.body_ctx()
                q = self.fresh('q')
                qu = q + '#q'
                ret = Bin('+', Bin('*', Field(Var(q, qu), 'a'), self.int_e(bc, 1)), StrLen(Field(Var(q, qu), 's')))
                body = Block([Decl(q, Var(ro, ru), uid=qu), Return(ret)])
                self.floors.pop()
                self.pop()
                h = self.fresh('h')
                hu = self.bind_hidden(h, 'handle')
                out.append(SpawnDecl(h, hu, self.next_tag(), body))
                hs.append((h, hu))
            out.extend(self.filler(ctx, depth, 0, 1))
            for h, hu in hs:
                out.extend(self.await_stmts(ctx, h, hu, 'Int'))
            return out
        mname, decl = self.new_mutex(ctx)
        self.res[self.uid(mname)] = '+'
        out.extend(decl)
        muid = self.uid(mname)
        var = self.fresh('i')

        def effects(bc, vu):
            q = self.fresh('q')
            qu = q + '#q'
            g, gu = self.guard()
            self.touch(muid)
            e = Bin('+', Field(Var(q, qu), 'a'), Bin('*', StrLen(Field(Var(q, qu), 's')), Var(var, vu)))
            return [Decl(q, Var(ro, ru), uid=qu),
                    BareBlock(Block([LockDecl(g, gu, mname, muid),
                                     GuardSet(g, gu, Bin('+', GuardGet(g, gu), e))]))]
        vu, body = self.pfor_body(ctx, depth, var, 'Int', effects)
        out.append(PFor(var, vu, self.pfor_count(ctx), body))
        out.extend(self.read_mutex(ctx, mname))
        return out


def generate(rng):
    g = ConcGen(rng)
    g.program()
    return g
