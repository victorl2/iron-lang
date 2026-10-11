"""Metamorphic variants of generated programs for cg_fuzz.py (`meta`).

A program from any prog_fuzz.py generator is rewritten by transforms that
keep its meaning, and the rewritten program must print what the original
is expected to print (and panic or fail to build the same way).

Transforms on the generator's tree (pf_lang nodes; the model is run again
on the result, so a transform the model disagrees with is dropped):
    wrap      the statements from some point to the end of a block go in a
              bare block of their own (scope and destruction order kept)
    temp      the leading expressions of a println, and the initializer of
              a binding, go in temporaries first (evaluation order kept)
    fold      a constant subexpression is replaced by its value (only
              values every integer type holds, and Bool comparisons)
    outline   a closed expression printed by println becomes a function
    negate    `if c {A} else {B}` becomes `if not (c) {B} else {A}`
    unchain   `if a {..} elif b {..} else {..}` becomes nested if / else
    ifmatch   `if x == K {A} else {B}` becomes `match x { K -> A else -> B }`
              (typed only by the compiler: a variant it rejects is dropped)
    forwhile  `for i in range(n)` becomes a while loop with a counter
    dead      statements that never run (`if false`, `while false`) and
              unused bindings are inserted
Transforms on the source text (any generator):
    rename    every local binding, parameter and function gets a new name
              (not fields or methods; inside string interpolations too)
    reorder   top-level functions, types and interfaces are permuted (the
              globals keep their order among themselves)
    deadfn    unused functions, objects and enums are added
A variant applies a random mix of transforms and is built in a random
build mode of cg_fuzz.py.
"""
import os
import random
import re
import shutil

import cg_fuzz
import prog_fuzz
from pf_lang import (Bail, Bin, Block, BareBlock, Decl, Assign, ForRange, If, Interp, Lit,
                     MatchInt, Paren, Print, Raw, Stmt, Un, Var, While, Continue, Return,
                     Break, ExprStmt, block_lines)

VARIANTS = 3


class RawStmt(Stmt):
    """Source lines that do nothing when run (dead code)."""

    def __init__(self, lines_):
        self.ls = lines_

    def lines(self, ind):
        return [ind + l for l in self.ls]

    def run(self, m, f):
        pass


class Ctx:
    def __init__(self, rng):
        self.r = rng
        self.n = 0
        self.extra_tops = []     # extra top-level declarations (lines)
        self.risky = False       # a transform the compiler alone types was applied
        self.applied = []

    def fresh(self, p):
        self.n += 1
        return f'mt{p}{self.n}'


def blocks_of(g):
    out = []
    roots = [g.mainblock] + [fe[0].body if isinstance(fe, (tuple, list)) else fe.body
                             for fe in getattr(g, 'funcs', [])]
    prog_fuzz.child_blocks(roots, out, set())
    return out


def is_closed(e):
    """An expression of literals and operators only."""
    if isinstance(e, Lit):
        return not isinstance(e.v, str) or True
    if isinstance(e, Bin):
        return is_closed(e.a) and is_closed(e.b)
    if isinstance(e, Un):
        return is_closed(e.a)
    if isinstance(e, Paren):
        return is_closed(e.a)
    return False


def is_scalar_expr(e):
    """An expression whose value is a number, Bool or String (no object
    that a temporary would destroy later)."""
    if isinstance(e, Bin):
        return e.op in ('+', '-', '*', '/', '%', '&', '|', '^', '<<', '>>', '==', '!=', '<',
                        '>', '<=', '>=', 'and', 'or')
    if isinstance(e, Un):
        return True
    if isinstance(e, Lit):
        return True
    if isinstance(e, Paren):
        return is_scalar_expr(e.a)
    return False


