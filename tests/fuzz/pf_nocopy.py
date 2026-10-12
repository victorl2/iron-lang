"""nocopy captures (manual 3.7, 6.7, 7.1; #512): closures and spawns that
borrow a nocopy value, and the captures that must be rejected.

A positive program is made of segments that declare nocopy values (a
nocopy object, an object holding one, an optional and an enum payload of
one, a `var` one, an `rc` one, a crypto Hasher) and borrow them: lambdas
passed directly to callees that only call them (directly, relayed through
another such callee, nested, through the list methods), spawns awaited
later in the same block (possibly in a loop), and, for the `var` and `rc`
forms only, closures that are stored. Every nocopy object prints its id
when dropped, so the oracle is that each one is dropped exactly once, at
the end of its segment, in reverse declaration order (a copy dropped a
second time prints a second line; a leak prints none). The program also
runs under the memory checkers, which see a native Hasher freed twice.

A negative program (about one in four) holds a single capture that is not
a borrow: a closure bound, returned, stored in a field, a list or through
push, handed to a callee that keeps it (directly or through another
callee), nested in a stored lambda, a parallel for, or a spawn whose handle
is discarded, never awaited, awaited in one branch only or after a possible
return. It must not build, with E0286.
"""
from pf_seg import Seg, SegProgram, iron_lit, ind
from pf_lang import Expected

HELPERS = '''nocopy object Res {
    val id: Int
    drop {
        println("drop {self.id}")
    }
}

object Holder {
    val r: Res
    val tag: String
}

enum Slot {
    Empty,
    Full(Res)
}

object Keeper {
    val f: func() -> Int
}

func twice(f: func() -> Int) -> Int {
    return f() + f()
}

func relay(f: func() -> Int) -> Int {
    return twice(f)
}

func apply(f: func(Int) -> Int, x: Int) -> Int {
    return f(x)
}

func keep(f: func() -> Int) -> Keeper {
    return Keeper(f)
}

func keep_relay(f: func() -> Int) -> Int {
    val k = keep(f)
    return k.f()
}
'''

LONG = 'a tag long enough to live on the heap rather than inline'

# kinds whose read is a single expression (usable in map / apply)
EXPR_KINDS = ('res', 'holder', 'var', 'rc', 'hasher')


class Val:
    """A declared nocopy value: its binding, kind and id (the line its drop
    prints, None for a Hasher, which prints nothing)."""

    def __init__(self, g, kind):
        self.kind = kind
        self.name = g.fresh('x')
        g.next_id += 1
        self.id = g.next_id
        self.tag = g.r.choice(['t', LONG])

    def decl(self):
        n, i = self.name, self.id
        return {
            'res': f'val {n} = Res({i})',
            'holder': f'val {n} = Holder(Res({i}), {iron_lit(self.tag)})',
            'opt': f'val {n}: Res? = Res({i})',
            'slot': f'val {n} = Slot.Full(Res({i}))',
            'var': f'var {n} = Res({i})',
            'rc': f'val {n} = rc Res({i})',
            'hasher': f'val {n} = Hasher.new(HashAlgorithm.Sha256)',
        }[self.kind]

    def value(self):
        return 0 if self.kind == 'hasher' else self.id

    def drops(self):
        return [] if self.kind == 'hasher' else [f'drop {self.id}']

    def read_expr(self):
        n = self.name
        return {
            'res': f'{n}.id', 'var': f'{n}.id', 'rc': f'{n}.id',
            'holder': f'{n}.r.id + {n}.tag.len() * 0',
            'hasher': f'{n}.handle * 0',
        }[self.kind]

    def body(self, k):
        """Statements returning value() * k."""
        n = self.name
        if self.kind in EXPR_KINDS:
            return [f'return ({self.read_expr()}) * {k}']
        if self.kind == 'opt':
            return [f'if {n} != null {{', f'    return {n}.id * {k}', '}', 'return 0']
        return [f'match {n} {{', f'    Slot.Full(p) -> return p.id * {k}',
                '    Slot.Empty -> return 0', '}']


def lam(body, params='', ret='Int'):
    """A lambda literal as lines (the first line opens it)."""
    return [f'func({params}) -> {ret} {{'] + ind(body) + ['}']


def inline_call(prefix, lines, suffix):
    """`prefix<lambda lines>suffix` as statement lines."""
    out = list(lines)
    out[0] = prefix + out[0]
    out[-1] = out[-1] + suffix
    return out


