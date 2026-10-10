#!/usr/bin/env python3
"""Differential program fuzzer for ironc.

Generates whole random programs in one of several areas, runs each one in
a Python model of Iron's semantics (pf_lang.py, written from the manual)
to get the expected output, compiles it with ironc and compares. With
--valgrind or --asan the program also runs under a memory checker, and a
leak or memory error is a failure like a wrong line.

Areas:
    stmt      control flow, functions, var parameters, recursion, enums, defer
    coll      lists, maps, sets and strings
    obj       objects, methods, copy and drop order, rc and weak rc, defer
    closure   lambdas capturing val and var, closures stored and called later

    prog_fuzz.py AREA IRONC OUTDIR SEED [COUNT] [--valgrind] [--asan RTDIR]
    prog_fuzz.py --suite IRONC OUTDIR          (the fixed seeds ctest runs)
    prog_fuzz.py --gen AREA SEED               (print one program and its output)
    prog_fuzz.py AREA IRONC OUTDIR SEED --reduce
        shrink a failing program by deleting and hoisting statements while
        it fails the same way; writes OUTDIR/reduced_AREA_SEED.iron

-j N runs N programs at a time.

--asan RTDIR compiles the C that `ironc build --emit-c` writes with
-fsanitize=address,undefined against a runtime built the same way into
RTDIR (built on first use from the source tree next to this script).

Exit status 1 when any program fails; failing programs are kept under
OUTDIR/fail/.
"""
import glob
import os
import random
import re
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from pf_lang import Bail  # noqa: E402

AREAS = ('stmt', 'coll', 'obj', 'closure')


def generator(area):
    mod = __import__('pf_' + area)
    return mod.generate


def make_gen(area, seed):
    gen = generator(area)
    for attempt in range(50):
        rng = random.Random(f'{area}-{seed}-{attempt}')
        try:
            return gen(rng)
        except Bail:
            continue
        except RecursionError:
            continue
    raise RuntimeError(f'{area} seed {seed}: no program after 50 attempts')


def make_program(area, seed):
    g = make_gen(area, seed)
    return g.render(), g.model()


# ------------------------------------------------------------ asan runtime

SAN = ['-g', '-O1', '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
       '-fno-sanitize-recover=undefined', '-std=gnu17', '-fwrapv',
       '-fno-strict-aliasing', '-Wno-integer-overflow', '-w']


def build_asan_rt(rtdir):
    lib = os.path.join(rtdir, 'libironrt_asan.a')
    if os.path.exists(lib):
        return lib
    src = os.path.normpath(os.path.join(HERE, '..', '..', 'src'))
    os.makedirs(rtdir, exist_ok=True)
    files = [f for f in glob.glob(os.path.join(src, 'runtime', 'iron_*.c'))
             if 'win_crt0' not in f and 'gencheck_count' not in f]
    files += [os.path.join(src, 'stdlib', f'iron_{n}.c') for n in
              ('math', 'io', 'time', 'log', 'hint', 'net', 'http', 'tls', 'websocket')]
    files += [os.path.join(src, 'util', n) for n in ('stb_ds_impl.c', 'arena.c', 'strbuf.c')]
    objs = []
    for f in files:
        o = os.path.join(rtdir, os.path.basename(f)[:-2] + '.o')
        subprocess.run(['clang', '-c'] + SAN + [f'-I{src}', f'-I{src}/vendor', f'-I{src}/stdlib',
                        f, '-o', o], check=True)
        objs.append(o)
    subprocess.run(['ar', 'rcs', lib + '.tmp'] + objs, check=True)
    os.replace(lib + '.tmp', lib)
    return lib


def asan_build(ironc, src, workdir, exe, rtlib):
    b = subprocess.run([ironc, 'build', '--emit-c', os.path.abspath(src)], cwd=workdir,
                       capture_output=True, text=True)
    if b.returncode != 0:
        return b
    name = os.path.splitext(os.path.basename(src))[0]
    c = os.path.join(workdir, '.iron-build', name + '.c')
    s = os.path.normpath(os.path.join(HERE, '..', '..', 'src'))
    return subprocess.run(['clang'] + SAN + [f'-I{s}', f'-I{s}/vendor', f'-I{s}/stdlib', c, rtlib,
                           '-lm', '-lpthread', '-o', exe], capture_output=True, text=True)