def has(node, cls, seen=None):
    """node contains an instance of cls (statements and expressions)."""
    seen = seen if seen is not None else set()
    if id(node) in seen:
        return False
    seen.add(id(node))
    if isinstance(node, cls):
        return True
    if isinstance(node, (list, tuple)):
        return any(has(x, cls, seen) for x in node)
    d = getattr(node, '__dict__', None)
    if d is None:
        return False
    for k, v in d.items():
        if k in ('enum', 'fn', 'frame'):
            continue
        if isinstance(v, (list, tuple)) or hasattr(v, '__dict__'):
            if has(v, cls, seen):
                return True
    return False


def model_value(e):
    from pf_lang import Machine, Frame
    return e.ev(Machine(), Frame())


# ------------------------------------------------------------ tree transforms

def t_wrap(c, g):
    bs = [b for b in blocks_of(g) if len(b.stmts) >= 2]
    if not bs:
        return False
    b = c.r.choice(bs)
    i = c.r.randrange(len(b.stmts))
    b.stmts[i:] = [BareBlock(Block(b.stmts[i:]))]
    return True


def t_temp(c, g):
    sites = []
    for b in blocks_of(g):
        for i, s in enumerate(b.stmts):
            if type(s) is Print and isinstance(s.e, Interp):
                k = 0
                for p in s.e.parts:
                    if isinstance(p, str):
                        continue
                    if not is_scalar_expr(p):
                        break
                    k += 1
                if k:
                    sites.append((b, i, 'print'))
            elif type(s) is Decl and is_scalar_expr(s.expr):
                sites.append((b, i, 'decl'))
    if not sites:
        return False
    b, i, kind = c.r.choice(sites)
    s = b.stmts[i]
    pre = []
    if kind == 'print':
        parts = []
        stop = False
        for p in s.e.parts:
            if isinstance(p, str) or stop:
                parts.append(p)
                continue
            if not is_scalar_expr(p):
                stop = True
                parts.append(p)
                continue
            t = c.fresh('t')
            pre.append(Decl(t, p))
            parts.append(Var(t))
        s.e = Interp(parts)
    else:
        t = c.fresh('t')
        pre.append(Decl(t, s.expr, ann=s.ann))
        s.expr = Var(t)
    b.stmts[i:i] = pre
    return True


def fold_sites(e, out, parent=None, key=None):
    if isinstance(e, Bin) and isinstance(e.a, Lit) and isinstance(e.b, Lit) and \
            not isinstance(e.a.v, str) and not isinstance(e.b.v, str):
        out.append((parent, key, e))
    for k in ('a', 'b'):
        x = getattr(e, k, None)
        if isinstance(x, (Bin, Un, Paren)):
            fold_sites(x, out, e, k)


def expr_slots(g):
    """(holder, attribute or index, expression) for every expression the
    tree transforms may rewrite in place."""
    out = []
    for b in blocks_of(g):
        for s in b.stmts:
            if type(s) is Print and isinstance(s.e, Interp):
                for j, p in enumerate(s.e.parts):
                    if not isinstance(p, str):
                        out.append((s.e.parts, j, p))
            elif type(s) is Decl:
                out.append((s, 'expr', s.expr))
            elif type(s) is If:
                for j, (cnd, blk) in enumerate(s.arms):
                    out.append((s, ('arm', j), cnd))
    return out


def set_slot(holder, k, v):
    if isinstance(k, tuple):
        holder.arms[k[1]] = (v, holder.arms[k[1]][1])
    elif isinstance(k, int):
        holder[k] = v
    else:
        setattr(holder, k, v)


def t_fold(c, g):
    sites = []
    for holder, k, e in expr_slots(g):
        found = []
        fold_sites(e, found, holder, k)
        for parent, key, x in found:
            sites.append((holder, k, parent, key, x))
    c.r.shuffle(sites)
    for holder, k, parent, key, x in sites:
        try:
            v = model_value(x)
        except Exception:
            continue
        a, b = x.a.v, x.b.v
        if isinstance(v, bool):
            if isinstance(a, bool) != isinstance(b, bool):
                continue
        elif isinstance(v, int):
            if isinstance(a, bool) or isinstance(b, bool):
                continue
            # values every integer type holds, from operands that do too
            if not (0 <= v <= 127 and 0 <= a <= 127 and 0 <= b <= 127):
                continue
        else:
            continue
        lit = Lit(v)
        if parent is holder and key == k:
            set_slot(holder, k, lit)
        else:
            setattr(parent, key, lit)
        return True
    return False


