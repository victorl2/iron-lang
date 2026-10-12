"""Random multi-file Iron packages for mod_fuzz.py.

A package is a set of files under src/ (nested directories included) and
vendor/ (a vendored library package with its own iron.toml and src/, or
loose files), each holding declarations that other files use: pub
functions calling private helpers and globals, globals initialized from
other files' functions (manual 5.7), objects with drop blocks, generic
objects and functions, interfaces implemented and used in other files,
enums matched elsewhere, patches, comptime calls across files, closures,
stdlib imports, and `import` lines naming local modules (cycles and
diamonds included, since imports only document dependencies).

The expected output is computed while generating (every value is a small
Int, String or Bool that Python computes the same way), so a package
carries it: `Pkg.expected`. A negative package breaks one rule of the
manual and carries the diagnostic codes one of which the build must report
(`Pkg.errors`).

Names are unique across the package (all files share one namespace,
manual 5.10), which also lets mod_fuzz.py flatten a positive package into
one file and require the same output.
"""

import random


class Bail(Exception):
    pass


def file_order(paths):
    """The order iron concatenates a package's files (manual 10.2):
    vendored files, src/lib.iron, the rest of src/ sorted by path,
    src/main.iron."""
    vend = sorted(p for p in paths if p.startswith('vendor/'))
    src = [p for p in paths if p.startswith('src/')]
    rest = sorted(p for p in src if p not in ('src/lib.iron', 'src/main.iron'))
    out = vend
    if 'src/lib.iron' in src:
        out.append('src/lib.iron')
    out += rest
    if 'src/main.iron' in src:
        out.append('src/main.iron')
    return out


def module_name(path):
    """The `import` path naming a file: src/a/b.iron -> a.b; a vendored
    package vendor/lib/src/x.iron -> lib (its directory)."""
    parts = path[:-5].split('/')
    if parts[0] == 'vendor':
        return parts[1]
    return '.'.join(parts[1:])


class Pkg:
    def __init__(self, rng):
        self.rng = rng
        self.files = {}          # path -> list of declaration texts
        self.imports = {}        # path -> set of import names
        self.main = []           # statements of main (4-space indented once)
        self.out = []            # expected lines main prints
        self.init_out = []       # (path, decl index, line): printed before main
        self.errors = None
        self.ids = 0
        self.vendor_pkgs = {}    # vendor/<name> -> True when it has iron.toml + src/
        self.extra = {}          # path -> raw file content (non-.iron files)
        self.negative = None
        self.removable = []      # (path) files nothing else uses (incr mode)
        self.styles = {}         # path -> how the file is written (encode_style)

    def nid(self, prefix):
        self.ids += 1
        return f'{prefix}{self.ids}'

    def add(self, path, text):
        self.files.setdefault(path, []).append(text.rstrip('\n'))
        return len(self.files[path]) - 1

    def imp(self, path, name):
        self.imports.setdefault(path, set()).add(name)

    def pick(self, exclude=()):
        cands = [p for p in self.files if p not in exclude and p != 'src/main.iron']
        if not cands:
            raise Bail()
        return self.rng.choice(sorted(cands))

    # -------------------------------------------------------------- render

    def render_file(self, path, flatten=False):
        lines = []
        imps = sorted(self.imports.get(path, ()))
        for i in imps:
            lines.append(f'import {i}')
        if imps:
            lines.append('')
        lines.append('\n\n'.join(self.files.get(path, [])))
        if path == 'src/main.iron':
            lines.append('')
            lines.append('func main() {')
            lines.extend('    ' + s for s in self.main)
            lines.append('}')
        return '\n'.join(lines).strip('\n') + '\n'

    def tree(self, name='pkg'):
        """{relative path: content} of the whole package."""
        t = {'iron.toml': f'[package]\nname = "{name}"\nversion = "0.1.0"\n'}
        for vp, has_toml in self.vendor_pkgs.items():
            if has_toml:
                t[f'{vp}/iron.toml'] = (f'[package]\nname = "{vp.split("/")[1]}"\n'
                                        'version = "1.0.0"\ntype = "lib"\n')
        for p in self.files:
            t[p] = encode_style(self.render_file(p), self.styles.get(p, 'lf'))
        t.update(self.extra)
        return t

    def flat(self):
        """The package as one file: the files in build order with local
        imports dropped (stdlib imports kept once, at the top)."""
        std = set()
        for p, imps in self.imports.items():
            std |= {i for i in imps if i in STDLIB}
        body = []
        for p in file_order(list(self.files)):
            saved = self.imports.get(p)
            self.imports[p] = set()
            body.append(f'-- {p}\n' + self.render_file(p))
            if saved is None:
                del self.imports[p]
            else:
                self.imports[p] = saved
        head = ''.join(f'import {i}\n' for i in sorted(std))
        return head + ('\n' if head else '') + '\n'.join(body)

    def expected(self):
        order = {p: i for i, p in enumerate(file_order(list(self.files)))}
        pre = [l for (_, _, l) in sorted(self.init_out, key=lambda t: (order[t[0]], t[1]))]
        return pre + self.out


