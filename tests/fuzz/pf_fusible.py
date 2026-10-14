"""List pipelines: fused, hint free and unoptimized builds must agree.

Each program runs a few random pipelines of map, filter, reduce, forEach and
sum (with copy, len and bindings in between) over random sources: literals,
val and var bindings, globals, parameters, call results, soa lists, bounded
vectors and interface lists. The elements and the values that flow between
stages are numbers, a plain object, Strings, lists, rc handles, optionals,
an object with a String field and an object with a drop hook. Callbacks are
named functions (some carrying //iron:fusible), lambdas, lambdas capturing a
val or a var, methods and generic functions; some print, write a global,
read a global, divide by an element, index a table, shift by an element or
recurse.

A Python model runs every pipeline one stage at a time, which is what the
program must print. The generator also applies the rules of manual 5.2 and
marks, in the source, every line that must become a fused loop
(`// @fused: map filter sum`) and every hinted function that cannot keep
the hint (`// @w0618: reason`). The runner (prog_fuzz.check) builds the
program as written, with the hints removed and with --no-optimize, compares
output, exit code and panic of the three, and reads the generated C and the
compiler's warnings against the marks.
"""
from pf_lang import Expected

MASK = (1 << 64) - 1
TRIVIAL = ('Int', 'Float', 'Bool', 'Pt')
TABLE = [3, 1, 4, 1, 5, 9]

SUPPORT = '''object Pt {
    val a: Int
    val b: Int
}
object Ref {
    val id: Int
}
object Named {
    val name: String
    val n: Int
}
object Tok {
    val n: Int
    drop {
        println("drop {self.n}")
    }
}
object Calc {
    val bias: Int
    readonly func apply(x: Int) -> Int {
        return x + self.bias
    }
}
interface Shape {
    readonly func area() -> Int
}
object Sq impl Shape {
    val s: Int
    readonly func area() -> Int {
        return self.s * self.s
    }
}
object Rc2 impl Shape {
    val w: Int
    val h: Int
    readonly func area() -> Int {
        return self.w * self.h
    }
}
var g_acc = 0
var g_bias = 2
val g_table = [3, 1, 4, 1, 5, 9]
func ident[T](x: T) -> T {
    return x
}
func chk(xs: [Int]) -> Int {
    var h = 7
    for x in xs {
        h = h * 31 + x
    }
    return h
}
'''


class Panic(Exception):
    pass


def wrap(v):
    v &= MASK
    return v - (1 << 64) if v >> 63 else v


def tdiv(a, b):
    q = abs(a) // abs(b)
    return wrap(q if (a < 0) == (b < 0) else -q)


def tmod(a, b):
    return wrap(a - b * tdiv(a, b))


def f2i(f):
    if f != f:
        return 0
    if f >= 9223372036854775808.0:
        return (1 << 63) - 1
    if f <= -9223372036854775808.0:
        return -(1 << 63)
    return int(f)


def shl(a, n):
    if n < 0:
        raise Panic('shift by a negative count')
    return 0 if n >= 64 else wrap(a << n)


def lit(c):
    return str(c) if c >= 0 else f'({c})'


def itype(kind):
    return {'Int': 'Int', 'Float': 'Float', 'Bool': 'Bool', 'Pt': 'Pt', 'Str': 'String',
            'List': '[Int]', 'Rc': 'rc Ref', 'Named': 'Named', 'Opt': 'Int?', 'Tok': 'Tok',
            'Shape': 'Shape'}[kind]


class Helper:
    """A named function: Iron source, a Python body, and whether the rules
    let it run inside a fused loop (`why` names the reason when not)."""

    def __init__(self, name, params, ret, body, py, why=None, src=None, out=None):
        self.name, self.params, self.ret, self.body, self.py = name, params, ret, body, py
        self.why = why
        self.src, self.out = src, out
        self.hint = False

    @property
    def safe(self):
        return self.why is None


