"""layout: soa object lists, checked against a model and the default layout.

Each program declares one or two element types with a random set of fields
(numbers of several widths, Bool, String, a nested object, lists, an
optional, a payload enum, an rc handle, a fixed array, a bounded vector)
and runs random sequences of list operations on `[E, layout: soa]` lists:
every list method, element and field writes, methods and pointer
parameters that borrow a whole element, slices, higher order methods, and
lists flowing through parameters, returns, fields, nested lists, maps,
channels, mutexes, tasks and rc. A Python model of the list predicts every
printed line.

The runner (prog_fuzz.check) then builds the same source with the layout
annotations removed and requires the same output, exit code and panic.
Programs whose element type has copy and drop hooks print from the hooks;
the model does not predict those lines, so such a program is checked by
the comparison of the two layouts alone (Expected.any_output).
"""
from pf_probe import Probe, ProbeGen

PAD = '-0123456789abcdef-pad'

SUPPORT = '''object Inner {
    var n: Int
    init(n: Int) { self.n = n }
    func inc(d: Int) { self.n += d }
}
enum Tok { Num(Int), Text(String), Nothing }
object Res { val id: Int }
func mv(m: Int?) -> Int {
    if m != null { return m }
    return -999
}
func tokstr(t: Tok) -> String {
    match t {
        Tok.Num(n) -> return "num{n}"
        Tok.Text(s) -> return "text:{s}"
        Tok.Nothing -> return "nothing"
    }
    return "?"
}
func count[T](xs: [T]) -> Int {
    var n = 0
    for x in xs { n += 1 }
    return n
}
func apply(f: func() -> Int) -> Int { return f() }
'''

# field name -> (Iron type, statements of init from v)
FIELDS = {
    'f':     ('Float',     ['self.f = Float(v) / 2.0']),
    'b':     ('Bool',      ['self.b = v % 2 == 0']),
    's':     ('String',    ['self.s = "s{v}%s"' % PAD]),
    'i8':    ('Int8',      ['self.i8 = 7']),
    'u8':    ('UInt8',     ['self.u8 = 200']),
    'i16':   ('Int16',     ['self.i16 = -300']),
    'i32':   ('Int32',     ['self.i32 = 70000']),
    'f32':   ('Float32',   ['self.f32 = 1.5']),
    'inner': ('Inner',     ['self.inner = Inner(v * 3)']),
    'nums':  ('[Int]',     ['self.nums = [v, v + 1]']),
    'names': ('[String]',  ['self.names = ["n{v}%s"]' % PAD]),
    'maybe': ('Int?',      ['if v % 3 == 0 { self.maybe = null } else { self.maybe = v }']),
    'tok':   ('Tok',       ['if v % 3 == 0 { self.tok = Tok.Num(v) } elif v % 3 == 1 '
                            '{ self.tok = Tok.Text("t{v}' + PAD + '") } else { self.tok = Tok.Nothing }']),
    'res':   ('rc Res',    ['self.res = rc Res(v)']),
    'arr':   ('[Int; 3]',  ['self.arr = [v, v * 2, v * 3]']),
    'bv':    ('[Int; <=4]', ['self.bv = [v]']),
}
SMALL = {'i8': (-128, 127), 'u8': (0, 255), 'i16': (-32768, 32767), 'i32': (-2000000, 2000000)}


