"""Compile-time evaluation edge cases (manual 8): Float computation at
compile time (series, Newton iterations, Horner over a list literal,
division by zero giving infinities and NaN as at run time, comparisons
with NaN), String results (literals chosen by logic, picked out of a list
literal, `len` of a literal with multi-byte characters), lists (fill,
index assignment, lists of lists and of floats), Int arithmetic that
wraps, objects without an init, `read_file` of a file next to the source
(printed whole and measured), and results used as constants (loop bounds,
arithmetic). Each function runs once at compile time (a global or local
`comptime`) and once at run time; both results are printed.

Errors: a method call or a generic construction at compile time (E0231),
an Int division by zero, an index out of range or a missing file (E0232),
recursion past 256 calls (E0230).

Left out (see the report): `%` on floats at compile time (E0231 though
the manual lists `%`), comparing strings, a comptime result as the size
of a fixed array.
"""
from pf_probe import Probe, ProbeGen
from pf_lang import wrap
from pf_numedge import flit, fdiv
from pf_num import fmt_float

FUNCS = {
    'fseries': '''func fseries(n: Int, x: Float) -> Float {
    var acc = 0.0
    var k = 1.0
    for i in range(n) {
        acc = acc + x / k
        k = k + 1.0
    }
    return acc
}''',
    'nsqrt': '''func nsqrt(a: Float, iters: Int) -> Float {
    var x = a
    var i = 0
    while i < iters {
        x = (x + a / x) / 2.0
        i += 1
    }
    return x
}''',
    'horner': '''func horner(cs: [Float], x: Float) -> Float {
    var acc = 0.0
    for c in cs {
        acc = acc * x + c
    }
    return acc
}''',
    'fdiv': '''func fdiv(x: Float, y: Float) -> Float {
    return x / y
}''',
    'fcmp': '''func fcmp(a: Float, b: Float) -> Bool {
    return a < b or a == b
}''',
    'pick': '''func pick(n: Int) -> String {
    if n % 15 == 0 {
        return "fizzbuzz"
    }
    if n % 5 == 0 {
        return "büzz"
    }
    if n % 3 == 0 {
        return "fïzz"
    }
    return "日本"
}''',
    'sidx': '''func sidx(i: Int) -> String {
    val xs = ["a", "é", "日本", "", "x😀y"]
    return xs[i]
}''',
    'tri': '''func tri(n: Int) -> [Int] {
    var xs = fill(n, 0)
    var s = 0
    for i in range(n) {
        s = s + i
        xs[i] = s
    }
    return xs
}''',
    'grid': '''func grid(n: Int, v: Int) -> [[Int]] {
    var g = fill(n, fill(n, 0))
    for i in range(n) {
        g[i][i] = v + i
    }
    return g
}''',
    'flist': '''func flist(n: Int, x: Float) -> [Float] {
    var xs = fill(n, 0.0)
    var acc = x
    for i in range(n) {
        acc = acc / 2.0
        xs[i] = acc
    }
    return xs
}''',
    'wrapmul': '''func wrapmul(n: Int, seed: Int) -> Int {
    var x = seed
    for i in range(n) {
        x = x * 6364136223846793005 + 1442695040888963407
    }
    return x
}''',
    'mkpt': '''object Pt {
    val x: Int
    val y: Float
}

func mkpt(a: Int, b: Float) -> Pt {
    return Pt(a * 2, b / 4.0)
}''',
}

HELPERS = '''func showi(xs: [Int]) -> String {
    var parts: [String] = []
    for x in xs {
        parts.push("{x}")
    }
    return "[" + ",".join(parts) + "]"
}

func showf(xs: [Float]) -> String {
    var parts: [String] = []
    for x in xs {
        parts.push("{x}")
    }
    return "[" + ",".join(parts) + "]"
}'''


def flt(v):
    return f'(-{flit(-v)})' if str(v).startswith('-') else flit(v)


def fseries(n, x):
    acc, k = 0.0, 1.0
    for _ in range(n):
        acc = acc + fdiv(x, k)
        k += 1.0
    return acc


def nsqrt(a, it):
    x = a
    for _ in range(it):
        x = fdiv(x + fdiv(a, x), 2.0)
    return x


def horner(cs, x):
    acc = 0.0
    for c in cs:
        acc = acc * x + c
    return acc


def pick(n):
    if n % 15 == 0:
        return 'fizzbuzz'
    if n % 5 == 0:
        return 'büzz'
    if n % 3 == 0:
        return 'fïzz'
    return '日本'