STDLIB = ('math', 'time', 'os', 'hint', 'log')

STYLES = ('lf', 'lf', 'lf', 'crlf', 'bom', 'nonl', 'comment_end', 'bom_crlf', 'tabs')


def encode_style(text, style):
    """A file's text written one of the ways editors write files: CRLF
    line ends, a UTF-8 byte order mark, no final newline, a final comment
    without a newline, tab indentation."""
    if style == 'tabs':
        text = text.replace('    ', '\t')
    if style in ('nonl', 'comment_end'):
        text = text.rstrip('\n')
        if style == 'comment_end':
            text += '\n-- the end, no newline'
    if style in ('crlf', 'bom_crlf'):
        text = text.replace('\n', '\r\n')
    if style in ('bom', 'bom_crlf'):
        text = '\ufeff' + text
    return text


# ------------------------------------------------------------- features

def f_func_chain(pk):
    """pub functions across files, each calling a private helper and
    private globals of its own file and the next function of the chain."""
    rng = pk.rng
    depth = rng.randint(1, 4)
    fns = []
    prev = None
    for _ in range(depth):
        path = pk.pick()
        fid = pk.nid('chain_')
        a, b, g = rng.randint(1, 3), rng.randint(-5, 9), rng.randint(0, 20)
        gname = pk.nid('K_')
        kind = rng.choice(('val', 'var'))
        pk.add(path, f'{kind} {gname} = {g}')
        pk.add(path, f'func {fid}_helper(x: Int) -> Int {{\n    return x * {a} + {gname}\n}}')
        call = f' + {prev[0]}(x - 1)' if prev else ''
        pk.add(path, f'pub func {fid}(x: Int) -> Int {{\n    return {fid}_helper(x) + {b}{call}\n}}')
        if prev and rng.random() < 0.5:
            pk.imp(path, module_name(prev[1]))
        p = prev

        def ev(x, a=a, b=b, g=g, p=p):
            return x * a + g + b + (p[2](x - 1) if p else 0)
        prev = (fid, path, ev)
        fns.append(prev)
    x = rng.randint(0, 9)
    pk.main.append(f'println("{prev[0]} {{{prev[0]}({x})}}")')
    pk.out.append(f'{prev[0]} {prev[2](x)}')


def f_globals(pk):
    """A global initialized from a function of another file that reads
    that file's own global (order across files, manual 5.7), and a var
    global mutated through pub functions."""
    rng = pk.rng
    a = pk.pick()
    b = pk.pick()
    gid = pk.nid('G')
    va = rng.randint(1, 50)
    k, c = rng.randint(1, 4), rng.randint(0, 9)
    pk.add(a, f'val {gid}_base = {va}')
    pk.add(a, f'pub func {gid}_get() -> Int {{\n    return {gid}_base\n}}')
    pk.add(b, f'val {gid}_derived = {gid}_get() * {k} + {c}')
    pk.add(b, f'pub func {gid}_derived_get() -> Int {{\n    return {gid}_derived\n}}')
    start, step, n = rng.randint(0, 9), rng.randint(1, 5), rng.randint(0, 4)
    m = pk.pick()
    pk.add(m, f'var {gid}_count = {start}')
    pk.add(m, f'pub func {gid}_bump() -> Int {{\n    {gid}_count += {step}\n    return {gid}_count\n}}')
    if rng.random() < 0.5:
        pk.imp(b, module_name(a))
    pk.main.append(f'println("{gid} {{{gid}_derived_get()}}")')
    pk.out.append(f'{gid} {va * k + c}')
    for i in range(n):
        pk.main.append(f'println("{gid} bump {{{gid}_bump()}}")')
        pk.out.append(f'{gid} bump {start + step * (i + 1)}')