def t_outline(c, g):
    sites = []
    for b in blocks_of(g):
        for s in b.stmts:
            if type(s) is Print and isinstance(s.e, Interp):
                for j, p in enumerate(s.e.parts):
                    if not isinstance(p, str) and is_closed(p) and not isinstance(p, Lit):
                        sites.append((s.e.parts, j, p))
    c.r.shuffle(sites)
    for parts, j, p in sites:
        try:
            v = model_value(p)
        except Exception:
            continue
        if isinstance(v, bool):
            t = 'Bool'
        elif isinstance(v, int):
            t = 'Int'
        elif isinstance(v, str):
            t = 'String'
        else:
            continue
        name = c.fresh('o')
        c.extra_tops.append([f'func {name}() -> {t} {{', f'    return {p.src()}', '}'])
        parts[j] = Raw(f'{name}()', (lambda e: lambda m, f: e.ev(m, f))(p))
        return True
    return False


def ifs(g):
    return [(b, i, s) for b in blocks_of(g) for i, s in enumerate(b.stmts) if type(s) is If]


def t_negate(c, g):
    sites = [(b, i, s) for b, i, s in ifs(g) if len(s.arms) == 1 and s.els is not None]
    if not sites:
        return False
    b, i, s = c.r.choice(sites)
    cnd, blk = s.arms[0]
    b.stmts[i] = If([(Un('not', Paren(cnd)), s.els)], blk)
    return True


def t_unchain(c, g):
    sites = [(b, i, s) for b, i, s in ifs(g) if len(s.arms) >= 2]
    if not sites:
        return False
    b, i, s = c.r.choice(sites)
    b.stmts[i] = If([s.arms[0]], Block([If(s.arms[1:], s.els)]))
    return True


def t_ifmatch(c, g):
    sites = []
    for b, i, s in ifs(g):
        cnd = s.arms[0][0]
        if isinstance(cnd, Bin) and cnd.op == '==' and isinstance(cnd.b, Lit) and \
                type(cnd.b.v) is int and isinstance(cnd.a, Var):
            sites.append((b, i, s))
    if not sites:
        return False
    b, i, s = c.r.choice(sites)
    cnd, blk = s.arms[0]
    rest = If(s.arms[1:], s.els) if len(s.arms) > 1 else None
    els = Block([rest]) if rest else (s.els or Block([]))
    b.stmts[i] = MatchInt(cnd.a, [(cnd.b.src(), cnd.b.v, blk)], els)
    c.risky = True
    return True


def t_forwhile(c, g):
    sites = [(b, i, s) for b in blocks_of(g) for i, s in enumerate(b.stmts)
             if type(s) is ForRange and not has(s.body, Continue)]
    if not sites:
        return False
    b, i, s = c.r.choice(sites)
    w, n = c.fresh('w'), c.fresh('n')
    body = Block([Decl(s.name, Var(w), uid=s.uid), Assign(w, '+=', Lit(1)),
                  BareBlock(s.body)])
    b.stmts[i] = BareBlock(Block([Decl(n, s.n), Decl(w, Lit(0), mut=True),
                                  While(Bin('<', Var(w), Var(n)), body)]))
    return True


DEAD = [
    ['if false {', '    println("never")', '}'],
    ['while false {', '    println("never")', '}'],
    ['if 1 > 2 {', '    println("never")', '} else {', '}'],
    ['{', '    val {x} = 41 + 1', '}'],
    ['val {x} = "unused"'],
    ['for {x} in range(0) {', '    println("never")', '}'],
]