SIDX = ['a', 'é', '日本', '', 'x😀y']


def showf(xs):
    return '[' + ','.join(fmt_float(x) for x in xs) + ']'


class CtEdgeGen(ProbeGen):
    NPROBES = (4, 9)

    def __init__(self, rng):
        super().__init__(rng)
        self.need('helpers', HELPERS)
        self.files = {}

    def rf(self):
        return self.r.choice([0.5, 1.5, 3.0, 0.1, 2.0, 10.0, 0.0, 7.25, -2.5, -0.0])

    def call(self):
        """(function key, source arguments, expected text)."""
        r = self.r
        k = r.choice(list(FUNCS))
        if k == 'fseries':
            n, x = r.randint(0, 40), self.rf()
            return k, f'{n}, {flt(x)}', fmt_float(fseries(n, x))
        if k == 'nsqrt':
            a, it = r.choice([2.0, 10.0, 0.0, 1e10, 0.25, 3.0]), r.randint(0, 12)
            return k, f'{flt(a)}, {it}', fmt_float(nsqrt(a, it))
        if k == 'horner':
            cs = [self.rf() for _ in range(r.randint(1, 5))]
            x = self.rf()
            return k, '[' + ', '.join(flt(c) for c in cs) + f'], {flt(x)}', fmt_float(horner(cs, x))
        if k == 'fdiv':
            x, y = self.rf(), r.choice([0.0, -0.0, 2.0, 0.1, 3.0])
            return k, f'{flt(x)}, {flt(y)}', fmt_float(fdiv(x, y))
        if k == 'fcmp':
            a = r.choice([self.rf(), float('nan')])
            b = self.rf()
            asrc = '(0.0 / 0.0)' if a != a else flt(a)
            return k, f'{asrc}, {flt(b)}', 'true' if (a < b or a == b) else 'false'
        if k == 'pick':
            n = r.randint(0, 60)
            return k, f'{n}', pick(n)
        if k == 'sidx':
            i = r.randrange(len(SIDX))
            return k, f'{i}', f'<{SIDX[i]}>'
        if k == 'tri':
            n = r.randint(0, 9)
            xs, s = [], 0
            for i in range(n):
                s += i
                xs.append(s)
            return k, f'{n}', '[' + ','.join(map(str, xs)) + ']'
        if k == 'grid':
            n, v = r.randint(1, 4), r.randint(0, 9)
            i = r.randrange(n)
            return k, f'{n}, {v}', f'{v + i}|{n}|{i}'
        if k == 'flist':
            n, x = r.randint(0, 6), self.rf()
            xs, acc = [], x
            for _ in range(n):
                acc = acc / 2.0
                xs.append(acc)
            return k, f'{n}, {flt(x)}', showf(xs)
        if k == 'wrapmul':
            n, seed = r.randint(0, 30), r.randint(-100, 100)
            x = seed
            for _ in range(n):
                x = wrap(x * 6364136223846793005 + 1442695040888963407)
            return k, f'{n}, {seed}' if seed >= 0 else f'{n}, 0 - {-seed}', str(x)
        a, b = r.randint(-50, 50), self.rf()
        return k, (f'{a}' if a >= 0 else f'0 - {-a}') + f', {flt(b)}', f'{a * 2} {fmt_float(b / 4.0)}'

    def show(self, k, e):
        """Text form of a result expression e of function k."""
        if k in ('tri',):
            return f'showi({e})'
        if k == 'flist':
            return f'showf({e})'
        if k == 'sidx':
            return f'"<" + {e} + ">"'
        if k == 'mkpt':
            return None
        return e

    def p_value(self):
        r = self.r
        k, args, text = self.call()
        self.need(k, FUNCS[k])
        g = self.fresh('C')
        tag = self.fresh('c')
        code = []
        if r.random() < 0.6:
            self.need('g_' + g, f'val {g} = comptime {k}({args})')
            ref = g
        else:
            code.append(f'val {g} = comptime {k}({args})')
            ref = g
        if k == 'grid':
            n, v = args.split(', ')
            i = int(text.split('|')[2])
            out = f'{tag} {text.split("|")[0]} {text.split("|")[1]} {text.split("|")[0]}'
            code.append(f'println("{tag} {{{ref}[{i}][{i}]}} {{len({ref})}} {{grid({args})[{i}][{i}]}}")')
            return Probe(code, [out])
        if k == 'mkpt':
            code.append(f'println("{tag} {{{ref}.x}} {{{ref}.y}}")')
            code.append(f'val {g}r = mkpt({args})')
            code.append(f'println("{tag} {{{g}r.x}} {{{g}r.y}}")')
            return Probe(code, [f'{tag} {text}', f'{tag} {text}'])
        a = self.show(k, ref)
        b = self.show(k, f'{k}({args})')
        code.append(f'println("{tag} {{{a}}} {{{b}}}")')
        return Probe(code, [f'{tag} {text} {text}'])

    def p_const(self):
        """A comptime result as a loop bound and in arithmetic."""
        r = self.r
        self.need('sz', '''func sz(n: Int) -> Int {
    var t = 0
    for i in range(n) {
        t = t + i % 3
    }
    return t
}''')
        n = r.randint(0, 30)
        v = sum(i % 3 for i in range(n))
        g = self.fresh('N')
        self.need('g_' + g, f'val {g} = comptime sz({n})')
        tag = self.fresh('k')
        code = [f'var {g}acc = 0', f'for i in range({g}) {{', f'    {g}acc += i', '}',
                f'println("{tag} {{{g}}} {{{g}acc}} {{{g} * 3 - 1}}")']
        return Probe(code, [f'{tag} {v} {v * (v - 1) // 2} {v * 3 - 1}'])

    def p_file(self):
        r = self.r
        name = f'data_{len(self.files) + 1}.txt'
        lines = [''.join(r.choice(['a', 'b', 'é', '日', ' ', '1', '😀', ',', '-'])
                         for _ in range(r.randint(0, 12))) for _ in range(r.randint(1, 4))]
        content = '\n'.join(lines) + ('\n' if r.random() < 0.5 else '')
        self.files[name] = content
        g = self.fresh('T')
        self.need('g_' + g, f'val {g} = comptime read_file("{name}")')
        self.need('g_' + g + 'n', f'val {g}n = comptime len(read_file("{name}"))')
        tag = self.fresh('f')
        code = [f'println("{tag} {{{g}n}} {{{g}.len()}} {{{g}.byte_len()}}")', f'print({g})', 'println("|")']
        out = [f'{tag} {len(content)} {len(content)} {len(content.encode())}'] + (content + '|').split('\n')
        return Probe(code, out)

    # ------------------------------------------------------------ errors
    def p_bad(self):
        r = self.r
        k = r.choice(['map', 'method', 'concat', 'div0', 'oob', 'nofile', 'deep'])
        n = self.fresh('bad')
        if k == 'map':
            self.need('badmap', '''func badmap() -> Int {
    var m = Map[String, Int]()
    m.put("a", 1)
    return m.len()
}''')
            return Probe([f'val {n} = comptime badmap()', f'println("{{{n}}}")'], errors={'E0231'})
        if k == 'method':
            self.need('badmeth', '''func badmeth() -> Int {
    var xs = [1, 2]
    xs.push(3)
    return len(xs)
}''')
            return Probe([f'val {n} = comptime badmeth()', f'println("{{{n}}}")'], errors={'E0231'})
        if k == 'concat':
            self.need('badcat', '''func badcat() -> String {
    return "a" + "b"
}''')
            return Probe([f'val {n} = comptime badcat()', f'println({n})'], errors={'E0231'})
        if k == 'div0':
            self.need('baddiv', '''func baddiv(a: Int, b: Int) -> Int {
    return a / b
}''')
            return Probe([f'val {n} = comptime baddiv(5, 0)', f'println("{{{n}}}")'], errors={'E0232'})
        if k == 'oob':
            self.need('tri', FUNCS['tri'])
            return Probe([f'val {n} = comptime tri(3)[5]', f'println("{{{n}}}")'], errors={'E0232'})
        if k == 'nofile':
            return Probe([f'val {n} = comptime read_file("data_missing.txt")', f'println({n})'],
                         errors={'E0232'})
        self.need('deep', '''func deep(n: Int) -> Int {
    if n == 0 {
        return 0
    }
    return 1 + deep(n - 1)
}''')
        return Probe([f'val {n} = comptime deep(300)', f'println("{{{n}}}")'], errors={'E0230'})

    def probe_kinds(self):
        return [(8, self.p_value), (2, self.p_const), (2, self.p_file)]

    def error_kinds(self):
        return [self.p_bad]

    error_rate = 0.15


def generate(rng):
    g = CtEdgeGen(rng)
    g.program()
    return g
