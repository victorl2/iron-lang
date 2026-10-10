#!/usr/bin/env python3
"""Differential expression fuzzer for ironc.

Generates random Int / Bool expressions over run-time values, prints them
with Iron's precedence table (minimal parentheses), evaluates them in Python
with Iron semantics, compiles with ironc and compares the output.

    expr_fuzz.py IRONC OUTDIR SEED [COUNT] [TYPE]
    expr_fuzz.py --suite IRONC OUTDIR      (the fixed matrix ctest runs)

TYPE is the integer type of the expressions (default Int): Int8, Int16,
Int32, UInt8, UInt16, UInt32 also work; results wrap to that width.

It found the unparenthesized `!` (#333), comparison of comparisons (#336)
and unwrapped 8 / 16 bit intermediates (#342). Exit status 1 on a wrong
value; C compiler warnings are printed but do not fail the run, since the
warning set differs between platforms.
"""
import os
import random
import subprocess
import sys

# Iron binary precedence (manual 3.1), higher binds tighter.
PREC = {'or': 2, 'and': 3, '|': 4, '^': 5, '&': 6, '==': 7, '!=': 7,
        '<': 8, '>': 8, '<=': 8, '>=': 8, '<<': 9, '>>': 9,
        '+': 10, '-': 10, '*': 11, '/': 11, '%': 11}
UNARY_PREC = 12
NVARS = 6
TYPES = {'Int': (64, True), 'Int8': (8, True), 'Int16': (16, True),
         'Int32': (32, True), 'UInt8': (8, False), 'UInt16': (16, False),
         'UInt32': (32, False)}
TNAME = 'Int'
BITS, SIGNED = 64, True


def wrap(x):
    x &= (1 << BITS) - 1
    return x - (1 << BITS) if SIGNED and x >> (BITS - 1) else x


def tdiv(a, b):
    q = abs(a) // abs(b)
    return q if (a >= 0) == (b >= 0) else -q


class G:
    def __init__(self, rng, vals):
        self.r = rng
        self.vals = vals

    def leaf_int(self):
        if self.r.random() < 0.6:
            i = self.r.randrange(NVARS)
            return ('var', i), self.vals[i]
        k = self.r.randint(0, 40)
        return ('lit', k), k

    def int_expr(self, d):
        if d <= 0 or self.r.random() < 0.25:
            return self.leaf_int()
        c = self.r.random()
        if c < 0.12 and SIGNED:
            e, v = self.int_expr(d - 1)
            return ('un', '-', e), wrap(-v)
        if c < 0.18:
            e, v = self.int_expr(d - 1)
            return ('un', '~', e), wrap(~v)
        if c < 0.28:
            # division / modulo by a nonzero constant (incl. -1)
            e, v = self.int_expr(d - 1)
            k = self.r.choice([-7, -3, -1, 1, 2, 3, 4, 5, 8] if SIGNED
                              else [1, 2, 3, 4, 5, 7, 8])
            op = self.r.choice(['/', '%'])
            q = tdiv(v, k)
            res = q if op == '/' else v - q * k
            return ('bin', op, e, ('lit', k)), wrap(res)
        if c < 0.36 and BITS == 64:
            # shift: left shift of a small non-negative value
            e, v = self.int_expr(d - 1)
            k = self.r.randint(0, 6)
            if self.r.random() < 0.5:
                masked = ('bin', '&', e, ('lit', 1023))
                return ('bin', '<<', masked, ('lit', k)), wrap((v & 1023) << k)
            return ('bin', '>>', e, ('lit', k)), v >> k
        op = self.r.choice(['+', '-', '*', '&', '|', '^'])
        a, va = self.int_expr(d - 1)
        b, vb = self.int_expr(d - 1)
        if op == '*':
            # keep products small: multiply by a masked operand
            b = ('bin', '&', b, ('lit', 15))
            vb = vb & 15
        res = {'+': va + vb, '-': va - vb, '*': va * vb,
               '&': va & vb, '|': va | vb, '^': va ^ vb}[op]
        return ('bin', op, a, b), wrap(res)

    def bool_expr(self, d):
        c = self.r.random()
        if d <= 0 or c < 0.35:
            op = self.r.choice(['==', '!=', '<', '>', '<=', '>='])
            a, va = self.int_expr(max(d - 1, 0))
            b, vb = self.int_expr(max(d - 1, 0))
            res = {'==': va == vb, '!=': va != vb, '<': va < vb,
                   '>': va > vb, '<=': va <= vb, '>=': va >= vb}[op]
            return ('bin', op, a, b), res
        if c < 0.5:
            e, v = self.bool_expr(d - 1)
            return ('un', 'not', e), not v
        if c < 0.65:
            # comparison of comparisons (#336)
            op = self.r.choice(['==', '!='])
            a, va = self.bool_expr(d - 1)
            b, vb = self.bool_expr(d - 1)
            return ('bin', op, a, b), (va == vb) if op == '==' else (va != vb)
        op = self.r.choice(['and', 'or'])
        a, va = self.bool_expr(d - 1)
        b, vb = self.bool_expr(d - 1)
        return ('bin', op, a, b), (va and vb) if op == 'and' else (va or vb)