def f_init_side_effect(pk):
    """A global nothing reads, whose initializer prints: it runs before
    main, in file order (manual 5.7)."""
    p = pk.pick()
    gid = pk.nid('U')
    pk.add(p, f'func {gid}_note() -> Int {{\n    println("init {gid}")\n    return 1\n}}')
    idx = pk.add(p, f'val {gid}_unread = {gid}_note()')
    pk.init_out.append((p, idx, f'init {gid}'))


def f_object_drop(pk):
    """A pub object with a drop block in one file, made in a second and
    used and dropped in a third."""
    rng = pk.rng
    a, b, c = pk.pick(), pk.pick(), pk.pick()
    oid = pk.nid('Res')
    pk.add(a, f'pub object {oid} {{\n    pub val id: Int\n    val secret: Int\n\n'
              f'    pub init(id: Int) {{\n        self.id = id\n        self.secret = id * 2\n    }}\n\n'
              f'    pub readonly func twice() -> Int {{\n        return self.secret\n    }}\n\n'
              f'    drop {{\n        println("drop {oid} {{self.id}}")\n    }}\n}}')
    pk.add(b, f'pub func make_{oid}(i: Int) -> {oid} {{\n    return {oid}(i)\n}}')
    n = rng.randint(1, 3)
    body = []
    exp = []
    for i in range(n):
        v = rng.randint(0, 99)
        body.append(f'    val r{i} = make_{oid}({v})')
        body.append(f'    println("use {oid} {{r{i}.id}} {{r{i}.twice()}}")')
        exp.append(f'use {oid} {v} {v * 2}')
    drops = [f'drop {oid} {l.split()[2]}' for l in exp][::-1]
    pk.add(c, f'pub func use_{oid}() {{\n' + '\n'.join(body) + '\n}')
    if rng.random() < 0.5:
        pk.imp(c, module_name(b))
    pk.main.append(f'use_{oid}()')
    pk.out += exp + drops


def f_generic(pk):
    """A generic object (pub fields) and a generic function in one file,
    instantiated with Int and String in others."""
    rng = pk.rng
    a, b = pk.pick(), pk.pick()
    gid = pk.nid('Gen')
    pk.add(a, f'pub object {gid}Box[T] {{\n    pub val v: T\n    pub val n: Int\n}}')
    pk.add(a, f'pub func {gid}_pick[T](x: T, y: T, first: Bool) -> T {{\n'
              f'    if first {{\n        return x\n    }}\n    return y\n}}')
    x, y, n = rng.randint(0, 50), rng.randint(0, 50), rng.randint(0, 9)
    first = rng.random() < 0.5
    fl = 'true' if first else 'false'
    s1, s2 = rng.choice(('ab', 'xyz', 'q')), rng.choice(('cd', 'w', 'mn'))
    pk.add(b, f'pub func {gid}_use() -> String {{\n'
              f'    val bx = {gid}Box[Int]({x}, {n})\n'
              f'    val sb = {gid}Box[String]("{s1}", {n + 1})\n'
              f'    val p = {gid}_pick({x}, {y}, {fl})\n'
              f'    val q = {gid}_pick[String]("{s1}", "{s2}", {fl})\n'
              f'    return "{{bx.v + bx.n}} {{sb.v}}{{sb.n}} {{p}} {{q}}"\n}}')
    pk.main.append(f'println("{gid} {{{gid}_use()}}")')
    pk.out.append(f'{gid} {x + n} {s1}{n + 1} {x if first else y} {s1 if first else s2}')


