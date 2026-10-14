"""Concurrency programs beyond pf_conc (manual 7, 9.9, 9.10): Mutex, RWLock
and Channel of String (short and heap allocated), objects with String
fields, enums with and without payloads, and maps; RWLocks written from
many threads while others read; Map and Set shared between threads as
`rc` behind a Mutex; values set from bindings and read into bindings that
outlive the guard; channels of capacity 0 to 4 with items left queued when
the channel goes away; spawned results of every element kind; deadlock free
patterns (a ring of threads passing a token through channels, two locks
always taken in the same order); channels and maps of closures (closures
sent between tasks and run by the receiver, a Map[String, func(Int) -> Int]
filled from threads behind a Mutex, higher-order closures, #506); and
panics inside spawned tasks (awaited, in a parallel for body, fire and
forget while main blocks).

Every update is commutative (a sum, an append whose length is counted,
an index added modulo 3), so the result does not depend on the schedule.
A thread never prints. A panic in a thread stops the whole program with
the panic message (manual 7.1 does not say what await does with it; the
runtime aborts, which is what the generator expects), so a panicking
segment is the program's last and prints nothing after the spawn.

Channel[T](0) behaves as a channel of capacity 1 (the runtime clamps the
capacity; the manual does not define 0), so at most max(cap, 1) items are
left queued.
"""
from pf_seg import Seg, SegProgram, iron_lit, ind

HELPERS = '''nocopy object NoC {
    val id: Int
    val s: String
    drop {
        println("drop noc {self.id}")
    }
}

object P {
    val a: Int
    val s: String
}

enum Color {
    Red,
    Green,
    Blue,
}

enum Tok {
    Num(Int),
    Word(String),
}

object Store {
    var m: Map[String, Int]
    var n: Int
    init(n: Int) {
        self.m = Map[String, Int]()
        self.n = n
    }
}

func color_of(n: Int) -> Color {
    val k = n % 3
    if k == 0 {
        return Color.Red
    }
    if k == 1 {
        return Color.Green
    }
    return Color.Blue
}

func color_idx(c: Color) -> Int {
    match c {
        Color.Red -> return 0
        Color.Green -> return 1
        Color.Blue -> return 2
    }
    return -1
}

func tok_add(t: Tok, i: Int) -> Tok {
    match t {
        Tok.Num(n) -> return Tok.Num(n + i)
        Tok.Word(w) -> return Tok.Word(w + "z")
    }
    return t
}

func adder(n: Int) -> func(Int) -> Int {
    val tag = "an adder closure with a heap string {n} inside its environment"
    return func(x: Int) -> Int {
        return x + n + tag.len() * 0
    }
}

func lpush(xs: [Int], v: Int) -> [Int] {
    var out = xs.copy()
    out.push(v)
    return out
}

func mput(m: Map[String, Int], k: String, v: Int) -> Map[String, Int] {
    var out = m.copy()
    out.put(k, out.get_or(k, 0) + v)
    return out
}

func msum(m: Map[String, Int]) -> Int {
    var t = 0
    for (k, v) in m {
        t += v
    }
    return t
}

func tok_show(t: Tok) -> String {
    match t {
        Tok.Num(n) -> return "num {n}"
        Tok.Word(w) -> return "word {w.len()} {w.count("z")}"
    }
    return ""
}
'''

LONG = 'a string long enough to live on the heap, not inline'


