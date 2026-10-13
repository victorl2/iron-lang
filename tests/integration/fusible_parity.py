#!/usr/bin/env python3
"""Loop fusion and the `//iron:fusible` hint never change what a program does.

Every fixture of tests/integration/v4/fusible is built three ways:

    hinted   as written, optimized (the only build that may fuse)
    plain    every `//iron:fusible` (and `@fusible`) removed, optimized
    noopt    as written, with --no-optimize: every list method call runs as
             written, one loop per stage

The three programs must print the same, exit the same and, when they panic,
panic with the same message at the same Iron line. The output must also be
the fixture's .expected file (or its `@expect-panic:` text), so a fixture
cannot pass by failing the same way three times.

The fixture also says where fusion happens. A line that ends in

    // @fused: map filter sum

must become exactly one fused loop of those stages in the optimized C, in
both the hinted and the plain build (the hint does not decide fusion), and
no other line may fuse. The --no-optimize build must contain no fused loop.

A function whose hint cannot be honored is marked on its `func` line:

    //iron:fusible
    func noisy(x: Int) -> Int {  // @w0618: calls 'println'

The hinted builds must report W0618 for exactly the marked hints, with a
reason that contains the text; the plain build must report none.

    fusible_parity.py IRONC [-j N] [--rt DIR] [--keep DIR] [FIXTURE.iron ...]

--rt DIR caches the prebuilt runtime the programs link against (keyed by
the runtime sources, so a stale cache is never used).
"""
import hashlib
import os
import re
import shutil
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
sys.path.insert(0, os.path.join(ROOT, 'tests', 'fuzz'))
import prog_fuzz  # noqa: E402

HINT = re.compile(r'(?m)^([ \t]*)(//iron:fusible|@fusible)[ \t]*$')
FUSED_MARK = re.compile(r'//\s*@fused:\s*([a-zA-Z; ]+?)\s*$')
FUSED_C = re.compile(r'/\* fused loop, line (\d+):((?: \w+)+) \*/')
W0618_MARK = re.compile(r'//\s*@w0618:\s*(.+?)\s*$')
PANIC_MARK = re.compile(r'(?m)^\s*//\s*@expect-panic:\s*(.+?)\s*$')


def strip_hints(text):
    """The program without its fusible hints, every line where it was."""
    return HINT.sub(lambda m: m.group(1), text)


def expected_fusion(text):
    """[(line, 'map filter sum')] from the `// @fused:` marks of a source,
    sorted. Two loops of one line are written `// @fused: map map; filter sum`."""
    out = []
    for n, line in enumerate(text.splitlines(), 1):
        m = FUSED_MARK.search(line)
        if m:
            out += [(n, ' '.join(part.split())) for part in m.group(1).split(';')]
    return sorted(out)


def actual_fusion(c_text):
    """[(line, 'map filter sum')] for every fused loop of a generated C file."""
    return sorted((int(m.group(1)), m.group(2).strip()) for m in FUSED_C.finditer(c_text))


def runtime_lib(rt_base):
    """The prebuilt runtime, in a directory named after its sources."""
    src = os.path.join(ROOT, 'src')
    h = hashlib.sha256()
    for sub in ('runtime', 'stdlib', 'util', 'vendor'):
        for dirpath, _, files in sorted(os.walk(os.path.join(src, sub))):
            for f in sorted(files):
                if f.endswith(('.c', '.h')):
                    with open(os.path.join(dirpath, f), 'rb') as fh:
                        h.update(f.encode() + b'\0' + fh.read())
    return prog_fuzz.build_asan_rt(os.path.join(rt_base, h.hexdigest()[:16]), 'plain')