def f_iface(pk):
    """A pub interface in one file, two implementors in others, used
    through a list of the interface type in a fourth file."""
    rng = pk.rng
    a, b, c, d = pk.pick(), pk.pick(), pk.pick(), pk.pick()
    iid = pk.nid('Shape')
    pk.add(a, f'pub interface {iid} {{\n    readonly func area() -> Int\n'
              f'    readonly func label() -> String {{\n        return "shape"\n    }}\n}}')
    pk.add(b, f'pub object {iid}Sq impl {iid} {{\n    val s: Int\n\n'
              f'    pub init(s: Int) {{\n        self.s = s\n    }}\n\n'
              f'    pub readonly func area() -> Int {{\n        return self.s * self.s\n    }}\n}}')
    pk.add(c, f'pub object {iid}Rect impl {iid} {{\n    val w: Int\n    val h: Int\n\n'
              f'    pub init(w: Int, h: Int) {{\n        self.w = w\n        self.h = h\n    }}\n\n'
              f'    pub readonly func area() -> Int {{\n        return self.w * self.h\n    }}\n\n'
              f'    pub readonly func label() -> String {{\n        return "rect"\n    }}\n}}')
    items, exp, total = [], [], 0
    for _ in range(rng.randint(1, 4)):
        if rng.random() < 0.5:
            s = rng.randint(0, 9)
            items.append(f'{iid}Sq({s})')
            exp.append(f'shape {s * s}')
            total += s * s
        else:
            w, h = rng.randint(0, 9), rng.randint(0, 9)
            items.append(f'{iid}Rect({w}, {h})')
            exp.append(f'rect {w * h}')
            total += w * h
    pk.add(d, f'pub func {iid}_total() -> Int {{\n'
              f'    val xs: [{iid}] = [{", ".join(items)}]\n'
              f'    var t = 0\n    for s in xs {{\n'
              f'        println("{{s.label()}} {{s.area()}}")\n'
              f'        t += s.area()\n    }}\n    return t\n}}')
    pk.main.append(f'println("{iid} {{{iid}_total()}}")')
    pk.out += exp + [f'{iid} {total}']


def f_enum(pk):
    rng = pk.rng
    a, b = pk.pick(), pk.pick()
    eid = pk.nid('Tok')
    pk.add(a, f'pub enum {eid} {{\n    Num(Int),\n    Word(String),\n    End,\n}}')
    pk.add(b, f'pub func {eid}_show(t: {eid}) -> String {{\n    match t {{\n'
              f'        {eid}.Num(n) -> return "num {{n}}"\n'
              f'        {eid}.Word(w) -> return "word {{w}}"\n'
              f'        {eid}.End -> return "end"\n    }}\n    return ""\n}}')
    for _ in range(rng.randint(1, 3)):
        r = rng.randint(0, 2)
        if r == 0:
            v = rng.randint(-9, 99)
            pk.main.append(f'println({eid}_show({eid}.Num({v})))')
            pk.out.append(f'num {v}')
        elif r == 1:
            w = rng.choice(('hi', 'x', 'iron'))
            pk.main.append(f'println({eid}_show({eid}.Word("{w}")))')
            pk.out.append(f'word {w}')
        else:
            pk.main.append(f'println({eid}_show({eid}.End))')
            pk.out.append('end')


def f_patch(pk):
    """An object in one file, a patch adding a method in another."""
    rng = pk.rng
    a, b = pk.pick(), pk.pick()
    oid = pk.nid('Pt')
    k = rng.randint(1, 5)
    pk.add(a, f'pub object {oid} {{\n    pub val x: Int\n}}')
    pk.add(b, f'pub patch object {oid} {{\n    pub readonly func scaled() -> Int {{\n'
              f'        return self.x * {k}\n    }}\n}}')
    x = rng.randint(0, 99)
    pk.main.append(f'println("{oid} {{{oid}({x}).scaled()}}")')
    pk.out.append(f'{oid} {x * k}')


def f_pub_global(pk):
    """A pub val and a pub var of one file, read and written from main.iron
    (the last file: a global is visible from its declaration on, #577)."""
    rng = pk.rng
    a = pk.pick()
    gid = pk.nid('Gl')
    k = rng.randint(1, 50)
    pk.add(a, f'pub val {gid}_K = {k}')
    pk.add(a, f'pub var {gid}_n = 0')
    pk.add(a, f'pub func {gid}_bump() -> Int {{\n    {gid}_n = {gid}_n + {gid}_K\n'
              f'    return {gid}_n\n}}')
    pk.main.append(f'val {gid}_first = {gid}_bump()')
    pk.main.append(f'{gid}_n = {gid}_n + {gid}_K')
    pk.main.append(f'println("{gid} {{{gid}_first}} {{{gid}_bump()}} {{{gid}_n}} {{{gid}_K}}")')
    pk.out.append(f'{gid} {k} {3 * k} {3 * k} {k}')


def f_comptime(pk):
    """A function of one file evaluated with comptime in another."""
    rng = pk.rng
    a, b = pk.pick(), pk.pick()
    cid = pk.nid('ct')
    n = rng.randint(0, 30)
    m = rng.randint(1, 4)
    pk.add(a, f'pub func {cid}_sum(n: Int) -> Int {{\n    var s = 0\n'
              f'    for i in range(n) {{\n        s += i * {m}\n    }}\n    return s\n}}')
    pk.add(b, f'val {cid}_value = comptime {cid}_sum({n})')
    pk.add(b, f'pub func {cid}_get() -> Int {{\n    return {cid}_value\n}}')
    pk.main.append(f'println("{cid} {{{cid}_get()}}")')
    pk.out.append(f'{cid} {sum(i * m for i in range(n))}')


