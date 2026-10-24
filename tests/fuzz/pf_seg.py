"""Programs made of independent segments, for the areas whose expected
output is easier to compute directly than through the statement model of
pf_lang (concurrency patterns, arenas with interfaces and generics, and
the json, crypto and http standard library modules).

A segment is one function `func sN()` called from main, with the lines it
prints. Its expected output is computed in Python while it is generated
(the generator chooses values whose result it knows: a commutative
update, a hash computed with hashlib, a JSON tree it built itself). Only
the last segment of a program may panic. The reducer of prog_fuzz.py
shrinks a failing program by dropping whole segments (`segs`).
"""
from pf_lang import Block, Expected, fuzz_identifier


class Seg:
    def __init__(self, body, out, panic=None, leaks_ok=False, top=()):
        self.body = list(body)      # statement lines of the function, unindented
        self.out = list(out)        # the lines it prints
        self.panic = panic          # text the error output must contain
        self.leaks_ok = leaks_ok
        self.top = list(top)        # extra top-level lines (declarations)


def ind(lines, n=1):
    pad = '    ' * n
    return [pad + l if l else l for l in lines]


def iron_lit(s):
    """An Iron string literal for text s (no interpolation)."""
    out = []
    for ch in s:
        if ch == '\\':
            out.append('\\\\')
        elif ch == '"':
            out.append('\\"')
        elif ch == '{':
            out.append('\\{')
        elif ch == '}':
            out.append('\\}')
        elif ch == '\n':
            out.append('\\n')
        elif ch == '\t':
            out.append('\\t')
        elif ord(ch) < 0x20 or ord(ch) == 0x7f:
            out.append('\\u{%x}' % ord(ch))
        else:
            out.append(ch)
    return '"' + ''.join(out) + '"'


class SegProgram:
    """Subclasses set HELPERS and IMPORTS and implement add_segment()."""
    HELPERS = ''
    IMPORTS = ()
    NSEGS = (2, 5)

    def __init__(self, rng):
        self.r = rng
        self.segs = []
        # prog_fuzz's statement reducer looks for these; it finds nothing
        self.funcs = []
        self.mainblock = Block([])
        self.uid = 0
        self.generated_names = set()

    def fresh(self, p='v'):
        self.uid += 1
        return fuzz_identifier(self.r, f'{p}{self.uid}', self.generated_names)

    def program(self):
        n = self.r.randint(*self.NSEGS)
        for _ in range(n):
            self.add_segment()
        # at most one panicking segment, and it goes last
        pan = [s for s in self.segs if s.panic]
        if pan:
            keep = pan[0]
            self.segs = [s for s in self.segs if not s.panic] + [keep]

    def render(self):
        lines = [f'import {m}' for m in self.IMPORTS]
        if lines:
            lines.append('')
        lines += self.HELPERS.splitlines() + ['']
        seen = set()
        for s in self.segs:
            for t in s.top:
                if t not in seen:
                    seen.add(t)
                    lines += t.splitlines() + ['']
        for i, s in enumerate(self.segs):
            lines.append(f'func s{i}() {{')
            lines += ind(s.body)
            lines += ['}', '']
        lines.append('func main() {')
        lines += [f'    s{i}()' for i in range(len(self.segs))]
        lines.append('}')
        return '\n'.join(lines) + '\n'

    def model(self):
        out = []
        panic = None
        leaks = False
        for s in self.segs:
            out += s.out
            leaks = leaks or s.leaks_ok
            if s.panic:
                panic = s.panic
                break
        return Expected(out, panic=panic, leaks_ok=leaks or bool(panic))
