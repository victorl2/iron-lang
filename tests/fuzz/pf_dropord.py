"""Destruction order and reference cycles (manual 3.7, 5.5, 6.1, 6.3, 6.7):
objects nested in objects (drop block first, then the fields in reverse
declaration order), interface values in bindings, fields and lists
(a list drops its elements in index order; assigning an interface binding
drops the object it held), closures capturing an object by value (the
copy in the environment is dropped with the last closure) or a `var`
(one cell shared by the frame and the closures), blocks dropping their
bindings in reverse declaration order, `rc` handles and `weak rc`
upgrades, parents owning children through `[rc N]` with weak back
pointers (all dropped), and reference cycles: two `rc` values holding
each other, an object holding a closure that captures the object's own
handle, two closures calling each other through two `var`s (never
dropped: the manual says such cycles are not reclaimed, and the leak
checkers are off for them), and a recursive lambda through one `var`
(reclaimed).
"""
from pf_probe import Probe, ProbeGen

DECLS = '''object D {
    val n: String
    drop {
        println("drop {self.n}")
    }
}

object Two {
    val a: D
    val b: D
    drop {
        println("drop two {self.a.n}{self.b.n}")
    }
}

interface Sh {
    readonly func id() -> String
}

object Ci impl Sh {
    val n: String
    readonly func id() -> String {
        return "ci{self.n}"
    }
    drop {
        println("drop ci{self.n}")
    }
}

object Sq impl Sh {
    val n: String
    val inner: D
    readonly func id() -> String {
        return "sq{self.n}"
    }
    drop {
        println("drop sq{self.n}")
    }
}

object Holder {
    val s: Sh
    val d: D
    drop {
        println("drop holder")
    }
}

object N {
    val name: String
    var kids: [rc N]
    var up: weak rc N
    init(name: String) {
        self.name = name
        self.kids = []
        self.up = weak rc null
    }
    drop {
        println("drop node {self.name}")
    }
}

object Cb {
    val name: String
    var f: func() -> Int
    init(name: String) {
        self.name = name
        self.f = func() -> Int { return 0 }
    }
    drop {
        println("drop cb {self.name}")
    }
}

func id_of(s: Sh) -> String {
    return s.id()
}'''


