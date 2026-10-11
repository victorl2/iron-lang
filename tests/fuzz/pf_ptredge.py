"""Pointer edge cases (manual 6.2, 6.4, 6.5): checked pointers as object
fields (`*T`, `*var T`, `?*T`, #469) written and read through, passed in
objects to functions and returned in them; pointers handed down chains of
calls whose frames take addresses of their own locals, recursion with a
pointer parameter, closures capturing a pointer and called inside its
frame or in a callee (all valid: #471); pointers into heap values passed
to functions; pointers to globals kept after the frame that took them
returned; RawPtr.of / Ptr.cast on bindings later written (the pointer sees
the write) and Ptr.offset / Ptr.diff on Box cells.

Panics: a pointer to a local that leaves its frame inside a closure or an
object and is read later ("dangling stack pointer to frame", #471); a
pointer into a heap value read after `free` ("stale pointer dereference").
"""
from pf_probe import Probe, ProbeGen

DECLS = '''object P {
    var hp: Int
    init(hp: Int) {
        self.hp = hp
    }
}

object View {
    val p: *P
}

object Editor {
    val p: *var P
    func hit(n: Int) {
        self.p.hp -= n
    }
}

object Maybe {
    var p: ?*P
    init() {
        self.p = null
    }
}

func read_view(v: View) -> Int {
    return v.p.hp
}

func heal(p: *var P, n: Int) {
    p.hp += n
}

func peek(p: *P) -> Int {
    return p.hp
}

func peek_int(p: *Int) -> String {
    return "{p}"
}

func wrap(p: *P) -> View {
    return View(p)
}

func own_frame(p: *var P, n: Int) -> Int {
    var mine = P(n)
    val q = &mine
    heal(q, 1)
    p.hp += q.hp
    return q.hp
}

func depth(p: *var P, k: Int) -> Int {
    if k == 0 {
        return p.hp
    }
    var local = P(k)
    val q = &local
    p.hp += 1
    return depth(p, k - 1) + q.hp
}

func call_with(f: func() -> Int) -> Int {
    return f()
}

var G = P(100)

func global_view() -> View {
    var tmp = P(1)
    val t = &tmp
    heal(t, 1)
    return View(&G)
}'''

ESCAPES = '''func esc_closure() -> func() -> Int {
    val local = P(41)
    val p = &local
    return func() -> Int {
        return p.hp
    }
}

func esc_view() -> View {
    val local = P(42)
    return wrap(&local)
}

func esc_editor() -> Editor {
    var local = P(43)
    return Editor(&local)
}'''