def half(v):
    """Float(v) / 2.0 as Iron prints it."""
    return str(v // 2) if v % 2 == 0 else ('-' if v < 0 else '') + f'{abs(v) // 2}.5'


class Schema:
    def __init__(self, k, names, hooks, nocopy_free=True):
        self.k = k
        self.t = f'E{k}'
        self.names = names            # field order, 'x' somewhere in it
        self.hooks = hooks
        self.lt = f'[{self.t}, layout: soa]'

    def has(self, f):
        return f in self.names

    def decl(self):
        t, lt = self.t, self.lt
        o = [f'object {t} {{']
        for f in self.names:
            o.append(f'    var {f}: {"Int" if f == "x" else FIELDS[f][0]}')
        o.append('    init(v: Int) {')
        for f in self.names:
            for st in (['self.x = v'] if f == 'x' else FIELDS[f][1]):
                o.append('        ' + st)
        o.append('    }')
        tot = ['self.x']
        if self.has('inner'):
            tot.append('self.inner.n')
        if self.has('nums'):
            tot.append('self.nums.len()')
        if self.has('arr'):
            tot.append('self.arr[2]')
        o.append('    readonly func total() -> Int { return ' + ' + '.join(tot) + ' }')
        o.append('    func bump(d: Int) { self.x += d }')
        if self.has('s'):
            o.append('    func rename(d: Int) { self.s = "r{d}%s" }' % PAD)
        if self.has('nums'):
            o.append('    func tag(d: Int) { self.nums.push(d) }')
        if self.has('inner'):
            o.append('    func deep(d: Int) {')
            o.append('        self.inner.inc(d)')
            o.append('        self.bump(d)')
            o.append('    }')
        if self.hooks:
            o.append('    copy { println("copy {self.x}") }')
            o.append('    drop { println("drop {self.x}") }')
        o.append('}')
        o.append(f'func row{self.k}(c: {t}) -> String {{')
        o.append('    var out = "{c.x}"')
        for f in self.names:
            e = {'f': '{c.f}', 'b': '{c.b}', 's': '{c.s}', 'i8': '{c.i8}', 'u8': '{c.u8}',
                 'i16': '{c.i16}', 'i32': '{c.i32}', 'f32': '{c.f32}', 'inner': '{c.inner.n}',
                 'nums': '{c.nums.len()}:{c.nums[c.nums.len() - 1]}',
                 'names': '{c.names.len()}:{c.names[0]}', 'maybe': '{mv(c.maybe)}',
                 'tok': '{tokstr(c.tok)}', 'res': '{c.res.id}',
                 'arr': '{c.arr[0]},{c.arr[2]}', 'bv': '{c.bv.len()}:{c.bv[0]}'}.get(f)
            if e:
                o.append(f'    out = "{{out}} {e}"')
        o.append('    return out')
        o.append('}')
        o += [f'func dump{self.k}(xs: {lt}) {{',
              '    println("len {xs.len()}")',
              f'    for c in xs {{ println(row{self.k}(c)) }}',
              '}',
              f'func peek{self.k}(p: *{t}) -> Int {{ return p.x + 1 }}',
              f'func edit{self.k}(p: *var {t}, d: Int) {{',
              '    p.x += d']
        if self.has('s'):
            o.append('    p.s = "e{d}%s"' % PAD)
        if self.has('nums'):
            o.append('    p.nums.push(d)')
        o += ['}',
              f'func tweak{self.k}(var e: {t}, d: Int) {{ e.x -= d }}',
              f'func grow{self.k}(var xs: {lt}, v: Int) {{ xs.push({t}(v)) }}',
              f'func shrink{self.k}(var xs: {lt}) -> Int {{',
              '    val gone = xs.pop()',
              '    return gone.x',
              '}',
              f'func make{self.k}(a: Int, n: Int) -> {lt} {{',
              f'    var out: {lt} = []',
              f'    for i in range(n) {{ out.push({t}(a + i)) }}',
              '    return out',
              '}',
              f'func plain{self.k}(xs: [{t}]) -> Int {{',
              '    var t = 0',
              '    for c in xs { t += c.total() }',
              '    return t',
              '}',
              f'object Hold{self.k} {{',
              f'    var xs: {lt}',
              '    var name: String',
              '    init(a: Int, n: Int) {',
              f'        self.xs = make{self.k}(a, n)',
              '        self.name = "hold{a}%s"' % PAD,
              '    }',
              '    func step(d: Int) {',
              '        for i in range(self.xs.len()) {',
              '            self.xs[i].x += d',
              '            self.xs[i].bump(i)',
              '        }',
              '    }',
              f'    func add(v: Int) {{ self.xs.push({t}(v)) }}',
              '}']
        return '\n'.join(o)

    # ---- model of one element
    def new(self, v):
        e = {'v': v, 'x': v}
        if self.has('s'):
            e['s'] = f's{v}{PAD}'
        for f, (_, _) in SMALL.items():
            if self.has(f):
                e[f] = {'i8': 7, 'u8': 200, 'i16': -300, 'i32': 70000}[f]
        if self.has('inner'):
            e['inner'] = v * 3
        if self.has('nums'):
            e['nums'] = [v, v + 1]
        if self.has('names'):
            e['names'] = [f'n{v}{PAD}']
        if self.has('maybe'):
            e['maybe'] = None if v % 3 == 0 else v
        if self.has('arr'):
            e['arr'] = [v, v * 2, v * 3]
        if self.has('bv'):
            e['bv'] = [v]
        return e

    def clone(self, e):
        return {k: (list(x) if isinstance(x, list) else x) for k, x in e.items()}

    def total(self, e):
        t = e['x']
        if self.has('inner'):
            t += e['inner']
        if self.has('nums'):
            t += len(e['nums'])
        if self.has('arr'):
            t += e['arr'][2]
        return t

    def row(self, e):
        v = e['v']
        out = [str(e['x'])]
        for f in self.names:
            if f == 'f':
                out.append(half(v))
            elif f == 'b':
                out.append('true' if v % 2 == 0 else 'false')
            elif f == 's':
                out.append(e['s'])
            elif f in SMALL:
                out.append(str(e[f]))
            elif f == 'f32':
                out.append('1.5')
            elif f == 'inner':
                out.append(str(e['inner']))
            elif f == 'nums':
                out.append(f'{len(e["nums"])}:{e["nums"][-1]}')
            elif f == 'names':
                out.append(f'{len(e["names"])}:{e["names"][0]}')
            elif f == 'maybe':
                out.append(str(-999 if e['maybe'] is None else e['maybe']))
            elif f == 'tok':
                m = abs(v) % 3 * (1 if v >= 0 else -1)      # C remainder
                out.append(f'num{v}' if m == 0 else f'text:t{v}{PAD}' if m == 1 else 'nothing')
            elif f == 'res':
                out.append(str(v))
            elif f == 'arr':
                out.append(f'{e["arr"][0]},{e["arr"][2]}')
            elif f == 'bv':
                out.append(f'{len(e["bv"])}:{e["bv"][0]}')
        return ' '.join(out)


class SoaGen(ProbeGen):
    NPROBES = (2, 4)
    error_rate = 0
    panic_rate = 0.12

    def __init__(self, rng):
        super().__init__(rng)
        self.need('support', SUPPORT)
        self.schemas = []
        self.hooked = False
        for k in range(rng.randint(1, 2)):
            pool = list(FIELDS)
            names = rng.sample(pool, rng.randint(0, min(7, len(pool))))
            if rng.random() < 0.15:
                names = []                      # one field only
            if rng.random() < 0.08:
                names = pool                    # every field
            names.insert(rng.randint(0, len(names)), 'x')
            hooks = rng.random() < 0.2
            self.hooked = self.hooked or hooks
            sc = Schema(k + 1, names, hooks)
            self.schemas.append(sc)
            self.need(f'schema{k}', sc.decl())
        # the model does not predict what the hooks print
        self.any_output = self.hooked

    def panic_kinds(self):
        # a hooked program is compared between the two layouts only, and a
        # panic of unknown output cannot be told from a crash
        return [] if self.hooked else [self.panic]

    # ------------------------------------------------------------ helpers
    def snap(self, sc, code, out, name, xs):
        code.append(f'dump{sc.k}({name})')
        out += [f'len {len(xs)}'] + [sc.row(e) for e in xs]

    def val(self):
        return self.r.randint(-40, 40)

    # ----------------------------------------------------------- sequence
    def sequence(self):
        r = self.r
        sc = r.choice(self.schemas)
        t, k = sc.t, sc.k
        n = self.fresh('ps')
        vals = [self.val() for _ in range(r.choice((0, 0, 1, 2, 3, 5, 9, 17, 40)))]
        xs = [sc.new(v) for v in vals]
        code, out = [], []
        if vals and r.random() < 0.3:
            code.append(f'var {n}: {sc.lt} = []')
            code += [f'{n}.push({t}({v}))' for v in vals]
        else:
            code.append(f'var {n}: {sc.lt} = [' + ', '.join(f'{t}({v})' for v in vals) + ']')
        ops = ['push', 'push', 'insert', 'remove', 'pop', 'assign', 'set', 'fx', 'fxadd',
               'bump', 'total', 'peek', 'edit', 'tweak', 'reverse', 'copy', 'slice', 'take',
               'clear', 'hof', 'grow', 'shrink', 'loop', 'wloop', 'forsum', 'swap', 'get',
               'plain', 'count', 'closure', 'fill']
        for f in sc.names:
            ops += {'s': ['fs', 'rename'], 'inner': ['finner', 'deep'], 'nums': ['npush', 'nset', 'tag'],
                    'maybe': ['fmaybe'], 'arr': ['farr'], 'bv': ['bvpush'], 'names': ['names'],
                    'i8': ['small'], 'u8': ['small'], 'i16': ['small'], 'i32': ['small']}.get(f, [])
        for _ in range(r.randint(8, 30)):
            op = r.choice(ops)
            v = self.val()
            i = r.randrange(len(xs)) if xs else None
            if op == 'push':
                code.append(f'{n}.push({t}({v}))')
                xs.append(sc.new(v))
            elif op == 'fill':
                c = r.randint(3, 40)
                code.append(f'for q in range({c}) {{ {n}.push({t}(q + {v})) }}')
                xs += [sc.new(q + v) for q in range(c)]
            elif op == 'grow':
                code.append(f'grow{k}({n}, {v})')
                xs.append(sc.new(v))
            elif op == 'insert':
                j = r.randint(0, len(xs))
                code.append(f'{n}.insert({j}, {t}({v}))')
                xs.insert(j, sc.new(v))
            elif op in ('remove', 'pop') and xs:
                j = len(xs) - 1 if op == 'pop' else i
                tmp = self.fresh('gone')
                code += [f'val {tmp} = ' + (f'{n}.pop()' if op == 'pop' else f'{n}.remove({j})'),
                         f'println("gone {{row{k}({tmp})}}")']
                out.append('gone ' + sc.row(xs.pop(j)))
            elif op == 'shrink' and xs:
                code.append(f'println("shrunk {{shrink{k}({n})}}")')
                out.append(f'shrunk {xs.pop()["x"]}')
            elif op == 'assign' and xs:
                code.append(f'{n}[{i}] = {t}({v})')
                xs[i] = sc.new(v)
            elif op == 'set' and xs:
                code.append(f'{n}.{r.choice(("set", "set_unchecked"))}({i}, {t}({v}))')
                xs[i] = sc.new(v)
            elif op == 'fx' and xs:
                code.append(f'{n}[{i}].x = {v}')
                xs[i]['x'] = v
            elif op == 'fxadd' and xs:
                code.append(f'{n}[{i}].x {r.choice(("+=", "-="))} {v}')
                xs[i]['x'] += v if code[-1].split()[1] == '+=' else -v
            elif op == 'fs' and xs:
                code.append(f'{n}[{i}].s = "w{v}{PAD}"')
                xs[i]['s'] = f'w{v}{PAD}'
            elif op == 'small' and xs:
                f = r.choice([f for f in sc.names if f in SMALL])
                w = r.randint(*SMALL[f])
                code.append(f'{n}[{i}].{f} = {w}')
                xs[i][f] = w
            elif op == 'finner' and xs:
                if r.random() < 0.5:
                    code.append(f'{n}[{i}].inner.n = {v}')
                    xs[i]['inner'] = v
                else:
                    code.append(f'{n}[{i}].inner.inc({v})')
                    xs[i]['inner'] += v
            elif op == 'deep' and xs:
                code.append(f'{n}[{i}].deep({v})')
                xs[i]['inner'] += v
                xs[i]['x'] += v
            elif op == 'npush' and xs:
                code.append(f'{n}[{i}].nums.push({v})')
                xs[i]['nums'].append(v)
            elif op == 'nset' and xs:
                code.append(f'{n}[{i}].nums[0] = {v}')
                xs[i]['nums'][0] = v
            elif op == 'tag' and xs:
                code.append(f'{n}[{i}].tag({v})')
                xs[i]['nums'].append(v)
            elif op == 'names' and xs:
                code.append(f'{n}[{i}].names[0] = "m{v}{PAD}"')
                xs[i]['names'][0] = f'm{v}{PAD}'
            elif op == 'fmaybe' and xs:
                if r.random() < 0.4:
                    code.append(f'{n}[{i}].maybe = null')
                    xs[i]['maybe'] = None
                else:
                    code.append(f'{n}[{i}].maybe = {v}')
                    xs[i]['maybe'] = v
            elif op == 'farr' and xs:
                j = r.choice((0, 2))
                code.append(f'{n}[{i}].arr[{j}] = {v}')
                xs[i]['arr'][j] = v
            elif op == 'bvpush' and xs and len(xs[i]['bv']) < 4:
                code.append(f'{n}[{i}].bv.push({v})')
                xs[i]['bv'].append(v)
            elif op == 'bump' and xs:
                code.append(f'{n}[{i}].bump({v})')
                xs[i]['x'] += v
            elif op == 'rename' and xs:
                code.append(f'{n}[{i}].rename({v})')
                xs[i]['s'] = f'r{v}{PAD}'
            elif op == 'total' and xs:
                code.append(f'println("total {{{n}[{i}].total()}}")')
                out.append(f'total {sc.total(xs[i])}')
            elif op == 'peek' and xs:
                code.append(f'println("peek {{peek{k}(&{n}[{i}])}}")')
                out.append(f'peek {xs[i]["x"] + 1}')
            elif op == 'edit' and xs:
                code.append(f'edit{k}(&{n}[{i}], {v})')
                xs[i]['x'] += v
                if sc.has('s'):
                    xs[i]['s'] = f'e{v}{PAD}'
                if sc.has('nums'):
                    xs[i]['nums'].append(v)
            elif op == 'tweak' and xs:
                code.append(f'tweak{k}({n}[{i}], {v})')
                xs[i]['x'] -= v
            elif op == 'get' and xs:
                m = r.choice(('get', 'get_unchecked'))
                code.append(f'println("get {{{n}.{m}({i}).x}} {{{n}[{i}].total()}}")')
                out.append(f'get {xs[i]["x"]} {sc.total(xs[i])}')
            elif op == 'reverse':
                code.append(f'{n}.reverse()')
                xs.reverse()
            elif op == 'copy':
                tmp = self.fresh('cp')
                code += [f'var {tmp} = {n}.copy()', f'{tmp}.push({t}(99))']
                cp = [sc.clone(e) for e in xs] + [sc.new(99)]
                if xs:
                    code.append(f'{tmp}[0].bump(1000)')
                    cp[0]['x'] += 1000
                    if sc.has('nums'):
                        code.append(f'{tmp}[0].nums.push(5)')
                        cp[0]['nums'].append(5)
                self.snap(sc, code, out, tmp, cp)
            elif op == 'slice':
                a = r.randint(0, len(xs))
                b = r.randint(a, len(xs))
                tmp = self.fresh('sl')
                code.append(f'var {tmp} = {n}[{a}..{b}]')
                sl = [sc.clone(e) for e in xs[a:b]]
                code.append(f'{tmp}.push({t}({v}))')
                sl.append(sc.new(v))
                self.snap(sc, code, out, tmp, sl)
            elif op == 'take':
                tmp = self.fresh('tk')
                code.append(f'val {tmp} = {n}.take()')
                self.snap(sc, code, out, tmp, xs)
                xs = []
            elif op == 'clear':
                code.append(f'{n}.clear()')
                xs = []
            elif op == 'hof':
                code.append(f'println("sum {{{n}.map(func(c: {t}) -> Int {{ return c.x }}).sum()}}")')
                out.append(f'sum {sum(e["x"] for e in xs)}')
                tmp = self.fresh('fl')
                code.append(f'val {tmp} = {n}.filter(func(c: {t}) -> Bool {{ return c.x % 2 == 0 }})')
                self.snap(sc, code, out, tmp, [sc.clone(e) for e in xs if e['x'] % 2 == 0])
                code.append(f'println("reduce {{{n}.reduce(0, func(a: Int, c: {t}) -> Int '
                            f'{{ return a + c.total() }})}}")')
                out.append(f'reduce {sum(sc.total(e) for e in xs)}')
                code.append(f'println("chain {{{n}.filter(func(c: {t}) -> Bool {{ return c.x > 0 }})'
                            f'.map(func(c: {t}) -> Int {{ return c.x * 2 }}).sum()}}")')
                out.append(f'chain {sum(e["x"] * 2 for e in xs if e["x"] > 0)}')
                code.append(f'{n}.forEach(func(c: {t}) {{ print("{{c.x}},") }})')
                code.append('println("")')
                out.append(''.join(f'{e["x"]},' for e in xs))
                tmp = self.fresh('mp')
                code.append(f'val {tmp}: {sc.lt} = {n}.map(func(c: {t}) -> {t} {{ return {t}(c.x + 1) }})')
                self.snap(sc, code, out, tmp, [sc.new(e['x'] + 1) for e in xs])
            elif op == 'loop':
                code.append(f'for q in range({n}.len()) {{ {n}[q].x += q }}')
                for q, e in enumerate(xs):
                    e['x'] += q
            elif op == 'wloop':
                code += ['var q = 0', f'while q < {n}.len() {{', f'    {n}[q].bump(q * 2)',
                         '    q += 1', '}']
                for q, e in enumerate(xs):
                    e['x'] += q * 2
                code[-5:] = ['{'] + ['    ' + l for l in code[-5:]] + ['}']
            elif op == 'forsum':
                tmp = self.fresh('acc')
                code += [f'var {tmp} = 0', f'for c in {n} {{ {tmp} += c.total() }}',
                         f'println("acc {{{tmp}}}")']
                out.append(f'acc {sum(sc.total(e) for e in xs)}')
            elif (op == 'swap' and len(xs) >= 2 and
                  not (sc.has('nums') or sc.has('names'))):   # E0328: no implicit list copy
                j = r.randrange(len(xs))
                tmp = self.fresh('tmp')
                code += [f'val {tmp} = {n}[{i}]', f'{n}[{i}] = {n}[{j}]', f'{n}[{j}] = {tmp}']
                a, b = sc.clone(xs[i]), sc.clone(xs[j])
                xs[i] = b
                xs[j] = a
            elif op == 'plain':
                code.append(f'println("plain {{plain{k}({n})}}")')
                out.append(f'plain {sum(sc.total(e) for e in xs)}')
            elif op == 'count':
                code.append(f'println("count {{count({n})}} {{{n}.len()}} {{len({n})}}")')
                out.append(f'count {len(xs)} {len(xs)} {len(xs)}')
            elif op == 'closure' and xs:
                code.append(f'println("closure {{apply(func() -> Int {{ return {n}[{i}].x + {n}.len() }})}}")')
                out.append(f'closure {xs[i]["x"] + len(xs)}')
            if r.random() < 0.35:
                self.snap(sc, code, out, n, xs)
        self.snap(sc, code, out, n, xs)
        return Probe(code, out)

    # --------------------------------------------------------------- flows
    def holder(self):
        r = self.r
        sc = r.choice(self.schemas)
        a, cnt, d, v = self.val(), r.randint(0, 6), self.val(), self.val()
        h = self.fresh('h')
        xs = [sc.new(a + i) for i in range(cnt)]
        code = [f'var {h} = Hold{sc.k}({a}, {cnt})', f'{h}.step({d})', f'{h}.add({v})']
        for i, e in enumerate(xs):
            e['x'] += d + i
        xs.append(sc.new(v))
        code.append(f'{h}.xs[{len(xs) - 1}].bump(3)')
        xs[-1]['x'] += 3
        code.append(f'edit{sc.k}(&{h}.xs[0], 2)')
        xs[0]['x'] += 2
        if sc.has('s'):
            xs[0]['s'] = f'e2{PAD}'
        if sc.has('nums'):
            xs[0]['nums'].append(2)
        code.append(f'grow{sc.k}({h}.xs, {d})')
        xs.append(sc.new(d))
        out = []
        self.snap(sc, code, out, f'{h}.xs', xs)
        code.append(f'println("{{{h}.name}}")')
        out.append(f'hold{a}{PAD}')
        return Probe(code, out)

    def returned(self):
        r = self.r
        sc = r.choice(self.schemas)
        a, cnt, v = self.val(), r.randint(0, 5), self.val()
        n = self.fresh('ret')
        xs = [sc.new(a + i) for i in range(cnt)] + [sc.new(v)]
        code = [f'var {n} = make{sc.k}({a}, {cnt})', f'grow{sc.k}({n}, {v})']
        out = []
        self.snap(sc, code, out, n, xs)
        code.append(f'println("temp {{make{sc.k}({v}, 3).len()}} {{plain{sc.k}(make{sc.k}({v}, 2))}}")')
        out.append(f'temp 3 {sc.total(sc.new(v)) + sc.total(sc.new(v + 1))}')
        return Probe(code, out)

    def containers(self):
        r = self.r
        sc = r.choice(self.schemas)
        t, lt, k = sc.t, sc.lt, sc.k
        v, w = self.val(), self.val()
        n = self.fresh('st')
        code = [f'var {n}: {lt} = [{t}({v}), {t}({w})]',
                f'var nested: [{lt}] = []',
                f'nested.push({n}.copy())',
                f'nested.push([{t}({w})])',
                'nested[0][1].bump(5)',
                f'nested[1].push({t}({v}))',
                f'println("nested {{nested[0][1].x}} {{nested[1].len()}} {{nested[1][1].x}}")',
                f'var values = Map[Int, {lt}]()',
                f'values.put(1, {n}.copy())',
                'val found = values.get_or(1, [])',
                'println("map {found[0].x} {found.len()}")',
                f'val ch = Channel[{lt}](1)',
                f'ch.send({n}.copy())',
                'val received = ch.recv()',
                'println("channel {received[1].x}")',
                f'val mu = Mutex[{lt}]({n}.copy())',
                '{', '    val guard = mu.lock()', '    val locked = guard.get()',
                '    println("mutex {locked[0].x}")', '}',
                f'val task = spawn("soa") {{ return {n}[0].x + {n}.len() }}',
                'println("spawn {await task}")',
                f'var shared: rc {lt} = rc [{t}({v}), {t}({w})]',
                'shared[1].bump(2)',
                f'shared.push({t}(1))',
                'println("rc {shared[1].x} {shared.len()}")']
        out = [f'nested {w + 5} 2 {v}', f'map {v} 2', f'channel {w}', f'mutex {v}',
               f'spawn {v + 2}', f'rc {w + 2} 3']
        return Probe(code, out)

    def panic(self):
        r = self.r
        sc = r.choice(self.schemas)
        t = sc.t
        cnt = r.randint(0, 3)
        code = [f'var ps: {sc.lt} = [' + ', '.join(f'{t}({i})' for i in range(cnt)) + ']']
        kind = r.choice(('pop', 'read', 'write', 'field', 'method', 'ptr', 'remove', 'insert',
                         'slice', 'get', 'set', 'var'))
        bad = r.choice((cnt, cnt + 3, -1))
        msg = 'index out of bounds'
        if kind == 'pop':
            code += ['ps.clear()', 'val gone = ps.pop()', 'println("{gone.x}")']
            msg = 'pop from an empty list'
        elif kind == 'read':
            code.append(f'println("{{ps[{bad}].x}}")')
        elif kind == 'write':
            code.append(f'ps[{bad}] = {t}(1)')
        elif kind == 'field':
            code.append(f'ps[{bad}].x {r.choice(("=", "+="))} 1')
        elif kind == 'method':
            code.append(r.choice((f'ps[{bad}].bump(1)', f'println("{{ps[{bad}].total()}}")')))
        elif kind == 'ptr':
            code.append(r.choice((f'println("{{peek{sc.k}(&ps[{bad}])}}")', f'edit{sc.k}(&ps[{bad}], 1)')))
        elif kind == 'var':
            code.append(f'tweak{sc.k}(ps[{bad}], 1)')
        elif kind == 'remove':
            code += [f'val gone = ps.remove({bad})', 'println("{gone.x}")']
        elif kind == 'insert':
            code.append(f'ps.insert({r.choice((cnt + 1, -1))}, {t}(1))')
        elif kind == 'slice':
            code += [f'val part = ps[0..{cnt + 1}]', 'println("{part.len()}")']
        elif kind == 'get':
            code.append(f'println("{{ps.get({bad}).x}}")')
        elif kind == 'set':
            code.append(f'ps.set({bad}, {t}(1))')
        return Probe(code, panic=msg)

    def probe_kinds(self):
        return [(6, self.sequence), (1, self.holder), (1, self.returned), (1, self.containers)]


def generate(rng):
    g = SoaGen(rng)
    g.program()
    return g
