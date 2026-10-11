"""Arena programs beyond pf_arena (manual 6.6, 5.5, 2.8, 3.7): arena
values used through interfaces (passed to interface parameters, stored in
lists of an interface type, kept in interface bindings), generic objects
allocated in arenas and generic functions that take an arena, arenas that
run out of memory (exactly full, one byte short, from one thread and from
a parallel for), closures stored in lists and fields that allocate from a
captured arena when they are called later (with reset in between),
threadsafe arenas allocated from by many threads at once, and save /
restore around those.

used() is predictable here because every object allocated in an arena
has only Int fields: an allocation of an object with k fields takes
align16(16 + 8k) bytes (a 16 byte header, the payload rounded up to 16;
src/runtime/iron_arena_rt.c), so used() after n allocations is the sum,
threadsafe or not. An allocation that does not fit stops the program with
`arena "name" out of memory`. Objects with String fields only appear with
`allow_drop_skip: true`; their strings leak by definition (W0605), so such
a program allows leaks.
"""
from pf_seg import Seg, SegProgram, iron_lit

HELPERS = '''interface Shape {
    readonly func area() -> Int
    readonly func kind() -> String {
        return "shape"
    }
}

object Sq impl Shape {
    val s: Int
    readonly func area() -> Int {
        return self.s * self.s
    }
}

object Rect impl Shape {
    val w: Int
    val h: Int
    readonly func area() -> Int {
        return self.w * self.h
    }
    readonly func kind() -> String {
        return "rect"
    }
}

object Box3 impl Shape {
    var a: Int
    var b: Int
    var c: Int
    init(a: Int, b: Int, c: Int) {
        self.a = a
        self.b = b
        self.c = c
    }
    readonly func area() -> Int {
        return self.a + self.b + self.c
    }
}

object Named impl Shape {
    val name: String
    readonly func area() -> Int {
        return self.name.len()
    }
}

object Cell[T] {
    var v: T
    init(v: T) {
        self.v = v
    }
}

object Pair[A, B] {
    val first: A
    val second: B
}

object Maker {
    val f: func(Int) -> Int
}

func area_of(s: Shape) -> Int {
    return s.area()
}

func total_area(xs: [Shape]) -> Int {
    var t = 0
    for x in xs {
        t += x.area()
    }
    return t
}

func make_cells[T](a: Arena, v: T, n: Int) -> Int {
    var c = 0
    for i in range(n) {
        val p = heap(in: a) Cell[T](v)
        c += 1
    }
    return c
}

func sq_in(a: Arena, n: Int) -> Int {
    val p = heap(in: a) Sq(n)
    return p.area()
}
'''


def need(k):
    """Arena bytes for one object with k Int fields."""
    n = 16 + 8 * k
    return (n + 15) // 16 * 16


SHAPES = {   # name: (fields, area of args)
    'Sq': (1, lambda a: a[0] * a[0]),
    'Rect': (2, lambda a: a[0] * a[1]),
    'Box3': (3, lambda a: a[0] + a[1] + a[2]),
}