class PtrEdgeGen(ProbeGen):
    NPROBES = (4, 10)

    def __init__(self, rng):
        super().__init__(rng)
        self.need('decls', DECLS)

    def p_fields(self):
        r = self.r
        code, out = [], []
        a, b = r.randint(0, 50), r.randint(0, 50)
        pa, pb = self.fresh('a'), self.fresh('b')
        code += [f'var {pa} = P({a})', f'var {pb} = P({b})']
        v, e, m = self.fresh('v'), self.fresh('e'), self.fresh('m')
        code += [f'val {v} = View(&{pa})', f'var {e} = Editor(&{pb})']
        tag = self.fresh('f')
        for _ in range(r.randint(1, 5)):
            k = r.choice(['hit', 'write', 'heal', 'read', 'passview', 'maybe', 'wrap'])
            if k == 'hit':
                n = r.randint(1, 9)
                code.append(f'{e}.hit({n})')
                b -= n
            elif k == 'write':
                n = r.randint(0, 99)
                code.append(f'{pa}.hp = {n}')
                a = n
            elif k == 'heal':
                n = r.randint(1, 9)
                code.append(f'heal(&{pa}, {n})' if r.random() < 0.5 else f'heal({pa}, {n})')
                a += n
            elif k == 'read':
                code.append(f'println("{tag} {{{v}.p.hp}} {{{e}.p.hp}} {{peek({pb})}}")')
                out.append(f'{tag} {a} {b} {b}')
            elif k == 'passview':
                code.append(f'println("{tag}v {{read_view({v})}} {{read_view(View(&{pb}))}}")')
                out.append(f'{tag}v {a} {b}')
            elif k == 'wrap':
                w = self.fresh('w')
                code += [f'val {w} = wrap(&{pb})', f'println("{tag}w {{{w}.p.hp}}")']
                out.append(f'{tag}w {b}')
            else:
                code += [f'var {m} = Maybe()', f'println("{tag}m {{{m}.p == null}}")', f'{m}.p = &{pa}',
                         f'val {m}q = {m}.p', f'if {m}q != null {{', f'    println("{tag}m {{{m}q.hp}}")', '}']
                out += [f'{tag}m true', f'{tag}m {a}']
                m = self.fresh('m')
        code.append(f'println("{tag}e {{{pa}.hp}} {{{pb}.hp}}")')
        out.append(f'{tag}e {a} {b}')
        return Probe(code, out)

    def p_frames(self):
        """Pointers passed down to frames that take addresses of their own."""
        r = self.r
        code, out = [], []
        a = r.randint(0, 50)
        pa = self.fresh('a')
        code.append(f'var {pa} = P({a})')
        q = self.fresh('q')
        code.append(f'val {q} = &{pa}')
        tag = self.fresh('d')
        for _ in range(r.randint(1, 4)):
            k = r.choice(['own', 'depth', 'peek', 'closure', 'callee'])
            if k == 'own':
                n = r.randint(0, 9)
                code.append(f'println("{tag}o {{own_frame(&{pa}, {n})}} {{{q}.hp}}")')
                a += n + 1
                out.append(f'{tag}o {n + 1} {a}')
            elif k == 'depth':
                kk = r.randint(0, 6)
                code.append(f'val {tag}r = depth(&{pa}, {kk})')
                code.append(f'println("{tag}r {{{tag}r}} {{{q}.hp}}")')
                a += kk
                out.append(f'{tag}r {a + kk * (kk + 1) // 2} {a}')
                tag = self.fresh('d')
            elif k == 'peek':
                code.append(f'println("{tag}p {{peek({q})}} {{{q}.hp}}")')
                out.append(f'{tag}p {a} {a}')
            elif k == 'closure':
                f = self.fresh('cl')
                code += [f'val {f} = func() -> Int {{', f'    return {q}.hp * 2', '}',
                         f'println("{tag}c {{{f}()}} {{call_with({f})}}")']
                out.append(f'{tag}c {a * 2} {a * 2}')
            else:
                code.append(f'println("{tag}k {{call_with(func() -> Int {{ return {q}.hp + 1 }})}}")')
                out.append(f'{tag}k {a + 1}')
        return Probe(code, out)

    def p_heap(self):
        r = self.r
        h = self.fresh('h')
        v = r.randint(0, 50)
        tag = self.fresh('hp')
        code = [f'var {h} = heap P({v})', f'defer free {h}',
                f'println("{tag} {{peek_int(&{h}.hp)}} {{peek(&{h})}}")']
        out = [f'{tag} {v} {v}']
        n = r.randint(1, 9)
        code += [f'heal(&{h}, {n})', f'println("{tag}b {{peek_int(&{h}.hp)}}")']
        out.append(f'{tag}b {v + n}')
        return Probe(code, out)

    def p_global(self):
        tag = self.fresh('g')
        n = self.r.randint(1, 9)
        w = self.fresh('gv')
        code = [f'val {w} = global_view()', f'heal(&G, {n})', f'println("{tag} {{{w}.p.hp}}")',
                f'heal(&G, 0 - {n})']
        return Probe(code, [f'{tag} {100 + n}'])

    def p_raw(self):
        r = self.r
        code, out = [], []
        tag = self.fresh('r')
        k = r.choice(['int', 'obj', 'float', 'box'])
        if k == 'int':
            n = self.fresh('n')
            a, b = r.randint(0, 99), r.randint(0, 99)
            code += [f'var {n}: Int = {a}', f'val {n}p: *unchecked Int = Ptr.cast[Int](RawPtr.of({n}))',
                     f'println("{tag} {{{n}p}}")', f'{n} = {b}', f'println("{tag} {{{n}p}}")']
            out += [f'{tag} {a}', f'{tag} {b}']
        elif k == 'obj':
            o = self.fresh('o')
            a, b = r.randint(0, 99), r.randint(0, 99)
            code += [f'var {o} = P({a})', f'val {o}p = Ptr.cast[P](RawPtr.of({o}))',
                     f'println("{tag} {{{o}p.hp}}")', f'{o}.hp = {b}', f'println("{tag} {{{o}p.hp}}")']
            out += [f'{tag} {a}', f'{tag} {b}']
        elif k == 'float':
            f = self.fresh('fl')
            code += [f'var {f}: Float = 2.5', f'val {f}p = Ptr.cast[Float](RawPtr.of({f}))',
                     f'{f} = 0.75', f'println("{tag} {{{f}p}}")']
            out.append(f'{tag} 0.75')
        else:
            b = self.fresh('bx')
            v = r.randint(0, 50)
            off = r.randint(-5, 9)
            code += [f'val {b} = Box(P({v}))', f'val {b}u: *var unchecked P = {b}.unwrap()',
                     f'{b}u.hp += 1', f'val {b}o = Ptr.offset({b}u, {off})' if off >= 0 else
                     f'val {b}o = Ptr.offset({b}u, 0 - {-off})',
                     f'println("{tag} {{Ptr.diff({b}o, {b}u)}} {{Ptr.diff({b}u, {b}o)}} {{{b}u.hp}}")',
                     f'{b}.free()']
            out.append(f'{tag} {off} {-off} {v + 1}')
        return Probe(code, out)

    # ------------------------------------------------------------ panics
    def p_dangling(self):
        self.need('escapes', ESCAPES)
        tag = self.fresh('x')
        k = self.r.choice(['closure', 'view', 'editor'])
        code = [f'println("{tag} before")']
        if k == 'closure':
            code += [f'val {tag}f = esc_closure()', f'println("{{{tag}f()}}")']
        elif k == 'view':
            code += [f'val {tag}v = esc_view()', f'println("{{{tag}v.p.hp}}")']
        else:
            code += [f'var {tag}e = esc_editor()', f'{tag}e.hit(1)', f'println("done")']
        return Probe(code, [f'{tag} before'], panic='dangling stack pointer to frame')

    def p_stale_heap(self):
        tag = self.fresh('s')
        h = self.fresh('h')
        return Probe([f'val {h} = heap P(5)', f'val {h}p: *P = &{h}', f'println("{tag} {{{h}p.hp}}")',
                      f'free {h}', f'println("{{{h}p.hp}}")'], [f'{tag} 5'],
                     panic='stale pointer dereference')

    def probe_kinds(self):
        return [(4, self.p_fields), (4, self.p_frames), (2, self.p_heap), (1, self.p_global),
                (2, self.p_raw)]

    def panic_kinds(self):
        return [self.p_dangling, self.p_dangling, self.p_stale_heap]

    panic_rate = 0.25


def generate(rng):
    g = PtrEdgeGen(rng)
    g.program()
    return g