# ----------------------------------------------------------------- running

def check(ironc, outdir, area, seed, mode='plain', rtlib=None):
    """Build and run one program. Returns (status, message)."""
    src_text, expected = make_program(area, seed)
    d = os.path.join(outdir, f'{area}_{seed}')
    st, msg, _ = check_text(ironc, d, f'{area}_{seed}', src_text, expected, mode, rtlib)
    if st == 'ok':
        shutil.rmtree(d, ignore_errors=True)
    return st, msg


def check_text(ironc, d, name, src_text, expected, mode='plain', rtlib=None):
    """Returns (status, message, signature)."""
    os.makedirs(d, exist_ok=True)
    src = os.path.join(d, f'{name}.iron')
    exe = os.path.join(d, f'{name}.bin')
    with open(src, 'w', encoding='utf-8') as f:
        f.write(src_text)
    with open(os.path.join(d, 'expected.txt'), 'w', encoding='utf-8') as f:
        f.write('\n'.join(expected) + ('\n' if expected else ''))
    if os.path.exists(exe):
        os.remove(exe)
    if mode == 'asan':
        b = asan_build(ironc, src, d, exe, rtlib)
    else:
        b = subprocess.run([ironc, 'build', src, '-o', exe], capture_output=True, text=True,
                           encoding='utf-8', errors='replace')
    if b.returncode != 0:
        out = b.stdout + b.stderr
        first = next((l for l in out.splitlines() if 'error' in l), '')
        return 'compile', out[:4000], ('compile', norm(first))
    cmd = [exe]
    env = dict(os.environ)
    if mode == 'valgrind':
        cmd = ['valgrind', '--quiet', '--leak-check=full',
               '--errors-for-leak-kinds=definite,indirect', '--error-exitcode=99', exe]
    if mode == 'asan':
        env['ASAN_OPTIONS'] = 'detect_leaks=1:abort_on_error=0:exitcode=99'
        env['UBSAN_OPTIONS'] = 'print_stacktrace=1:halt_on_error=1'
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=120, env=env,
                           encoding='utf-8',
                           errors='replace')
    except subprocess.TimeoutExpired:
        return 'timeout', '', ('timeout', '')
    got = r.stdout.splitlines()
    err1 = next((l for l in r.stderr.splitlines()
                 if l.strip() and not l.startswith('==')), '')
    if 'Sanitizer' in r.stderr or 'definitely lost' in r.stderr:
        err1 = next((l for l in r.stderr.splitlines() if 'Sanitizer' in l or 'lost' in l), err1)
    sig = ('rc' if r.returncode else 'ok', norm(err1))
    if got != expected:
        sig = ('wrong',) + sig
        for i, (x, y) in enumerate(zip(expected, got)):
            if x != y:
                return ('wrong', f'line {i + 1}: expected {x!r} got {y!r} (rc={r.returncode})\n'
                        f'{r.stderr[:1500]}', sig)
        return 'wrong', (f'{len(expected)} lines expected, {len(got)} produced '
                         f'(rc={r.returncode})\n{r.stderr[:1500]}'), sig
    if r.returncode != 0:
        return ('memory' if r.returncode == 99 else 'crash',
                f'rc={r.returncode}\n{r.stderr[:3000]}', sig)
    return 'ok', '', ('ok',)


def norm(line):
    """A failure line with the program-specific parts taken out."""
    line = re.sub(r'/tmp/\S+|\S+\.iron', 'F', line)
    line = re.sub(r'0x[0-9a-f]+', 'A', line)
    return re.sub(r'\d+', 'N', line).strip()[:120]


# ---------------------------------------------------------------- reducer