class FusibleGen:
    def __init__(self, rng):
        self.r = rng
        self.helpers = []
        self.cases = []          # (lines of the case function, model thunk)
        self.any_output = False
        self.n = 0

    # ---------------------------------------------------------------- helpers

    def fresh(self, stem):
        self.n += 1
        return f'{stem}_{self.n}'

    def add(self, h):
        h.hint = self.r.random() < 0.5
        self.helpers.append(h)
        return h

    def effect(self, name):
        """A random effect or panic to put in front of a helper's result:
        (statements, python hook(st, x), reason, is an effect rather than a
        possible panic) or None for none."""
        r = self.r
        pick = r.random()
        if pick < 0.62:
            return None
        if pick < 0.74:
            return ([f'println("{name} {{x}}")'],
                    lambda st, x: st['out'].append(f'{name} {x}'), "calls 'println'", True)
        if pick < 0.84:
            def hook(st, x):
                st['g_acc'] = wrap(st['g_acc'] * 3 + x)
            return (['g_acc = g_acc * 3 + x'], hook, "writes the global 'g_acc'", True)
        if pick < 0.90:
            k = r.randint(-2, 6)

            def hook(st, x):
                if x == k:
                    raise Panic('integer division by zero')
            return ([f'val q_ = 100 / (x - {lit(k)})'], hook,
                    'divides by a value that may be zero', False)
        if pick < 0.95:
            def hook(st, x):
                i = x & 7
                if i >= len(TABLE):
                    raise Panic('index out of bounds')
            return (['val t_ = g_table[x & 7]'], hook, 'indexes a list or array', False)
        k = r.randint(-3, 4)

        def hook(st, x):
            shl(1, x - k)
        return ([f'val s_ = 1 << (x - {lit(k)})'], hook,
                'shifts by a count that may be negative', False)

    def int_expr(self):
        """(Iron expression over x, python function, reason or None)."""
        r = self.r
        c = r.randint(-5, 9)
        k = r.randint(2, 7)
        choice = r.randint(0, 11)
        if choice == 0:
            return f'x + {lit(c)}', lambda st, x: wrap(x + c), None
        if choice == 1:
            return f'x * {lit(c)}', lambda st, x: wrap(x * c), None
        if choice == 2:
            return f'{lit(c)} - x', lambda st, x: wrap(c - x), None
        if choice == 3:
            return f'(x * {lit(c)}) % {k}', lambda st, x: tmod(wrap(x * c), k), None
        if choice == 4:
            return f'x / {k}', lambda st, x: tdiv(x, k), None
        if choice == 5:
            return f'x & {c + 6}', lambda st, x: x & (c + 6), None
        if choice == 6:
            s, t = r.randint(0, 5), r.randint(0, 3)
            return f'(x << {s}) >> {t}', lambda st, x: wrap(x << s) >> t, None
        if choice == 7:
            return 'Int(UInt8(x)) - 100', lambda st, x: (x & 255) - 100, None
        if choice == 8:
            return 'Int(Float(x) * 0.5)', lambda st, x: f2i(float(x) * 0.5), None
        if choice == 9:
            return 'x + g_bias', lambda st, x: wrap(x + st['g_bias']), None
        if choice == 10:
            return f'ident(x) - {lit(c)}', lambda st, x: wrap(x - c), None
        return f'x * x + {lit(c)}', lambda st, x: wrap(x * x + c), None

    def helper(self, src, out):
        """A fresh helper from kind `src` to kind `out` ('Bool' for a
        predicate)."""
        r = self.r
        name = self.fresh({'Bool': 'keep'}.get(out, 'to_' + out.lower()))
        t_in, t_out = itype(src), itype(out)
        pre, hook, why, strong = [], None, None, False
        if src == 'Int':
            eff = self.effect(name)
            if eff:
                pre, hook, why, strong = eff

        def blame(call_reason, other_reason=None):
            """The reason the compiler reports: an effect or an unprovable
            call before a possible panic, before anything else."""
            if strong:
                return why
            return call_reason or why or other_reason

        def mk(body_expr, py, reason=None, call_reason=None):
            def run(st, x):
                if hook:
                    hook(st, x)
                return py(st, x)
            lines = pre + [f'return {body_expr}']
            return self.add(Helper(name, f'x: {t_in}', t_out, lines, run,
                                   blame(call_reason, reason), src, out))

        if src == 'Int' and out == 'Int':
            shape = r.random()
            if shape < 0.12:
                # branch and loop: still free of effects
                c, d = r.randint(0, 6), r.randint(1, 4)

                def py(st, x):
                    if x > c or x < -40:
                        return wrap(x - d)
                    t = x
                    while t < c:
                        t = wrap(t + d)
                    return t
                h = Helper(name, 'x: Int', 'Int',
                           pre + [f'if x > {c} or x < -40 {{', f'    return x - {d}', '}',
                                  'var t = x', f'while t < {c} {{', f'    t = t + {d}', '}',
                                  'return t'],
                           (lambda st, x: (hook(st, x) if hook else None, py(st, x))[1]),
                           why, src, out)
                return self.add(h)
            if shape < 0.20:
                lim = r.randint(3, 30)

                def rec(st, x):
                    # the statements in front run at every level
                    if hook:
                        hook(st, x)
                    return 0 if x <= 0 or x > lim else rec(st, x - 1) + 1
                h = Helper(name, 'x: Int', 'Int',
                           pre + [f'if x <= 0 or x > {lim} {{', '    return 0', '}',
                                  f'return {name}(x - 1) + 1'],
                           rec,
                           blame('is recursive'), src, out)
                return self.add(h)
            if shape < 0.32:
                inner = [h for h in self.helpers if h.src == 'Int' and h.out == 'Int']
                if inner:
                    g = r.choice(inner)
                    c = r.randint(-3, 3)
                    return mk(f'{g.name}(x) + {lit(c)}', lambda st, x: wrap(g.py(st, x) + c),
                              call_reason=None if g.safe else f"calls '{g.name}'")
            e, py, reason = self.int_expr()
            return mk(e, py, reason)
        if src == 'Int' and out == 'Bool':
            c = r.randint(-4, 12)
            choice = r.randint(0, 4)
            if choice == 0:
                return mk(f'x > {lit(c)}', lambda st, x: x > c)
            if choice == 1:
                return mk('x % 2 == 0', lambda st, x: tmod(x, 2) == 0)
            if choice == 2:
                return mk(f'x != {lit(c)}', lambda st, x: x != c)
            if choice == 3:
                return mk(f'x > {lit(c - 6)} and x < {c + 9}', lambda st, x: c - 6 < x < c + 9)
            return mk('x > g_bias', lambda st, x: x > st['g_bias'])
        c = r.randint(0, 5)
        table = {
            ('Int', 'Pt'): (f'Pt(x, x * {c})', lambda st, x: ('Pt', x, wrap(x * c)), None),
            ('Pt', 'Int'): (f'x.a * {c} - x.b', lambda st, x: wrap(x[1] * c - x[2]), None),
            ('Pt', 'Bool'): (f'x.a + {c} > x.b', lambda st, x: wrap(x[1] + c) > x[2], None),
            ('Pt', 'Pt'): (f'Pt(x.b, x.a + {c})', lambda st, x: ('Pt', x[2], wrap(x[1] + c)), None),
            ('Int', 'Float'): ('Float(x) * 0.5', lambda st, x: float(x) * 0.5, None),
            ('Float', 'Float'): (f'x * 2.0 + {c}.25', lambda st, x: x * 2.0 + float(f'{c}.25'), None),
            ('Float', 'Bool'): (f'x > {c}.5', lambda st, x: x > float(f'{c}.5'), None),
            ('Float', 'Int'): ('Int(x * 4.0)', lambda st, x: f2i(x * 4.0), None),
            ('Int', 'Str'): ('"s{x}-pad-pad-pad-pad-pad"', lambda st, x: f's{x}-pad-pad-pad-pad-pad',
                             'builds a String'),
            ('Str', 'Int'): (f'x.len() + {c}', lambda st, x: len(x) + c, "calls '"),
            ('Str', 'Bool'): (f'x.len() > {c + 10}', lambda st, x: len(x) > c + 10, "calls '"),
            ('Str', 'Str'): ('"{x}!"', lambda st, x: x + '!', 'builds a String'),
            ('Int', 'List'): (f'[x, x + {c}]', lambda st, x: [x, wrap(x + c)], 'builds a list'),
            ('List', 'Int'): (f'x[1] + {c}', lambda st, x: wrap(x[1] + c), 'indexes a list or array'),
            ('List', 'Bool'): (f'x[0] > {c}', lambda st, x: x[0] > c, 'indexes a list or array'),
            ('Int', 'Rc'): ('rc Ref(x)', lambda st, x: ('Rc', x), 'allocates'),
            ('Rc', 'Int'): (f'x.id + {c}', lambda st, x: wrap(x[1] + c), 'reads a field through'),
            ('Rc', 'Bool'): (f'x.id > {c}', lambda st, x: x[1] > c, 'reads a field through'),
            ('Int', 'Named'): ('Named("n{x}-pad-pad-pad-pad-pad", x)',
                               lambda st, x: ('Named', x), 'builds a String'),
            ('Named', 'Int'): (f'x.n + {c}', lambda st, x: wrap(x[1] + c), None),
            ('Named', 'Bool'): (f'x.n > {c}', lambda st, x: x[1] > c, None),
            ('Int', 'Tok'): ('Tok(x)', lambda st, x: ('Tok', x), None),
            ('Tok', 'Int'): (f'x.n + {c}', lambda st, x: wrap(x[1] + c), None),
            ('Tok', 'Bool'): (f'x.n > {c}', lambda st, x: x[1] > c, None),
            ('Shape', 'Int'): ('x.area()', lambda st, x: x, "calls '"),
            ('Shape', 'Bool'): (f'x.area() > {c}', lambda st, x: x > c, "calls '"),
        }
        if (src, out) == ('Int', 'Opt'):
            h = Helper(name, 'x: Int', 'Int?',
                       pre + ['if x % 2 == 0 {', '    return null', '}', 'return x'],
                       (lambda st, x: (hook(st, x) if hook else None,
                                       None if tmod(x, 2) == 0 else x)[1]),
                       blame(None, 'performs an operation'), src, out)
            return self.add(h)
        if (src, out) == ('Opt', 'Int'):
            h = Helper(name, 'x: Int?', 'Int',
                       ['if x != null {', '    return x', '}', f'return {c}'],
                       lambda st, x: c if x is None else x, 'performs an operation', src, out)
            return self.add(h)
        if (src, out) == ('Opt', 'Bool'):
            return self.add(Helper(name, 'x: Int?', 'Bool', ['return x != null'],
                                   lambda st, x: x is not None, None, src, out))
        e, py, reason = table[(src, out)]
        if reason and reason.startswith('calls'):
            return mk(e, py, call_reason=reason)
        return mk(e, py, reason)

    def reducer(self, src):
        """(acc kind, helper, init expression, init python value)."""
        r = self.r
        name = self.fresh('fold')
        c = r.randint(2, 5)
        if src == 'Float':
            h = Helper(name, 'acc: Float, x: Float', 'Float', ['return acc + x'],
                       lambda st, a, x: a + x, None)
            return 'Float', self.add(h), '0.5', 0.5
        if src == 'Pt':
            h = Helper(name, 'acc: Int, x: Pt', 'Int', [f'return acc * {c} + x.a - x.b'],
                       lambda st, a, x: wrap(a * c + x[1] - x[2]), None)
            return 'Int', self.add(h), '1', 1
        pick = r.random()
        if pick < 0.2:
            def py(st, a, x):
                st['out'].append(f'{name} {x}')
                return wrap(a + x)
            h = Helper(name, 'acc: Int, x: Int', 'Int',
                       [f'println("{name} {{x}}")', 'return acc + x'], py, "calls 'println'")
            return 'Int', self.add(h), '0', 0
        if pick < 0.35:
            h = Helper(name, 'acc: Pt, x: Int', 'Pt', ['return Pt(acc.a + x, acc.b + 1)'],
                       lambda st, a, x: ('Pt', wrap(a[1] + x), wrap(a[2] + 1)), None)
            return 'Pt', self.add(h), 'Pt(0, 0)', ('Pt', 0, 0)
        if pick < 0.45:
            h = Helper(name, 'acc: String, x: Int', 'String', ['return "{acc}{x};"'],
                       lambda st, a, x: f'{a}{x};', 'builds a String')
            return 'Str', self.add(h), '""', ''
        h = Helper(name, 'acc: Int, x: Int', 'Int', [f'return acc * {c} + x'],
                   lambda st, a, x: wrap(a * c + x), None)
        init = r.randint(0, 9)
        return 'Int', self.add(h), str(init), init

    # ----------------------------------------------------------------- cases

    def literal(self, kind, v):
        if kind == 'Int':
            return str(v)
        if kind == 'Float':
            return repr(v)
        if kind == 'Pt':
            return f'Pt({v[1]}, {v[2]})'
        if kind == 'Str':
            return f'"{v}"'
        if kind == 'Shape':
            return f'Sq({v[0]})' if v[1] is None else f'Rc2({v[0]}, {v[1]})'
        raise AssertionError(kind)

    def source(self, lines, decls):
        """Returns (expression, kind, python list, var name or None)."""
        r = self.r
        kind = r.choice(('Int',) * 6 + ('Float', 'Pt', 'Pt', 'Str', 'Shape'))
        count = r.choice((0, 1, 2, 3, 3, 4, 5, 6, 7))
        if kind == 'Int':
            vals = [r.randint(-6, 14) for _ in range(count)]
        elif kind == 'Float':
            vals = [r.randint(-8, 16) * 0.25 for _ in range(count)]
        elif kind == 'Pt':
            vals = [('Pt', r.randint(-5, 9), r.randint(-5, 9)) for _ in range(count)]
        elif kind == 'Str':
            vals = ['w' * r.randint(0, 30) for _ in range(count)]
        else:
            raw = [(r.randint(1, 5), r.choice((None, r.randint(1, 4)))) for _ in range(count)]
            lit_ = '[' + ', '.join(self.literal('Shape', v) for v in raw) + ']'
            lines.append(f'val src: [Shape] = {lit_}')
            return 'src', 'Shape', [a * a if b is None else a * b for a, b in raw], None
        lit_ = '[' + ', '.join(self.literal(kind, v) for v in vals) + ']'
        t = itype(kind)
        form = r.choice(('literal', 'val', 'var', 'var', 'global', 'param', 'call', 'soa',
                         'bounded', 'copy'))
        if form == 'soa' and kind == 'Pt':
            lines.append(f'val src: [Pt, layout: soa] = {lit_}')
            return 'src', kind, vals, None
        if form == 'bounded' and kind == 'Int':
            lines.append(f'var src: [Int; <=16] = {lit_}')
            lines.append('src.push(1)')
            self.bounded = True
            return 'src', kind, vals + [1], None
        if form == 'global' and kind == 'Int' and vals:
            g = self.fresh('g_src')
            decls.append(f'val {g} = {lit_}')
            return g, kind, vals, None
        if form == 'call' and vals:
            f = self.fresh('make')
            decls.append(f'func {f}() -> [{t}] {{\n    return {lit_}\n}}')
            return f'{f}()', kind, vals, None
        if form == 'literal' and vals:
            return lit_, kind, vals, None
        if form == 'copy':
            lines.append(f'val base: [{t}] = {lit_}')
            return 'base.copy()', kind, vals, None
        if form == 'var' and kind == 'Int':
            lines.append(f'var src: [{t}] = {lit_}')
            lines.append('src.push(2)')
            return 'src', kind, vals + [2], 'src'
        lines.append(f'val src: [{t}] = {lit_}')
        return 'src', kind, vals, None

    def callback(self, h, lines):
        """(Iron argument, python function(st, x), may run fused?). Also
        returns statements to run after the pipeline's statement."""
        r = self.r
        form = r.random()
        t_in, t_out = itype(h.src), itype(h.out)
        if form < 0.55:
            return h.name, h.py, h.safe, []
        if form < 0.70:
            return f'func(x: {t_in}) -> {t_out} {{ return {h.name}(x) }}', h.py, h.safe, []
        if h.out == 'Int':
            if form < 0.80:
                cap = self.fresh('cap')
                c = r.randint(1, 9)
                lines.append(f'val {cap} = {c}')
                return (f'func(x: {t_in}) -> Int {{ return {h.name}(x) + {cap} }}',
                        lambda st, x: wrap(h.py(st, x) + c), h.safe, [])
            if form < 0.88:
                cap = self.fresh('vcap')
                c = r.randint(1, 9)
                lines.append(f'var {cap} = {c}')
                return (f'func(x: {t_in}) -> Int {{ return {h.name}(x) + {cap} }}',
                        lambda st, x: wrap(h.py(st, x) + c), False,
                        [f'{cap} = {cap} + 1', f'println("{{{cap}}}")', ('out', str(c + 1))])
            if form < 0.94:
                calc = self.fresh('calc')
                c = r.randint(-4, 4)
                lines.append(f'val {calc} = Calc({c})')
                return (f'func(x: {t_in}) -> Int {{ return {calc}.apply({h.name}(x)) }}',
                        lambda st, x: wrap(h.py(st, x) + c), h.safe, [])
            return (f'func(x: {t_in}) -> Int {{ return ident({h.name}(x)) }}', h.py, h.safe, [])
        return h.name, h.py, h.safe, []

    def case(self, index):
        r = self.r
        decls, lines, after = [], [], []
        self.bounded = False
        expr, kind, vals, var_name = self.source(lines, decls)
        # ops: text (".map(f)"), method, run(st, list) -> list or scalar,
        # src (kind read), kind (kind produced), cand (may be a stage)
        ops = []
        cur = kind

        def op(text, method, out, cand, run):
            # A stage that reads an interface list is never fused.
            ops.append({'text': text, 'method': method, 'src': cur, 'kind': out,
                        'cand': cand and cur != 'Shape', 'run': run})

        def mapper(py):
            return lambda st, xs: [py(st, x) for x in xs]

        for _ in range(r.randint(1, 5)):
            roll = r.random()
            # (a bounded vector has no copy(): it is copied by value)
            if roll < 0.08 and cur not in ('Shape', 'Tok') and (ops or not self.bounded):
                op('.copy()', None, cur, False, lambda st, xs: list(xs))
                continue
            if roll < 0.50:
                outs = {'Int': ('Int',) * 8 + ('Pt', 'Pt', 'Float', 'Str', 'List', 'Rc', 'Named',
                                               'Opt', 'Tok'),
                        'Float': ('Float', 'Int'), 'Pt': ('Pt', 'Int', 'Int'),
                        'Str': ('Int', 'Str'), 'List': ('Int',), 'Rc': ('Int',),
                        'Named': ('Int',), 'Opt': ('Int',), 'Tok': ('Int',),
                        'Shape': ('Int',)}[cur]
                out = r.choice(outs)
                h = self.helper(cur, out)
                arg, py, safe, post = self.callback(h, lines)
                after += post
                if out == 'Tok':
                    self.any_output = True
                op(f'.map({arg})', 'map', out, safe and out in TRIVIAL, mapper(py))
                cur = out
            else:
                if cur == 'Float' and r.random() < 0.5:
                    continue
                h = self.helper(cur, 'Bool')
                arg = h.name
                if r.random() < 0.3:
                    arg = f'func(x: {itype(cur)}) -> Bool {{ return {h.name}(x) }}'
                op(f'.filter({arg})', 'filter', cur, h.safe,
                   (lambda py: lambda st, xs: [x for x in xs if py(st, x)])(h.py))
        # bring the stream to something the last stage can print
        if cur not in ('Int', 'Float', 'Pt'):
            h = self.helper(cur, 'Int')
            op(f'.map({h.name})', 'map', 'Int', h.safe, mapper(h.py))
            cur = 'Int'
        last = r.choice({'Int': ('reduce', 'reduce', 'sum', 'sum', 'sum', 'side', 'each', 'len',
                                 'list', 'list'),
                         'Float': ('reduce', 'sum', 'sum', 'len'),
                         'Pt': ('reduce', 'reduce', 'len', 'collect')}[cur])
        show = None      # how the statement prints its result
        if last == 'reduce':
            acc_kind, h, init, init_py = self.reducer(cur)
            op(f'.reduce({init}, {h.name})', 'reduce', acc_kind, h.safe and acc_kind in TRIVIAL,
               (lambda py, i0: lambda st, xs: self.fold(st, py, i0, xs))(h.py, init_py))
            show = acc_kind
        elif last == 'sum':
            op('.sum()', 'sum', cur, True, lambda st, xs: self.total(xs))
            show = cur
        elif last == 'side':
            side = self.fresh('side')
            lines.append(f'var {side} = 0')
            op(f'.forEach(func(x: Int) {{ {side} += x }})', 'forEach', None, False,
               lambda st, xs: self.total(xs))
            show = ('side', side)
        elif last == 'each':
            def each(st, xs):
                for x in xs:
                    st['out'].append(f'each {x}')
            op('.forEach(func(x: Int) { println("each {x}") })', 'forEach', None, False, each)
            show = 'none'
        elif last == 'len':
            op('.len()', None, 'Int', False, lambda st, xs: len(xs))
            show = 'Int'
        else:
            show = 'list' if cur == 'Int' else 'len'
        if show == 'none' or isinstance(show, tuple):
            # a statement cannot start with a list literal
            if expr.startswith('['):
                lines.append(f'val lit_src = {expr}')
                expr = 'lit_src'
        self.emit(index, decls, lines, after, expr, vals, var_name, ops, show)

    @staticmethod
    def fold(st, py, init, xs):
        acc = init
        for x in xs:
            acc = py(st, acc, x)
        return acc

    @staticmethod
    def total(xs):
        if xs and isinstance(xs[0], float):
            return sum(xs, 0.0)
        t = 0
        for x in xs:
            t = wrap(t + x)
        return t

    def emit(self, index, decls, lines, after, expr, vals, var_name, ops, show):
        """Lay the pipeline out over statements, mark the fused lines and
        build the model."""
        r = self.r
        n = len(ops)
        # split points: ops[i] ends a statement (bound to a val) when i in cut
        cut = set()
        if n >= 2 and r.random() < 0.35:
            for i in range(n - 1):
                if ops[i]['method'] in ('map', 'filter', None) and ops[i]['text'] != '.len()' \
                        and r.random() < 0.4:
                    cut.add(i)
        loop = not cut and r.random() < 0.12
        reassign = (not cut and not loop and var_name and show in ('list', 'len') and
                    all(o['kind'] == 'Int' and o['method'] in ('map', 'filter') for o in ops)
                    and r.random() < 0.6)
        steps = []       # model steps: ('ops', [op...]) | ('py', thunk)
        stmts = []       # (text, [ops indexes in this statement])
        recv = expr
        start = 0
        between = {}     # cut index -> kind of statement written after it
        extra_use = set()
        for i in sorted(cut):
            name = f't{i}'
            stmts.append((f'val {name} = {recv}' + ''.join(o['text'] for o in ops[start:i + 1]),
                          list(range(start, i + 1))))
            pick = r.random()
            if pick < 0.55:
                between[i] = 'none'
            elif pick < 0.67:
                between[i] = 'const'
                stmts.append((f'val k{i} = {r.randint(1, 9)}', []))
            elif pick < 0.77:
                between[i] = 'print'
                stmts.append(('println("between")', ('out', 'between')))
            elif pick < 0.87:
                between[i] = 'global'
                d = r.randint(1, 5)
                stmts.append((f'g_bias = g_bias + {d}', ('bias', d)))
            elif pick < 0.93 and var_name:
                between[i] = 'push'
                stmts.append((f'{var_name}.push(77)', ('push', 77)))
            else:
                between[i] = 'use'
                extra_use.add(i)
                stmts.append((f'println("{{{name}.len()}}")', ('len', i)))
            recv = name
            start = i + 1
        tail = ''.join(o['text'] for o in ops[start:])
        last_ops = list(range(start, n))
        if reassign:
            stmts.append((f'{var_name} = {recv}{tail}', last_ops))
            stmts.append((f'println("{{{var_name}.len()}} {{chk({var_name})}}")', ('show', 'list')))
        elif show == 'none':
            stmts.append((f'{recv}{tail}', last_ops))
        elif isinstance(show, tuple):
            stmts.append((f'{recv}{tail}', last_ops))
            stmts.append((f'println("{{{show[1]}}}")', ('show', 'Int')))
        elif show == 'list':
            stmts.append((f'val res = {recv}{tail}', last_ops))
            stmts.append(('println("{res.len()} {chk(res)}")', ('show', 'list')))
        elif show == 'len':
            stmts.append((f'val res = {recv}{tail}', last_ops))
            stmts.append(('println("{res.len()}")', ('show', 'len')))
        elif show == 'Float':
            stmts.append((f'println("{{Int(({recv}{tail}) * 4.0)}}")', last_ops))
        elif show == 'Pt':
            stmts.append((f'val res = {recv}{tail}', last_ops))
            stmts.append(('println("{res.a} {res.b}")', ('show', 'Pt')))
        elif show == 'Str':
            stmts.append((f'println({recv}{tail})', last_ops))
        else:
            stmts.append((f'println("{{{recv}{tail}}}")', last_ops))

        # ---- which lines fuse (manual 5.2)
        def stream_ok(i):
            return ops[i]['method'] in ('map', 'filter') and ops[i]['kind'] in TRIVIAL

        linked = []
        for i in range(n - 1):
            ok = ops[i]['cand'] and ops[i + 1]['cand'] and stream_ok(i)
            if i in cut and (between[i] not in ('none', 'const') or i in extra_use):
                ok = False
            linked.append(ok)
        op_stmt = {}
        for si, (_, which) in enumerate(stmts):
            if isinstance(which, list):
                for i in which:
                    op_stmt[i] = si
        marks = {}
        i = 0
        while i < n:
            j = i
            while j < n - 1 and linked[j]:
                j += 1
            if j > i:
                stages = ' '.join(ops[k]['method'] for k in range(i, j + 1))
                marks.setdefault(op_stmt[j], []).append(stages)
            i = j + 1

        body = list(lines)
        if loop:
            body.append('for round in range(2) {')
        for si, (text, _) in enumerate(stmts):
            mark = f'  // @fused: {"; ".join(marks[si])}' if si in marks else ''
            body.append(('    ' if loop else '') + text + mark)
        if loop:
            body.append('}')
        for a in after:
            if isinstance(a, str):
                body.append(a)
        param = ''
        call = f'case_{index}()'
        self.cases.append((decls, [f'func case_{index}({param}) {{'] +
                           ['    ' + b for b in body] + ['}'], call))

        # ---- model
        src0 = list(vals)

        def model(st):
            src = list(src0)
            side_total = 0
            for _ in range(2 if loop else 1):
                cur = src
                held = {}
                for text, which in stmts:
                    if isinstance(which, list):
                        for i in which:
                            cur = ops[i]['run'](st, cur)
                            if i in cut:
                                held[i] = cur
                        if which and which[-1] == n - 1:
                            result = cur
                            if reassign:
                                src = result
                            elif show == 'none' or isinstance(show, tuple) or show in ('list', 'len',
                                                                                     'Pt'):
                                pass
                            elif show == 'Float':
                                st['out'].append(str(f2i(result * 4.0)))
                            elif show == 'Str':
                                st['out'].append(result)
                            elif show == 'Bool':
                                st['out'].append('true' if result else 'false')
                            else:
                                st['out'].append(str(result))
                    elif which == ():
                        pass
                    elif which[0] == 'out':
                        st['out'].append(which[1])
                    elif which[0] == 'bias':
                        st['g_bias'] = wrap(st['g_bias'] + which[1])
                    elif which[0] == 'push':
                        src.append(which[1])
                    elif which[0] == 'len':
                        st['out'].append(str(len(held[which[1]])))
                    elif which[0] == 'show':
                        if reassign:
                            st['out'].append(f'{len(src)} {self.checksum(src)}')
                        elif which[1] == 'list':
                            st['out'].append(f'{len(result)} {self.checksum(result)}')
                        elif which[1] == 'len':
                            st['out'].append(str(len(result)))
                        elif which[1] == 'Pt':
                            st['out'].append(f'{result[1]} {result[2]}')
                        elif isinstance(show, tuple):
                            side_total = wrap(side_total + result)
                            st['out'].append(str(side_total))
                        else:
                            st['out'].append(str(result))
            for a in after:
                if isinstance(a, tuple):
                    st['out'].append(a[1])
        self.cases[-1] = self.cases[-1] + (model,)

    @staticmethod
    def checksum(xs):
        h = 7
        for x in xs:
            h = wrap(h * 31 + x)
        return h

    # --------------------------------------------------------------- program

    def program(self):
        for index in range(self.r.randint(3, 6)):
            self.case(index)

    def render(self):
        out = [SUPPORT]
        for h in self.helpers:
            if h.hint:
                out.append('//iron:fusible')
            mark = f'  // @w0618: {h.why}' if h.hint and h.why else ''
            out.append(f'func {h.name}({h.params}) -> {h.ret} {{{mark}')
            out += ['    ' + line for line in h.body]
            out.append('}')
        for decls, lines, _, _ in self.cases:
            out += decls
            out += lines
        out.append('func main() {')
        for index, (_, _, call, _) in enumerate(self.cases):
            out.append(f'    println("case {index}")')
            out.append(f'    {call}')
        out.append('    println("end {g_acc} {g_bias}")')
        out.append('}')
        return '\n'.join(out) + '\n'

    def model(self):
        st = {'out': [], 'g_acc': 0, 'g_bias': 2}
        panic = None
        try:
            for index, (_, _, _, run) in enumerate(self.cases):
                st['out'].append(f'case {index}')
                run(st)
            st['out'].append(f'end {st["g_acc"]} {st["g_bias"]}')
        except Panic as e:
            panic = str(e)
        return Expected(st['out'], panic=panic)


def generate(rng):
    g = FusibleGen(rng)
    g.program()
    return g