def t_dead(c, g):
    bs = blocks_of(g)
    if not bs:
        return False
    b = c.r.choice(bs)
    # not after a statement that leaves the block
    pos = [i for i in range(len(b.stmts) + 1)
           if i == 0 or not isinstance(b.stmts[i - 1], (Return, Break, Continue))]
    if not pos:
        return False
    i = c.r.choice(pos)
    x = c.fresh('d')
    b.stmts.insert(i, RawStmt([l.replace('{x}', x) for l in c.r.choice(DEAD)]))
    return True


TREE = [t_wrap, t_temp, t_fold, t_outline, t_negate, t_unchain, t_ifmatch, t_forwhile, t_dead]


# ------------------------------------------------------------ text transforms

KEYWORDS = set('''and await break comptime continue copy defer drop elif else enum extends extern
false for free func heap if impl import in init interface is leak match mut nocopy not null object
or parallel patch pool private pub pure rc readonly return self spawn super true unchecked val var
weak while val var func if elif else while for in return break continue match object
enum interface impl true false null and or not is defer free leak heap rc weak spawn await
parallel comptime import extern pub readonly self super drop copy nocopy patch in arena as
range println print len main Int Int8 Int16 Int32 Int64 UInt UInt8 UInt16 UInt32 UInt64 Float
Float32 Float64 Bool String Map Set List Channel Mutex RWLock Box Ptr RawPtr test'''.split())

TOKEN = re.compile(r'"(?:[^"\\]|\\.)*"|[A-Za-z_][A-Za-z0-9_]*|\S')


def _interp_end(src, j):
    """src[j] is the `{` opening an interpolation: the index of its `}`
    (strings inside it are skipped, with their own interpolations)."""
    depth, k, n = 1, j + 1, len(src)
    while k < n:
        ch = src[k]
        if ch == '"':
            k = _string_end(src, k)
            continue
        if ch == '{':
            depth += 1
        elif ch == '}':
            depth -= 1
            if depth == 0:
                return k
        k += 1
    return n - 1


def _string_end(src, i):
    """src[i] is a `"`: the index just past the closing quote."""
    j, n = i + 1, len(src)
    while j < n and src[j] != '"':
        if src[j] == '\\':
            j += 2
            continue
        if src[j] == '{':
            j = _interp_end(src, j) + 1
            continue
        j += 1
    return j + 1


def split_code(src):
    """(is_code, text) pieces of src: the text of string literals outside
    their interpolations is not code; interpolations are, recursively."""
    out, buf, i, n = [], [], 0, len(src)
    while i < n:
        ch = src[i]
        if ch == '/' and src[i:i + 2] == '//' or ch == '-' and src[i:i + 2] == '--':
            j = src.find('\n', i)
            j = n if j < 0 else j
            out.append((True, ''.join(buf)))
            out.append((False, src[i:j]))
            buf = []
            i = j
            continue
        if ch != '"':
            buf.append(ch)
            i += 1
            continue
        out.append((True, ''.join(buf)))
        buf = []
        j, lit = i + 1, ['"']
        while j < n and src[j] != '"':
            if src[j] == '\\':
                lit.append(src[j:j + 2])
                j += 2
                continue
            if src[j] == '{':
                out.append((False, ''.join(lit) + '{'))
                lit = []
                k = _interp_end(src, j)
                out.extend(split_code(src[j + 1:k]))
                lit.append('}')
                j = k + 1
                continue
            lit.append(src[j])
            j += 1
        lit.append('"')
        out.append((False, ''.join(lit)))
        i = j + 1
    out.append((True, ''.join(buf)))
    return out


