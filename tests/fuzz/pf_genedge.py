"""Generic edge cases (manual 2.8, 5.5, 5.6, 6.1, 6.7, 9.10): constraints
other than Hashable (a user interface with a default method, several
constrained parameters, the interface type itself as the argument),
generic objects with copy and drop blocks (#468) copied, moved into
wrappers, stored in lists and maps, returned, and built by an init that
stores a copy; nocopy generic objects (E0286 when copied); recursive
generic enums (lists and binary trees) built, walked and rebuilt by
generic recursive functions, with unit variants passed as payload
arguments (#470); lists and maps as type arguments of objects, enums and
functions; instances nested up to eight deep; and calls that break a
constraint (E0206). Static functions (#455): on objects and patches,
declared by interfaces with and without default bodies, called on a type
and on a constrained type parameter (`T.make(n)`), `Self` in signatures
and default bodies, defaults that build a value field by field
(`Self.__fields(...)` and val field writes), and the misuses E0350 to
E0356 and E0265.

The copy and drop order follows manual 6.1 and 6.7 and the rules pf_obj
checked against the compiler: a binding copied from another runs copy;
a positional construction and a return move; an init storing a parameter
in a field stores a copy and the argument temporary is dropped after the
construction; put drops the value it replaces; a block drops its bindings
in reverse order, an object its drop block before its fields, a list its
elements in index order.
"""
from pf_probe import Probe, ProbeGen
from pf_lang import Bail

SHAPES = '''interface Shape {
    readonly func area() -> Int
    readonly func name() -> String {
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
    readonly func name() -> String {
        return "rect"
    }
}

interface Ranked {
    pure func rank() -> Int
}

object Card impl Ranked {
    val r: Int
    pure func rank() -> Int {
        return self.r
    }
}

func total_area[T: Shape](xs: [T]) -> Int {
    var t = 0
    for x in xs {
        t += x.area()
    }
    return t
}

func describe[T: Shape](x: T) -> String {
    return "{x.name()}:{x.area()}"
}

func best[T: Ranked](xs: [T]) -> Int {
    var b = -1
    for x in xs {
        if x.rank() > b {
            b = x.rank()
        }
    }
    return b
}

func tag_of[K: Hashable, S: Shape](k: K, s: S) -> String {
    return "{s.area()}"
}

object Bag[T: Shape] {
    var items: [T]
    init() {
        self.items = []
    }
    func add(x: T) {
        self.items.push(x)
    }
    readonly func sum() -> Int {
        var t = 0
        for x in self.items {
            t += x.area()
        }
        return t
    }
}'''

HOOKS = '''object Tr[T] {
    val v: T
    val tag: String
    copy {
        println("copy {self.tag}")
    }
    drop {
        println("drop {self.tag}")
    }
}

object W[T] {
    val inner: T
    drop {
        println("drop W")
    }
}

object Holder[T] {
    var inner: T
    init(inner: T) {
        self.inner = inner
    }
    drop {
        println("drop holder")
    }
}

func mk_tr[T](v: T, t: String) -> Tr[T] {
    return Tr(v, t)
}'''