class Kind:
    """An element type: its initial value, a commutative update by an Int
    i, an invariant readers check, and how a value is shown."""

    def __init__(self, r, name):
        self.name = name
        self.k = r.randint(1, 9)
        if name == 'Int':
            self.init = r.randint(-50, 50)
            self.t = 'Int'
        elif name == 'String':
            self.init = r.choice(['s', '', 'start', LONG, LONG + ' and then some'])
            self.tok = r.choice([1, 1, 2, 30])     # 30: every update allocates
            self.t = 'String'
        elif name == 'P':
            self.init = (r.randint(0, 9), r.choice(['p', LONG]))
            self.t = 'P'
        elif name == 'Color':
            self.init = r.randint(0, 2)
            self.t = 'Color'
        elif name == 'Tok':
            self.init = ('Num', r.randint(0, 20)) if r.random() < 0.5 else ('Word', r.choice(['w', LONG]))
            self.t = 'Tok'
        elif name == 'Store':
            self.init = r.randint(0, 5)
            self.t = 'Store'
        elif name == 'List':
            self.init = [r.randint(-5, 5) for _ in range(r.randint(1, 3))]
            self.t = '[Int]'
        elif name == 'Rc':
            self.init = (r.randint(0, 9), r.choice(['r', LONG]))
            self.t = 'rc P'
        elif name == 'Fn':
            self.init = r.randint(0, 20)
            self.t = 'func(Int) -> Int'
        elif name == 'Map':
            self.init = {}
            self.K = r.randint(1, 4)
            self.t = 'Map[String, Int]'

    # Iron text ---------------------------------------------------------
    def init_src(self):
        n = self.name
        if n == 'Int':
            return str(self.init)
        if n == 'String':
            return iron_lit(self.init)
        if n == 'P':
            return f'P({self.init[0]}, {iron_lit(self.init[1])})'
        if n == 'Color':
            return f'color_of({self.init})'
        if n == 'Tok':
            v, x = self.init
            return f'Tok.Num({x})' if v == 'Num' else f'Tok.Word({iron_lit(x)})'
        if n == 'List':
            return '[' + ', '.join(map(str, self.init)) + ']' if self.init else '[Int]()'
        if n == 'Rc':
            return f'rc P({self.init[0]}, {iron_lit(self.init[1])})'
        if n == 'Fn':
            return f'adder({self.init})'
        if n == 'Map':
            return 'Map[String, Int]()'
        return f'Store({self.init})'

    def owned(self, name):
        """name handed to a new owner: a list or map binding is moved out
        (manual 6.8: never duplicated implicitly)."""
        if self.name in ('List', 'Map'):
            return f'{name}.take()'
        return name

    def upd_src(self, cur, i):
        """An expression for the updated value (Store: statements)."""
        n = self.name
        if n == 'Int':
            return f'{cur} + {i} * {self.k}'
        if n == 'String':
            if self.tok == 30:
                return f'{cur} + "{"x" * 30}"'
            return f'{cur} + "x".repeat(1 + {i} % {self.tok + 1})'
        if n == 'P':
            return f'P({cur}.a + {i} * {self.k}, {cur}.s + "y")'
        if n == 'Color':
            return f'color_of(color_idx({cur}) + {i})'
        if n == 'Tok':
            return f'tok_add({cur}, {i})'
        if n == 'List':
            return f'lpush({cur}, {i})'
        if n == 'Rc':
            return f'rc P({cur}.a + {i} * {self.k}, {cur}.s + "y")'
        if n == 'Fn':
            return f'adder({cur}(0) + {i})'
        if n == 'Map':
            return f'mput({cur}, "k{{{i} % {self.K}}}", {i})'
        raise ValueError(n)

    def inv_src(self, c):
        n = self.name
        if n == 'Int':
            return f'{c} >= {self.init}'
        if n == 'String':
            return f'{c}.len() >= {len(self.init)}'
        if n == 'P':
            return f'{c}.s.len() >= {len(self.init[1])}'
        if n == 'Color':
            return f'color_idx({c}) < 3'
        if n == 'Tok':
            return f'tok_show({c}).len() > 0'
        if n == 'List':
            return f'{c}.len() >= {len(self.init)}'
        if n == 'Rc':
            return f'{c}.s.len() >= {len(self.init[1])}'
        if n == 'Fn':
            return f'{c}(0) >= {self.init}'
        if n == 'Map':
            return f'{c}.len() >= 0'
        return f'{c}.n >= {self.init}'

    def show_src(self, c):
        n = self.name
        if n == 'Int':
            return f'{{{c}}}'
        if n == 'String':
            return f'{{{c}.len()}} {{{c}.count("x")}}'
        if n == 'P':
            return f'{{{c}.a}} {{{c}.s.len()}}'
        if n == 'Color':
            return f'{{color_idx({c})}}'
        if n == 'Tok':
            return f'{{tok_show({c})}}'
        if n == 'List':
            return f'{{{c}.len()}} {{{c}.sum()}}'
        if n == 'Rc':
            return f'{{{c}.a}} {{{c}.s.len()}}'
        if n == 'Fn':
            return f'{{{c}(0)}}'
        if n == 'Map':
            return f'{{{c}.len()}} {{msum({c})}}'
        return f'{{{c}.n}} {{{c}.m.len()}}'

    # Python model ------------------------------------------------------
    def py_init(self):
        n = self.name
        if n == 'P':
            return list(self.init)
        if n == 'Tok':
            return list(self.init)
        if n == 'Store':
            return [self.init, {}]
        if n == 'List':
            return list(self.init)
        if n == 'Map':
            return {}
        if n == 'Rc':
            return list(self.init)
        return self.init

    def py_upd(self, v, i):
        n = self.name
        if n == 'Int':
            return v + i * self.k
        if n == 'String':
            return v + ('x' * 30 if self.tok == 30 else 'x' * (1 + i % (self.tok + 1)))
        if n == 'P':
            return [v[0] + i * self.k, v[1] + 'y']
        if n == 'Color':
            return (v + i) % 3
        if n == 'Tok':
            return ['Num', v[1] + i] if v[0] == 'Num' else ['Word', v[1] + 'z']
        if n == 'List':
            return v + [i]
        if n == 'Rc':
            return [v[0] + i * self.k, v[1] + 'y']
        if n == 'Fn':
            return v + i
        if n == 'Map':
            d = dict(v)
            d[f'k{i % self.K}'] = d.get(f'k{i % self.K}', 0) + i
            return d
        raise ValueError(n)

    def py_show(self, v):
        n = self.name
        if n == 'Int':
            return str(v)
        if n == 'String':
            return f'{len(v)} {v.count("x")}'
        if n == 'P':
            return f'{v[0]} {len(v[1])}'
        if n == 'Color':
            return str(v)
        if n == 'Tok':
            return f'num {v[1]}' if v[0] == 'Num' else f'word {len(v[1])} {v[1].count("z")}'
        if n == 'List':
            return f'{len(v)} {sum(v)}'
        if n == 'Rc':
            return f'{v[0]} {len(v[1])}'
        if n == 'Fn':
            return str(v)
        if n == 'Map':
            return f'{len(v)} {sum(v.values())}'
        return f'{v[0]} {len(v[1])}'