def tx_rename(c, src):
    code = ''.join(t for is_code, t in split_code(src) if is_code)
    declared = set(re.findall(r'\b(?:val|var|for)\s+([a-z_][A-Za-z0-9_]*)', code))
    declared |= set(re.findall(r'\bfunc\s+([a-z_][A-Za-z0-9_]*)\s*[\[(]', code))
    # parameters of functions, methods, lambdas and init (not the named
    # arguments of a call such as heap(in: a))
    for m in re.finditer(r'\b(?:func|init)\b[^(\n]*\(([^()]*)\)', code):
        declared |= set(re.findall(r'(?:^|,)\s*(?:var\s+)?([a-z_][A-Za-z0-9_]*)\s*:', m.group(1)))
    for m in re.finditer(r'\bfor\s*\(([^)]*)\)', code):
        declared |= set(re.findall(r'[a-z_][A-Za-z0-9_]*', m.group(1)))
    # not fields, methods or named arguments: names used after a `.` or
    # declared inside an object, or written `name:` in a call
    after_dot = set(re.findall(r'\.\s*([A-Za-z_][A-Za-z0-9_]*)', code))
    fields = set()
    for m in re.finditer(r'^(?:pub\s+)?(?:object|interface|enum)\b[^\n]*\{\n(.*?)^\}', code,
                         re.M | re.S):
        fields |= set(re.findall(r'^\s+(?:pub\s+)?(?:val|var)?\s*([a-z_][A-Za-z0-9_]*)\s*:',
                                 m.group(1), re.M))
        fields |= set(re.findall(r'\bfunc\s+([a-z_][A-Za-z0-9_]*)', m.group(1)))
    names = {d for d in declared - after_dot - fields - KEYWORDS
             if d != '_' and d != 'main' and not d.startswith('__')}
    if not names:
        return src
    sub = {n: n + '_q' for n in names if c.r.random() < 0.8}
    out = []
    for is_code, t in split_code(src):
        if is_code:
            t = re.sub(r'(?<![.\w])([A-Za-z_][A-Za-z0-9_]*)\b',
                       lambda m: sub.get(m.group(1), m.group(1)), t)
        out.append(t)
    return ''.join(out)


def top_chunks(src):
    lines = src.split('\n')
    chunks, cur = [], []
    for l in lines:
        if l and not l[0].isspace() and l[0] not in '}])' and cur and \
                not cur[-1].rstrip().endswith((',', '(', '[')):
            chunks.append(cur)
            cur = []
        cur.append(l)
    if cur:
        chunks.append(cur)
    return chunks


def tx_reorder(c, src):
    chunks = top_chunks(src)
    head, movable, globs = [], [], []
    for ch in chunks:
        first = ch[0].strip()
        if first.startswith('import') or first == '' or first.startswith(('#', '//')):
            head.append(ch)
        elif re.match(r'(pub\s+)?(val|var)\b', first):
            globs.append(ch)
        else:
            movable.append(ch)
    c.r.shuffle(movable)
    seq = movable[:]
    pos = sorted(c.r.randint(0, len(seq)) for _ in globs)
    for k, (p, gch) in enumerate(zip(pos, globs)):
        seq.insert(p + k, gch)
    out = []
    for ch in head + seq:
        while ch and ch[-1] == '':
            ch = ch[:-1]
        if ch:
            out.append('\n'.join(ch))
    return '\n\n'.join(out) + '\n'


def tx_deadfn(c, src):
    k = c.fresh('x')
    extra = c.r.choice([
        [f'func {k}(a: Int) -> Int {{', '    return a * 2 + 1', '}'],
        [f'object Mt{k} {{', '    val a: Int', '    val s: String', '}'],
        [f'enum Mt{k} {{', '    A,', '    B(Int),', '}'],
        [f'func {k}() {{', '    println("never called")', '}'],
    ])
    return src.rstrip('\n') + '\n\n' + '\n'.join(extra) + '\n'


TEXT = [tx_rename, tx_reorder, tx_deadfn]


# ------------------------------------------------------------------ checking

