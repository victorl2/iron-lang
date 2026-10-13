"""Complex type shapes for cg_fuzz.py (area `shapes`).

Random nested types: lists, maps, optionals and tuples of each other and of
objects, enums (payloads of every shape, tuples included), generic objects
(Gb[T], Gp[A, B]) and a generic enum (Mb[T]) instantiated with any of them,
and interfaces holding generic and concrete implementors. Each probe builds
a value of such a type, moves it through a binding, a generic function, a
generic object, a list, a map, an optional, a function return or an
interface, and prints it with a show function the program declares per
type; the expected text comes from a model of the same value.

The values are written in typed positions only (an annotated binding, a
field, an argument, a return), so `null`, `[]` and `Mb.No` always have a
type to take. Maps have no literal: a probe fills a `var` map with put and
hands it over with take().
"""
from pf_lang import Bail
from pf_probe import Probe, ProbeGen

BASE = ('Int', 'String', 'Bool')
STRS = ['', 'a', 'bc', 'xyz', 'q r', 'Z9']
INTS = [0, 1, -1, 7, 42, -300, 65536, 9223372036854775807, -9223372036854775807]


class Shapes(ProbeGen):
    NPROBES = (4, 10)

    def __init__(self, rng):
        super().__init__(rng)
        self.objs = {}      # name -> [field types]
        self.enums = {}     # name -> [(variant, [payload types])]
        self.shows = set()
        self.helpers = set()
        self.k = 0

    # ---------------------------------------------------------------- types

    def rand_type(self, depth=0, no_tuple=False, no_opt=False):
        r = self.r
        if depth >= 3 or r.random() < 0.25 + depth * 0.15:
            return (r.choice(BASE),)
        k = r.choice(['list', 'list', 'map', 'opt', 'opt', 'tup', 'obj', 'enum', 'gbox', 'gpair',
                      'gopt'])
        if k == 'tup' and no_tuple:
            k = 'list'
        if k == 'opt' and no_opt:
            k = 'gbox'
        if k == 'list':
            return ('List', self.rand_type(depth + 1, no_tuple=True))
        if k == 'map':
            return ('Map', self.rand_type(depth + 1, no_tuple=True))
        if k == 'opt':
            return ('Opt', self.rand_type(depth + 1, no_opt=True, no_tuple=True))
        if k == 'tup':
            # (a tuple holding a list cannot be taken apart out of a binding
            # or parameter, E0328: its show function could not read it)
            return ('Tup', self.listless(depth + 1), self.listless(depth + 1))
        if k == 'gbox':
            return ('Gb', self.rand_type(depth + 1))
        if k == 'gpair':
            return ('Gp', self.rand_type(depth + 1), self.rand_type(depth + 1))
        if k == 'gopt':
            return ('Mb', self.rand_type(depth + 1))
        if k == 'obj':
            done = sorted(n for n, f in self.objs.items() if f is not None)
            if done and r.random() < 0.5:
                return ('Obj', r.choice(done))
            name = f'R{len(self.objs) + 1}'
            self.objs[name] = None   # reserved (a field may not hold its own type)
            self.objs[name] = [self.rand_type(depth + 1) for _ in range(r.randint(1, 3))]
            return ('Obj', name)
        done = sorted(n for n, v in self.enums.items() if v is not None)
        if done and r.random() < 0.5:
            return ('Enum', r.choice(done))
        name = f'E{len(self.enums) + 1}'
        self.enums[name] = None
        vs = []
        for i in range(r.randint(1, 4)):
            n = r.choice([0, 0, 1, 1, 2])
            vs.append((f'V{i}', [self.rand_type(depth + 1) for _ in range(n)]))
        self.enums[name] = vs
        return ('Enum', name)

    def listless(self, depth):
        for _ in range(10):
            t = self.rand_type(depth)
            if not self.holds_list(t):
                return t
        return ('Int',)

    def syn(self, t):
        k = t[0]
        if k in BASE:
            return k
        if k == 'List':
            return f'[{self.syn(t[1])}]'
        if k == 'Map':
            return f'Map[String, {self.syn(t[1])}]'
        if k == 'Opt':
            return f'{self.syn(t[1])}?'
        if k == 'Tup':
            return f'({self.syn(t[1])}, {self.syn(t[2])})'
        if k == 'Gb':
            return f'Gb[{self.syn(t[1])}]'
        if k == 'Gp':
            return f'Gp[{self.syn(t[1])}, {self.syn(t[2])}]'
        if k == 'Mb':
            return f'Mb[{self.syn(t[1])}]'
        return t[1]

    def mang(self, t):
        k = t[0]
        if k in BASE:
            return k[0]
        if k in ('Obj', 'Enum'):
            return t[1]
        return {'List': 'L', 'Map': 'M', 'Opt': 'O', 'Tup': 'T', 'Gb': 'G', 'Gp': 'P',
                'Mb': 'Q'}[k] + ''.join(self.mang(x) for x in t[1:]) + 'e'

    # --------------------------------------------------------- declarations

    def declare_types(self):
        if self.objs:
            for name, fields in self.objs.items():
                body = '\n'.join(f'    val f{i}: {self.syn(ft)}' for i, ft in enumerate(fields))
                self.need('obj ' + name, f'object {name} {{\n{body}\n}}')
        for name, vs in self.enums.items():
            body = []
            for v, ps in vs:
                body.append(f'    {v}({", ".join(self.syn(p) for p in ps)}),' if ps else f'    {v},')
            self.need('enum ' + name, f'enum {name} {{\n' + '\n'.join(body) + '\n}')

    def need_generics(self):
        self.need('Gb', 'object Gb[T] {\n    val v: T\n}')
        self.need('Gp', 'object Gp[A, B] {\n    val a: A\n    val b: B\n}')
        self.need('Mb', 'enum Mb[T] {\n    No,\n    So(T),\n}')
        self.need('ident', 'func ident[T](x: T) -> T {\n    return x\n}')
        self.need('Show', 'interface Show {\n    readonly func show() -> String\n}')
        self.need('GH', 'object GH[T] impl Show {\n    val v: T\n    val tag: String\n'
                        '    readonly func show() -> String {\n        return self.tag\n    }\n}')

    def show_fn(self, t):
        """The name of the show function of t (declared on first use)."""
        name = 'sh_' + self.mang(t)
        if name in self.shows:
            return name
        self.shows.add(name)
        k = t[0]
        ts = self.syn(t)
        if k == 'Int' or k == 'Bool':
            body = ['    return "{x}"']
        elif k == 'String':
            body = ['    return "\'" + x + "\'"']
        elif k == 'List':
            body = ['    var s = "["', '    for e in x {',
                    f'        s = s + {self.show_fn(t[1])}(e) + ","', '    }', '    return s + "]"']
        elif k == 'Map':
            body = ['    var ks = x.keys()', '    ks.sort()', '    var s = "<"', '    for k in ks {',
                    f'        s = s + k + ":" + {self.show_fn(t[1])}(x.get(k)) + ","', '    }',
                    '    return s + ">"']
        elif k == 'Opt':
            body = ['    if x != null {', f'        return "S(" + {self.show_fn(t[1])}(x) + ")"',
                    '    }', '    return "N"']
        elif k == 'Tup':
            body = ['    val (a, b) = x',
                    f'    return "(" + {self.show_fn(t[1])}(a) + "," + {self.show_fn(t[2])}(b) + ")"']
        elif k == 'Gb':
            body = [f'    return "Gb(" + {self.show_fn(t[1])}(x.v) + ")"']
        elif k == 'Gp':
            body = [f'    return "Gp(" + {self.show_fn(t[1])}(x.a) + "," + {self.show_fn(t[2])}(x.b) + ")"']
        elif k == 'Mb':
            body = ['    match x {', '        Mb.No -> return "No"',
                    f'        Mb.So(p0) -> return "So(" + {self.show_fn(t[1])}(p0) + ")"', '    }',
                    '    return ""']
        elif k == 'Obj':
            fields = self.objs[t[1]]
            parts = ' + "," + '.join(f'{self.show_fn(ft)}(x.f{i})' for i, ft in enumerate(fields))
            body = [f'    return "{t[1]}(" + {parts} + ")"']
        else:
            body = ['    match x {']
            for v, ps in self.enums[t[1]]:
                if not ps:
                    body.append(f'        {t[1]}.{v} -> return "{v}"')
                else:
                    binds = ', '.join(f'p{i}' for i in range(len(ps)))
                    parts = ' + "," + '.join(f'{self.show_fn(p)}(p{i})' for i, p in enumerate(ps))
                    body.append(f'        {t[1]}.{v}({binds}) -> return "{v}(" + {parts} + ")"')
            body += ['    }', '    return ""']
        self.need('show ' + name, f'func {name}(x: {ts}) -> String {{\n' + '\n'.join(body) + '\n}')
        return name

    def mb_helpers(self, t):
        """so_X(v) -> Mb[T] and no_X() -> Mb[T]: a typed position for v."""
        m = self.mang(t)
        if m not in self.helpers:
            self.helpers.add(m)
            ts = self.syn(t)
            self.need('so ' + m, f'func so_{m}(x: {ts}) -> Mb[{ts}] {{\n    return Mb.So(x)\n}}')
            self.need('no ' + m, f'func no_{m}() -> Mb[{ts}] {{\n    return Mb.No\n}}')
        return f'so_{m}', f'no_{m}'

    # --------------------------------------------------------------- values

    def rand_val(self, t, depth=0):
        r, k = self.r, t[0]
        if k == 'Int':
            return r.choice(INTS)
        if k == 'String':
            return r.choice(STRS)
        if k == 'Bool':
            return r.random() < 0.5
        if k == 'List':
            return ['L'] + [self.rand_val(t[1], depth + 1) for _ in range(r.randint(0, 3 - min(depth, 2)))]
        if k == 'Map':
            n = r.randint(0, 3 - min(depth, 2))
            return ['M'] + [(f'k{i}', self.rand_val(t[1], depth + 1)) for i in r.sample(range(9), n)]
        if k == 'Opt':
            return None if r.random() < 0.35 else ('S', self.rand_val(t[1], depth + 1))
        if k in ('Tup', 'Gp'):
            return (k, self.rand_val(t[1], depth + 1), self.rand_val(t[2], depth + 1))
        if k == 'Gb':
            return ('Gb', self.rand_val(t[1], depth + 1))
        if k == 'Mb':
            return ('No',) if r.random() < 0.3 else ('So', self.rand_val(t[1], depth + 1))
        if k == 'Obj':
            return ('Obj',) + tuple(self.rand_val(ft, depth + 1) for ft in self.objs[t[1]])
        v, ps = r.choice(self.enums[t[1]])
        return ('En', v) + tuple(self.rand_val(p, depth + 1) for p in ps)

    def show(self, t, v):
        k = t[0]
        if k == 'Int':
            return str(v)
        if k == 'Bool':
            return 'true' if v else 'false'
        if k == 'String':
            return "'" + v + "'"
        if k == 'List':
            return '[' + ''.join(self.show(t[1], e) + ',' for e in v[1:]) + ']'
        if k == 'Map':
            return '<' + ''.join(kk + ':' + self.show(t[1], vv) + ',' for kk, vv in sorted(v[1:])) + '>'
        if k == 'Opt':
            return 'N' if v is None else 'S(' + self.show(t[1], v[1]) + ')'
        if k == 'Tup':
            return '(' + self.show(t[1], v[1]) + ',' + self.show(t[2], v[2]) + ')'
        if k == 'Gp':
            return 'Gp(' + self.show(t[1], v[1]) + ',' + self.show(t[2], v[2]) + ')'
        if k == 'Gb':
            return 'Gb(' + self.show(t[1], v[1]) + ')'
        if k == 'Mb':
            return 'No' if v[0] == 'No' else 'So(' + self.show(t[1], v[1]) + ')'
        if k == 'Obj':
            return t[1] + '(' + ','.join(self.show(ft, x) for ft, x in zip(self.objs[t[1]], v[1:])) + ')'
        ps = dict(self.enums[t[1]])[v[1]]
        if not ps:
            return v[1]
        return v[1] + '(' + ','.join(self.show(p, x) for p, x in zip(ps, v[2:])) + ')'

    def expr(self, t, v, pre):
        """Iron source for value v of type t in a typed position; pre gets
        the statements that must run first (maps)."""
        k = t[0]
        if k == 'Int':
            return str(v)
        if k == 'Bool':
            return 'true' if v else 'false'
        if k == 'String':
            return '"' + v + '"'
        if k == 'List':
            return '[' + ', '.join(self.expr(t[1], e, pre) for e in v[1:]) + ']'
        if k == 'Map':
            self.k += 1
            m = f'm{self.k}'
            stmts = [f'var {m} = Map[String, {self.syn(t[1])}]()']
            for kk, vv in v[1:]:
                stmts.append(f'{m}.put("{kk}", {self.expr(t[1], vv, pre)})')
            pre.extend(stmts)
            return f'{m}.take()'
        if k == 'Opt':
            return 'null' if v is None else self.expr(t[1], v[1], pre)
        if k == 'Tup':
            return '(' + self.expr(t[1], v[1], pre) + ', ' + self.expr(t[2], v[2], pre) + ')'
        if k == 'Gp':
            return (f'Gp[{self.syn(t[1])}, {self.syn(t[2])}](' + self.expr(t[1], v[1], pre) + ', '
                    + self.expr(t[2], v[2], pre) + ')')
        if k == 'Gb':
            return f'Gb[{self.syn(t[1])}](' + self.expr(t[1], v[1], pre) + ')'
        if k == 'Mb':
            # in a typed position: the expected type fills Mb's argument
            return 'Mb.No' if v[0] == 'No' else 'Mb.So(' + self.expr(t[1], v[1], pre) + ')'
        if k == 'Obj':
            fields = self.objs[t[1]]
            return t[1] + '(' + ', '.join(self.expr(ft, x, pre) for ft, x in zip(fields, v[1:])) + ')'
        ps = dict(self.enums[t[1]])[v[1]]
        if not ps:
            return f'{t[1]}.{v[1]}'
        return f'{t[1]}.{v[1]}(' + ', '.join(self.expr(p, x, pre) for p, x in zip(ps, v[2:])) + ')'

    # ---------------------------------------------------------------- probes

    def fresh_t(self):
        t = self.rand_type()
        if t[0] in BASE and self.r.random() < 0.7:
            t = self.rand_type()
        return t

    def p_bind(self):
        t = self.fresh_t()
        v = self.rand_val(t)
        pre = []
        e = self.expr(t, v, pre)
        x = self.fresh('x')
        sh = self.show_fn(t)
        return Probe(pre + [f'val {x}: {self.syn(t)} = {e}', f'println({sh}({x}))'],
                     [self.show(t, v)])

    def holds_list(self, t, seen=()):
        """A value of t owns a list or map (ident would duplicate it: E0328)."""
        k = t[0]
        if k in ('List', 'Map'):
            return True
        if k in BASE:
            return False
        if k == 'Obj':
            return t[1] not in seen and any(self.holds_list(f, seen + (t[1],))
                                            for f in (self.objs[t[1]] or []))
        if k == 'Enum':
            return t[1] not in seen and any(self.holds_list(p, seen + (t[1],))
                                            for _, ps in (self.enums[t[1]] or []) for p in ps)
        return any(self.holds_list(x, seen) for x in t[1:])

    def p_ident(self):
        t = self.fresh_t()
        if self.holds_list(t):
            return self.p_bind()
        v = self.rand_val(t)
        pre = []
        e = self.expr(t, v, pre)
        return Probe(pre + [f'println({self.show_fn(t)}(ident[{self.syn(t)}]({e})))'],
                     [self.show(t, v)])

    def p_box(self):
        t = self.fresh_t()
        v = self.rand_val(t)
        pre = []
        e = self.expr(t, v, pre)
        b = self.fresh('b')
        sh = self.show_fn(t)
        sb = self.show_fn(('Gb', t))
        return Probe(pre + [f'val {b} = Gb[{self.syn(t)}]({e})', f'println({sh}({b}.v))',
                            f'println({sb}({b}))'],
                     [self.show(t, v), 'Gb(' + self.show(t, v) + ')'])

    def p_list(self):
        t = self.rand_type(no_tuple=True)
        v1, v2 = self.rand_val(t), self.rand_val(t)
        pre = []
        e1, e2 = self.expr(t, v1, pre), self.expr(t, v2, pre)
        lst = self.fresh('l')
        lt = ('List', t)
        code = pre + [f'var {lst}: [{self.syn(t)}] = [{e1}]', f'{lst}.push({e2})',
                      f'println({self.show_fn(lt)}({lst}))',
                      f'println({self.show_fn(t)}({lst}[1]))', f'println("{{len({lst})}}")']
        return Probe(code, [self.show(lt, ['L', v1, v2]), self.show(t, v2), '2'])

    def p_map(self):
        t = self.rand_type(no_tuple=True)
        v1, v2 = self.rand_val(t), self.rand_val(t)
        pre = []
        e1, e2 = self.expr(t, v1, pre), self.expr(t, v2, pre)
        m = self.fresh('mp')
        mt = ('Map', t)
        code = pre + [f'var {m} = Map[String, {self.syn(t)}]()', f'{m}.put("b", {e1})',
                      f'{m}.put("a", {e2})', f'println({self.show_fn(mt)}({m}))',
                      f'println({self.show_fn(t)}({m}.get("b")))',
                      f'println("{{{m}.remove("a")}} {{{m}.len()}} {{{m}.has("a")}}")']
        return Probe(code, [self.show(mt, ['M', ('b', v1), ('a', v2)]), self.show(t, v1),
                            'true 1 false'])

    def p_opt(self):
        t = self.rand_type(no_opt=True, no_tuple=True)
        v = self.rand_val(t)
        pre = []
        e = self.expr(t, v, pre)
        o, n = self.fresh('o'), self.fresh('o')
        ot = ('Opt', t)
        code = pre + [f'val {o}: {self.syn(t)}? = {e}', f'val {n}: {self.syn(t)}? = null',
                      f'println({self.show_fn(ot)}({o}) + {self.show_fn(ot)}({n}))',
                      f'if {o} != null {{', f'    println({self.show_fn(t)}({o}))', '}']
        return Probe(code, ['S(' + self.show(t, v) + ')N', self.show(t, v)])

    def p_return(self):
        t = self.fresh_t()
        v = self.rand_val(t)
        pre = []
        e = self.expr(t, v, pre)
        f = self.fresh('mk')
        body = '\n'.join('    ' + l for l in pre + [f'return {e}'])
        self.need('fn ' + f, f'func {f}() -> {self.syn(t)} {{\n{body}\n}}')
        return Probe([f'println({self.show_fn(t)}({f}()))'], [self.show(t, v)])

    def p_iface(self):
        t = self.fresh_t()
        v1, v2 = self.rand_val(t), self.rand_val(t)
        pre = []
        e1, e2 = self.expr(t, v1, pre), self.expr(t, v2, pre)
        e2b = self.expr(t, v2, pre)   # its own copy (a map's take() empties it)
        h = f'H{self.mang(t)}'
        self.need('holder ' + h, f'object {h} impl Show {{\n    val v: {self.syn(t)}\n'
                                 f'    readonly func show() -> String {{\n'
                                 f'        return {self.show_fn(t)}(self.v)\n    }}\n}}')
        s, ss = self.fresh('s'), self.fresh('ss')
        code = pre + [f'val {s}: Show = {h}({e1})', f'println({s}.show())',
                      f'val {ss}: [Show] = [GH[{self.syn(t)}]({e2}, "gh"), {h}({e2b})]',
                      f'for q in {ss} {{', '    println(q.show())', '}']
        return Probe(code, [self.show(t, v1), 'gh', self.show(t, v2)])

    def p_enum_payload(self):
        """A value of t inside a fresh enum variant and a tuple payload."""
        t = self.listless(0)
        v = self.rand_val(t)
        pre = []
        e = self.expr(t, v, pre)
        name = f'W{len(self.enums) + 1}'
        self.enums[name] = [('A', []), ('B', [t, ('Int',)]), ('C', [('Tup', t, ('String',))])]
        et = ('Enum', name)
        which = self.r.randrange(3)
        if which == 0:
            val, src = ('En', 'A'), f'{name}.A'
        elif which == 1:
            val, src = ('En', 'B', v, 5), f'{name}.B({e}, 5)'
        else:
            val, src = ('En', 'C', ('Tup', v, 'z')), f'{name}.C(({e}, "z"))'
        w = self.fresh('w')
        return Probe(pre + [f'val {w}: {name} = {src}', f'println({self.show_fn(et)}({w}))'],
                     [self.show(et, val)])

    def probe_kinds(self):
        return [(3, self.p_bind), (2, self.p_ident), (2, self.p_box), (2, self.p_list),
                (2, self.p_map), (2, self.p_opt), (2, self.p_return), (2, self.p_iface),
                (2, self.p_enum_payload)]

    def render(self):
        self.declare_types()
        return super().render()


def generate(rng):
    g = Shapes(rng)
    g.need_generics()
    g.program()
    if len(g.render()) > 60000:
        raise Bail('too big')
    return g