class SyncGen(SegProgram):
    HELPERS = HELPERS
    NSEGS = (2, 6)
    KINDS = ['Int', 'String', 'String', 'P', 'P', 'Color', 'Tok', 'Tok', 'List', 'Map', 'Rc', 'Fn']

    def add_segment(self):
        r = self.r
        pick = r.choices(
            ['mutex', 'rwlock', 'shared', 'chan', 'results', 'ring', 'order', 'store', 'panic', 'rpc',
             'fnchan', 'fnmap', 'nocopy'],
            [5, 3, 2, 4, 2, 1, 1, 1, 0.6, 1, 1.5, 1.5, 1.5])[0]
        self.segs.append(getattr(self, 'seg_' + pick)())

    # ---------------------------------------------------------- workers
    def workers(self, body_fn, n_hint):
        """Run body_fn(i_expr) for the update indexes, on threads. Returns
        (lines, list of indexes the updates used)."""
        r = self.r
        style = r.choice(['pfor', 'pfor', 'spawn', 'nested'])
        if style == 'pfor':
            n = n_hint
            i = self.fresh('i')
            return [f'for {i} in range({n}) parallel {{'] + ind(body_fn(i)) + ['}'], list(range(n))
        if style == 'spawn':
            k = r.randint(1, 4)
            m = r.randint(0, 6)
            lines, idx, hs = [], [], []
            for t in range(k):
                h = self.fresh('h')
                j = self.fresh('j')
                base = t * 100
                lines += [f'val {h} = spawn("w{t}") {{',
                          f'    for {j} in range({m}) {{'] + ind(body_fn(f'({j} + {base})'), 2) + \
                         ['    }', f'    return {m}', '}']
                hs.append(h)
                idx += [base + q for q in range(m)]
            r.shuffle(hs)
            for h in hs:
                lines.append(f'val {self.fresh("z")} = await {h}')
            return lines, idx
        # a parallel for inside a spawn, awaited
        n = n_hint
        i = self.fresh('i')
        h = self.fresh('h')
        lines = [f'val {h} = spawn("outer") {{',
                 f'    for {i} in range({n}) parallel {{'] + ind(body_fn(i), 2) + \
                ['    }', f'    return {n}', '}', f'val {self.fresh("z")} = await {h}']
        return lines, list(range(n))

    # ------------------------------------------------------------ mutex
    def seg_mutex(self):
        r = self.r
        kd = Kind(r, r.choice(self.KINDS))
        m = self.fresh('m')
        lines = [f'val {m} = Mutex({kd.init_src()})']

        def body(i):
            g = self.fresh('g')
            c = self.fresh('c')
            out = [f'val {g} = {m}.lock()', f'val {c}: {kd.t} = {g}.get()']
            if r.random() < 0.4:
                nv = self.fresh('nv')
                out += [f'var {nv} = {kd.upd_src(c, i)}', f'{g}.set({kd.owned(nv)})']
            else:
                out.append(f'{g}.set({kd.upd_src(c, i)})')
            return out
        n = r.choice([0, 1, 3, 8, 20, 50])
        wl, idx = self.workers(body, n)
        lines += wl
        v = kd.py_init()
        for i in idx:
            v = kd.py_upd(v, i)
        # read it into a binding that outlives the guard
        c = self.fresh('c')
        g = self.fresh('g')
        lines += [f'var {c} = {kd.init_src()}', '{', f'    val {g} = {m}.lock()',
                  f'    {c} = {g}.get()', '}', f'println("{kd.show_src(c)}")']
        out = [kd.py_show(v)]
        if r.random() < 0.5:
            # replace the value from a binding, then read it again
            nv = self.fresh('nv')
            g2 = self.fresh('g')
            c2 = self.fresh('c')
            lines += [f'var {nv} = {kd.upd_src(c, "7")}', f'val {nv}k = {kd.upd_src(c, "7")}',
                      '{', f'    val {g2} = {m}.lock()',
                      f'    {g2}.set({kd.owned(nv)})', '}', '{', f'    val {g2} = {m}.lock()',
                      f'    val {c2}: {kd.t} = {g2}.get()', f'    println("{kd.show_src(c2)}")', '}',
                      f'println("{kd.show_src(nv + "k")}")']
            v2 = kd.py_upd(v, 7)
            out += [kd.py_show(v2), kd.py_show(v2)]
        return Seg(lines, out)

    # ----------------------------------------------------------- rwlock
    def seg_rwlock(self):
        r = self.r
        kd = Kind(r, r.choice(self.KINDS))
        rw = self.fresh('rw')
        cnt = self.fresh('cnt')
        every = r.randint(1, 4)
        lines = [f'val {rw} = RWLock({kd.init_src()})', f'val {cnt} = Mutex(0)']

        def body(i):
            w = self.fresh('w')
            c = self.fresh('c')
            rd = self.fresh('rd')
            g = self.fresh('g')
            return [f'if {i} % {every} == 0 {{',
                    f'    val {w} = {rw}.write()',
                    f'    val {c}: {kd.t} = {w}.get()',
                    f'    {w}.set({kd.upd_src(c, i)})',
                    '} else {',
                    f'    val {rd} = {rw}.read()',
                    f'    val {c}: {kd.t} = {rd}.get()',
                    f'    if {kd.inv_src(c)} {{',
                    f'        val {g} = {cnt}.lock()',
                    f'        {g}.set({g}.get() + 1)',
                    '    }',
                    '}']
        n = r.choice([1, 4, 10, 30, 60])
        wl, idx = self.workers(body, n)
        lines += wl
        v = kd.py_init()
        reads = 0
        for i in idx:
            if i % every == 0:
                v = kd.py_upd(v, i)
            else:
                reads += 1
        rd = self.fresh('rd')
        c = self.fresh('c')
        g = self.fresh('g')
        lines += ['{', f'    val {rd} = {rw}.read()', f'    val {c}: {kd.t} = {rd}.get()',
                  f'    val {g} = {cnt}.lock()',
                  f'    println("{kd.show_src(c)} {{{g}.get()}}")', '}']
        return Seg(lines, [f'{kd.py_show(v)} {reads}'])

    # ----------------------------------------------------- shared tables
    def seg_shared(self):
        r = self.r
        mp = self.fresh('mp')
        st = self.fresh('st')
        lk = self.fresh('lk')
        K = r.randint(1, 6)
        long = r.random() < 0.4
        pre = 'key with a long prefix to allocate ' if long else 'k'
        lines = [f'val {mp} = rc Map[String, Int]()', f'val {st} = rc Set[Int]()',
                 f'val {lk} = Mutex(0)']
        rem = r.random() < 0.4

        def body(i):
            g = self.fresh('g')
            key = self.fresh('key')
            out = [f'val {g} = {lk}.lock()', f'val {key} = "{pre}{{{i} % {K}}}"',
                   f'{mp}.put({key}, {mp}.get_or({key}, 0) + {i})',
                   f'val {self.fresh("b")} = {st}.add({i} % {K + 2})']
            return out
        n = r.choice([0, 1, 5, 20, 40])
        wl, idx = self.workers(body, n)
        lines += wl
        d, s = {}, set()
        for i in idx:
            k = f'{pre}{i % K}'
            d[k] = d.get(k, 0) + i
            s.add(i % (K + 2))
        if rem:
            def body2(i):
                g = self.fresh('g')
                return [f'val {g} = {lk}.lock()',
                        f'val {self.fresh("b")} = {mp}.remove("{pre}{{{i} % {K}}}")',
                        f'val {self.fresh("b")} = {st}.remove({i} % 2)']
            n2 = r.randint(0, 4)
            wl2, idx2 = self.workers(body2, n2)
            lines += wl2
            for i in idx2:
                d.pop(f'{pre}{i % K}', None)
                s.discard(i % 2)
        ks = self.fresh('ks')
        k = self.fresh('k')
        tot = self.fresh('tot')
        x = self.fresh('x')
        lines += [f'var {ks} = {mp}.keys()', f'{ks}.sort()',
                  f'for {k} in {ks} {{', f'    println("{{{k}}}={{{mp}.get({k})}}")', '}',
                  f'var {tot} = 0', f'for {x} in {st} {{', f'    {tot} += {x}', '}',
                  f'println("{{{mp}.len()}} {{{st}.len()}} {{{tot}}}")']
        out = [f'{k}={d[k]}' for k in sorted(d)] + [f'{len(d)} {len(s)} {sum(s)}']
        return Seg(lines, out)

    # ------------------------------------------------------- Store in Mutex
    def seg_store(self):
        """A Mutex of an object that owns a Map: get hands out a copy."""
        r = self.r
        m = self.fresh('m')
        init = r.randint(0, 5)
        lines = [f'val {m} = Mutex(Store({init}))']
        K = r.randint(1, 5)

        def body(i):
            g = self.fresh('g')
            c = self.fresh('c')
            return [f'val {g} = {m}.lock()', f'var {c}: Store = {g}.get()',
                    f'{c}.m.put("k{{{i} % {K}}}", {c}.m.get_or("k{{{i} % {K}}}", 0) + {i})',
                    f'{c}.n += 1', f'{g}.set({c}.copy())']
        n = r.choice([0, 1, 4, 12])
        wl, idx = self.workers(body, n)
        lines += wl
        d = {}
        for i in idx:
            d[f'k{i % K}'] = d.get(f'k{i % K}', 0) + i
        g = self.fresh('g')
        c = self.fresh('c')
        lines += ['{', f'    val {g} = {m}.lock()', f'    val {c}: Store = {g}.get()',
                  f'    println("{{{c}.n}} {{{c}.m.len()}} {{{c}.m.get_or("k0", -1)}}")', '}']
        return Seg(lines, [f'{init + len(idx)} {len(d)} {d.get("k0", -1)}'])

    # ---------------------------------------------------------- channels
    def seg_chan(self):
        r = self.r
        kd = Kind(r, r.choice(['Int', 'String', 'String', 'P', 'Color', 'Tok', 'List', 'Map', 'Rc',
                               'Fn']))
        cap = r.choice([0, 0, 1, 2, 4])
        eff = max(cap, 1)
        ch = self.fresh('ch')
        lines = [f'val {ch} = Channel[{kd.t}]({cap})']
        nprod = r.choice([1, 1, 2, 3])
        counts = [r.randint(0, 6) for _ in range(nprod)]
        hs = []
        items = []
        for p, k in enumerate(counts):
            h = self.fresh('h')
            j = self.fresh('j')
            cur = self.fresh('cur')
            lines += [f'val {h} = spawn("prod{p}") {{', f'    var {cur} = {kd.init_src()}',
                      f'    for {j} in range({k}) {{',
                      f'        {cur} = {kd.upd_src(cur, f"({j} + {p * 10})")}',
                      f'        {ch}.send({cur}.copy())' if kd.name in ('List', 'Map') else f'        {ch}.send({cur})', '    }', f'    return {k}', '}']
            v = kd.py_init()
            seq = []
            for j in range(k):
                v = kd.py_upd(v, j + p * 10)
                seq.append(v)
            items.append(seq)
            hs.append(h)
        total = sum(counts)
        left = r.randint(0, min(eff, total)) if nprod == 1 and r.random() < 0.4 else 0
        take = total - left
        j = self.fresh('j')
        x = self.fresh('x')
        out = []
        if nprod == 1:
            lines += [f'for {j} in range({take}) {{', f'    val {x}: {kd.t} = {ch}.recv()',
                      f'    println("{kd.show_src(x)}")', '}']
            out += [kd.py_show(v) for v in items[0][:take]]
        else:
            got = self.fresh('got')
            lines += [f'var {got}: [String] = []', f'for {j} in range({take}) {{',
                      f'    val {x}: {kd.t} = {ch}.recv()', f'    {got}.push("{kd.show_src(x)}")', '}',
                      f'{got}.sort()', f'for {x} in {got} {{', f'    println({x})', '}']
            out += sorted(kd.py_show(v) for seq in items for v in seq)
        r.shuffle(hs)
        for h in hs:
            lines.append(f'val {self.fresh("z")} = await {h}')
        if r.random() < 0.15:
            # a consumer nobody feeds: it blocks until the program ends
            c2 = self.fresh('ch')
            lines += [f'val {c2} = Channel[{kd.t}](1)',
                      'spawn("idle") {', f'    val {self.fresh("v")}: {kd.t} = {c2}.recv()',
                      '    return 0', '}']
        return Seg(lines, out)

    # ------------------------------------------------ closures (#506)
    def seg_fnchan(self):
        """Producers send closures that capture their index (and a String)
        over a Channel[func(Int) -> Int]; the receiver runs each one. A
        Channel of higher-order closures applies them to a fixed function.
        Sums are order independent."""
        r = self.r
        cap = r.choice([0, 1, 2, 4])
        ch = self.fresh('ch')
        base = r.randint(0, 50)
        tag = self.fresh('tag')
        lines = [f'val {ch} = Channel[func(Int) -> Int]({cap})',
                 f'val {tag} = {iron_lit(r.choice(["t", LONG]))}']
        nprod = r.choice([1, 2, 3])
        counts = [r.randint(0, 5) for _ in range(nprod)]
        hs = []
        expect = 0
        arg = r.randint(0, 9)
        for p, k in enumerate(counts):
            h, j, kk = self.fresh('h'), self.fresh('j'), self.fresh('k')
            how = r.choice(['lambda', 'adder', 'bound'])
            if how == 'lambda':
                send = [f'        val {kk} = {j} + {p * 10} + {base}',
                        f'        {ch}.send(func(x: Int) -> Int {{ return x + {kk} + {tag}.len() * 0 }})']
            elif how == 'adder':
                send = [f'        {ch}.send(adder({j} + {p * 10} + {base}))']
            else:
                f = self.fresh('f')
                send = [f'        val {f}: func(Int) -> Int = adder({j} + {p * 10} + {base})',
                        f'        {ch}.send({f})']
            lines += [f'val {h} = spawn("fprod{p}") {{', f'    for {j} in range({k}) {{'] + send + \
                     ['    }', f'    return {k}', '}']
            hs.append(h)
            expect += sum(arg + j + p * 10 + base for j in range(k))
        total = sum(counts)
        acc, j, f = self.fresh('acc'), self.fresh('j'), self.fresh('f')
        lines += [f'var {acc} = 0', f'for {j} in range({total}) {{',
                  f'    val {f}: func(Int) -> Int = {ch}.recv()', f'    {acc} += {f}({arg})', '}']
        r.shuffle(hs)
        for h in hs:
            lines.append(f'val {self.fresh("z")} = await {h}')
        lines.append(f'println("fnchan {{{acc}}}")')
        out = [f'fnchan {expect}']
        if r.random() < 0.5:
            hi, hh, g, y = self.fresh('hi'), self.fresh('h'), self.fresh('g'), self.fresh('y')
            m = r.randint(1, 9)
            lines += [f'val {hi} = Channel[func(func(Int) -> Int) -> Int](1)',
                      f'val {hh} = spawn("hi") {{',
                      f'    {hi}.send(func(g: func(Int) -> Int) -> Int {{ return g({m}) }})',
                      '    return 0', '}',
                      f'val {g}: func(func(Int) -> Int) -> Int = {hi}.recv()',
                      f'val {self.fresh("z")} = await {hh}',
                      f'println("hi {{{g}(adder({base}))}}")']
            out.append(f'hi {m + base}')
        if r.random() < 0.3:
            # closures left queued when the channel goes away
            lc = self.fresh('lc')
            n = r.randint(1, 3)
            lines += [f'val {lc} = Channel[func(Int) -> Int]({n})']
            lines += [f'{lc}.send(adder({q}))' for q in range(n)]
        return Seg(lines, out)

    def seg_fnmap(self):
        """A Map[String, func(Int) -> Int] shared as rc behind a Mutex and
        filled from threads (the value stored under a key depends only on
        the key, so the result does not depend on the schedule), then a
        local map of closures put from bindings and from temporaries, and a
        map of lists of closures."""
        r = self.r
        mp, lk = self.fresh('mp'), self.fresh('lk')
        K = r.randint(1, 5)
        lines = [f'val {mp} = rc Map[String, func(Int) -> Int]()', f'val {lk} = Mutex(0)']

        def body(i):
            g, key, f = self.fresh('g'), self.fresh('key'), self.fresh('f')
            return [f'val {g} = {lk}.lock()', f'val {key} = "k{{{i} % {K}}}"',
                    f'val {f} = adder({i} % {K})', f'{mp}.put({key}, {f})',
                    f'{g}.set({g}.get() + 1)']
        n = r.choice([0, 1, 4, 12])
        wl, idx = self.workers(body, n)
        lines += wl
        keys = sorted({f'k{i % K}' for i in idx})
        ks, k, x = self.fresh('ks'), self.fresh('k'), self.fresh('x')
        arg = r.randint(0, 9)
        lines += [f'var {ks} = {mp}.keys()', f'{ks}.sort()',
                  f'for {k} in {ks} {{', f'    println("{{{k}}} {{{mp}.get({k})({arg})}}")', '}']
        out = [f'{key} {arg + int(key[1:])}' for key in keys]
        loc = self.fresh('loc')
        lines.append(f'var {loc} = Map[String, func(Int) -> Int]()')
        tot = 0
        for q in range(r.randint(1, 4)):
            v = r.randint(0, 20)
            how = r.choice(['bind', 'temp', 'lambda'])
            if how == 'bind':
                f = self.fresh('f')
                lines += [f'val {f} = adder({v})', f'{loc}.put("q{q}", {f})']
            elif how == 'temp':
                lines.append(f'{loc}.put("q{q}", adder({v}))')
            else:
                lines.append(f'{loc}.put("q{q}", func(x: Int) -> Int {{ return x + {v} }})')
            tot += arg + v
        s = self.fresh('s')
        lines += [f'var {s} = 0', f'for ({k}, {x}) in {loc} {{', f'    {s} += {x}({arg})', '}',
                  f'println("fnmap {{{s}}} {{{loc}.len()}}")']
        nloc = len([ln for ln in lines if f'{loc}.put(' in ln])
        out.append(f'fnmap {tot} {nloc}')
        if r.random() < 0.5:
            gm = self.fresh('gm')
            c = r.randint(1, 3)
            lines += [f'var {gm} = Map[String, [func(Int) -> Int]]()',
                      f'{gm}.put("g", [' + ', '.join(f'adder({q})' for q in range(c)) + '])',
                      f'println("groups {{{gm}.get("g").len()}}")']
            out.append(f'groups {c}')
        return Seg(lines, out)

    # ----------------------------------------------------- spawn results
    def seg_results(self):
        r = self.r
        lines, out, hs = [], [], []
        cap_s = self.fresh('cs')
        lines.append(f'val {cap_s} = {iron_lit(r.choice(["c", LONG]))}')
        for t in range(r.randint(1, 4)):
            kd = Kind(r, r.choice(self.KINDS[:-2] + ['Tok']))
            h = self.fresh('h')
            c = self.fresh('c')
            k = r.randint(0, 5)
            lines += [f'val {h} = spawn("r{t}") {{', f'    var {c} = {kd.init_src()}',
                      f'    for {self.fresh("j")} in range({k}) {{',
                      f'        {c} = {kd.upd_src(c, str(t))}', '    }']
            nested = r.random() < 0.3
            if nested:
                h2 = self.fresh('h')
                lines += [f'    val {h2} = spawn("inner") {{', f'        return {cap_s}.len()', '    }',
                          f'    val {self.fresh("z")} = await {h2}']
            lines += [f'    return {c}', '}']
            v = kd.py_init()
            for _ in range(k):
                v = kd.py_upd(v, t)
            hs.append((h, kd, v))
        r.shuffle(hs)
        for h, kd, v in hs:
            x = self.fresh('x')
            lines += [f'val {x} = await {h}', f'println("{kd.show_src(x)}")']
            out.append(kd.py_show(v))
        return Seg(lines, out)

    # --------------------------------------------------- deadlock free
    def seg_ring(self):
        """k threads in a ring of channels pass a token R times."""
        r = self.r
        k = r.randint(1, 4)
        R = r.randint(1, 6)
        cs = [self.fresh('c') for _ in range(k + 1)]
        lines = [f'val {c} = Channel[Int]({r.choice([0, 1, 2])})' for c in cs]
        hs = []
        for j in range(k):
            h = self.fresh('h')
            q = self.fresh('q')
            v = self.fresh('v')
            lines += [f'val {h} = spawn("ring{j}") {{', f'    for {q} in range({R}) {{',
                      f'        val {v}: Int = {cs[j]}.recv()', f'        {cs[j + 1]}.send({v} + {j + 1})',
                      '    }', f'    return {R}', '}']
            hs.append(h)
        q = self.fresh('q')
        y = self.fresh('y')
        lines += [f'for {q} in range({R}) {{', f'    {cs[0]}.send({q} * 10)',
                  f'    val {y}: Int = {cs[k]}.recv()', f'    println("ring {{{y}}}")', '}']
        for h in hs:
            lines.append(f'val {self.fresh("z")} = await {h}')
        add = sum(j + 1 for j in range(k))
        return Seg(lines, [f'ring {q * 10 + add}' for q in range(R)])

    def seg_nocopy(self):
        """Tasks borrow nocopy values (#512): each task is awaited later in
        the block that starts it, reads the values without copying them, and
        a Mutex(rc NoC) is shared by tasks and main; each value is dropped
        once, by main, at the end of the segment."""
        r = self.r
        n = r.randint(1, 3)
        vals = []
        lines = []
        for _ in range(n):
            x = self.fresh('nc')
            ident = r.randint(1, 999)
            vals.append((x, ident))
            lines.append(f'val {x} = NoC({ident}, {iron_lit(r.choice(["n", LONG]))})')
        shared = r.random() < 0.5
        sid = r.randint(1000, 1999)
        m = self.fresh('m')
        if shared:
            lines.append(f'val {m} = Mutex(rc NoC({sid}, "shared"))')
        k = r.randint(1, 4)
        hs, total = [], 0
        for t in range(k):
            h, j = self.fresh('h'), self.fresh('j')
            mul = r.randint(1, 5)
            body = [f'var {j} = 0']
            for x, ident in vals:
                body.append(f'{j} += {x}.id * {mul} + {x}.s.len() * 0')
                total += ident * mul
            if shared:
                g, v = self.fresh('g'), self.fresh('v')
                body += [f'val {g} = {m}.lock()', f'val {v}: rc NoC = {g}.get()', f'{j} += {v}.id']
                total += sid
            body.append(f'return {j}')
            lines += [f'val {h} = spawn("nc{t}") {{'] + ind(body) + ['}']
            hs.append(h)
        r.shuffle(hs)
        acc = self.fresh('acc')
        lines.append(f'var {acc} = 0')
        for h in hs:
            lines.append(f'{acc} += await {h}')
        lines.append(f'println("nocopy {{{acc}}}")')
        out = [f'nocopy {total}']
        if shared:
            out.append(f'drop noc {sid}')
        out += [f'drop noc {ident}' for _, ident in reversed(vals)]
        return Seg(lines, out)

    def seg_order(self):
        """Two locks always taken in the same order by every thread."""
        r = self.r
        ma, mb = self.fresh('ma'), self.fresh('mb')
        a0, b0 = r.randint(0, 100), r.randint(0, 100)
        lines = [f'val {ma} = Mutex({a0})', f'val {mb} = Mutex({b0})']

        def body(i):
            ga, gb = self.fresh('ga'), self.fresh('gb')
            return [f'val {ga} = {ma}.lock()', f'val {gb} = {mb}.lock()',
                    f'{ga}.set({ga}.get() - {i})', f'{gb}.set({gb}.get() + {i} * 2)']
        n = r.choice([1, 5, 20, 64])
        wl, idx = self.workers(body, n)
        lines += wl
        ga, gb = self.fresh('ga'), self.fresh('gb')
        lines += ['{', f'    val {ga} = {ma}.lock()', f'    val {gb} = {mb}.lock()',
                  f'    println("{{{ga}.get()}} {{{gb}.get()}}")', '}']
        return Seg(lines, [f'{a0 - sum(idx)} {b0 + 2 * sum(idx)}'])

    def seg_rpc(self):
        """Workers answer requests: each takes a request from one channel and
        replies on its own; main sends one request at a time per worker, so
        the replies come back in order. Each reply also updates a Mutex and
        an RWLock, always in that order."""
        r = self.r
        kd = Kind(r, r.choice(['Int', 'String', 'P', 'Tok', 'Fn']))
        nw = r.randint(1, 3)
        rounds = r.randint(1, 5)
        m, rw = self.fresh('m'), self.fresh('rw')
        lines = [f'val {m} = Mutex(0)', f'val {rw} = RWLock(0)']
        reqs, resps, hs = [], [], []
        for w in range(nw):
            rq, rp, h = self.fresh('rq'), self.fresh('rp'), self.fresh('h')
            q, x, cur, g1, g2 = (self.fresh('q'), self.fresh('x'), self.fresh('cur'),
                                 self.fresh('g'), self.fresh('w'))
            lines += [f'val {rq} = Channel[Int]({r.choice([0, 1, 2])})',
                      f'val {rp} = Channel[{kd.t}]({r.choice([0, 1])})',
                      f'val {h} = spawn("worker{w}") {{',
                      f'    for {q} in range({rounds}) {{',
                      f'        val {x}: Int = {rq}.recv()',
                      f'        var {cur} = {kd.init_src()}',
                      f'        {cur} = {kd.upd_src(cur, f"({x} + {w})")}',
                      f'        {{', f'            val {g1} = {m}.lock()', f'            val {g2} = {rw}.write()',
                      f'            {g1}.set({g1}.get() + {x})', f'            {g2}.set({g2}.get() + 1)', '        }',
                      f'        {rp}.send({cur})', '    }', f'    return {rounds}', '}']
            reqs.append(rq)
            resps.append(rp)
            hs.append(h)
        out = []
        tot = 0
        q, y = self.fresh('q'), self.fresh('y')
        body = []
        for w in range(nw):
            body += [f'    {reqs[w]}.send({q} * 3 + {w})', f'    val {y}{w}: {kd.t} = {resps[w]}.recv()',
                     f'    println("rpc{w} {kd.show_src(y + str(w))}")']
        lines += [f'for {q} in range({rounds}) {{'] + body + ['}']
        for qq in range(rounds):
            for w in range(nw):
                v = kd.py_upd(kd.py_init(), qq * 3 + w + w)
                out.append(f'rpc{w} {kd.py_show(v)}')
                tot += qq * 3 + w
        for h in hs:
            lines.append(f'val {self.fresh("z")} = await {h}')
        g1, g2 = self.fresh('g'), self.fresh('rd')
        lines += ['{', f'    val {g1} = {m}.lock()', f'    val {g2} = {rw}.read()',
                  f'    println("rpc {{{g1}.get()}} {{{g2}.get()}}")', '}']
        out.append(f'rpc {tot} {rounds * nw}')
        return Seg(lines, out)

    # ------------------------------------------------------------- panic
    def seg_panic(self):
        r = self.r
        kind = r.choice(['index', 'assert', 'map', 'pfor', 'fire'])
        n = self.fresh('n')
        k = r.randint(3, 9)
        lines = [f'val {n} = {k}', f'println("before {k}")']
        out = [f'before {k}']
        if kind == 'index':
            h = self.fresh('h')
            xs = self.fresh('xs')
            lines += [f'val {h} = spawn("bad") {{', f'    val {xs} = [1, 2, 3]',
                      f'    return {xs}[{n}]', '}', f'val {self.fresh("z")} = await {h}',
                      'println("not reached")']
            return Seg(lines, out, panic='index out of bounds')
        if kind == 'assert':
            h = self.fresh('h')
            lines += [f'val {h} = spawn("bad") {{', f'    assert({n} < 0, "worker check {{{n}}}")',
                      f'    return {n}', '}', f'val {self.fresh("z")} = await {h}',
                      'println("not reached")']
            return Seg(lines, out, panic=f'worker check {k}')
        if kind == 'map':
            h = self.fresh('h')
            mp = self.fresh('mp')
            lines += [f'val {h} = spawn("bad") {{', f'    var {mp} = Map[String, Int]()',
                      f'    {mp}.put("a", 1)', f'    return {mp}.get("b{{{n}}}")', '}',
                      f'val {self.fresh("z")} = await {h}', 'println("not reached")']
            return Seg(lines, out, panic='key not found')
        if kind == 'pfor':
            i = self.fresh('i')
            xs = self.fresh('xs')
            tot = self.fresh('tot')
            lines += [f'val {tot} = Mutex(0)', f'val {xs} = [1, 2, 3]',
                      f'for {i} in range({k} + 1) parallel {{', f'    val {self.fresh("g")} = {tot}.lock()',
                      f'    val {self.fresh("v")} = {xs}[{i}]', '}', 'println("not reached")']
            return Seg(lines, out, panic='index out of bounds')
        # fire and forget: main blocks on a channel nobody feeds
        ch = self.fresh('ch')
        xs = self.fresh('xs')
        lines += [f'val {ch} = Channel[Int](1)', 'spawn("bad") {', f'    val {xs} = [1, 2, 3]',
                  f'    return {xs}[{n}]', '}', f'val {self.fresh("v")}: Int = {ch}.recv()',
                  'println("not reached")']
        return Seg(lines, out, panic='index out of bounds')


def generate(rng):
    g = SyncGen(rng)
    g.program()
    return g