ENUMS = '''enum Lst[T] {
    Nil,
    Cons(T, Lst[T]),
}

enum Tree[T] {
    Leaf,
    Node(Tree[T], T, Tree[T]),
}

enum Opt[T] {
    None,
    Some(T),
}

func llen[T](l: Lst[T]) -> Int {
    match l {
        Lst.Nil -> return 0
        Lst.Cons(h, t) -> return 1 + llen(t)
    }
    return 0
}

func lsum(l: Lst[Int]) -> Int {
    match l {
        Lst.Nil -> return 0
        Lst.Cons(h, t) -> return h + lsum(t)
    }
    return 0
}

func ljoin(l: Lst[String]) -> String {
    match l {
        Lst.Nil -> return ""
        Lst.Cons(h, t) -> return h + ljoin(t)
    }
    return ""
}

func lrev[T](l: Lst[T], acc: Lst[T]) -> Lst[T] {
    match l {
        Lst.Nil -> return acc
        Lst.Cons(h, t) -> return lrev(t, Lst.Cons(h, acc))
    }
    return acc
}

func lrange(n: Int) -> Lst[Int] {
    var l: Lst[Int] = Lst.Nil
    var i = n
    while i > 0 {
        l = Lst.Cons(i, l)
        i -= 1
    }
    return l
}

func tins(t: Tree[Int], v: Int) -> Tree[Int] {
    match t {
        Tree.Leaf -> return Tree.Node(Tree.Leaf, v, Tree.Leaf)
        Tree.Node(l, x, r) -> {
            if v < x {
                return Tree.Node(tins(l, v), x, r)
            }
            return Tree.Node(l, x, tins(r, v))
        }
    }
    return Tree.Leaf
}

func tsize[T](t: Tree[T]) -> Int {
    match t {
        Tree.Leaf -> return 0
        Tree.Node(l, x, r) -> return tsize(l) + 1 + tsize(r)
    }
    return 0
}

func tdepth[T](t: Tree[T]) -> Int {
    match t {
        Tree.Leaf -> return 0
        Tree.Node(l, x, r) -> return 1 + max(tdepth(l), tdepth(r))
    }
    return 0
}

func tsum(t: Tree[Int]) -> Int {
    match t {
        Tree.Leaf -> return 0
        Tree.Node(l, x, r) -> return tsum(l) + x + tsum(r)
    }
    return 0
}

func torder(t: Tree[Int]) -> String {
    match t {
        Tree.Leaf -> return ""
        Tree.Node(l, x, r) -> return torder(l) + "{x}," + torder(r)
    }
    return ""
}

func tmirror[T](t: Tree[T]) -> Tree[T] {
    match t {
        Tree.Leaf -> return Tree.Leaf
        Tree.Node(l, x, r) -> return Tree.Node(tmirror(r), x, tmirror(l))
    }
    return Tree.Leaf
}'''

BOXES = '''object Bx[T] {
    val v: T
}

object Pair[A, B] {
    val a: A
    val b: B
}

func size_of[T](xs: [T]) -> Int {
    return xs.len()
}

func keys_of[K: Hashable, V](m: Map[K, V]) -> Int {
    return m.len()
}'''


def ilist(xs):
    return '[' + ', '.join(str(x) for x in xs) + ']'


def lst_src(xs, kind='Int'):
    s = 'Lst.Nil'
    for x in reversed(xs):
        v = str(x) if kind == 'Int' else f'"{x}"'
        s = f'Lst.Cons({v}, {s})'
    return s


def tree_ins(t, v):
    if t is None:
        return (None, v, None)
    l, x, r = t
    if v < x:
        return (tree_ins(l, v), x, r)
    return (l, x, tree_ins(r, v))


def tree_size(t):
    return 0 if t is None else tree_size(t[0]) + 1 + tree_size(t[2])


def tree_depth(t):
    return 0 if t is None else 1 + max(tree_depth(t[0]), tree_depth(t[2]))


def tree_sum(t):
    return 0 if t is None else tree_sum(t[0]) + t[1] + tree_sum(t[2])


def tree_order(t):
    return '' if t is None else tree_order(t[0]) + f'{t[1]},' + tree_order(t[2])


def tree_mirror(t):
    return None if t is None else (tree_mirror(t[2]), t[1], tree_mirror(t[0]))


def tree_src(t):
    """A literal tree whose unit variants are payload arguments (#470)."""
    if t is None:
        return 'Tree.Leaf'
    return f'Tree.Node({tree_src(t[0])}, {t[1]}, {tree_src(t[2])})'