def variant(area, seed, k):
    """(source, expected, transforms applied, risky) for variant k."""
    g = cg_fuzz.make_gen(area, seed)
    expected = prog_fuzz.expected_of(g)
    c = Ctx(random.Random(f'meta-{area}-{seed}-{k}'))
    for _ in range(c.r.randint(1, 6)):
        t = c.r.choice(TREE)
        try:
            if t(c, g):
                c.applied.append(t.__name__[2:])
        except (Bail, RecursionError, AttributeError, KeyError, TypeError):
            continue
    # the model must agree that the tree transforms kept the meaning
    try:
        again = prog_fuzz.expected_of(g)
        if list(again) != list(expected) or again.panic != expected.panic:
            return None
    except Exception:
        return None
    src = g.render()
    if c.extra_tops:
        src = src.rstrip('\n') + '\n\n' + '\n\n'.join('\n'.join(t) for t in c.extra_tops) + '\n'
    for t in TEXT:
        if c.r.random() < 0.45:
            src = t(c, src)
            c.applied.append(t.__name__[3:])
    return src, expected, c.applied, c.risky


RISKY_CODES = {'E0323', 'E0202', 'E0224'}


def check(ironc, outdir, area, seed):
    g, src_text, expected = cg_fuzz.program(area, seed)
    name = f'{area}_{seed}'
    d = os.path.join(outdir, name)
    src = cg_fuzz.write_case(d, name, src_text, expected)
    exe = os.path.join(d, name + '.bin')
    o = cg_fuzz.build(ironc, src, exe, cwd=d)
    if o.built:
        cg_fuzz.run(o, exe, cwd=d, leaks_ok=getattr(expected, 'leaks_ok', False))
    msg = cg_fuzz.versus_model(o, expected)
    if msg:  # the original fails already: a prog_fuzz failure
        return keep(d, [('original', msg, src_text, o)])
    fails = []
    r = random.Random(f'meta-mode-{area}-{seed}')
    for k in range(VARIANTS):
        try:
            v = variant(area, seed, k)
        except (Bail, RuntimeError, RecursionError):
            continue
        if v is None:
            continue
        vsrc, vexp, applied, risky = v
        vname = f'{name}_v{k}'
        vpath = cg_fuzz.write_case(d, vname, vsrc, None)
        mode = r.choice(['base', 'base', 'release', 'debug', 'noopt', 'O0', 'skip'])
        flags, cc, skip = cg_fuzz.MODES[mode]
        if skip == 'random':
            skip = cg_fuzz.skip_set(seed * 7 + k)
        vexe = os.path.join(d, vname + '.bin')
        vo = cg_fuzz.build(ironc, vpath, vexe, flags, cc, skip, cwd=d)
        if vo.built:
            cg_fuzz.run(vo, vexe, cwd=d, leaks_ok=getattr(vexp, 'leaks_ok', False))
            os.remove(vexe)
        if risky and not vo.built and vo.codes and vo.codes <= RISKY_CODES:
            os.remove(vpath)
            continue
        m = cg_fuzz.versus_model(vo, vexp)
        if not m and vo.cwarn:
            m = 'C compiler output:\n' + vo.cwarn[:2000]
        if m:
            fails.append((f'v{k} {mode}{"[" + skip + "]" if skip else ""} '
                          f'{"+".join(applied)}', m, vsrc, vo))
        else:
            os.remove(vpath)
    if os.path.exists(exe):
        os.remove(exe)
    if not fails:
        shutil.rmtree(d, ignore_errors=True)
        return 'ok', ''
    return keep(d, fails)


def keep(d, fails):
    with open(os.path.join(d, 'failure.txt'), 'w', encoding='utf-8') as f:
        for label, msg, _, o in fails:
            f.write(f'[{label}] {msg}\n---- stdout\n{o.stdout[:3000]}\n---- stderr\n'
                    f'{o.stderr[:3000]}\n---- build\n{o.buildout[:3000]}\n')
    return 'fail', '\n'.join(f'[{l}] {m}' for l, m, _, _ in fails)
