"""Probe programs for the edge-case areas of prog_fuzz.py.

A probe is one self-contained check: a few source lines inside `main` (in
a block of their own, so names and destruction stay local), the lines the
manual says it prints, and optionally the panic it ends the program with
or the compile error the whole program must fail with. A generator
(pf_numedge, pf_strcoll, ...) draws random probes and computes each
expected output with a small model of the construct while it generates.

ProbeGen has the shape prog_fuzz expects of a generator (render, model,
funcs, mainblock), so the reducer deletes probes and statements as usual.
"""
from pf_lang import (Block, Stmt, Expected, Bail, iron_str,
                     fuzz_identifier)  # noqa: F401


class Probe(Stmt):
    """code: source lines (relative indentation with 4 spaces);
    out: the lines printed; panic: text of the run-time error the probe
    ends the program with; errors: diagnostic codes, one of which the
    build must report; leaks_ok: the probe leaks by definition."""

    def __init__(self, code, out=(), panic=None, errors=None, block=True, leaks_ok=False):
        self.code = list(code)
        self.out = list(out)
        self.panic = panic
        self.errors = set(errors) if errors else None
        self.block = block
        self.leaks_ok = leaks_ok

    def lines(self, ind):
        if not self.block:
            return [ind + l for l in self.code]
        return [ind + '{'] + [ind + '    ' + l for l in self.code] + [ind + '}']

    def run(self, m, f):
        for l in self.out:
            m.emit(l)


class ProbeGen:
    """Collects top-level declarations and probes."""
    NPROBES = (6, 16)

    def __init__(self, rng):
        self.r = rng
        self.funcs = []           # the reducer drops functions from here (none)
        self.tops = {}            # key -> lines, in insertion order
        self.imports = []
        self.mainblock = Block([])
        self.n = 0
        self.generated_names = set()
        self.leaks_ok = False

    def fresh(self, p='v'):
        self.n += 1
        return fuzz_identifier(self.r, f'{p}{self.n}', self.generated_names)

    def need(self, key, text):
        """A top-level declaration (text: a string of lines), once."""
        if key not in self.tops:
            self.tops[key] = text.strip('\n').split('\n')

    def need_import(self, mod):
        if mod not in self.imports:
            self.imports.append(mod)

    def add(self, probe):
        self.mainblock.stmts.append(probe)

    def render(self):
        lines = [f'import {m}' for m in self.imports]
        if lines:
            lines.append('')
        for t in self.tops.values():
            lines += t + ['']
        lines += ['func main() {'] + self.mainblock.lines('    ') + ['}']
        return '\n'.join(lines) + '\n'

    def model(self):
        out = []
        leaks = self.leaks_ok
        for p in self.mainblock.stmts:
            if getattr(p, 'errors', None):
                return Expected([], errors=p.errors)
        for p in self.mainblock.stmts:
            out += p.out
            leaks = leaks or p.leaks_ok
            if p.panic:
                e = Expected(out, panic=p.panic)
                e.leaks_ok = True
                return e
        e = Expected(out)
        e.leaks_ok = leaks
        return e

    # A generator overrides probe_kinds() -> [(weight, method)] and may set
    # panic_kinds (probes that end the program, at most one, last) and
    # error_kinds (probes that make the program fail to build).
    panic_rate = 0.15
    error_rate = 0.08

    def probe_kinds(self):
        return []

    def panic_kinds(self):
        return []

    def error_kinds(self):
        return []

    def program(self):
        r = self.r
        kinds = self.probe_kinds()
        total = sum(w for w, _ in kinds)
        for _ in range(r.randint(*self.NPROBES)):
            x = r.random() * total
            for w, fn in kinds:
                x -= w
                if x <= 0:
                    break
            p = fn()
            if p is not None:
                self.add(p)
        pk = self.panic_kinds()
        if pk and r.random() < self.panic_rate:
            p = r.choice(pk)()
            if p is not None:
                self.add(p)
        ek = self.error_kinds()
        if ek and r.random() < self.error_rate:
            p = r.choice(ek)()
            if p is not None:
                self.mainblock.stmts.insert(r.randint(0, len(self.mainblock.stmts)), p)
        if not self.mainblock.stmts:
            raise Bail('empty program')