def f_closure(pk):
    rng = pk.rng
    a, b = pk.pick(), pk.pick()
    cid = pk.nid('clo')
    k = rng.randint(-5, 9)
    pk.add(a, f'pub func {cid}_adder(k: Int) -> func(Int) -> Int {{\n'
              f'    return func(x: Int) -> Int {{ return x + k }}\n}}')
    x = rng.randint(0, 50)
    pk.add(b, f'pub func {cid}_apply(f: func(Int) -> Int, x: Int) -> Int {{\n    return f(f(x))\n}}')
    pk.main.append(f'println("{cid} {{{cid}_apply({cid}_adder({k}), {x})}}")')
    pk.out.append(f'{cid} {x + 2 * k}')


def f_stdlib(pk):
    """A stdlib import in the file that uses it."""
    rng = pk.rng
    a = pk.pick()
    sid = pk.nid('std')
    k = rng.randint(0, 9)
    pk.imp(a, 'math')
    pk.add(a, f'pub func {sid}_calc() -> Int {{\n    return Math.random_int({k}, {k}) + Int(Math.floor(2.7))\n}}')
    pk.main.append(f'println("{sid} {{{sid}_calc()}}")')
    pk.out.append(f'{sid} {k + 2}')


def f_string_const(pk):
    rng = pk.rng
    a = pk.pick()
    sid = pk.nid('S')
    txt = rng.choice(('hello', 'naïve', 'a b', '日本', ''))
    pk.add(a, f'val {sid}_text = "{txt}"')
    pk.add(a, f'pub func {sid}_len() -> Int {{\n    return {sid}_text.len()\n}}')
    pk.add(a, f'pub func {sid}_get() -> String {{\n    return {sid}_text\n}}')
    pk.main.append(f'println("{sid} [{{{sid}_get()}}]")')
    pk.out.append(f'{sid} [{txt}]')


def f_generic_drop(pk):
    """A generic container in one file holding an object with a drop block
    from another, filled and dropped in a third; nested instances."""
    rng = pk.rng
    a, b, c = pk.pick(), pk.pick(), pk.pick()
    gid = pk.nid('Hold')
    pk.add(a, f'pub object {gid}Item {{\n    pub val k: Int\n\n'
              f'    drop {{\n        println("drop {gid}Item {{self.k}}")\n    }}\n}}')
    pk.add(b, f'pub object {gid}[T] {{\n    pub var items: [T]\n\n'
              f'    pub init() {{\n        self.items = []\n    }}\n\n'
              f'    pub func put(x: T) {{\n        self.items.push(x)\n    }}\n\n'
              f'    pub readonly func size() -> Int {{\n        return len(self.items)\n    }}\n}}')
    n = rng.randint(0, 3)
    ks = [rng.randint(0, 99) for _ in range(n)]
    body = [f'    var h = {gid}[{gid}Item]()']
    body += [f'    h.put({gid}Item({k}))' for k in ks]
    body.append(f'    var hh = {gid}[String]()')
    body.append(f'    hh.put("s{n}")')
    body.append(f'    println("{gid} {{h.size()}} {{hh.size()}}")')
    pk.add(c, f'pub func {gid}_run() {{\n' + '\n'.join(body) + '\n}')
    pk.main.append(f'{gid}_run()')
    # put(x) pushes a copy of its parameter, which drops when put returns
    pk.out += [f'drop {gid}Item {k}' for k in ks]
    pk.out.append(f'{gid} {n} 1')
    pk.out += [f'drop {gid}Item {k}' for k in ks]


def f_rc_shared(pk):
    """An rc object made in one file, shared by another, dropped once."""
    rng = pk.rng
    a, b = pk.pick(), pk.pick()
    rid = pk.nid('Shared')
    v = rng.randint(0, 99)
    pk.add(a, f'pub object {rid} {{\n    pub val v: Int\n\n'
              f'    drop {{\n        println("drop {rid} {{self.v}}")\n    }}\n}}')
    pk.add(a, f'pub func {rid}_make(v: Int) -> rc {rid} {{\n    return rc {rid}(v)\n}}')
    pk.add(b, f'pub func {rid}_sum(x: rc {rid}, y: rc {rid}) -> Int {{\n    return x.v + y.v\n}}')
    pk.add(b, f'pub func {rid}_run() {{\n    val r = {rid}_make({v})\n    val r2 = r\n'
              f'    println("{rid} {{{rid}_sum(r, r2)}}")\n}}')
    pk.main.append(f'{rid}_run()')
    pk.out += [f'{rid} {2 * v}', f'drop {rid} {v}']