def build_and_run(ironc, workdir, name, text, build_args, rtlib, timeout=120):
    """Returns a dict: build (rc, output), c (generated C), run (rc, stdout,
    panic lines)."""
    os.makedirs(workdir, exist_ok=True)
    src = os.path.join(workdir, name + '.iron')
    exe = os.path.join(workdir, name + '.bin')
    with open(src, 'w', encoding='utf-8') as f:
        f.write(text)
    res = {'build_rc': 1, 'build_out': '', 'c': '', 'rc': None, 'stdout': '', 'panic': '',
           'stderr': '', 'w0618': []}
    b = subprocess.run([ironc, 'build'] + list(build_args) + ['--emit-c', src], cwd=workdir,
                       capture_output=True, text=True, encoding='utf-8', errors='replace')
    res['build_out'] = (b.stdout or '') + (b.stderr or '')
    res['w0618'] = warnings_of(res['build_out'])
    cfile = os.path.join(workdir, '.iron-build', name + '.c')
    if b.returncode != 0 or not os.path.exists(cfile):
        res['build_rc'] = b.returncode or 1
        return res
    with open(cfile, encoding='utf-8', errors='replace') as f:
        res['c'] = f.read()
    inc = os.path.join(ROOT, 'src')
    cc = subprocess.run(['clang'] + prog_fuzz.SAN_COMMON + prog_fuzz.SAN_FLAGS['plain'] +
                        prog_fuzz.ssl_flags() +
                        [f'-I{inc}', f'-I{inc}/vendor', f'-I{inc}/stdlib', cfile, rtlib,
                         '-lm', '-lpthread'] + prog_fuzz.ssl_libs() + ['-o', exe],
                        capture_output=True, text=True, encoding='utf-8', errors='replace')
    res['build_rc'] = cc.returncode
    if cc.returncode != 0 or not os.path.exists(exe):
        res['build_out'] += cc.stdout + cc.stderr
        return res
    try:
        r = subprocess.run([exe], capture_output=True, text=True, timeout=timeout,
                           encoding='utf-8', errors='replace', cwd=workdir)
    except subprocess.TimeoutExpired:
        res['rc'] = 'timeout'
        return res
    res['rc'] = r.returncode
    res['stdout'] = r.stdout
    res['stderr'] = r.stderr
    # A panic is its message, its Iron site and its details; the native
    # stack trace under it differs between builds.
    keep = []
    for line in r.stderr.splitlines():
        if line.startswith('iron:') or (line.startswith('  ') and not line.startswith('   ')
                                        and ':' in line):
            keep.append(line.replace(workdir, '<dir>'))
    res['panic'] = '\n'.join(keep)
    return res


W0618 = re.compile(r'warning\[W0618\]: (.*)\n\s*--> [^\n:]*:(\d+):')


def warnings_of(build_out):
    """[(line, message)] of the W0618 warnings in a compiler's output."""
    return sorted((int(m.group(2)), m.group(1)) for m in W0618.finditer(build_out))


def expected_warnings(text):
    """{line of the hint: substring of the reason} from `// @w0618: reason`
    marks, written at the end of the `func` line under the hint."""
    out = {}
    for n, line in enumerate(text.splitlines(), 1):
        m = W0618_MARK.search(line)
        if m:
            out[n - 1] = m.group(1)
    return out


def outcome(res):
    return (res['build_rc'], res['rc'], res['stdout'], res['panic'])


def check_program(ironc, workdir, name, text, rtlib, expected_stdout=None,
                  expected_panic=None, fused=None):
    """Build `text` three ways and compare. `fused` is [(line, stages)] for the
    optimized builds (default: the `// @fused:` marks of the text). Returns
    a list of failure messages (empty when the program passes)."""
    plain_text = strip_hints(text)
    variants = [('hinted', text, ()), ('noopt', text, ('--no-optimize',))]
    if plain_text != text:
        variants.insert(1, ('plain', plain_text, ()))
    got = {v: build_and_run(ironc, os.path.join(workdir, v), name, t, a, rtlib)
           for v, t, a in variants}
    fails = []
    base = got['hinted']
    if base['build_rc'] != 0:
        return [f'hinted build failed:\n{base["build_out"][:3000]}']
    for v, _, _ in variants[1:]:
        if got[v]['build_rc'] != 0:
            fails.append(f'{v} build failed:\n{got[v]["build_out"][:3000]}')
        elif outcome(got[v]) != outcome(base):
            fails.append(f'hinted and {v} differ\n' + describe(base, got[v], v))
    if fails:
        return fails
    if base['rc'] == 'timeout':
        return ['timeout']
    if expected_panic is not None:
        if base['rc'] == 0 or expected_panic not in base['stderr']:
            fails.append(f'expected a panic containing {expected_panic!r}, got exit '
                         f'{base["rc"]}\n{base["stderr"][:1500]}')
    elif base['rc'] != 0:
        fails.append(f'exit {base["rc"]}\n{base["stderr"][:1500]}')
    if expected_stdout is not None and base['stdout'] != expected_stdout:
        fails.append('output is not the expected output\n' +
                     diff_text(expected_stdout, base['stdout'], 'expected', 'got'))
    want = sorted(fused) if fused is not None else expected_fusion(text)
    warn = expected_warnings(text)
    for v, _, _ in variants:
        have = actual_fusion(got[v]['c'])
        target = [] if v == 'noopt' else want
        if have != target:
            fails.append(f'{v}: fused loops (line, stages) {have}, expected {target}')
        # W0618 is the same with and without optimization, and gone with
        # the hints.
        seen = got[v]['w0618']
        target_w = {} if v == 'plain' else warn
        if sorted(l for l, _ in seen) != sorted(target_w) or any(
                target_w.get(l, '') not in msg for l, msg in seen):
            fails.append(f'{v}: W0618 warnings {seen}, expected {sorted(target_w.items())}')
    return fails