def child_blocks(obj, out, seen):
    """Every Block reachable from obj (statements, functions, lambdas)."""
    from pf_lang import Block
    if id(obj) in seen:
        return
    seen.add(id(obj))
    if isinstance(obj, Block):
        out.append(obj)
        for s in obj.stmts:
            child_blocks(s, out, seen)
        return
    if isinstance(obj, (list, tuple)):
        for x in obj:
            child_blocks(x, out, seen)
        return
    d = getattr(obj, '__dict__', None)
    if d is None:
        return
    for k, v in d.items():
        if k in ('enum', 'fn', 'frame'):
            continue
        if isinstance(v, (list, tuple)) or hasattr(v, '__dict__'):
            child_blocks(v, out, seen)


def reduce_program(ironc, outdir, area, seed, mode='plain', rtlib=None, jobs=8):
    """Delete and hoist statements (and drop functions) while the program
    still fails the same way. Candidates are checked `jobs` at a time; the
    first one in program order that still fails is kept."""
    from concurrent.futures import ThreadPoolExecutor
    g = make_gen(area, seed)
    d = os.path.join(outdir, f'reduce_{area}_{seed}')
    name = f'{area}_{seed}'
    st, msg, base = check_text(ironc, d, name, g.render(), g.model(), mode, rtlib)
    if st == 'ok':
        print('does not fail')
        return 1
    print(f'failure: {st} {base}', flush=True)
    tries = [0]
    pool = ThreadPoolExecutor(jobs)

    def first_failing(cands):
        """cands: (apply, undo) pairs. Returns the index of the first
        candidate that keeps the failure, or -1. Leaves the program as it
        was."""
        work = []
        for idx, (apply, undo) in enumerate(cands):
            apply()
            try:
                exp = g.model()
                src = g.render()
                work.append((idx, src, exp))
            except Exception:
                # Bail, or an exit statement hoisted out of its loop or function
                pass
            undo()
        for k in range(0, len(work), jobs):
            batch = work[k:k + jobs]
            tries[0] += len(batch)
            futs = [pool.submit(check_text, ironc, f'{d}_{j}', name, src, exp, mode, rtlib)
                    for j, (idx, src, exp) in enumerate(batch)]
            for (idx, _, _), fu in zip(batch, futs):
                if fu.result()[2] == base:
                    for f2 in futs:
                        f2.result()
                    return idx
        return -1

    def deletion(lst, i, n):
        saved = lst[:]
        return (lambda: lst.__delitem__(slice(i, i + n)), lambda: lst.__setitem__(slice(None), saved))

    def hoist(lst, i, inner):
        saved = lst[:]
        return (lambda: lst.__setitem__(slice(i, i + 1), inner.stmts),
                lambda: lst.__setitem__(slice(None), saved))

    changed = True
    while changed:
        changed = False
        while True:
            cands = [deletion(g.funcs, i, 1) for i in range(len(g.funcs) - 1, -1, -1)]
            k = first_failing(cands)
            if k < 0:
                break
            cands[k][0]()
            changed = True
        done = set()
        while True:
            # the blocks still in the program, outermost first; a block
            # whose statement was deleted is gone with it
            blocks = []
            child_blocks([g.mainblock] + [fd.body for fd, _ in g.funcs], blocks, set())
            blk = next((b for b in blocks if id(b) not in done), None)
            if blk is None:
                break
            done.add(id(blk))
            chunk = max(1, len(blk.stmts))
            while chunk >= 1:
                while True:
                    cands = [deletion(blk.stmts, i, chunk)
                             for i in range(len(blk.stmts) - chunk, -1, -chunk)]
                    k = first_failing(cands)
                    if k < 0:
                        break
                    cands[k][0]()
                    changed = True
                chunk //= 2
            # hoist the body of a compound statement in its place
            while True:
                cands = []
                for i in range(len(blk.stmts) - 1, -1, -1):
                    inner = []
                    child_blocks(blk.stmts[i], inner, set())
                    cands += [hoist(blk.stmts, i, ib) for ib in inner[:4]]
                k = first_failing(cands)
                if k < 0:
                    break
                cands[k][0]()
                changed = True
        print(f'pass done, {tries[0]} tries, {len(g.render().splitlines())} lines', flush=True)
    pool.shutdown()
    src = g.render()
    exp = g.model()
    with open(os.path.join(outdir, f'reduced_{name}.iron'), 'w', encoding='utf-8') as f:
        f.write(src)
    with open(os.path.join(outdir, f'reduced_{name}.expected'), 'w', encoding='utf-8') as f:
        f.write('\n'.join(exp) + ('\n' if exp else ''))
    print(src)
    print('-- expected:')
    print('\n'.join(exp))
    print('-- failure:')
    print(check_text(ironc, d, name, src, exp, mode, rtlib)[1][:3000])
    return 0


