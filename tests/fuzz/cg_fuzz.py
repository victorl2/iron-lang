#!/usr/bin/env python3
"""Code generation fuzzer for ironc: build modes, metamorphic variants and
near-valid mutants of the programs the prog_fuzz.py generators write.

Kinds of check:

  modes   builds each program in several ways and requires the same
          output, exit status and panic text from every build, and the
          output the generator's model expects:
              base        ironc build (C at -O3, every LIR pass)
              release     --release (C at -O2)
              debug       --debug (C at -O0 -g, no LIR optimization, no
                          inlining, #line directives)
              debugbuild  --debug-build (debug allocator, no check elision)
              noopt       --no-optimize (no LIR optimization)
              O0, O1      every LIR pass, C at -O0 / -O1 (IRON_CC_FLAGS)
              skip        a random subset of the LIR passes left out
                          (IRON_LIR_SKIP, lir_optimize.c)
              ubsan       C built with -fsanitize=undefined, any report fails
              emitdet     --emit-c twice: the C must be byte for byte the same
          A build whose C compiler output has a warning fails (the C the
          compiler writes must compile clean under the flags it uses).
  meta    applies semantics-preserving transforms to each program
          (pf_meta.py: blocks around statement suffixes, temporaries, hand
          folded constants, outlined expressions, renamed bindings,
          reordered declarations, dead code, if/else and loop rewrites)
          and requires the transformed program to print what the original
          is expected to print.
  mutate  mutates each program (pf_mutate.py: token and statement level:
          deleted, duplicated and swapped statements, swapped types, wrong
          arity, shadowing, misplaced keywords, ...) and requires ironc to
          either build it (and the C compiler to accept the C) or reject it
          with a diagnostic code: never a crash, a hang, an internal error,
          C the C compiler rejects, or a failure without an error[Ennnn].

    cg_fuzz.py KIND AREA IRONC OUTDIR SEED [COUNT] [-j N] [--modes a,b,...]
    cg_fuzz.py --suite KIND IRONC OUTDIR [AREA...]   (the fixed seeds ctest runs)
    cg_fuzz.py --record AREA IRONC OUTDIR SEED COUNT [-j N]
        prints one line per program: seed, a hash of its base build's
        output, exit status and panic text (compare two machines with diff)

AREA is a prog_fuzz.py area, `all` (every area in turn by seed) or
`shapes` (pf_shapes.py: nested generic, enum, tuple and optional types).
Failing programs are kept under OUTDIR/fail/. Exit status 1 when any fails.
"""
import hashlib
import os
import random
import re
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import prog_fuzz  # noqa: E402
from pf_lang import Bail  # noqa: E402

# the LIR passes IRON_LIR_SKIP can leave out (lir_optimize.c)
PASSES = ('inline', 'copyprop', 'constfold', 'dce', 'sle', 'sr', 'dae', 'coalesce',
          'rcpe', 'gencheck', 'bce', 'emitinline')

UBSAN = '-fsanitize=undefined -fno-sanitize-recover=undefined'

# mode -> (ironc build flags, IRON_CC_FLAGS, IRON_LIR_SKIP)
MODES = {
    'base': ([], None, None),
    'release': (['--release'], None, None),
    'debug': (['--debug'], None, None),
    'debugbuild': (['--debug-build'], None, None),
    'noopt': (['--no-optimize'], None, None),
    'O0': ([], '-O0', None),
    'O1': ([], '-O1', None),
    'skip': ([], None, 'random'),
    'ubsan': ([], '-O1 ' + UBSAN, None),
}
DEFAULT_MODES = ('base', 'release', 'debug', 'debugbuild', 'noopt', 'O0', 'O1', 'skip',
                 'ubsan', 'emitdet')

CWARN = re.compile(r'^\S+\.[ch]:\d+:\d+: (warning|error):', re.M)
DIAG = re.compile(r'error\[(E\d{4})\]')


def timeout():
    return int(os.environ.get('PF_TIMEOUT', '120'))


def all_areas():
    extra = ['shapes'] if os.path.exists(os.path.join(HERE, 'pf_shapes.py')) else []
    return list(prog_fuzz.available()) + extra


def area_for(area, seed):
    if area == 'all':
        av = all_areas()
        return av[seed % len(av)]
    return area


def make_gen(area, seed):
    if area == 'shapes':
        import pf_shapes
        gen = pf_shapes.generate
        for attempt in range(50):
            try:
                return gen(random.Random(f'shapes-{seed}-{attempt}'))
            except (Bail, RecursionError):
                continue
        raise RuntimeError(f'shapes seed {seed}: no program')
    return prog_fuzz.make_gen(area, seed)


