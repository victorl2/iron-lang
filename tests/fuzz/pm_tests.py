"""Test blocks across a package, for mod_fuzz.py (area `tests`).

A package whose src/ files (nested directories included), vendor/ files
and tests/ helper files hold `test "name" { ... }` blocks that pass, fail
an assert / assert_eq / assert_ne, or panic, some calling private
functions of their own file (manual 10.2.1). Names mix spaces, non-ASCII
text, punctuation and the escapes a string literal allows. `iron test`
runs with no filter and with filters (substrings of names, odd strings,
strings matching nothing); the per-test lines, the failure details, the
summary line and the exit status must match what the generator expects.
Sometimes a standalone tests/test_*.iron program with its own main is
added; it runs after the blocks and its failure fails `iron test`.
"""
import os
import random

M = None   # mod_fuzz, bound by bind()


def bind(mod):
    global M
    M = mod


NAME_PARTS = ('adds', 'works', 'edge case', 'ünïcode', '日本', 'a-b_c', 'x.y', '(paren)', '[br]',
              'dots...', 'n=3', '100%', 'q?', 'star*', 'plus+', 'caret^', '$dollar', 'back\\\\slash',
              'quote \\"q\\"', 'brace \\{x\\}', 'tab\\there', '  lead', 'trail  ', '#hash', "it's")


def display(lit):
    """The name as a test line shows it: the literal's escapes decoded."""
    return (lit.replace('\\\\', '\x00').replace('\\"', '"').replace('\\{', '{').replace('\\}', '}')
            .replace('\\t', '\t').replace('\x00', '\\'))


class TCase:
    def __init__(self, lit, kind, path):
        self.lit, self.kind, self.path = lit, kind, path
        self.name = display(lit)
        self.detail = None      # expected first detail line for failures (substring)
        self.line_token = None  # unique text on the failing line


def gen(rng):
    files = {}     # path -> list of chunks
    order_paths = []
    paths = ['src/main.iron']
    for _ in range(rng.randint(1, 4)):
        paths.append(rng.choice(('src/alpha.iron', 'src/beta.iron', 'src/util/deep.iron',
                                 'src/z/last.iron', 'tests/helpers.iron', 'tests/more_checks.iron',
                                 'vendor/lib/x.iron')))
    paths = sorted(set(paths))
    for p in paths:
        files[p] = []
    files['src/main.iron'].append('func main() {\n    println("main is not run by tests")\n}')
    used = set()
    cases = []
    uid = [0]

    def fresh_name():
        for _ in range(100):
            k = rng.randint(1, 3)
            lit = ' '.join(rng.choice(NAME_PARTS) for _ in range(k))
            if rng.random() < 0.1:
                lit = f'{lit} {rng.randint(0, 999)}'
            nm = display(lit)
            if nm not in used:
                used.add(nm)
                return lit
        raise M.Fail('nogen', 'no fresh name')

    for p in paths:
        for _ in range(rng.randint(0, 4)):
            uid[0] += 1
            u = uid[0]
            fn = f'helper_{u}'
            a, b = rng.randint(0, 50), rng.randint(0, 50)
            files[p].append(f'func {fn}(x: Int) -> Int {{\n    return x + {a}\n}}')
            kind = rng.choice(('pass', 'pass', 'pass', 'fail_eq', 'fail_assert', 'fail_ne', 'panic',
                               'print_pass'))
            tc = TCase(fresh_name(), kind, p)
            if kind == 'pass':
                body = f'    assert_eq({fn}({b}), {a + b})\n    assert({fn}(0) == {a})'
            elif kind == 'print_pass':
                body = f'    println("output of test {u}")\n    assert_ne({fn}(1), {a})'
            elif kind == 'fail_eq':
                tc.line_token = f'assert_eq({fn}({b}), {a + b + 1})'
                body = f'    assert({fn}(0) == {a})\n    {tc.line_token}'
                tc.detail = f'assertion failed: assert_eq at {p}:{{LINE}}: expected {a + b + 1}, got {a + b}'
            elif kind == 'fail_ne':
                tc.line_token = f'assert_ne({fn}({b}), {a + b})'
                body = f'    {tc.line_token}'
                tc.detail = f'{p}:{{LINE}}'
            elif kind == 'fail_assert':
                tc.line_token = f'assert({fn}({b}) == {a + b + 7})'
                body = f'    {tc.line_token}'
                tc.detail = f'assertion failed: {p}:{{LINE}}'
            else:
                tc.line_token = f'println("{{xs[i{u}]}}")'
                body = (f'    val xs = [{a}, {b}]\n    val i{u} = {rng.randint(2, 9)}\n'
                        f'    println("before the panic {u}")\n    {tc.line_token}')
                tc.detail = f'before the panic {u}'
            files[p].append(f'test "{tc.lit}" {{\n{body}\n}}')
            cases.append(tc)
    standalone = None
    if rng.random() < 0.3:
        ok = rng.random() < 0.5
        standalone = ok
        files['tests/test_program.iron'] = [
            'func main() {\n    println("standalone")\n' +
            ('' if ok else '    val xs = [1]\n    val i = 5\n    println("{xs[i]}")\n') + '}']
    return files, cases, standalone