def f_copy_block(pk):
    """An object with a copy block in one file, copied in another."""
    rng = pk.rng
    a, b = pk.pick(), pk.pick()
    cid = pk.nid('Cp')
    v = rng.randint(0, 50)
    pk.add(a, f'pub object {cid} {{\n    pub var n: Int\n\n    pub init(n: Int) {{\n        self.n = n\n    }}\n\n'
              f'    copy {{\n        self.n = self.n + 1000\n    }}\n}}')
    pk.add(b, f'pub func {cid}_run() -> Int {{\n    val x = {cid}({v})\n    var y = x\n'
              f'    y.n = y.n + 1\n    return x.n * 10000 + y.n\n}}')
    pk.main.append(f'println("{cid} {{{cid}_run()}}")')
    pk.out.append(f'{cid} {v * 10000 + v + 1001}')


def f_generic_enum(pk):
    rng = pk.rng
    a, b = pk.pick(), pk.pick()
    eid = pk.nid('Opt')
    pk.add(a, f'pub enum {eid}[T] {{\n    Some(T),\n    None,\n}}')
    pk.add(b, f'pub func {eid}_or[T](o: {eid}[T], d: T) -> T {{\n    match o {{\n'
              f'        {eid}.Some(v) -> return v\n        {eid}.None -> return d\n    }}\n    return d\n}}')
    v, d = rng.randint(0, 99), rng.randint(0, 99)
    some = rng.random() < 0.5
    if some:
        pk.main.append(f'println("{eid} {{{eid}_or({eid}.Some({v}), {d})}}")')
        pk.out.append(f'{eid} {v}')
    else:
        pk.main.append(f'val {eid}_n: {eid}[Int] = {eid}.None')
        pk.main.append(f'println("{eid} {{{eid}_or({eid}_n, {d})}}")')
        pk.out.append(f'{eid} {d}')
    pk.main.append(f'println({eid}_or({eid}.Some("s{v}"), "none"))')
    pk.out.append(f's{v}')


def f_global_object(pk):
    """A global object (fields read through pub functions) initialized
    from a constructor of another file."""
    rng = pk.rng
    a, b = pk.pick(), pk.pick()
    gid = pk.nid('GObj')
    x, y = rng.randint(0, 50), rng.randint(0, 50)
    pk.add(a, f'pub object {gid} {{\n    pub val x: Int\n    pub val label: String\n}}')
    pk.add(a, f'pub func {gid}_new(x: Int) -> {gid} {{\n    return {gid}(x, "g{{x}}")\n}}')
    pk.add(b, f'val {gid}_inst = {gid}_new({x} + {gid}_offset())')
    pk.add(b, f'val {gid}_off = {y}')
    pk.add(b, f'func {gid}_offset() -> Int {{\n    return {gid}_off\n}}')
    pk.add(b, f'pub func {gid}_show() -> String {{\n    return "{{{gid}_inst.x}} {{{gid}_inst.label}}"\n}}')
    pk.main.append(f'println("{gid} {{{gid}_show()}}")')
    pk.out.append(f'{gid} {x + y} g{x + y}')


def f_iface_generic_fn(pk):
    """A generic function constrained by an interface of another file,
    called with an implementor of a third."""
    rng = pk.rng
    a, b, c = pk.pick(), pk.pick(), pk.pick()
    iid = pk.nid('Val')
    pk.add(a, f'pub interface {iid} {{\n    readonly func value() -> Int\n}}')
    pk.add(b, f'pub func {iid}_twice[T: {iid}](x: T) -> Int {{\n    return x.value() * 2\n}}')
    k = rng.randint(0, 99)
    pk.add(c, f'pub object {iid}Num impl {iid} {{\n    pub val n: Int\n\n'
              f'    pub readonly func value() -> Int {{\n        return self.n\n    }}\n}}')
    pk.main.append(f'println("{iid} {{{iid}_twice({iid}Num({k}))}}")')
    pk.out.append(f'{iid} {2 * k}')


