#!/usr/bin/env python3
"""Fuzzer for packages, the module system and the iron project tool.

Areas:
    pkg    random multi-file packages (pm_pkg.py): nested src/ directories,
           vendored libraries, globals initialized across files, objects
           with drop, generics, interfaces, enums, patches, comptime and
           closures used across files, local and stdlib imports (cycles and
           diamonds). Oracles: `iron build` + the binary print the output
           the generator computed; `iron run` and `iron check` agree; the
           package flattened into one file prints the same under `ironc
           run`; a package that breaks a rule fails with one of the
           diagnostic codes the manual gives, never a crash and never C
           that does not compile.
    incr   a package edited between builds: files added, deleted, renamed,
           moved into subdirectories and changed; each rebuild must equal a
           clean build of the same tree. Also concurrent builds of one
           package, and package directories with spaces and non-ASCII
           names.
    tests  test blocks across the files of a package (pm_tests.py):
           passing, failing and panicking tests, `iron test` with name
           filters; the per-test lines and the summary must match.

    mod_fuzz.py AREA IRON OUTDIR SEED [COUNT] [-j N] [--valgrind]
    mod_fuzz.py --suite IRON OUTDIR AREA...   (the fixed seeds ctest runs)
    mod_fuzz.py --gen AREA SEED               (print one case)

IRON is the iron binary; ironc is the one next to it. Run them under
AddressSanitizer by passing the iron of a sanitizer build. --valgrind runs
the built programs under valgrind (leaks and memory errors fail). A crash
of iron or ironc (a signal, a sanitizer report) fails any case. Failing
cases are kept under OUTDIR/fail/.
"""
import os
import random
import re
import shutil
import subprocess
import sys
import threading

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import pm_pkg  # noqa: E402

TIMEOUT = int(os.environ.get('MF_TIMEOUT', '300'))


class Fail(Exception):
    def __init__(self, kind, msg):
        super().__init__(msg)
        self.kind = kind


def write_tree(root, tree):
    for rel, content in tree.items():
        p = os.path.join(root, rel)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, 'w', encoding='utf-8', newline='\n') as f:
            f.write(content)


def env_for(iron):
    env = dict(os.environ)
    env['PATH'] = os.path.dirname(iron) + os.pathsep + env.get('PATH', '')
    env.setdefault('ASAN_OPTIONS', 'detect_leaks=0:abort_on_error=0')
    env.setdefault('UBSAN_OPTIONS', 'print_stacktrace=1:halt_on_error=1')
    env.pop('NO_COLOR', None)
    env['NO_COLOR'] = '1'
    return env


SAN = re.compile(r'AddressSanitizer|LeakSanitizer|runtime error:|UndefinedBehaviorSanitizer|'
                 r'crashed \(|this is a compiler bug|internal compiler error|ICE:')


def run(cmd, cwd, env, what, timeout=TIMEOUT, stdin=None):
    try:
        r = subprocess.run(cmd, cwd=cwd, env=env, capture_output=True, text=True,
                           encoding='utf-8', errors='replace', timeout=timeout,
                           stdin=subprocess.DEVNULL if stdin is None else stdin)
    except subprocess.TimeoutExpired:
        raise Fail('timeout', f'{what}: {" ".join(cmd)} timed out')
    out = r.stdout + r.stderr
    if r.returncode < 0 or r.returncode >= 128 or SAN.search(r.stderr):
        raise Fail('crash', f'{what}: {" ".join(cmd)} rc={r.returncode}\n{out[-4000:]}')
    return r


def codes(text):
    return set(re.findall(r'error\[(E\d{4})\]', text))


def check_build_failed(r, errors, what):
    out = r.stdout + r.stderr
    if r.returncode == 0:
        raise Fail('wrong', f'{what} succeeded, but {" or ".join(sorted(errors))} was expected')
    got = codes(out)
    if not got:
        raise Fail('ccfail', f'{what} failed without a diagnostic code:\n{out[-4000:]}')
    if not (got & errors):
        raise Fail('wrong', f'{what}: expected {" or ".join(sorted(errors))}, got '
                            f'{" ".join(sorted(got))}:\n{out[-3000:]}')


def check_built(r, what):
    if r.returncode != 0:
        out = r.stdout + r.stderr
        kind = 'compile' if codes(out) else 'ccfail'
        raise Fail(kind, f'{what} failed:\n{out[-4000:]}')