def show(e, parent=0, right=False):
    k = e[0]
    if k == 'var':
        return f'v{e[1]}'
    if k == 'lit':
        if TNAME != 'Int':      # literal-only subexpressions stay Int (#341)
            return f'{TNAME}({e[1]})'
        return str(e[1]) if e[1] >= 0 else f'({e[1]})'
    if k == 'un':
        op, x = e[1], e[2]
        inner = show(x, UNARY_PREC)
        if op == '-' and inner.startswith('-'):
            inner = f'({inner})'        # `--` starts a comment in Iron
        s = (op + ' ' if op == 'not' else op) + inner
        return f'({s})' if parent > UNARY_PREC else s
    op, a, b = e[1], e[2], e[3]
    p = PREC[op]
    s = f'{show(a, p)} {op} {show(b, p + 1)}'   # left associative
    return f'({s})' if p < parent else s


def fmt(v):
    return ('true' if v else 'false') if isinstance(v, bool) else str(v)


SUITE = [('Int', s) for s in (1, 2, 3, 4)] + \
        [(t, s) for t in ('Int8', 'UInt8', 'Int16', 'UInt16', 'Int32', 'UInt32')
         for s in (1, 2)]


def main():
    if sys.argv[1] == '--suite':
        ironc, outdir = sys.argv[2], sys.argv[3]
        rc = 0
        for tname, seed in SUITE:
            r = run_one(ironc, outdir, seed, 40, tname)
            print(f'{tname} seed {seed}: {"ok" if r == 0 else "FAIL"}')
            rc = rc or r
        return rc
    ironc, outdir, seed = sys.argv[1], sys.argv[2], int(sys.argv[3])
    count = int(sys.argv[4]) if len(sys.argv) > 4 else 60
    tname = sys.argv[5] if len(sys.argv) > 5 else 'Int'
    return run_one(ironc, outdir, seed, count, tname)


def run_one(ironc, outdir, seed, count, tname):
    global TNAME, BITS, SIGNED
    TNAME = tname
    BITS, SIGNED = TYPES[TNAME]
    rng = random.Random(seed)
    lo, hi = (-(1 << (BITS - 1)), (1 << (BITS - 1)) - 1) if SIGNED else (0, (1 << BITS) - 1)
    def pickval():
        if BITS == 64:
            return rng.randint(-60, 60)
        return rng.choice([rng.randint(lo, hi), rng.randint(max(lo, -60), 60), lo, hi])
    vals = [pickval() for _ in range(NVARS)]
    g = G(rng, vals)
    body = '    return n' if TNAME == 'Int' else f'    return {TNAME}(n)'
    lines = [f'func pick(n: Int) -> {TNAME} {{', body, '}', '',
             'func main() {']
    for i, v in enumerate(vals):
        lines.append(f'    val v{i} = pick({v})' if v >= 0 else f'    val v{i} = pick(0 - {-v})')
    expected = []
    for i in range(count):
        if rng.random() < 0.5:
            e, v = g.int_expr(rng.randint(1, 4))
        else:
            e, v = g.bool_expr(rng.randint(1, 3))
        lines.append(f'    val r{i} = {show(e)}')
        lines.append(f'    println("{i} {{r{i}}}")')
        expected.append(f'{i} {fmt(v)}')
        if isinstance(v, bool):
            lines.append(f'    if {show(e)} {{')
            lines.append(f'        println("{i} T")')
            lines.append('    } else {')
            lines.append(f'        println("{i} F")')
            lines.append('    }')
            expected.append(f'{i} {"T" if v else "F"}')
    lines.append('}')
    os.makedirs(outdir, exist_ok=True)
    src = os.path.join(outdir, f'fuzz_{TNAME}_{seed}.iron')
    exe = os.path.join(outdir, f'fuzz_{TNAME}_{seed}.bin')
    with open(src, 'w') as f:
        f.write('\n'.join(lines) + '\n')
    b = subprocess.run([ironc, 'build', src, '-o', exe], capture_output=True, text=True)
    if b.returncode != 0:
        print(f'seed {seed}: COMPILE FAIL\n{b.stdout}{b.stderr}'[:3000])
        return 2
    warn = [l for l in (b.stdout + b.stderr).splitlines()
            if 'warning' in l and 'self-comparison' not in l and 'W0601' not in l]
    r = subprocess.run([exe], capture_output=True, text=True, timeout=30)
    got = r.stdout.strip().splitlines()
    bad = [(x, y) for x, y in zip(expected, got) if x != y]
    if len(got) != len(expected):
        bad.append(('<count>', f'{len(expected)} vs {len(got)} rc={r.returncode} {r.stderr[:300]}'))
    for w in warn[:3]:
        print(f'seed {seed}: C WARNING {w}')
    if bad:
        print(f'seed {seed}: MISMATCH {len(bad)}: {src}')
        for x, y in bad[:5]:
            print(f'  expected {x!r} got {y!r}')
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