def _check_job(a):
    try:
        return a[3], check(*a)
    except RuntimeError as e:
        return a[3], ('nogen', str(e))


def run(ironc, outdir, area, seeds, mode='plain', rtlib=None, quiet=False, jobs=1):
    bad = 0
    work = [(ironc, outdir, area, s, mode, rtlib) for s in seeds]
    if jobs > 1:
        from concurrent.futures import ProcessPoolExecutor
        ex = ProcessPoolExecutor(jobs)
        results = ex.map(_check_job, work, chunksize=1)
    else:
        results = map(_check_job, work)
    for seed, (st, msg) in results:
        if st == 'nogen':
            print(msg, flush=True)
            continue
        if st == 'compile':
            errs, keep = [], True
            for l in msg.splitlines():
                if l and not l[0].isspace():
                    keep = not l.startswith(('warning', 'note'))
                if keep:
                    errs.append(l)
            msg = '\n'.join(errs)
        if st != 'ok':
            bad += 1
            d = os.path.join(outdir, 'fail')
            os.makedirs(d, exist_ok=True)
            k = f'{area}_{seed}'
            for fn in (f'{k}.iron', 'expected.txt'):
                p = os.path.join(outdir, k, fn)
                if os.path.exists(p):
                    shutil.copy(p, os.path.join(d, fn if fn.endswith('.iron') else f'{k}.expected'))
            print(f'{area} seed {seed}: {st.upper()} {os.path.join(d, k + ".iron")}\n'
                  + '\n'.join('    ' + l for l in msg.splitlines()[:30]), flush=True)
        elif not quiet:
            print(f'{area} seed {seed}: ok', flush=True)
    return bad


# The fixed seeds ctest runs (prog_fuzz): each area's programs found bugs
# when they were added, so they guard the fixes.
SUITE = {'stmt': range(1, 9), 'coll': range(1, 9), 'obj': range(1, 9), 'closure': range(1, 9)}


def main(argv):
    if argv[0] == '--gen':
        src, out = make_program(argv[1], int(argv[2]))
        print(src)
        print('-- expected:')
        print('\n'.join(out))
        return 0
    if argv[0] == '--suite':
        ironc, outdir = os.path.abspath(argv[1]), argv[2]
        jobs = min(4, os.cpu_count() or 1)
        bad = 0
        for area, seeds in SUITE.items():
            if area not in available():
                continue
            n = run(ironc, outdir, area, seeds, quiet=True, jobs=jobs)
            print(f'{area}: {len(seeds) - n}/{len(seeds)} ok')
            bad += n
        return 1 if bad else 0
    mode, rtlib, jobs, reduce = 'plain', None, 1, False
    args = []
    i = 0
    while i < len(argv):
        if argv[i] == '--reduce':
            reduce = True
        elif argv[i] == '-j':
            jobs = int(argv[i + 1])
            i += 1
        elif argv[i] == '--valgrind':
            mode = 'valgrind'
        elif argv[i] == '--asan':
            mode = 'asan'
            rtlib = build_asan_rt(argv[i + 1])
            i += 1
        else:
            args.append(argv[i])
        i += 1
    area, ironc, outdir, seed = args[0], os.path.abspath(args[1]), args[2], int(args[3])
    count = int(args[4]) if len(args) > 4 else 1
    if reduce:
        return reduce_program(ironc, outdir, area, seed, mode, rtlib, jobs=max(jobs, 8))
    bad = run(ironc, outdir, area, range(seed, seed + count), mode, rtlib,
              quiet=count > 20, jobs=jobs)
    print(f'{area}: {count - bad}/{count} ok ({mode})', flush=True)
    return 1 if bad else 0


def available():
    return [a for a in AREAS if os.path.exists(os.path.join(HERE, f'pf_{a}.py'))]


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