class NocopyGen(SegProgram):
    HELPERS = HELPERS
    IMPORTS = ('crypto',)
    NSEGS = (1, 4)

    def __init__(self, rng):
        super().__init__(rng)
        self.next_id = 0
        self.negative = rng.random() < 0.25
        self.bad = None

    def program(self):
        if self.negative:
            self.segs.append(self.seg_negative())
        else:
            super().program()

    def model(self):
        if self.negative:
            return Expected([], errors={'E0286'})
        return super().model()

    # ------------------------------------------------------------- positive
    def add_segment(self):
        r = self.r
        kinds = ['res', 'res', 'holder', 'opt', 'slot', 'var', 'rc', 'hasher']
        vals = [Val(self, r.choice(kinds)) for _ in range(r.randint(1, 3))]
        body, out = [], []
        for v in vals:
            body.append(v.decl())
        for _ in range(r.randint(1, 5)):
            v = r.choice(vals)
            b, o = self.use(v)
            body += b
            out += o
        for v in reversed(vals):
            out += v.drops()
        self.segs.append(Seg(body, out))

    def use(self, v):
        r = self.r
        k = r.randint(1, 9)
        forms = ['twice', 'relay', 'nested', 'spawn', 'spawn_gap', 'loop']
        if v.kind in EXPR_KINDS:
            forms += ['map', 'apply', 'filter']
        if v.kind in ('var', 'rc'):
            forms += ['stored', 'stored', 'keeper']
        form = r.choice(forms)
        val = v.value() * k
        t = self.fresh('t')
        if form == 'twice':
            return inline_call(f'val {t} = twice(', lam(v.body(k)), ')') + [f'println("{{{t}}}")'], [str(2 * val)]
        if form == 'relay':
            return inline_call(f'val {t} = relay(', lam(v.body(k)), ')') + [f'println("{{{t}}}")'], [str(2 * val)]
        if form == 'nested':
            inner = inline_call('return twice(', lam(v.body(k)), ')')
            return inline_call(f'val {t} = twice(', lam(inner), ')') + [f'println("{{{t}}}")'], [str(4 * val)]
        if form == 'map':
            c = r.randint(0, 9)
            return ([f'val {t} = [{c}, {c + 1}].map(func(z: Int) -> Int {{ return z + ({v.read_expr()}) * {k} }})',
                     f'println("{{{t}[1]}}")'], [str(c + 1 + val)])
        if form == 'filter':
            return ([f'val {t} = [1, 2, 3].filter(func(z: Int) -> Bool {{ return z * 1000 > ({v.read_expr()}) * {k} }})',
                     f'println("{{{t}.len()}}")'],
                    [str(sum(1 for z in (1, 2, 3) if z * 1000 > val))])
        if form == 'apply':
            c = r.randint(0, 9)
            return ([f'val {t} = apply(func(z: Int) -> Int {{ return z + ({v.read_expr()}) * {k} }}, {c})',
                     f'println("{{{t}}}")'], [str(c + val)])
        if form in ('spawn', 'spawn_gap'):
            h = self.fresh('h')
            lines = inline_call(f'val {h} = spawn("w") ', ['{'] + ind(v.body(k)) + ['}'], '')
            if form == 'spawn_gap':
                q = self.fresh('q')
                lines += [f'val {q} = {k} + 1', f'if {q} > 100 {{', f'    println("big")', '}']
            lines += [f'val {t} = await {h}', f'println("{{{t}}}")']
            return lines, [str(val)]
        if form == 'loop':
            n = r.randint(1, 3)
            i = self.fresh('i')
            h = self.fresh('h')
            inner = inline_call(f'val {h} = spawn("l") ', ['{'] + ind(v.body(k)) + ['}'], '') \
                if r.random() < 0.5 else None
            if inner:
                lines = [f'for {i} in range({n}) {{'] + ind(inner + [f'val {t} = await {h}', f'println("{{{t} + {i}}}")']) + ['}']
                return lines, [str(val + j) for j in range(n)]
            lines = [f'for {i} in range({n}) {{'] + \
                ind(inline_call(f'val {t} = twice(', lam(v.body(k)), ')') + [f'println("{{{t} + {i}}}")']) + ['}']
            return lines, [str(2 * val + j) for j in range(n)]
        if form == 'stored':
            f = self.fresh('f')
            return inline_call(f'val {f} = ', lam(v.body(k)), '') + [f'println("{{{f}() + {f}()}}")'], [str(2 * val)]
        # keeper: a callee that stores the closure; fine for var and rc
        return inline_call(f'val {t} = keep_relay(', lam(v.body(k)), ')') + [f'println("{{{t}}}")'], [str(val)]

    # ------------------------------------------------------------- negative
    def seg_negative(self):
        r = self.r
        kind = r.choice(['res', 'res', 'holder', 'opt', 'slot', 'hasher', 'param'])
        v = Val(self, 'res' if kind == 'param' else kind)
        form = r.choice(['bound', 'returned', 'field', 'field_assign', 'list', 'push', 'keeper',
                         'keeper_relay', 'nested_stored', 'spawn_stmt', 'spawn_unawaited',
                         'spawn_if', 'spawn_return', 'parallel'])
        if kind == 'param' and form == 'returned':
            form = 'bound'
        if form == 'parallel' and v.kind not in EXPR_KINDS:
            form = 'bound'
        self.bad = (kind, form)
        top = []
        k = r.randint(1, 9)
        if kind == 'param':
            fn = self.fresh('pf')
            fbody = self.bad_capture(v, form, k, fn, top)
            top.append('\n'.join([f'func {fn}({v.name}: Res) -> Int {{'] + ind(fbody + ['return 0']) + ['}']))
            body = [f'println("{{{fn}(Res({v.id}))}}")']
        elif form == 'returned':
            fn = self.fresh('mk')
            fbody = [v.decl()] + self.bad_capture(v, form, k, fn, top)
            top.append('\n'.join([f'func {fn}() -> func() -> Int {{'] + ind(fbody) + ['}']))
            body = [f'println("{{{fn}()()}}")']
        else:
            body = [v.decl()] + self.bad_capture(v, form, k, None, top)
        return Seg(body, [], top=top)

    def bad_capture(self, v, form, k, fn, top):
        r = self.r
        t = self.fresh('t')
        body = v.body(k)
        if form == 'returned':
            return inline_call('return ', lam(body), '')
        if form == 'bound':
            return inline_call(f'val {t} = ', lam(body), '') + [f'println("{{{t}()}}")']
        if form == 'field':
            return inline_call(f'val {t} = Keeper(', lam(body), ')') + [f'println("{{{t}.f()}}")']
        if form == 'field_assign':
            top.append('object Slotted {\n    var f: func() -> Int\n    init(g: func() -> Int) {\n'
                       '        self.f = g\n    }\n}')
            return [f'var {t} = Slotted(func() -> Int {{ return 0 }})'] + \
                inline_call(f'{t}.f = ', lam(body), '') + [f'println("{{{t}.f()}}")']
        if form == 'list':
            return inline_call(f'val {t} = [', lam(body), ']') + [f'println("{{{t}[0]()}}")']
        if form == 'push':
            return [f'var {t}: [func() -> Int] = []'] + inline_call(f'{t}.push(', lam(body), ')') + \
                [f'println("{{{t}.len()}}")']
        if form == 'keeper':
            return inline_call(f'val {t} = keep(', lam(body), ')') + [f'println("{{{t}.f()}}")']
        if form == 'keeper_relay':
            return inline_call(f'val {t} = keep_relay(', lam(body), ')') + [f'println("{{{t}}}")']
        if form == 'nested_stored':
            g = self.fresh('g')
            inner = inline_call(f'val {g} = ', lam(body), '') + [f'return {g}()']
            return inline_call(f'val {t} = twice(', lam(inner), ')') + [f'println("{{{t}}}")']
        if form == 'parallel':
            i = self.fresh('i')
            return [f'for {i} in range(2) parallel {{'] + \
                ind([f'println("{{{i} + {v.read_expr()}}}")']) + ['}']
        h = self.fresh('h')
        spawn = inline_call(f'val {h} = spawn("w") ', ['{'] + ind(body) + ['}'], '')
        if form == 'spawn_stmt':
            return inline_call('spawn("w") ', ['{'] + ind(body) + ['}'], '')
        if form == 'spawn_unawaited':
            return spawn + ['println("started")']
        if form == 'spawn_if':
            c = r.randint(0, 3)
            return spawn + [f'if {c} > 1 {{', f'    val {t} = await {h}', f'    println("{{{t}}}")', '}']
        # spawn_return: a return before the await may leave the block
        c = r.randint(0, 3)
        return spawn + [f'if {c} > 5 {{', '    return 0' if fn else '    return', '}',
                        f'val {t} = await {h}', f'println("{{{t}}}")']


def generate(rng):
    g = NocopyGen(rng)
    g.program()
    return g