def run_binary(exe, cwd, expected, mode, what):
    cmd = [exe]
    if mode == 'valgrind':
        cmd = ['valgrind', '--quiet', '--leak-check=full', '--errors-for-leak-kinds=definite,indirect',
               '--error-exitcode=99', exe]
    try:
        r = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True, encoding='utf-8',
                           errors='replace', timeout=TIMEOUT)
    except subprocess.TimeoutExpired:
        raise Fail('timeout', f'{what} timed out')
    got = r.stdout.splitlines()
    if got != expected:
        for i, (x, y) in enumerate(zip(expected, got)):
            if x != y:
                raise Fail('wrong', f'{what}: line {i + 1}: expected {x!r} got {y!r} '
                                    f'(rc={r.returncode})\n{r.stderr[-1500:]}')
        raise Fail('wrong', f'{what}: {len(expected)} lines expected, {len(got)} produced '
                            f'(rc={r.returncode})\n{r.stderr[-1500:]}')
    if r.returncode != 0:
        raise Fail('memory' if r.returncode == 99 else 'crash',
                   f'{what}: rc={r.returncode}\n{r.stderr[-3000:]}')


# ------------------------------------------------------------------ pkg

def make_pkg(seed, negative=None):
    for attempt in range(50):
        rng = random.Random(f'pkg-{seed}-{attempt}')
        try:
            return pm_pkg.generate(rng, negative)
        except pm_pkg.Bail:
            continue
    raise RuntimeError(f'pkg seed {seed}: no package')


def exe_name(name):
    return name + ('.exe' if os.name == 'nt' else '')


def check_pkg(iron, d, seed, mode):
    pk = make_pkg(seed)
    ironc = os.path.join(os.path.dirname(iron), exe_name('ironc'))
    env = env_for(iron)
    root = os.path.join(d, 'pkg')
    write_tree(root, pk.tree('pkg'))
    exp = pk.expected()
    with open(os.path.join(d, 'expected.txt'), 'w', encoding='utf-8') as f:
        f.write('\n'.join(exp) + '\n')
        if pk.errors:
            f.write(f'-- compile error: {" or ".join(sorted(pk.errors))} ({pk.negative})\n')
    # a build profile picked per seed; every one must print the same
    prof = random.Random(f'profile-{seed}').choice(
        ([], [], ['--release'], ['--no-optimize'], ['--debug'], ['--release', '--no-optimize']))
    b = run([iron, 'build'] + prof, root, env, 'iron build ' + ' '.join(prof))
    c = run([iron, 'check'], root, env, 'iron check')
    if pk.errors:
        check_build_failed(b, pk.errors, 'iron build')
        check_build_failed(c, pk.errors, 'iron check')
        return
    check_built(b, 'iron build')
    check_built(c, 'iron check')
    run_binary(os.path.join(root, 'target', exe_name('pkg')), root, exp, mode, 'target/pkg')
    # iron run prints its status lines on stderr and the program's on stdout
    r = run([iron, 'run'], root, env, 'iron run')
    if r.returncode != 0 or r.stdout.splitlines() != exp:
        raise Fail('wrong', f'iron run differs from iron build (rc={r.returncode}):\n'
                            f'{r.stdout[-2000:]}\n{r.stderr[-2000:]}')
    # iron fmt on every file keeps the program, and a second pass changes
    # nothing (--check)
    if random.Random(f'fmt-{seed}').random() < 0.5:
        froot = os.path.join(d, 'fmtpkg')
        write_tree(froot, pk.tree('pkg'))
        srcs = [p for p in pk.tree() if p.endswith('.iron')]
        for p in srcs:
            f = run([iron, 'fmt', p], froot, env, f'iron fmt {p}')
            if f.returncode != 0:
                raise Fail('fmt', f'iron fmt {p} failed:\n{(f.stdout + f.stderr)[-2000:]}')
        for p in srcs:
            f = run([iron, 'fmt', '--check', p], froot, env, f'iron fmt --check {p}')
            if f.returncode != 0:
                raise Fail('fmt', f'iron fmt is not idempotent on {p}:\n{(f.stdout + f.stderr)[-2000:]}')
        fb = run([iron, 'build'], froot, env, 'iron build after fmt')
        check_built(fb, 'iron build after iron fmt')
        run_binary(os.path.join(froot, 'target', exe_name('pkg')), froot, exp, 'plain',
                   'target/pkg after iron fmt')
    # the same program as one file
    flat = os.path.join(d, 'flat.iron')
    with open(flat, 'w', encoding='utf-8', newline='\n') as f:
        f.write(pk.flat())
    fexe = os.path.join(d, exe_name('flat'))
    fb = run([ironc, 'build', flat, '-o', fexe], d, env, 'ironc build flat.iron')
    check_built(fb, 'ironc build flat.iron')
    run_binary(fexe, d, exp, 'plain', 'flat')


# ---------------------------------------------------------------- incr

