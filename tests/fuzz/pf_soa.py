"""Explicit object list layouts, checked against a model and the AoS build.

Each seed produces a sequence of mutations interleaved with snapshots, plus
higher-order operations and storage flowing through parameters and returns.
The runner also builds the same source with layout annotations removed.
"""
from pf_probe import Probe, ProbeGen


HELPERS = '''object Inner { val n: Int }
object Cell {
    var x: Int
    val y: Float
    val live: Bool
    val tag: String
    val inner: Inner
    val nums: [Int]
    val maybe: Int?
    init(x: Int) {
        self.x = x
        self.y = Float(x) / 2.0
        self.live = x % 2 == 0
        self.tag = "v{x}"
        self.inner = Inner(x * 3)
        self.nums = [x, x + 1]
        self.maybe = x
    }
}
func dump(xs: [Cell, layout: soa]) {
    println("len {xs.len()}")
    for c in xs {
        println("{c.x} {c.y} {c.live} {c.tag} {c.inner.n} {c.nums[1]}")
    }
}
func grow(var xs: [Cell, layout: soa], n: Int) { xs.push(Cell(n)) }
func make(n: Int) -> [Cell, layout: soa] { return [Cell(n), Cell(n + 1)] }
func apply(f: func() -> Int) -> Int { return f() }
'''


def row(c):
    x, original = c
    y = original / 2
    ys = str(int(y)) if y == int(y) else str(y)
    return f'{x} {ys} {str(original % 2 == 0).lower()} v{original} {original * 3} {original + 1}'


class SoaGen(ProbeGen):
    NPROBES = (2, 4)
    error_rate = 0
    panic_rate = 0.08

    def __init__(self, rng):
        super().__init__(rng)
        self.need('helpers', HELPERS)

    def snapshot(self, code, out, name, xs):
        code.append(f'dump({name})')
        out += [f'len {len(xs)}'] + [row(c) for c in xs]

    def sequence(self):
        r = self.r
        n = self.fresh('ps')
        values = [r.randint(-10, 10) for _ in range(r.randint(0, 12))]
        xs = [(v, v) for v in values]
        code = [f'var {n}: [Cell, layout: soa] = [' + ', '.join(f'Cell({v})' for v in values) + ']']
        out = []
        self.snapshot(code, out, n, xs)
        for _ in range(r.randint(10, 35)):
            k = r.choice(('push', 'insert', 'remove', 'pop', 'set', 'field', 'reverse',
                          'copy', 'slice', 'take', 'clear', 'hof', 'grow'))
            v = r.randint(-10, 10)
            if k in ('push', 'grow'):
                code.append(f'{n}.push(Cell({v}))' if k == 'push' else f'grow({n}, {v})')
                xs.append((v, v))
            elif k == 'insert':
                i = r.randint(0, len(xs))
                code.append(f'{n}.insert({i}, Cell({v}))')
                xs.insert(i, (v, v))
            elif k in ('pop', 'remove') and xs:
                i = len(xs) - 1 if k == 'pop' else r.randrange(len(xs))
                tmp = self.fresh('removed')
                expr = f'{n}.pop()' if k == 'pop' else f'{n}.remove({i})'
                code += [f'val {tmp} = {expr}', f'println("removed {{{tmp}.x}} {{{tmp}.tag}}")']
                c = xs.pop(i)
                out.append(f'removed {c[0]} v{c[1]}')
            elif k in ('set', 'field') and xs:
                i = r.randrange(len(xs))
                if k == 'field':
                    code.append(f'{n}[{i}].x = {v}')
                    xs[i] = (v, xs[i][1])
                else:
                    code.append(f'{n}[{i}] = Cell({v})')
                    xs[i] = (v, v)
            elif k == 'reverse':
                code.append(f'{n}.reverse()')
                xs.reverse()
            elif k == 'copy':
                tmp = self.fresh('cp')
                code += [f'var {tmp} = {n}.copy()', f'{tmp}.push(Cell(99))']
                self.snapshot(code, out, tmp, xs + [(99, 99)])
            elif k == 'slice':
                a = r.randint(0, len(xs))
                b = r.randint(a, len(xs))
                tmp = self.fresh('sl')
                code.append(f'val {tmp} = {n}[{a}..{b}]')
                self.snapshot(code, out, tmp, xs[a:b])
            elif k == 'take':
                tmp = self.fresh('tk')
                code.append(f'val {tmp} = {n}.take()')
                self.snapshot(code, out, tmp, xs)
                xs = []
            elif k == 'clear':
                code.append(f'{n}.clear()')
                xs = []
            elif k == 'hof':
                code.append(f'println("sum {{{n}.map(func(c: Cell) -> Int {{ return c.x }}).sum()}}")')
                out.append(f'sum {sum(c[0] for c in xs)}')
                tmp = self.fresh('filtered')
                code.append(f'val {tmp} = {n}.filter(func(c: Cell) -> Bool {{ return c.live }})')
                self.snapshot(code, out, tmp, [c for c in xs if c[1] % 2 == 0])
                code.append(f'println("reduce {{{n}.reduce(0, func(a: Int, c: Cell) -> Int {{ return a + c.x }})}}")')
                out.append(f'reduce {sum(c[0] for c in xs)}')
            self.snapshot(code, out, n, xs)
        return Probe(code, out)

    def flow(self):
        v = self.r.randint(-10, 10)
        name = self.fresh('returned')
        code = [f'var {name} = make({v})', f'grow({name}, {v + 2})']
        out = []
        self.snapshot(code, out, name, [(v, v), (v + 1, v + 1), (v + 2, v + 2)])
        code.append(f'println("capture {{apply(func() -> Int {{ return {name}[0].x + {name}.len() }})}}")')
        out.append(f'capture {v + 3}')
        return Probe(code, out)

    def containers(self):
        v = self.r.randint(-20, 20)
        n = self.fresh('storage')
        code = [f'var {n}: [Cell, layout: soa] = [Cell({v})]',
                f'var nested: [[Cell, layout: soa]] = []',
                f'nested.push({n}.copy())',
                'println("nested {nested[0][0].x}")',
                'var values = Map[Int, [Cell, layout: soa]]()',
                f'values.put(1, {n}.copy())',
                'val found = values.get_or(1, [])',
                'println("map {found[0].x}")',
                'val ch = Channel[[Cell, layout: soa]](1)',
                f'ch.send({n}.copy())',
                'val received = ch.recv()',
                'println("channel {received[0].x}")',
                f'val mu = Mutex[[Cell, layout: soa]]({n}.copy())',
                '{', '    val guard = mu.lock()', '    val locked = guard.get()',
                '    println("mutex {locked[0].x}")', '}',
                f'val task = spawn("soa") {{ return {n}[0].x + {n}.len() }}',
                'println("spawn {await task}")']
        return Probe(code, [f'nested {v}', f'map {v}', f'channel {v}',
                            f'mutex {v}', f'spawn {v + 1}'])

    def panic(self):
        code = ['var ps: [Cell, layout: soa] = []', 'val p = ps.pop()', 'println("{p.x}")']
        return Probe(code, panic='pop from an empty list')

    def probe_kinds(self):
        return [(5, self.sequence), (1, self.flow), (1, self.containers)]

    def panic_kinds(self):
        return [self.panic]


def generate(rng):
    g = SoaGen(rng)
    g.program()
    return g