STATICS = '''interface Make {
    static func make(n: Int) -> Self
    static func many(ns: [Int]) -> [Self] {
        var out: [Self] = []
        for n in ns {
            out.push(Self.make(n))
        }
        return out
    }
    static func label() -> String {
        return "made"
    }
    readonly func val_of() -> Int
    readonly func same(o: Self) -> Bool
    readonly func tagged() -> String {
        return "{Self.label()}:{self.val_of()}"
    }
}

interface Fill {
    static func fill(a: Int, b: String) -> Self {
        var v = Self.__fields(0, "", [])
        v.a = a
        v.b = b
        v.log.push(b)
        return v
    }
}

object Ctr impl Make, Fill {
    val a: Int
    val b: String
    var log: [String]
    init(a: Int) {
        self.a = a
        self.b = "init"
        self.log = []
    }
    static func make(n: Int) -> Self {
        return Ctr(n * 2)
    }
    readonly func val_of() -> Int {
        return self.a
    }
    readonly func same(o: Ctr) -> Bool {
        return o.a == self.a
    }
}

object Lbl impl Make {
    val s: String
    static func make(n: Int) -> Lbl {
        return Lbl("L{n}")
    }
    static func label() -> String {
        return "lbl"
    }
    readonly func val_of() -> Int {
        return self.s.len()
    }
    readonly func same(o: Self) -> Bool {
        return o.s == self.s
    }
}

patch object Lbl {
    static func twice(n: Int) -> (Lbl, Lbl) {
        return (Lbl.make(n), Self.make(n + 1))
    }
}

func make_sum[T: Make](ns: [Int]) -> Int {
    var t = 0
    for n in ns {
        t += T.make(n).val_of()
    }
    return t
}

func count_same[T: Make](ns: [Int], probe: Int) -> Int {
    val p = T.make(probe)
    var c = 0
    for x in T.many(ns) {
        if x.same(p) {
            c += 1
        }
    }
    return c
}

func label_of[T: Make]() -> String {
    return "{T.label()}/{T.make(1).tagged()}"
}
'''