ODD_DIRS = ('my package', 'pkg é ü', '日本語 dir', "it's here", 'a&b (c)', 'x' * 60)


def tree_build(iron, root, env, name='pkg'):
    """Build root with iron build; returns (ok, output lines or codes)."""
    b = run([iron, 'build'], root, env, f'iron build in {root}')
    if b.returncode != 0:
        got = codes(b.stdout + b.stderr)
        if not got:
            raise Fail('ccfail', f'iron build in {root} failed without a code:\n'
                                 f'{(b.stdout + b.stderr)[-3000:]}')
        return ('err', tuple(sorted(got)))
    exe = os.path.join(root, 'target', exe_name(name))
    r = subprocess.run([exe], cwd=root, capture_output=True, text=True, encoding='utf-8',
                       errors='replace', timeout=TIMEOUT)
    return ('ok', r.returncode, tuple(r.stdout.splitlines()))


def read_tree(root):
    t = {}
    for dp, dns, fns in os.walk(root):
        dns[:] = [x for x in dns if x != 'target']
        for fn in fns:
            p = os.path.join(dp, fn)
            rel = os.path.relpath(p, root).replace(os.sep, '/')
            with open(p, encoding='utf-8', newline='') as f:
                t[rel] = f.read()
    return t


def edit(rng, root, pk, step):
    """One random edit of the package on disk. Returns its description."""
    srcs = sorted(p for p in read_tree(root) if p.endswith('.iron') and p != 'src/main.iron')
    r = rng.random()
    if r < 0.25 or not srcs:
        # a new file with a function main does not call, sometimes nested
        d = rng.choice(('src', 'src/new', 'src/new/deeper', 'vendor/extra'))
        name = f'{d}/added_{step}.iron'
        write_tree(root, {name: f'pub func added_fn_{step}() -> Int {{\n    return {step}\n}}\n'})
        return f'add {name}'
    p = rng.choice(srcs)
    full = os.path.join(root, p)
    if r < 0.40:
        # rename / move into another directory
        q = rng.choice(('src/moved_%d.iron', 'src/sub_%d/x.iron', 'src/a/b/c_%d.iron')) % step
        os.makedirs(os.path.dirname(os.path.join(root, q)), exist_ok=True)
        os.rename(full, os.path.join(root, q))
        return f'move {p} -> {q}'
    if r < 0.55:
        os.remove(full)
        return f'delete {p}'
    if r < 0.75:
        # change a constant: every `= <int>` global of the file
        with open(full, encoding='utf-8', newline='') as f:
            txt = f.read()
        new = re.sub(r'^((?:val|var) \w+ = )(\d+)(?=\r?$)', lambda m: m.group(1) + str(int(m.group(2)) + step),
                     txt, flags=re.M)
        with open(full, 'w', encoding='utf-8', newline='\n') as f:
            f.write(new)
        return f'change constants of {p}'
    if r < 0.85:
        # touch without change (mtime only)
        os.utime(full, None)
        return f'touch {p}'
    # break the file, then the next edit may repair it
    with open(full, 'a', encoding='utf-8') as f:
        f.write(f'\nfunc broken_{step}( {{\n')
    return f'break {p}'


def check_incr(iron, d, seed, mode):
    rng = random.Random(f'incr-{seed}')
    pk = make_pkg(seed, negative=False)
    env = env_for(iron)
    odd = rng.random() < 0.5
    base = os.path.join(d, rng.choice(ODD_DIRS) if odd else 'w')
    root = os.path.join(base, 'pkg')
    write_tree(root, pk.tree('pkg'))
    exp = pk.expected()
    first = tree_build(iron, root, env)
    if first != ('ok', 0, tuple(exp)):
        raise Fail('wrong', f'first build: {first!r}\nexpected {exp!r}')
    # concurrent builds of the unchanged package: every one must succeed
    # and produce the same program
    if rng.random() < 0.5:
        n = rng.randint(2, 4)
        results = [None] * n

        def job(i):
            try:
                cmd = [iron, rng.choice(('build', 'check'))] if i else [iron, 'build']
                results[i] = run(cmd, root, env, f'concurrent {cmd[1]} {i}')
            except Fail as e:
                results[i] = e
        ts = [threading.Thread(target=job, args=(i,)) for i in range(n)]
        for t in ts:
            t.start()
        for t in ts:
            t.join()
        for i, r in enumerate(results):
            if isinstance(r, Fail):
                raise r
            if r.returncode != 0:
                raise Fail('race', f'concurrent build {i} failed:\n{(r.stdout + r.stderr)[-3000:]}')
        again = tree_build(iron, root, env)
        if again != first:
            raise Fail('race', f'after concurrent builds: {again!r}')
    log = []
    for step in range(1, rng.randint(3, 8)):
        log.append(edit(rng, root, pk, step))
        got = tree_build(iron, root, env)
        clean = os.path.join(d, f'clean_{step}')
        shutil.rmtree(clean, ignore_errors=True)
        write_tree(clean, read_tree(root))
        want = tree_build(iron, clean, env)
        if got != want:
            raise Fail('stale', 'after edits:\n  ' + '\n  '.join(log) +
                       f'\nincremental: {got!r}\nclean:       {want!r}')
        shutil.rmtree(clean, ignore_errors=True)