def program(area, seed):
    g = make_gen(area, seed)
    return g, g.render(), prog_fuzz.expected_of(g, cached=True)


# ----------------------------------------------------------- build and run

class Outcome:
    """What one build of a program did."""

    def __init__(self):
        self.built = False
        self.codes = set()       # diagnostic codes when the build failed
        self.buildout = ''
        self.cwarn = ''          # C compiler warnings or errors
        self.stdout = ''
        self.stderr = ''
        self.rc = None
        self.timeout = False
        self.leaks_ok = False

    def panic_line(self):
        """The first line of the run's error output with numbers in
        addresses taken out (the panic text)."""
        for l in self.stderr.splitlines():
            if self.leaks_ok and 'leaked at exit' in l:
                continue   # the debug allocator's report of an allowed leak
            if l.strip() and not l.startswith('=='):
                return re.sub(r'0x[0-9a-fA-F]+', 'A', l.strip())
        return ''

    def key(self):
        if not self.built:
            return ('nobuild', tuple(sorted(self.codes)))
        return ('run', self.stdout, self.rc, self.panic_line(), self.timeout)


def build(ironc, src, exe, flags=(), cc=None, skip=None, cwd=None):
    env = dict(os.environ)
    env.pop('IRON_CC_FLAGS', None)
    env.pop('IRON_LIR_SKIP', None)
    if cc:
        env['IRON_CC_FLAGS'] = cc
    if skip:
        env['IRON_LIR_SKIP'] = skip
    o = Outcome()
    if os.path.exists(exe):
        os.remove(exe)
    try:
        b = subprocess.run([ironc, 'build'] + list(flags) + [src, '-o', exe], capture_output=True,
                           text=True, encoding='utf-8', errors='replace', env=env,
                           timeout=timeout(), cwd=cwd)
    except subprocess.TimeoutExpired:
        o.buildout = 'ironc timed out'
        o.timeout = True
        return o
    o.buildout = b.stdout + b.stderr
    o.cwarn = '\n'.join(m.group(0) + l.split(m.group(0), 1)[1]
                        for l in o.buildout.splitlines() for m in [CWARN.match(l)] if m)
    o.codes = set(DIAG.findall(o.buildout))
    o.built = b.returncode == 0 and os.path.exists(exe)
    o.build_rc = b.returncode
    return o


def run(o, exe, cwd=None, env_extra=None, leaks_ok=False):
    o.leaks_ok = leaks_ok
    env = dict(os.environ)
    env['UBSAN_OPTIONS'] = 'print_stacktrace=1:halt_on_error=1'
    if env_extra:
        env.update(env_extra)
    try:
        r = subprocess.run([exe], capture_output=True, text=True, encoding='utf-8',
                           errors='replace', timeout=timeout(), env=env, cwd=cwd)
    except subprocess.TimeoutExpired:
        o.timeout = True
        return o
    o.stdout, o.stderr, o.rc = r.stdout, r.stderr, r.returncode
    return o


def versus_model(o, expected):
    """None when the outcome is what the model expects, else a message."""
    errors = getattr(expected, 'errors', None)
    panic = getattr(expected, 'panic', None)
    if errors:
        if o.built:
            return 'built, but ' + ' or '.join(sorted(errors)) + ' was expected'
        if not (o.codes & errors):
            return (' or '.join(sorted(errors)) + ' expected, got:\n' + o.buildout[:3000])
        return None
    if not o.built:
        return 'does not build:\n' + o.buildout[:3000]
    if o.timeout:
        return 'timed out'
    got = o.stdout.splitlines()
    if got != list(expected):
        for i, (x, y) in enumerate(zip(expected, got)):
            if x != y:
                return f'line {i + 1}: expected {x!r} got {y!r} (rc={o.rc})\n{o.stderr[:1500]}'
        return (f'{len(expected)} lines expected, {len(got)} produced (rc={o.rc})\n'
                f'{o.stderr[:1500]}')
    if 'runtime error:' in o.stderr and 'Sanitizer' not in o.stderr and '.c:' in o.stderr:
        return 'UBSan: ' + o.stderr[:2000]
    if panic is not None:
        if o.rc == 0 or panic not in o.stderr:
            return f'expected a runtime error containing {panic!r} (rc={o.rc})\n{o.stderr[:1500]}'
        return None
    if o.rc != 0:
        return f'exit status {o.rc}\n{o.stderr[:2000]}'
    if o.panic_line():
        return 'unexpected error output:\n' + o.stderr[:2000]
    return None


def skip_set(seed):
    r = random.Random(f'skip-{seed}')
    if r.random() < 0.4:
        return r.choice(PASSES)
    return ','.join(sorted(r.sample(PASSES, r.randint(2, len(PASSES)))))