def render(files):
    tree = {'iron.toml': '[package]\nname = "tpkg"\nversion = "0.1.0"\n'}
    for p, chunks in files.items():
        tree[p] = '\n\n'.join(chunks) + '\n'
    return tree


def build_order(paths):
    """vendor/, src/ (lib, sorted, main), then tests/ helpers sorted."""
    src = [p for p in paths if not p.startswith('tests/')]
    order = __import__('pm_pkg').file_order(src)
    return order + sorted(p for p in paths if p.startswith('tests/') and
                          not os.path.basename(p).startswith('test_'))


def expect(cases, tree, flt):
    order = build_order(list(tree))
    pos = {p: i for i, p in enumerate(order)}
    listed = sorted(cases, key=lambda c: pos[c.path])   # stable: file order, then order in file
    sel = [c for c in listed if flt is None or flt in c.name]
    return listed, sel


def line_of(tree, tc):
    for i, l in enumerate(tree[tc.path].splitlines()):
        if tc.line_token in l:
            return i + 1
    return None


def check(iron, d, seed, mode):
    rng = random.Random(f'tests-{seed}')
    files, cases, standalone = gen(rng)
    tree = render(files)
    root = os.path.join(d, 'tpkg')
    M.write_tree(root, tree)
    env = M.env_for(iron)
    names = [c.name for c in cases]
    filters = [None]
    if names:
        nm = rng.choice(names)
        if nm:
            i = rng.randrange(len(nm))
            filters.append(nm[i:i + rng.randint(1, 6)])
    filters.append(rng.choice(('zzz-no-match', 'ü', ' ', '.', '*', '\\', '"', '{x}', '日本', 'e')))
    for flt in filters:
        cmd = [iron, 'test'] + ([flt] if flt is not None else [])
        if flt is not None and flt.startswith('-'):
            continue
        r = M.run(cmd, root, env, f'iron test {flt!r}')
        listed, sel = expect(cases, tree, flt)
        got = [l for l in r.stdout.splitlines()]
        tests_lines = [l for l in got if l.startswith('test ')]
        want_lines = [f'test {c.name} ... ' + ('ok' if c.kind in ('pass', 'print_pass') else 'FAILED')
                      for c in sel]
        ctx = f'iron test {flt!r}\n--- stdout:\n{r.stdout[-3000:]}\n--- stderr:\n{r.stderr[-1500:]}'
        if tests_lines != want_lines:
            raise M.Fail('wrong', f'test lines differ: want {want_lines!r}\n{ctx}')
        failed = sum(1 for c in sel if c.kind not in ('pass', 'print_pass'))
        passed = len(sel) - failed
        if sel:
            summary = f'{passed} passed, {failed} failed'
            if len(sel) < len(listed):
                summary += f', {len(listed) - len(sel)} filtered out'
            if summary not in got:
                raise M.Fail('wrong', f'summary {summary!r} missing\n{ctx}')
        elif listed:
            if 'no test matches the filter' not in got:
                raise M.Fail('wrong', f'"no test matches the filter" missing\n{ctx}')
        # failure details: the indented lines after a FAILED line
        for c in sel:
            if not c.detail:
                continue
            want = c.detail.replace('{LINE}', str(line_of(tree, c)))
            head = f'test {c.name} ... FAILED'
            k = got.index(head)
            block = []
            for l in got[k + 1:]:
                if not l.startswith('    '):
                    break
                block.append(l[4:])
            if not any(want in l for l in block):
                raise M.Fail('wrong', f'{head}: detail {want!r} missing in {block!r}\n{ctx}')
        bad = failed > 0 or (standalone is False and flt is None) or \
            (standalone is False and flt is not None)
        if standalone is not None and 'standalone' not in r.stdout:
            raise M.Fail('wrong', f'the standalone test program did not run\n{ctx}')
        want_rc = 1 if bad else 0
        if (r.returncode != 0) != bool(want_rc):
            raise M.Fail('wrong', f'exit status {r.returncode}, expected '
                                  f'{"non-zero" if want_rc else "0"}\n{ctx}')


def describe(seed):
    rng = random.Random(f'tests-{seed}')
    files, cases, standalone = gen(rng)
    out = []
    for p, c in sorted(render(files).items()):
        out.append(f'==> {p}\n{c}')
    out.append('-- tests: ' + ', '.join(f'{c.name!r}:{c.kind}' for c in cases))
    out.append(f'-- standalone: {standalone}')
    return '\n'.join(out)