# ---------------------------------------------------------------- driver

AREAS = {'pkg': check_pkg, 'incr': check_incr}


def load_tests_area():
    try:
        import pm_tests
        AREAS['tests'] = pm_tests.check
        pm_tests.bind(sys.modules[__name__])
    except ImportError:
        pass


load_tests_area()


def check_one(a):
    iron, outdir, area, seed, mode = a
    d = os.path.join(outdir, f'{area}_{seed}')
    shutil.rmtree(d, ignore_errors=True)
    os.makedirs(d)
    try:
        AREAS[area](iron, d, seed, mode)
    except Fail as e:
        return seed, e.kind, str(e)
    except RuntimeError as e:
        return seed, 'nogen', str(e)
    shutil.rmtree(d, ignore_errors=True)
    return seed, 'ok', ''


def run_area(iron, outdir, area, seeds, mode='plain', jobs=1, quiet=False):
    work = [(iron, outdir, area, s, mode) for s in seeds]
    if jobs > 1:
        from concurrent.futures import ProcessPoolExecutor
        ex = ProcessPoolExecutor(jobs)
        results = ex.map(check_one, work, chunksize=1)
    else:
        results = map(check_one, work)
    bad = 0
    for seed, st, msg in results:
        if st == 'ok':
            if not quiet:
                print(f'{area} seed {seed}: ok', flush=True)
            continue
        bad += 1
        src = os.path.join(outdir, f'{area}_{seed}')
        dst = os.path.join(outdir, 'fail', f'{area}_{seed}')
        if os.path.isdir(src):
            shutil.rmtree(dst, ignore_errors=True)
            shutil.copytree(src, dst, symlinks=True,
                            ignore=shutil.ignore_patterns('target', '*.bin', 'flat', 'flat.exe'))
        print(f'{area} seed {seed}: {st.upper()} {dst}\n' +
              '\n'.join('    ' + l for l in msg.splitlines()[:40]), flush=True)
    return bad


# Fixed seeds for ctest, about a minute per entry on 4 cores.
SUITE = {'pkg': range(1, 25), 'incr': range(1, 7), 'tests': range(1, 17)}


def main(argv):
    if argv and argv[0] == '--gen':
        area, seed = argv[1], int(argv[2])
        if area == 'tests':
            import pm_tests
            print(pm_tests.describe(seed))
            return 0
        pk = make_pkg(seed, negative=False if area == 'incr' else None)
        for p, c in sorted(pk.tree().items()):
            print(f'==> {p}\n{c}')
        print('-- expected')
        print('\n'.join(pk.expected()))
        if pk.errors:
            print(f'-- compile error: {" or ".join(sorted(pk.errors))} ({pk.negative})')
        return 0
    if argv and argv[0] == '--suite':
        iron, outdir = os.path.abspath(argv[1]), os.path.abspath(argv[2])
        jobs = min(4, os.cpu_count() or 1)
        bad = 0
        for area in argv[3:] or ('pkg',):
            seeds = SUITE[area]
            n = run_area(iron, outdir, area, seeds, jobs=jobs, quiet=True)
            print(f'{area}: {len(seeds) - n}/{len(seeds)} ok', flush=True)
            bad += n
        return 1 if bad else 0
    mode, jobs, args = 'plain', 1, []
    i = 0
    while i < len(argv):
        if argv[i] == '-j':
            jobs = int(argv[i + 1])
            i += 1
        elif argv[i].startswith('-j') and argv[i][2:].isdigit():
            jobs = int(argv[i][2:])
        elif argv[i] == '--valgrind':
            mode = 'valgrind'
        else:
            args.append(argv[i])
        i += 1
    if len(args) < 4:
        print(__doc__)
        return 2
    area, iron, outdir, seed = args[0], os.path.abspath(args[1]), os.path.abspath(args[2]), int(args[3])
    count = int(args[4]) if len(args) > 4 else 1
    bad = run_area(iron, outdir, area, range(seed, seed + count), mode, jobs, quiet=count > 20)
    print(f'{area}: {count - bad}/{count} ok ({mode})', flush=True)
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