class ArenaXGen(SegProgram):
    HELPERS = HELPERS
    NSEGS = (2, 5)

    def add_segment(self):
        r = self.r
        pick = r.choices(['iface', 'generic', 'oom', 'lambda', 'contend', 'saverest', 'named'],
                         [4, 3, 1.2, 3, 3, 2, 1])[0]
        self.segs.append(getattr(self, 'seg_' + pick)())

    def shape(self):
        r = self.r
        nm = r.choice(list(SHAPES))
        k, area = SHAPES[nm]
        args = [r.randint(0, 9) for _ in range(k)]
        return nm, args, area(args), need(k)

    def alloc(self, a, nm, args, style=None):
        style = style or self.r.choice(['in', 'heapin'])
        txt = f'{nm}({", ".join(map(str, args))})'
        return f'heap(in: {a}) {txt}' if style == 'heapin' else f'heap {txt}'

    # ------------------------------------------------------------ iface
    def seg_iface(self):
        r = self.r
        a = self.fresh('a')
        lines = [f'val {a} = Arena({r.choice([4096, 65536])})']
        n = r.randint(1, 6)
        used = 0
        total = 0
        xs = self.fresh('xs')
        lines.append(f'var {xs}: [Shape] = []')
        out = []
        in_block = r.random() < 0.5
        body = []
        for _ in range(n):
            nm, args, area, nd = self.shape()
            p = self.fresh('p')
            style = 'in' if in_block and r.random() < 0.7 else 'heapin'
            body.append(f'val {p} = {self.alloc(a, nm, args, style)}')
            how = r.choice(['param', 'push', 'bind'])
            if how == 'param':
                body.append(f'println("area {{area_of({p})}}")')
                out.append(f'area {area}')
            elif how == 'bind':
                v = self.fresh('v')
                body += [f'val {v}: Shape = {p}', f'println("{{{v}.kind()}} {{{v}.area()}}")']
                out.append(f'{"rect" if nm == "Rect" else "shape"} {area}')
            body.append(f'{xs}.push({p})')
            total += area
            used += nd
        if in_block:
            lines += [f'in {a} {{'] + ['    ' + l for l in body] + ['}']
        else:
            lines += body
        lines += [f'println("total {{total_area({xs})}} {{{xs}.len()}} {{{a}.used()}}")']
        out.append(f'total {total} {n} {used}')
        return Seg(lines, out)

    # ---------------------------------------------------------- generic
    def seg_generic(self):
        r = self.r
        a = self.fresh('a')
        lines = [f'val {a} = Arena(65536)']
        out = []
        used = 0
        c = self.fresh('c')
        v0 = r.randint(-9, 9)
        lines.append(f'var {c} = heap(in: {a}) Cell[Int]({v0})')
        used += need(1)
        v = v0
        for _ in range(r.randint(0, 4)):
            d = r.randint(1, 9)
            lines.append(f'{c}.v = {c}.v + {d}')
            v += d
        lines.append(f'println("cell {{{c}.v}}")')
        out.append(f'cell {v}')
        n = r.randint(0, 7)
        cnt = self.fresh('n')
        lines.append(f'val {cnt} = make_cells[Int]({a}, {r.randint(0, 9)}, {n})')
        used += n * need(1)
        p = self.fresh('pr')
        x, y = r.randint(0, 99), r.randint(0, 99)
        lines.append(f'val {p} = heap(in: {a}) Pair[Int, Int]({x}, {y})')
        used += need(2)
        lines.append(f'println("{{{cnt}}} {{{p}.first + {p}.second}} {{{a}.used()}}")')
        out.append(f'{n} {x + y} {used}')
        if r.random() < 0.4:
            # a generic cell of an object type, read through the interface
            nm, args, area, nd = self.shape()
            g = self.fresh('g')
            lines += [f'val {g} = heap(in: {a}) Cell[{nm}]({nm}({", ".join(map(str, args))}))',
                      f'println("inner {{area_of({g}.v)}}")']
            out.append(f'inner {area}')
        return Seg(lines, out)

    # -------------------------------------------------------------- oom
    def seg_oom(self):
        r = self.r
        nm, args, area, nd = self.shape()
        fit = r.randint(0, 5)
        exact = r.random() < 0.5
        cap = fit * nd + (0 if exact else r.randint(1, nd - 1))
        a = self.fresh('small')
        lines = [f'val {a} = Arena({cap})', f'println("cap {{{a}.capacity()}}")']
        out = [f'cap {cap}']
        i = self.fresh('i')
        p = self.fresh('p')
        if r.random() < 0.3:
            # exactly as many as fit: no panic
            lines += [f'for {i} in range({fit}) {{',
                      f'    val {p} = heap(in: {a}) {nm}({", ".join(map(str, args))})',
                      f'    println("{{{p}.area()}} {{{a}.used()}}")', '}']
            out += [f'{area} {(j + 1) * nd}' for j in range(fit)]
            return Seg(lines, out)
        if r.random() < 0.3:
            # many threads run it out of memory together
            ta = self.fresh('ta')
            m = self.fresh('m')
            lines += [f'val {ta} = Arena.threadsafe({cap})', f'val {m} = Mutex(0)',
                      f'for {i} in range({fit + 8}) parallel {{',
                      f'    val {p} = heap(in: {ta}) {nm}({", ".join(map(str, args))})',
                      f'    val {self.fresh("g")} = {m}.lock()', '}', 'println("not reached")']
            return Seg(lines, out, panic='out of memory')
        lines += [f'for {i} in range({fit + 3}) {{',
                  f'    val {p} = heap(in: {a}) {nm}({", ".join(map(str, args))})',
                  f'    println("{{{p}.area()}} {{{a}.used()}}")', '}', 'println("not reached")']
        out += [f'{area} {(j + 1) * nd}' for j in range(fit)]
        return Seg(lines, out, panic='out of memory')

    # ----------------------------------------------------------- lambda
    def seg_lambda(self):
        r = self.r
        a = self.fresh('a')
        lines = [f'val {a} = Arena(65536)']
        fs = self.fresh('fs')
        lines.append(f'var {fs}: [func(Int) -> Int] = []')
        lams = []
        for _ in range(r.randint(1, 3)):
            nm = r.choice(list(SHAPES))
            k, area = SHAPES[nm]
            n = self.fresh('n')
            mul = [r.randint(1, 4) for _ in range(k)]
            args = ', '.join(f'{n} * {m}' for m in mul)
            q = self.fresh('q')
            via = r.random() < 0.3
            if via:
                body = [f'return sq_in({a}, {n})']
                fn = (lambda x: x * x, need(1))
            else:
                body = [f'val {q} = heap(in: {a}) {nm}({args})', f'return {q}.area()']
                fn = ((lambda x, mul=mul, area=area: area([x * m for m in mul])), need(k))
            lam = [f'func({n}: Int) -> Int {{'] + ['    ' + l for l in body] + ['}']
            lams.append((lam, fn))
        holder = None
        for lam, fn in lams:
            lines.append(f'{fs}.push({lam[0]}')
            lines += ['    ' + l for l in lam[1:-1]]
            lines.append('})')
        if r.random() < 0.4:
            holder = self.fresh('mk')
            lam, fn = lams[0]
            lines.append(f'val {holder} = Maker({lam[0]}')
            lines += ['    ' + l for l in lam[1:-1]]
            lines.append('})')
            lams.append((lam, fn))
        out = []
        used = 0
        f = self.fresh('f')
        for rnd in range(r.randint(1, 3)):
            x = r.randint(0, 6)
            if rnd > 0 and r.random() < 0.4:
                lines.append(f'{a}.reset()')
                used = 0
            lines += [f'for {f} in {fs} {{', f'    println("call {{{f}({x})}}")', '}']
            for lam, (fn, nd) in lams[:len(lams) - (1 if holder else 0)]:
                out.append(f'call {fn(x)}')
                used += nd
            if holder:
                lam, (fn, nd) = lams[-1]
                lines.append(f'println("held {{{holder}.f({x})}}")')
                out.append(f'held {fn(x)}')
                used += nd
            lines.append(f'println("used {{{a}.used()}}")')
            out.append(f'used {used}')
        return Seg(lines, out)

    # ---------------------------------------------------------- contend
    def seg_contend(self):
        r = self.r
        ta = self.fresh('ta')
        m = self.fresh('m')
        nm, args, area, nd = self.shape()
        lines = [f'val {ta} = Arena.threadsafe({1 << 20})', f'val {m} = Mutex(0)']
        n = r.choice([1, 4, 16, 64])
        per = r.randint(1, 4)
        i = self.fresh('i')
        j = self.fresh('j')
        p = self.fresh('p')
        g = self.fresh('g')
        lines += [f'for {i} in range({n}) parallel {{', f'    for {j} in range({per}) {{',
                  f'        val {p} = heap(in: {ta}) {nm}({", ".join(map(str, args))})',
                  f'        val {g} = {m}.lock()', f'        {g}.set({g}.get() + {p}.area() + {i})',
                  '    }', '}']
        total = n * per * area + per * sum(range(n))
        count = n * per
        hs = []
        for t in range(r.randint(0, 3)):
            h = self.fresh('h')
            k = r.randint(1, 5)
            q = self.fresh('q')
            lines += [f'val {h} = spawn("alloc{t}") {{', f'    var {self.fresh("s")} = 0',
                      f'    for {j} in range({k}) {{',
                      f'        val {q} = heap(in: {ta}) {nm}({", ".join(map(str, args))})',
                      f'        val {g} = {m}.lock()', f'        {g}.set({g}.get() + {q}.area())',
                      '    }', f'    return {k}', '}']
            hs.append(h)
            total += k * area
            count += k
        for h in hs:
            lines.append(f'val {self.fresh("z")} = await {h}')
        lines += ['{', f'    val {g} = {m}.lock()', f'    println("sum {{{g}.get()}} {{{ta}.used()}}")', '}']
        return Seg(lines, [f'sum {total} {count * nd}'])

    # --------------------------------------------------------- saverest
    def seg_saverest(self):
        r = self.r
        a = self.fresh('a')
        thr = r.random() < 0.3
        lines = [f'val {a} = {"Arena.threadsafe" if thr else "Arena"}(65536)']
        out = []
        keep = []
        used = 0
        for _ in range(r.randint(0, 3)):
            nm, args, area, nd = self.shape()
            p = self.fresh('p')
            lines.append(f'val {p} = heap(in: {a}) {nm}({", ".join(map(str, args))})')
            keep.append((p, area))
            used += nd
        mk = self.fresh('mk')
        lines.append(f'val {mk} = {a}.save()')
        mark = used
        for _ in range(r.randint(0, 4)):
            nm, args, area, nd = self.shape()
            lines.append(f'val {self.fresh("t")} = heap(in: {a}) {nm}({", ".join(map(str, args))})')
            used += nd
        lines.append(f'println("before {{{a}.used()}}")')
        out.append(f'before {used}')
        lines.append(f'{a}.restore({mk})')
        lines.append(f'println("after {{{a}.used()}}")')
        out.append(f'after {mark}')
        s = ' + '.join(f'area_of({p})' for p, _ in keep) or '0'
        lines.append(f'println("kept {{{s}}}")')
        out.append(f'kept {sum(x for _, x in keep)}')
        nm, args, area, nd = self.shape()
        p = self.fresh('p')
        lines += [f'val {p} = heap(in: {a}) {nm}({", ".join(map(str, args))})',
                  f'println("again {{{p}.area()}} {{{a}.used()}}")']
        out.append(f'again {area} {mark + nd}')
        return Seg(lines, out)

    # ------------------------------------------------------------ named
    def seg_named(self):
        """Objects owning a String in an arena (allow_drop_skip): they
        leak by definition, the values are still right."""
        r = self.r
        a = self.fresh('a')
        lines = [f'val {a} = Arena(65536)']
        out = []
        xs = self.fresh('xs')
        lines.append(f'var {xs}: [Shape] = []')
        tot = 0
        for _ in range(r.randint(1, 4)):
            s = r.choice(['n', 'a name long enough to live on the heap, not inline'])
            p = self.fresh('p')
            lines += [f'val {p} = heap(in: {a}, allow_drop_skip: true) Named({iron_lit(s)})',
                      f'{xs}.push({p})']
            tot += len(s)
        lines.append(f'println("named {{total_area({xs})}}")')
        out.append(f'named {tot}')
        return Seg(lines, out, leaks_ok=True)


def generate(rng):
    g = ArenaXGen(rng)
    g.program()
    return g