def write_case(d, name, src_text, expected):
    os.makedirs(d, exist_ok=True)
    src = os.path.join(d, f'{name}.iron')
    with open(src, 'w', encoding='utf-8') as f:
        f.write(src_text)
    for fname, content in (getattr(expected, 'files', None) or {}).items():
        with open(os.path.join(d, fname), 'w', encoding='utf-8', newline='') as f:
            f.write(content)
    if expected is not None:
        with open(os.path.join(d, 'expected.txt'), 'w', encoding='utf-8') as f:
            f.write('\n'.join(expected) + ('\n' if expected else ''))
            if getattr(expected, 'errors', None):
                f.write('-- compile error: ' + ' or '.join(sorted(expected.errors)) + '\n')
            if getattr(expected, 'panic', None):
                f.write('-- then panics: ' + expected.panic + '\n')
            if getattr(expected, 'leaks_ok', False):
                f.write('-- leaks allowed\n')
    return src


def emit_c(ironc, src, d):
    """The C `--emit-c` writes for src (built in d), or None."""
    name = os.path.splitext(os.path.basename(src))[0]
    c = os.path.join(d, '.iron-build', name + '.c')
    if os.path.exists(c):
        os.remove(c)
    try:
        subprocess.run([ironc, 'build', '--emit-c', src], cwd=d, capture_output=True,
                       timeout=timeout())
    except subprocess.TimeoutExpired:
        return None
    if not os.path.exists(c):
        return None
    with open(c, 'rb') as f:
        return f.read()


def check_modes(ironc, outdir, area, seed, modes=DEFAULT_MODES):
    """Returns (status, message)."""
    g, src_text, expected = program(area, seed)
    name = f'{area}_{seed}'
    d = os.path.join(outdir, name)
    src = write_case(d, name, src_text, expected)
    errors = getattr(expected, 'errors', None)
    if errors:   # a program that must not build: every mode rejects it the same way
        modes = [m for m in modes if m in ('base', 'debug', 'noopt', 'skip')]
    outs = {}
    fails = []
    for m in modes:
        if m == 'emitdet':
            if errors:
                continue
            a = emit_c(ironc, src, d)
            b = emit_c(ironc, src, d)
            if a is None:
                fails.append((m, 'emit-c wrote no C'))
            elif a != b:
                fails.append((m, 'two --emit-c runs wrote different C'))
            continue
        flags, cc, skip = MODES[m]
        if skip == 'random':
            skip = skip_set(seed)
        exe = os.path.join(d, f'{name}.{m}.bin')
        o = build(ironc, src, exe, flags, cc, skip, cwd=d)
        if o.built:
            run(o, exe, cwd=d, leaks_ok=getattr(expected, 'leaks_ok', False))
            os.remove(exe)
        label = m + (f'[{skip}]' if skip else '')
        outs[label] = o
        if o.cwarn and m != 'ubsan':
            fails.append((label, 'C compiler output:\n' + o.cwarn[:2000]))
        if 'internal compiler error' in o.buildout or 'internal error' in o.buildout:
            fails.append((label, 'internal error:\n' + o.buildout[:2000]))
        msg = versus_model(o, expected)
        if msg:
            fails.append((label, msg))
    keys = {}
    for label, o in outs.items():
        keys.setdefault(o.key(), []).append(label)
    if len(keys) > 1:
        desc = []
        for k, labels in keys.items():
            o = outs[labels[0]]
            desc.append(f'  {",".join(labels)}: ' +
                        (f'no build {sorted(o.codes)}' if not o.built else
                         f'rc={o.rc} {len(o.stdout.splitlines())} lines '
                         f'panic={o.panic_line()[:100]!r} sha={sha(o.stdout)}'))
        fails.append(('*', 'builds disagree:\n' + '\n'.join(desc)))
    if not fails:
        shutil.rmtree(d, ignore_errors=True)
        return 'ok', ''
    with open(os.path.join(d, 'failure.txt'), 'w', encoding='utf-8') as f:
        for label, msg in fails:
            f.write(f'[{label}] {msg}\n')
            o = outs.get(label)
            if o is not None:
                f.write(f'---- stdout\n{o.stdout[:4000]}\n---- stderr\n{o.stderr[:4000]}\n'
                        f'---- build\n{o.buildout[:4000]}\n')
    return 'fail', '\n'.join(f'[{l}] {m}' for l, m in fails)


def sha(s):
    return hashlib.sha1(s.encode('utf-8', 'replace')).hexdigest()[:10]