def diff_text(a, b, an, bn):
    import difflib
    return ''.join(list(difflib.unified_diff(a.splitlines(True), b.splitlines(True), an, bn))[:60])


def describe(a, b, bn):
    out = []
    if a['rc'] != b['rc']:
        out.append(f'exit: hinted {a["rc"]}, {bn} {b["rc"]}')
    if a['stdout'] != b['stdout']:
        out.append(diff_text(a['stdout'], b['stdout'], 'hinted', bn))
    if a['panic'] != b['panic']:
        out.append(diff_text(a['panic'] + '\n', b['panic'] + '\n', 'hinted panic', bn + ' panic'))
    return '\n'.join(out)


def check_fixture(ironc, work, path, rtlib):
    name = os.path.splitext(os.path.basename(path))[0]
    with open(path, encoding='utf-8') as f:
        text = f.read()
    pm = PANIC_MARK.search(text)
    exp_path = path[:-5] + '.expected'
    expected = None
    if os.path.exists(exp_path):
        with open(exp_path, encoding='utf-8') as f:
            expected = f.read()
    elif not pm:
        return name, ['no .expected file and no @expect-panic']
    return name, check_program(ironc, os.path.join(work, name), name, text, rtlib,
                               expected, pm.group(1) if pm else None)


def main(argv):
    jobs, rt, keep, paths = 2, None, None, []
    i = 1
    while i < len(argv):
        if argv[i] == '-j':
            jobs = int(argv[i + 1])
            i += 1
        elif argv[i] == '--rt':
            rt = argv[i + 1]
            i += 1
        elif argv[i] == '--keep':
            keep = argv[i + 1]
            i += 1
        else:
            paths.append(argv[i])
        i += 1
    ironc = os.path.abspath(argv[0])
    if not paths:
        d = os.path.join(HERE, 'v4', 'fusible')
        paths = sorted(os.path.join(d, f) for f in os.listdir(d) if f.endswith('.iron'))
    work = keep or tempfile.mkdtemp(prefix='iron_fusible_parity_')
    rt_tmp = None if rt else tempfile.mkdtemp(prefix='iron_fusible_rt_')
    try:
        rtlib = runtime_lib(rt or rt_tmp)
        with ThreadPoolExecutor(jobs) as pool:
            results = list(pool.map(lambda p: check_fixture(ironc, work, p, rtlib), paths))
    finally:
        if rt_tmp:
            shutil.rmtree(rt_tmp, ignore_errors=True)
        if not keep:
            shutil.rmtree(work, ignore_errors=True)
    bad = 0
    fused = 0
    for (name, fails), path in zip(results, paths):
        with open(path, encoding='utf-8') as f:
            fused += len(expected_fusion(f.read()))
        if fails:
            bad += 1
            print(f'[FAIL] {name}')
            for msg in fails:
                print('    ' + msg.replace('\n', '\n    '))
    print(f'fusible parity: {len(paths) - bad} passed, {bad} failed '
          f'({fused} fused loops checked)')
    return 1 if bad or not paths else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