def f_closure_global(pk):
    """A closure stored in a global of one file, capturing nothing, and a
    counter closure over a var global of another."""
    rng = pk.rng
    a, b = pk.pick(), pk.pick()
    cid = pk.nid('cg')
    k = rng.randint(1, 9)
    pk.add(a, f'var {cid}_total = 0')
    pk.add(a, f'pub func {cid}_add(x: Int) {{\n    {cid}_total += x\n}}')
    pk.add(a, f'pub func {cid}_get() -> Int {{\n    return {cid}_total\n}}')
    pk.add(b, f'pub func {cid}_run(n: Int) -> Int {{\n    val f = func(i: Int) {{ {cid}_add(i * {k}) }}\n'
              f'    for i in range(n) {{\n        f(i)\n    }}\n    return {cid}_get()\n}}')
    n = rng.randint(0, 6)
    pk.main.append(f'println("{cid} {{{cid}_run({n})}}")')
    pk.out.append(f'{cid} {sum(i * k for i in range(n))}')


FEATURES = [f_pub_global, f_generic_drop, f_rc_shared, f_copy_block, f_generic_enum, f_global_object,
            f_iface_generic_fn, f_closure_global, f_func_chain, f_globals, f_init_side_effect, f_object_drop, f_generic,
            f_iface, f_enum, f_patch, f_comptime, f_closure, f_stdlib, f_string_const]


# --------------------------------------------------------------- layout

SRC_NAMES = ('alpha', 'beta', 'gamma', 'delta', 'util', 'core', 'shapes', 'zeta', 'io_helpers',
             'model', 'z_last', 'a_first', 'lib2')
DIR_NAMES = ('geom', 'util', 'deep', 'a', 'b', 'models')


def layout(pk):
    rng = pk.rng
    paths = {'src/main.iron'}
    n = rng.randint(1, 7)
    if rng.random() < 0.2:
        paths.add('src/lib.iron')
    for _ in range(n):
        name = rng.choice(SRC_NAMES)
        r = rng.random()
        if r < 0.55:
            paths.add(f'src/{name}.iron')
        elif r < 0.85:
            d = '/'.join(rng.choice(DIR_NAMES) for _ in range(rng.randint(1, 3)))
            paths.add(f'src/{d}/{name}.iron')
        elif r < 0.93:
            lib = rng.choice(('greeter', 'mathx', 'strs'))
            pk.vendor_pkgs[f'vendor/{lib}'] = True
            paths.add(f'vendor/{lib}/src/{rng.choice(("lib", name))}.iron')
        else:
            lib = rng.choice(('loose', 'third'))
            d = rng.choice(('', 'nested/'))
            pk.vendor_pkgs.setdefault(f'vendor/{lib}', False)
            paths.add(f'vendor/{lib}/{d}{name}.iron')
    # a module and a directory of the same name would make `import x`
    # ambiguous; keep it, imports only document
    for p in sorted(paths):
        pk.files[p] = []
        pk.styles[p] = rng.choice(STYLES)
    # vendored packages with iron.toml compile only src/: a loose file
    # put directly in such a package directory would be skipped
    for p in list(pk.files):
        parts = p.split('/')
        if parts[0] == 'vendor' and pk.vendor_pkgs.get('/'.join(parts[:2])) and parts[2] != 'src':
            del pk.files[p]


def random_imports(pk):
    """Local imports between files: random edges (cycles and diamonds
    happen), all valid since they name package files."""
    rng = pk.rng
    paths = sorted(pk.files)
    for p in paths:
        for _ in range(rng.randint(0, 2)):
            q = rng.choice(paths)
            if q != p and not q.endswith('main.iron'):
                pk.imp(p, module_name(q))


# ------------------------------------------------------------ negatives

def neg_private_func(pk):
    a = pk.pick()
    hid = pk.nid('hidden_')
    pk.add(a, f'func {hid}() -> Int {{\n    return 1\n}}')
    pk.main.append(f'println("{{{hid}()}}")')
    return {'E0320'}


def neg_private_object(pk):
    a = pk.pick()
    oid = pk.nid('Priv')
    pk.add(a, f'object {oid} {{\n    val x: Int\n}}')
    pk.main.append(f'val p = {oid}(1)')
    pk.main.append('println("{p.x}")')
    return {'E0320'}