def record(ironc, outdir, area, seed):
    g, src_text, expected = program(area, seed)
    name = f'{area}_{seed}'
    d = os.path.join(outdir, name)
    src = write_case(d, name, src_text, expected)
    exe = os.path.join(d, name + '.bin')
    o = build(ironc, src, exe, cwd=d)
    if o.built:
        run(o, exe, cwd=d)
    shutil.rmtree(d, ignore_errors=True)
    if not o.built:
        return f'{name} nobuild {",".join(sorted(o.codes))}'
    return f'{name} rc={o.rc} out={sha(o.stdout)} panic={o.panic_line()[:80]!r}'


# ----------------------------------------------------------------- drivers

def check_one(kind, ironc, outdir, area, seed, modes):
    a = area_for(area, seed)
    try:
        if kind == 'modes':
            return a, seed, check_modes(ironc, outdir, a, seed, modes)
        if kind == 'meta':
            import pf_meta
            return a, seed, pf_meta.check(ironc, outdir, a, seed)
        if kind == 'mutate':
            import pf_mutate
            return a, seed, pf_mutate.check(ironc, outdir, a, seed)
        if kind == 'record':
            return a, seed, ('ok', record(ironc, outdir, a, seed))
    except RuntimeError as e:
        return a, seed, ('nogen', str(e))
    raise SystemExit(f'unknown kind {kind}')


def _job(args):
    return check_one(*args)


def run_all(kind, ironc, outdir, area, seeds, jobs=1, modes=DEFAULT_MODES, quiet=False):
    work = [(kind, ironc, outdir, area, s, modes) for s in seeds]
    if jobs > 1:
        from concurrent.futures import ProcessPoolExecutor
        results = ProcessPoolExecutor(jobs).map(_job, work, chunksize=1)
    else:
        results = map(_job, work)
    bad = 0
    for a, seed, (st, msg) in results:
        if kind == 'record':
            print(msg, flush=True)
            continue
        if st == 'nogen':
            print(msg, flush=True)
            continue
        if st != 'ok':
            bad += 1
            k = f'{a}_{seed}'
            src = os.path.join(outdir, k)
            dst = os.path.join(outdir, 'fail', k)
            if os.path.isdir(src):
                shutil.rmtree(dst, ignore_errors=True)
                shutil.copytree(src, dst, ignore=shutil.ignore_patterns('*.bin', '.iron-build'))
                shutil.rmtree(src, ignore_errors=True)
            print(f'{kind} {a} seed {seed}: FAIL {dst}\n' +
                  '\n'.join('    ' + l for l in msg.splitlines()[:40]), flush=True)
        elif not quiet:
            print(f'{kind} {a} seed {seed}: ok', flush=True)
    return bad


# The fixed seeds ctest runs, about a minute each (cg_fuzz_*): see
# CMakeLists.txt.
SUITE = {
    # kind: (area, seeds, modes)
    'modes': ('all', range(1, 9), DEFAULT_MODES),
    'meta': ('all', range(1, 17), DEFAULT_MODES),
    'mutate': ('all', range(1, 17), DEFAULT_MODES),
    'shapes': ('shapes', range(1, 17), ('base', 'release', 'debug', 'noopt', 'skip')),
}


def main(argv):
    jobs, modes, args = 1, DEFAULT_MODES, []
    i = 0
    while i < len(argv):
        if argv[i] == '-j':
            jobs = int(argv[i + 1])
            i += 1
        elif argv[i].startswith('-j') and argv[i][2:].isdigit():
            jobs = int(argv[i][2:])
        elif argv[i] == '--modes':
            modes = tuple(argv[i + 1].split(','))
            i += 1
        else:
            args.append(argv[i])
        i += 1
    if args[0] == '--suite':
        kind, ironc, outdir = args[1], os.path.abspath(args[2]), args[3]
        area, seeds, smodes = SUITE[kind]
        if kind == 'shapes':
            kind = 'modes'
        if len(args) > 4:
            area = args[4]
        bad = run_all(kind, ironc, outdir, area, seeds, jobs=max(jobs, min(4, os.cpu_count() or 1)),
                      modes=smodes, quiet=True)
        print(f'{kind} {area}: {len(seeds) - bad}/{len(seeds)} ok')
        return 1 if bad else 0
    if args[0] == '--record':
        area, ironc, outdir, seed, count = args[1], os.path.abspath(args[2]), args[3], \
            int(args[4]), int(args[5])
        run_all('record', ironc, outdir, area, range(seed, seed + count), jobs)
        return 0
    kind, area, ironc, outdir, seed = args[0], args[1], os.path.abspath(args[2]), args[3], \
        int(args[4])
    count = int(args[5]) if len(args) > 5 else 1
    bad = run_all(kind, ironc, outdir, area, range(seed, seed + count), jobs, modes,
                  quiet=count > 20)
    print(f'{kind} {area}: {count - bad}/{count} ok', flush=True)
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