class GenEdgeGen(ProbeGen):
    NPROBES = (4, 10)

    # --------------------------------------------------------- constraints
    def shape(self):
        r = self.r
        if r.random() < 0.5:
            s = r.randint(0, 9)
            return f'Sq({s})', s * s, 'shape'
        w, h = r.randint(0, 9), r.randint(0, 9)
        return f'Rect({w}, {h})', w * h, 'rect'

    def p_constraint(self):
        r = self.r
        self.need('shapes', SHAPES)
        code, out = [], []
        # (the interface type itself as the argument of [T: Shape] is E0206:
        # the manual does not say whether an interface satisfies itself)
        k = r.choice(['total', 'describe', 'best', 'bag', 'multi'])
        tag = self.fresh('c')
        if k == 'total':
            kind = r.choice(['Sq', 'Rect'])
            shapes = [self.shape() for _ in range(r.randint(0, 5))]
            shapes = [s for s in shapes if s[0].startswith(kind)]
            n = self.fresh('xs')
            code.append(f'val {n}: [{kind}] = [' + ', '.join(s[0] for s in shapes) + ']')
            code.append(f'println("{tag} {{total_area({n})}}")')
            out.append(f'{tag} {sum(s[1] for s in shapes)}')
        elif k == 'describe':
            src, a, nm = self.shape()
            code.append(f'println("{tag} {{describe({src})}}")')
            out.append(f'{tag} {nm}:{a}')
        elif k == 'best':
            rs = [r.randint(-3, 20) for _ in range(r.randint(0, 5))]
            n = self.fresh('cs')
            code.append(f'val {n}: [Card] = [' + ', '.join(f'Card({x})' for x in rs) + ']')
            code.append(f'println("{tag} {{best({n})}}")')
            out.append(f'{tag} {max(rs + [-1])}')
        elif k == 'bag':
            kind = r.choice(['Sq', 'Rect'])
            n = self.fresh('bag')
            code.append(f'var {n} = Bag[{kind}]()')
            tot = 0
            for _ in range(r.randint(0, 5)):
                src, a, nm = self.shape()
                if not src.startswith(kind):
                    continue
                code.append(f'{n}.add({src})')
                tot += a
            code.append(f'println("{tag} {{{n}.sum()}} {{{n}.items.len()}}")')
            out.append(f'{tag} {tot} {sum(1 for l in code if ".add(" in l)}')
        elif k == 'iface':
            # the interface itself as the type argument
            shapes = [self.shape() for _ in range(r.randint(0, 5))]
            n = self.fresh('xs')
            code.append(f'val {n}: [Shape] = [' + ', '.join(s[0] for s in shapes) + ']')
            code.append(f'println("{tag} {{total_area({n})}}")')
            out.append(f'{tag} {sum(s[1] for s in shapes)}')
            if shapes:
                i = r.randrange(len(shapes))
                code.append(f'println("{tag}d {{describe({n}[{i}])}}")')
                out.append(f'{tag}d {shapes[i][2]}:{shapes[i][1]}')
        else:
            src, a, nm = self.shape()
            key = r.choice(['1', '"k"', 'true'])
            code.append(f'println("{tag} {{tag_of({key}, {src})}}")')
            out.append(f'{tag} {a}')
        return Probe(code, out)

    def p_bad_constraint(self):
        self.need('shapes', SHAPES)
        r = self.r
        n = self.fresh('bad')
        k = r.choice(['total', 'describe', 'bag', 'best'])
        if k == 'total':
            return Probe([f'val {n}: [Int] = [1, 2]', f'println("{{total_area({n})}}")'], errors={'E0206'})
        if k == 'describe':
            return Probe([f'println("{{describe(Card(1))}}")'], errors={'E0206'})
        if k == 'best':
            return Probe([f'val {n}: [Sq] = [Sq(1)]', f'println("{{best({n})}}")'], errors={'E0206'})
        return Probe([f'var {n} = Bag[Card]()', f'println("{{{n}.sum()}}")'], errors={'E0206'})

    # --------------------------------------------------------------- hooks
    def tr(self, tags):
        t = self.fresh('t')
        tags.append(t)
        # (an object holding a list is not copied implicitly: E0328)
        v = self.r.choice(['1', '"s"', 'Bx(3)', '2.5'])
        if 'Bx' in v:
            self.need('boxes', BOXES)
        return f'Tr({v}, "{t}")', t

    def p_hooks(self):
        r = self.r
        self.need('hooks', HOOKS)
        code, out = [], []
        drops = []          # bindings' drop events at block exit (in order of declaration)
        tags = []
        for _ in range(r.randint(1, 4)):
            k = r.choice(['copy', 'wrap', 'nest', 'list', 'map', 'ret', 'holder', 'assign', 'opt'])
            if k == 'copy':
                src, t = self.tr(tags)
                a, b = self.fresh('a'), self.fresh('b')
                code += [f'val {a} = {src}', f'val {b} = {a}']
                out.append(f'copy {t}')
                drops += [[f'drop {t}'], [f'drop {t}']]
            elif k == 'wrap':
                src, t = self.tr(tags)
                w = self.fresh('w')
                code.append(f'val {w} = W({src})')
                drops.append(['drop W', f'drop {t}'])
            elif k == 'nest':
                src, t = self.tr(tags)
                d = r.randint(2, 4)
                w = self.fresh('w')
                code.append(f'val {w} = ' + 'W(' * d + src + ')' * d)
                code.append(f'println("{{{w}' + '.inner' * d + '.tag}")')
                out.append(t)
                drops.append(['drop W'] * d + [f'drop {t}'])
            elif k == 'list':
                ts = [self.fresh('t') for _ in range(r.randint(1, 3))]
                n = self.fresh('xs')
                code.append(f'val {n} = [' + ', '.join(f'Tr({i}, "{t}")' for i, t in enumerate(ts)) + ']')
                code.append(f'println("{{{n}.len()}}")')
                out.append(str(len(ts)))
                drops.append([f'drop {t}' for t in ts])
            elif k == 'map':
                m = self.fresh('m')
                t1, t2 = self.fresh('t'), self.fresh('t')
                code += [f'var {m} = Map[Int, Tr[Int]]()', f'{m}.put(1, Tr(1, "{t1}"))',
                         f'{m}.put(1, Tr(2, "{t2}"))']
                out.append(f'drop {t1}')
                code.append(f'println("{{{m}.len()}} {{{m}.has(1)}}")')
                out.append('1 true')
                drops.append([f'drop {t2}'])
            elif k == 'ret':
                t = self.fresh('t')
                n = self.fresh('rt')
                code.append(f'val {n} = mk_tr({r.randint(0, 9)}, "{t}")')
                drops.append([f'drop {t}'])
            elif k == 'holder':
                src, t = self.tr(tags)
                h = self.fresh('h')
                code.append(f'val {h} = Holder({src})')
                out += [f'copy {t}', f'drop {t}']
                drops.append(['drop holder', f'drop {t}'])
            elif k == 'assign':
                t1, t2 = self.fresh('t'), self.fresh('t')
                a, b = self.fresh('a'), self.fresh('b')
                code += [f'var {a} = Tr(1, "{t1}")', f'val {b} = Tr(2, "{t2}")', f'{a} = {b}']
                out += [f'copy {t2}', f'drop {t1}']
                drops += [[f'drop {t2}'], [f'drop {t2}']]
            else:
                self.need('enums', ENUMS)
                t = self.fresh('t')
                o = self.fresh('o')
                code.append(f'val {o} = Opt.Some(Tr({r.randint(0, 9)}, "{t}"))')
                drops.append([f'drop {t}'])
        for d in reversed(drops):
            out += d
        return Probe(code, out)

    def p_bad_nocopy(self):
        self.need('uniq', '''nocopy object Uniq[T] {
    val v: T
}''')
        a, b = self.fresh('u'), self.fresh('u')
        return Probe([f'val {a} = Uniq({self.r.randint(0, 9)})', f'val {b} = {a}',
                      f'println("{{{b}.v}}")'], errors={'E0286'})

    def p_nocopy_ok(self):
        self.need('uniq', '''nocopy object Uniq[T] {
    val v: T
}''')
        a = self.fresh('u')
        v = self.r.randint(0, 99)
        tag = self.fresh('n')
        return Probe([f'val {a} = Uniq({v})', f'println("{tag} {{{a}.v}}")'], [f'{tag} {v}'])

    # --------------------------------------------------------------- enums
    def p_lst(self):
        r = self.r
        self.need('enums', ENUMS)
        code, out = [], []
        tag = self.fresh('l')
        if r.random() < 0.5:
            xs = [r.randint(-9, 99) for _ in range(r.randint(0, 6))]
            n = self.fresh('l')
            ann = ': Lst[Int]' if not xs or r.random() < 0.3 else ''
            code.append(f'val {n}{ann} = {lst_src(xs)}')
            parts, texts = [f'{{llen({n})}}', f'{{lsum({n})}}'], [str(len(xs)), str(sum(xs))]
            if r.random() < 0.6:
                rv = self.fresh('rv')
                code.append(f'val {rv} = lrev({n}, Lst.Nil)')
                parts.append(f'{{lsum({rv})}} {{llen({rv})}}')
                texts.append(f'{sum(xs)} {len(xs)}')
            code.append(f'println("{tag} ' + ' '.join(parts) + '")')
            out.append(f'{tag} ' + ' '.join(texts))
        elif r.random() < 0.5:
            xs = [r.choice(['a', 'b', 'é', 'xy', '']) for _ in range(r.randint(0, 5))]
            n = self.fresh('l')
            ann = ': Lst[String]' if not xs or r.random() < 0.3 else ''
            code.append(f'val {n}{ann} = {lst_src(xs, "String")}')
            rv = self.fresh('rv')
            code.append(f'val {rv} = lrev({n}, Lst.Nil)')
            code.append(f'println("{tag} {{llen({n})}} [{{ljoin({n})}}] [{{ljoin({rv})}}]")')
            out.append(f'{tag} {len(xs)} [{"".join(xs)}] [{"".join(reversed(xs))}]')
        else:
            k = r.randint(0, 2000)
            n = self.fresh('l')
            code.append(f'val {n} = lrange({k})')
            code.append(f'println("{tag} {{llen({n})}} {{lsum({n})}} {{lsum(lrev({n}, Lst.Nil))}}")')
            out.append(f'{tag} {k} {k * (k + 1) // 2} {k * (k + 1) // 2}')
        return Probe(code, out)

    def p_tree(self):
        r = self.r
        self.need('enums', ENUMS)
        code, out = [], []
        tag = self.fresh('t')
        n = self.fresh('tr')
        t = None
        vals = [r.randint(-20, 50) for _ in range(r.randint(0, 9))]
        for v in vals:
            t = tree_ins(t, v)
        if r.random() < 0.4 and tree_size(t) <= 6:
            ann = ': Tree[Int]' if t is None or r.random() < 0.3 else ''
            code.append(f'val {n}{ann} = {tree_src(t)}')
        else:
            code.append(f'var {n}: Tree[Int] = Tree.Leaf')
            for v in vals:
                code.append(f'{n} = tins({n}, {v})')
        parts = [f'{{tsize({n})}}', f'{{tdepth({n})}}', f'{{tsum({n})}}', f'[{{torder({n})}}]']
        texts = [str(tree_size(t)), str(tree_depth(t)), str(tree_sum(t)), f'[{tree_order(t)}]']
        if r.random() < 0.5:
            m = self.fresh('mi')
            code.append(f'val {m} = tmirror({n})')
            parts.append(f'[{{torder({m})}}]')
            texts.append(f'[{tree_order(tree_mirror(t))}]')
        code.append(f'println("{tag} ' + ' '.join(parts) + '")')
        out.append(f'{tag} ' + ' '.join(texts))
        if r.random() < 0.3:
            # a tree of strings through the generic functions
            s = self.fresh('st')
            code.append(f'val {s} = Tree.Node(Tree.Leaf, "x", Tree.Node(Tree.Leaf, "y", Tree.Leaf))')
            code.append(f'println("{tag}s {{tsize({s})}} {{tdepth({s})}}")')
            out.append(f'{tag}s 2 2')
        return Probe(code, out)

    def p_opt(self):
        r = self.r
        self.need('enums', ENUMS)
        code, out = [], []
        tag = self.fresh('o')
        k = r.choice(['int', 'list', 'nested', 'none'])
        n = self.fresh('o')
        if k == 'int':
            v = r.randint(-5, 50)
            code.append(f'val {n} = Opt.Some({v})')
            res = str(v)
        elif k == 'list':
            xs = [r.randint(0, 9) for _ in range(r.randint(1, 4))]
            code.append(f'val {n}: Opt[[Int]] = Opt.Some({ilist(xs)})')
            res = str(len(xs))
        elif k == 'nested':
            v = r.randint(0, 9)
            code.append(f'val {n} = Opt.Some(Opt.Some({v}))')
            res = f'in {v}'
        else:
            code.append(f'val {n}: Opt[Int] = Opt.None')
            res = 'none'
        code.append(f'match {n} {{')
        if k == 'list':
            code.append('    Opt.Some(x) -> println("' + tag + ' {x.len()}")')
        elif k == 'nested':
            code += ['    Opt.Some(x) -> {', '        match x {',
                     f'            Opt.Some(y) -> println("{tag} in {{y}}")',
                     f'            Opt.None -> println("{tag} inner none")', '        }', '    }']
        else:
            code.append('    Opt.Some(x) -> println("' + tag + ' {x}")')
        code.append(f'    Opt.None -> println("{tag} none")')
        code.append('}')
        out.append(f'{tag} {res}')
        return Probe(code, out)

    # ------------------------------------------------------- type arguments
    def p_typeargs(self):
        r = self.r
        self.need('boxes', BOXES)
        code, out = [], []
        tag = self.fresh('a')
        k = r.choice(['boxlist', 'boxmap', 'pair', 'grid', 'sizeof', 'keys', 'mapval'])
        n = self.fresh('b')
        if k == 'boxlist':
            xs = [r.randint(0, 9) for _ in range(r.randint(0, 5))]
            if not xs:
                code.append(f'val {n} = Bx(fill(0, 0))')
            else:
                code.append(f'val {n} = Bx({ilist(xs)})')
            code.append(f'println("{tag} {{{n}.v.len()}} {{size_of({n}.v)}}")')
            out.append(f'{tag} {len(xs)} {len(xs)}')
        elif k == 'boxmap':
            code.append(f'val {n} = Bx(Map[String, Int]())')
            code.append(f'println("{tag} {{{n}.v.len()}} {{keys_of({n}.v)}}")')
            out.append(f'{tag} 0 0')
        elif k == 'pair':
            xs = [r.randint(0, 9) for _ in range(r.randint(1, 4))]
            ss = [r.choice(['p', 'q', 'é']) for _ in range(r.randint(1, 3))]
            code.append(f'val {n} = Pair({ilist(xs)}, [' + ', '.join(f'"{s}"' for s in ss) + '])')
            code.append(f'println("{tag} {{{n}.a.len()}} {{{n}.b.len()}} {{{n}.b[0]}}")')
            out.append(f'{tag} {len(xs)} {len(ss)} {ss[0]}')
        elif k == 'grid':
            g = [[r.randint(0, 9) for _ in range(r.randint(1, 3))] for _ in range(r.randint(1, 3))]
            code.append(f'val {n} = Bx([' + ', '.join(ilist(x) for x in g) + '])')
            i = r.randrange(len(g))
            code.append(f'println("{tag} {{{n}.v.len()}} {{{n}.v[{i}].len()}} {{size_of({n}.v)}}")')
            out.append(f'{tag} {len(g)} {len(g[i])} {len(g)}')
        elif k == 'sizeof':
            g = [[r.randint(0, 9) for _ in range(r.randint(0, 3))] for _ in range(r.randint(0, 4))]
            code.append(f'val {n}: [[Int]] = [' + ', '.join(ilist(x) for x in g) + ']')
            code.append(f'println("{tag} {{size_of({n})}}")')
            out.append(f'{tag} {len(g)}')
        elif k == 'keys':
            m = self.fresh('m')
            ks = sorted(set(r.randint(0, 9) for _ in range(r.randint(0, 6))))
            code.append(f'var {m} = Map[Int, [Int]]()')
            for x in ks:
                code.append(f'{m}.put({x}, [{x}])')
            code.append(f'println("{tag} {{keys_of({m})}}")')
            out.append(f'{tag} {len(ks)}')
        else:
            m = self.fresh('m')
            code.append(f'var {m} = Map[String, Bx[[Int]]]()')
            code.append(f'{m}.put("a", Bx([1, 2, 3]))')
            code.append(f'println("{tag} {{{m}.get("a").v.len()}} {{{m}.len()}}")')
            out.append(f'{tag} 3 1')
        return Probe(code, out)

    def p_bad_listarg(self):
        """A generic init storing a list parameter: E0328 at the instance."""
        self.need('stack', '''object Stack[T] {
    var items: [T]
    init() {
        self.items = []
    }
    func push(x: T) {
        self.items.push(x)
    }
}''')
        n = self.fresh('st')
        return Probe([f'var {n} = Stack[[Int]]()', f'{n}.push([1])'], errors={'E0328'})

    def p_nested(self):
        """Instances nested several deep."""
        r = self.r
        self.need('boxes', BOXES)
        self.need('enums', ENUMS)
        code, out = [], []
        tag = self.fresh('n')
        d = r.randint(1, 8)
        v = r.randint(0, 99)
        n = self.fresh('bx')
        k = r.choice(['box', 'pair', 'opt'])
        if k == 'box':
            code.append(f'val {n} = ' + 'Bx(' * d + str(v) + ')' * d)
            code.append(f'println("{tag} {{{n}' + '.v' * d + '}")')
            out.append(f'{tag} {v}')
        elif k == 'pair':
            src = str(v)
            acc = ''
            for i in range(d):
                if r.random() < 0.5:
                    src = f'Pair({src}, "{i}")'
                    acc = '.a' + acc
                else:
                    src = f'Pair({i}, {src})'
                    acc = '.b' + acc
            code.append(f'val {n} = {src}')
            code.append(f'println("{tag} {{{n}{acc}}}")')
            out.append(f'{tag} {v}')
        else:
            src = str(v)
            for i in range(d):
                src = f'Opt.Some({src})' if r.random() < 0.6 else f'Bx({src})'
            code.append(f'val {n} = {src}')
            code.append(f'println("{tag} built")')
            out.append(f'{tag} built')
        return Probe(code, out)

    # ----------------------------------------------------- static functions
    def p_static(self):
        r = self.r
        self.need('statics', STATICS)
        tag = self.fresh('s')
        kind = r.choice(['Ctr', 'Lbl'])
        ns = [r.randint(0, 12) for _ in range(r.randint(0, 5))]
        lst = '[' + ', '.join(map(str, ns)) + ']'

        def val_of(n):
            return n * 2 if kind == 'Ctr' else len(f'L{n}')
        lab = 'made' if kind == 'Ctr' else 'lbl'
        k = r.choice(['sum', 'same', 'label', 'direct', 'many', 'fill', 'twice'])
        if k == 'sum':
            return Probe([f'println("{tag} {{make_sum[{kind}]({lst})}}")'],
                         [f'{tag} {sum(val_of(n) for n in ns)}'])
        if k == 'same':
            p = r.randint(0, 12)
            return Probe([f'println("{tag} {{count_same[{kind}]({lst}, {p})}}")'],
                         [f'{tag} {sum(1 for n in ns if val_of(n) == val_of(p) and (kind == "Ctr" or n == p))}'])
        if k == 'label':
            return Probe([f'println("{tag} {{label_of[{kind}]()}}")'],
                         [f'{tag} {lab}/{lab}:{val_of(1)}'])
        if k == 'direct':
            n = r.randint(0, 99)
            return Probe([f'println("{tag} {{{kind}.make({n}).val_of()}} {{{kind}.label()}}")'],
                         [f'{tag} {val_of(n)} {lab}'])
        if k == 'many':
            v = self.fresh('m')
            return Probe([f'val {v} = {kind}.many({lst})', f'println("{tag} {{{v}.len()}}")'],
                         [f'{tag} {len(ns)}'])
        if k == 'fill':
            a = r.randint(-5, 50)
            b = r.choice(['x', 'yy', 'zzz'])
            v = self.fresh('f')
            return Probe([f'val {v} = Ctr.fill({a}, "{b}")',
                          f'println("{tag} {{{v}.a}} {{{v}.b}} {{{v}.log.len()}} {{Ctr({a}).b}}")'],
                         [f'{tag} {a} {b} 1 init'])
        n = r.randint(0, 20)
        a, b = self.fresh('p'), self.fresh('q')
        return Probe([f'val ({a}, {b}) = Lbl.twice({n})', f'println("{tag} {{{a}.s}} {{{b}.s}}")'],
                     [f'{tag} L{n} L{n + 1}'])

    def p_bad_static(self):
        r = self.r
        self.need('statics', STATICS)
        k = r.choice(['on_value', 'on_type', 'iface_value', 'reserved', 'val_write'])
        if k == 'on_value':
            return Probe(['println("{Ctr(1).make(2).a}")'], errors={'E0350'})
        if k == 'on_type':
            return Probe(['println("{Ctr.val_of()}")'], errors={'E0351'})
        if k == 'iface_value':
            n = self.fresh('i')
            return Probe([f'val {n}: Make = Ctr(1)', f'println("{{{n}.same(Ctr(1))}}")'], errors={'E0354'})
        if k == 'reserved':
            n = self.fresh('r')
            return Probe([f'val {n} = Ctr.__fields(1, "b", [])', f'println("{{{n}.a}}")'], errors={'E0356'})
        n = self.fresh('w')
        return Probe([f'var {n} = Ctr(1)', f'{n}.a = 5', f'println("{{{n}.a}}")'], errors={'E0265'})

    def probe_kinds(self):
        return [(4, self.p_constraint), (5, self.p_hooks), (1, self.p_nocopy_ok), (3, self.p_lst),
                (3, self.p_tree), (2, self.p_opt), (3, self.p_typeargs), (2, self.p_nested),
                (4, self.p_static)]

    def error_kinds(self):
        return [self.p_bad_constraint, self.p_bad_constraint, self.p_bad_nocopy, self.p_bad_listarg,
                self.p_bad_static]


def generate(rng):
    g = GenEdgeGen(rng)
    g.program()
    return g