class DropGen(ProbeGen):
    NPROBES = (3, 8)

    def __init__(self, rng):
        super().__init__(rng)
        self.need('decls', DECLS)

    def t(self):
        return self.fresh('x')

    def sh(self, drops_tail):
        """An interface value: (source, id text, drop events)."""
        n = self.t()
        if self.r.random() < 0.5:
            return f'Ci("{n}")', f'ci{n}', [f'drop ci{n}']
        i = self.t()
        return f'Sq("{n}", D("{i}"))', f'sq{n}', [f'drop sq{n}', f'drop {i}']

    def p_block(self):
        """Bindings of every kind in one block: dropped in reverse order."""
        r = self.r
        code, out, drops = [], [], []
        for _ in range(r.randint(1, 5)):
            k = r.choice(['d', 'two', 'sh', 'list', 'holder', 'closure', 'varcell', 'reassign'])
            b = self.fresh('b')
            if k == 'd':
                n = self.t()
                code.append(f'val {b} = D("{n}")')
                drops.append([f'drop {n}'])
            elif k == 'two':
                a, c = self.t(), self.t()
                code.append(f'val {b} = Two(D("{a}"), D("{c}"))')
                drops.append([f'drop two {a}{c}', f'drop {c}', f'drop {a}'])
            elif k == 'sh':
                src, idt, ev = self.sh(drops)
                code += [f'val {b}: Sh = {src}', f'println("{{{b}.id()}} {{id_of({b})}}")']
                out.append(f'{idt} {idt}')
                drops.append(ev)
            elif k == 'list':
                # one implementor per list: an interface list destroys its
                # elements grouped by concrete type, and the manual does not
                # say in which order (see the report)
                items = [self.sh(drops) for _ in range(r.randint(0, 4))]
                if items:
                    kind = items[0][1][:2]
                    items = [x for x in items if x[1][:2] == kind]
                code.append(f'val {b}: [Sh] = [' + ', '.join(s for s, _, _ in items) + ']')
                code += [f'for s in {b} {{', '    print("{s.id()},")', '}', 'println("")']
                out.append(''.join(f'{i},' for _, i, _ in items))
                ev = []
                for _, _, e in items:
                    ev += e
                drops.append(ev)
            elif k == 'holder':
                src, idt, ev = self.sh(drops)
                n = self.t()
                code += [f'val {b} = Holder({src}, D("{n}"))', f'println("{{{b}.s.id()}}")']
                out.append(idt)
                drops.append(['drop holder', f'drop {n}'] + ev)
            elif k == 'closure':
                n = self.t()
                code += [f'val {b} = D("{n}")', f'val {b}f = func() -> String {{ return {b}.n }}',
                         f'println("{{{b}f()}}")']
                out.append(n)
                # the closure's copy goes first (declared last), then the binding
                drops.append([f'drop {n}'])
                drops.append([f'drop {n}'])
            elif k == 'varcell':
                n, m = self.t(), self.t()
                code += [f'var {b} = D("{n}")', f'val {b}f = func() -> String {{ return {b}.n }}',
                         f'{b} = D("{m}")', f'println("{{{b}f()}}")']
                out += [f'drop {n}', m]
                # one cell shared by the frame and the closure: dropped once,
                # when the last of the two goes (the binding, declared first)
                drops.append([f'drop {m}'])
                drops.append([])
            else:
                src1, i1, ev1 = self.sh(drops)
                src2, i2, ev2 = self.sh(drops)
                code += [f'var {b}: Sh = {src1}', f'{b} = {src2}', f'println("{{{b}.id()}}")']
                out += ev1 + [i2]
                drops.append(ev2)
        for d in reversed(drops):
            out += d
        return Probe(code, out)

    def p_rc(self):
        r = self.r
        code, out = [], []
        n = self.t()
        a, w = self.fresh('r'), self.fresh('w')
        tag = self.fresh('rc')
        code += [f'var {w}: weak rc D = weak rc null', '{', f'    val {a} = rc D("{n}")',
                 f'    {w} = {a}.downgrade()']
        extra = r.randint(0, 3)
        for i in range(extra):
            code.append(f'    val {a}c{i} = {a}')
        code += [f'    println("{tag} {{{w}.upgrade() != null}}")', '}',
                 f'println("{tag} {{{w}.upgrade() == null}}")']
        out += [f'{tag} true', f'drop {n}', f'{tag} true']
        return Probe(code, out)

    def p_tree(self):
        """A parent owning children through [rc N], children pointing back
        with weak rc: everything is dropped when the parent goes."""
        r = self.r
        code, out = [], []
        p = self.fresh('p')
        pn = self.t()
        code.append(f'val {p} = rc N("{pn}")')
        kids = []
        for _ in range(r.randint(0, 3)):
            k = self.t()
            kb = self.fresh('k')
            code += [f'val {kb} = rc N("{k}")', f'{kb}.up = {p}.downgrade()', f'{p}.kids.push({kb})']
            kids.append((kb, k))
        tag = self.fresh('t')
        if kids:
            kb, k = r.choice(kids)
            code += [f'val {tag}u = {kb}.up.upgrade()', f'if {tag}u != null {{',
                     f'    println("{tag} {{{tag}u.name}} {{{tag}u.kids.len()}}")', '}']
            out.append(f'{tag} {pn} {len(kids)}')
        # block exit, reverse order: the kid bindings and the upgraded
        # handle go first (the list still holds the kids), then the parent:
        # its drop block, then its kids in index order
        out.append(f'drop node {pn}')
        out += [f'drop node {k}' for _, k in kids]
        return Probe(code, out)

    def p_cycle(self):
        r = self.r
        k = r.choice(['rc2', 'cbself', 'cbweak', 'mutual', 'rec'])
        tag = self.fresh('cy')
        if k == 'rc2':
            a, b = self.fresh('a'), self.fresh('b')
            code = [f'val {a} = rc N("{a}")', f'val {b} = rc N("{b}")', f'{a}.kids.push({b})',
                    f'{b}.kids.push({a})', f'println("{tag} {{{a}.kids.len()}}")']
            return Probe(code, [f'{tag} 1'], leaks_ok=True)
        if k == 'cbself':
            c = self.fresh('c')
            code = [f'val {c} = rc Cb("{c}")', f'val {c}h = {c}',
                    f'{c}.f = func() -> Int {{ return {c}h.name.len() }}',
                    f'println("{tag} {{{c}.f()}}")']
            return Probe(code, [f'{tag} {len(c)}'], leaks_ok=True)
        if k == 'cbweak':
            c = self.fresh('c')
            code = [f'val {c} = rc Cb("{c}")', f'val {c}w = {c}.downgrade()',
                    f'{c}.f = func() -> Int {{',
                    f'    val s = {c}w.upgrade()', '    if s != null {', '        return s.name.len()', '    }',
                    '    return -1', '}', f'println("{tag} {{{c}.f()}}")']
            return Probe(code, [f'{tag} {len(c)}', f'drop cb {c}'])
        if k == 'mutual':
            code = [f'var {tag}ev: func(Int) -> Bool = func(n: Int) -> Bool {{ return true }}',
                    f'var {tag}od: func(Int) -> Bool = func(n: Int) -> Bool {{ return false }}',
                    f'{tag}ev = func(n: Int) -> Bool {{', '    if n == 0 {', '        return true', '    }',
                    f'    return {tag}od(n - 1)', '}',
                    f'{tag}od = func(n: Int) -> Bool {{', '    if n == 0 {', '        return false', '    }',
                    f'    return {tag}ev(n - 1)', '}']
            n = r.randint(0, 20)
            code.append(f'println("{tag} {{{tag}ev({n})}}")')
            return Probe(code, [f'{tag} {"true" if n % 2 == 0 else "false"}'], leaks_ok=True)
        n = r.randint(0, 15)
        d = self.t()
        code = [f'val {tag}d = D("{d}")',
                f'var {tag}f: func(Int) -> Int = func(n: Int) -> Int {{ return 0 }}',
                f'{tag}f = func(n: Int) -> Int {{', '    if n <= 1 {', f'        return {tag}d.n.len()',
                '    }', f'    return n * {tag}f(n - 1)', '}', f'println("{tag} {{{tag}f({n})}}")']
        v = len(d)
        for i in range(2, n + 1):
            v *= i
        # the lambda's copy of the D goes with the reclaimed pair, then the
        # binding
        return Probe(code, [f'{tag} {v}', f'drop {d}', f'drop {d}'])

    def probe_kinds(self):
        return [(6, self.p_block), (2, self.p_rc), (2, self.p_tree), (2, self.p_cycle)]


def generate(rng):
    g = DropGen(rng)
    g.program()
    return g