def neg_private_field(pk):
    a = pk.pick()
    oid = pk.nid('Half')
    pk.add(a, f'pub object {oid} {{\n    pub val x: Int\n    val y: Int\n}}')
    pk.add(a, f'pub func make_{oid}() -> {oid} {{\n    return {oid}(1, 2)\n}}')
    pk.main.append(f'val h = make_{oid}()')
    pk.main.append('println("{h.y}")')
    return {'E0320'}


def neg_private_method(pk):
    a = pk.pick()
    oid = pk.nid('Meth')
    pk.add(a, f'pub object {oid} {{\n    pub val x: Int\n\n    readonly func secret() -> Int {{\n'
              f'        return self.x\n    }}\n}}')
    pk.main.append(f'val m = {oid}(1)')
    pk.main.append('println("{m.secret()}")')
    return {'E0320'}


def neg_dup_pub(pk):
    a = pk.pick()
    b = pk.pick(exclude=(a,))
    did = pk.nid('dup_')
    pk.add(a, f'pub func {did}() -> Int {{\n    return 1\n}}')
    pk.add(b, f'pub func {did}() -> Int {{\n    return 2\n}}')
    pk.main.append(f'println("{{{did}()}}")')
    return {'E0201'}


def neg_dup_private(pk):
    a = pk.pick()
    b = pk.pick(exclude=(a,))
    did = pk.nid('same_')
    kind = pk.rng.choice(('func', 'val', 'object'))
    for p, v in ((a, 1), (b, 2)):
        if kind == 'func':
            pk.add(p, f'func {did}() -> Int {{\n    return {v}\n}}')
        elif kind == 'val':
            pk.add(p, f'val {did} = {v}')
        else:
            pk.add(p, f'object {did} {{\n    val v{v}: Int\n}}')
    return {'E0201'}


def neg_bad_import(pk):
    a = pk.rng.choice(sorted(pk.files))
    pk.imp(a, pk.rng.choice(('nosuch', 'util.nosuch', 'mathh', 'a.b.c.d', 'main.nosuch')))
    return {'E0209'}


def neg_import_as(pk):
    a = pk.rng.choice(sorted(pk.files))
    pk.files[a].insert(0, 'import math as m')
    return {'E0101', 'E0209'}


def neg_private_global(pk):
    """A global without pub is private to its file."""
    a = pk.pick()
    gid = pk.nid('PG')
    pk.add(a, f'{pk.rng.choice(("val", "var"))} {gid} = 3')
    pk.main.append(f'println("{{{gid}}}")')
    return {'E0320'}


def neg_global_cycle(pk):
    a = pk.pick()
    b = pk.pick()
    cid = pk.nid('cyc')
    pk.add(a, f'val {cid}_a = {cid}_get_b() + 1')
    pk.add(a, f'pub func {cid}_get_a() -> Int {{\n    return {cid}_a\n}}')
    pk.add(b, f'val {cid}_b = {cid}_get_a() + 1')
    pk.add(b, f'pub func {cid}_get_b() -> Int {{\n    return {cid}_b\n}}')
    pk.main.append(f'println("{{{cid}_get_a()}}")')
    return {'E0345'}


def neg_private_keyword(pk):
    a = pk.pick()
    pk.add(a, f'private func {pk.nid("pk_")}() {{\n}}')
    return {'E0101'}


NEGATIVES = [neg_private_func, neg_private_object, neg_private_field, neg_private_method,
             neg_dup_pub, neg_dup_private, neg_bad_import, neg_import_as, neg_private_global,
             neg_global_cycle, neg_private_keyword]


def generate(rng, negative=None):
    """A random package. negative: None (random), True or False."""
    pk = Pkg(rng)
    layout(pk)
    nfeat = rng.randint(2, 9)
    for _ in range(nfeat):
        rng.choice(FEATURES)(pk)
    if rng.random() < 0.6:
        random_imports(pk)
    if negative is None:
        negative = rng.random() < 0.25
    if negative:
        neg = rng.choice(NEGATIVES)
        pk.negative = neg.__name__
        pk.errors = neg(pk)
    # files a feature never used still exist (empty files are legal)
    return pk


if __name__ == '__main__':
    import sys
    pk = generate(random.Random(sys.argv[1] if len(sys.argv) > 1 else 1))
    for p, c in sorted(pk.tree().items()):
        print(f'==> {p}\n{c}')
    print('-- expected')
    print('\n'.join(pk.expected()))
    print('-- errors', pk.errors)
