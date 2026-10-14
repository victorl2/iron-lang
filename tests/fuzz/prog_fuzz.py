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
    conc      spawn / await, Mutex, RWLock, channels, parallel for, rc shared
              between threads (outputs deterministic by construction)
    arena     arena allocation, `in` blocks, save / restore / reset, arenas
              passed to functions, owning fields (W0605), stale pointers
    num       every integer width, Float and Float32: arithmetic, conversions,
              shifts, compound assignments, float text; widths mixed (errors)
    generic   generic objects, enums and functions, constraints, Map and Set
              with generic keys, instances of instances, rc type arguments
    ptr       checked pointers, `*var`, `?*T`, heap with defer free, Box and
              unchecked pointers; stale reads (panics) and rejected forms
    comptime  functions evaluated with comptime and called at run time; the
              constructs comptime leaves out (E0231)
    numedge   NaN, infinities, signed zeros, huge and subnormal floats, float
              to integer conversion, overflow, shift counts, division by zero
    strcoll   String methods on empty, multi-byte and zero-byte strings, out of
              range positions, parsing; list, Map and Set edge cases
    genedge   constraints, generic copy / drop / nocopy, recursive generic
              enums, lists and maps as type arguments, deep instances
    ptredge   pointer fields, pointers across frames and closures, heap and
              global pointers, RawPtr / Ptr; dangling and stale pointer panics
    ctedge    comptime Float, String and list computation, read_file, comptime
              results as constants, E0230 / E0231 / E0232
    dropord   destruction order of objects, interfaces, lists, closures, rc;
              reference cycles

    prog_fuzz.py AREA IRONC OUTDIR SEED [COUNT] [--valgrind | --helgrind [RTDIR] |
                 --drd [RTDIR] | --asan RTDIR | --tsan RTDIR] [--runs N]
    prog_fuzz.py --suite IRONC OUTDIR [AREA...] (the fixed seeds ctest runs)
    prog_fuzz.py --gen AREA SEED               (print one program and its output)
    prog_fuzz.py AREA IRONC OUTDIR SEED --reduce
        shrink a failing program by deleting and hoisting statements while
        it fails the same way; writes OUTDIR/reduced_AREA_SEED.iron

-j N runs N programs at a time. --runs N runs each binary N times (a
thread schedule that differs between runs can change the output). A run
longer than PF_TIMEOUT seconds (default 120; lower it to reduce a hang)
fails as a timeout.

--asan RTDIR compiles the C that `ironc build --emit-c` writes with
-fsanitize=address,undefined against a runtime built the same way into
RTDIR (built on first use from the source tree next to this script);
--tsan RTDIR does the same with -fsanitize=thread. --helgrind and --drd
run the binary under those valgrind tools; a reported race fails it. Given
an RTDIR, they build the program and the runtime at -O0 without inlining,
so valgrind_threads.supp can hide what the tools cannot model (C11 atomic
refcounts and one-time initialization, glibc's thread stack cache).

A program's expected outcome (pf_lang.Expected) is its output lines plus,
optionally: `errors`, diagnostic codes one of which the build must report
(the program does not build); `panic`, a text the error output must contain
after the lines; `leaks_ok`, leaks allowed (arena objects whose fields own
memory leak by definition, manual 6.6). A generator sets them on the
Expected its model returns, or as attributes (`expect_panic`, `leaks_ok`).
The leak checkers are off for a program that panics or allows leaks.

Exit status 1 when any program fails; failing programs are kept under
OUTDIR/fail/.
"""
import glob
import json
import os
import random
import re
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from pf_lang import Bail, Expected  # noqa: E402

AREAS = ('stmt', 'coll', 'obj', 'closure', 'conc', 'arena', 'num', 'generic', 'ptr', 'comptime',
         'numedge', 'genedge', 'ptredge', 'ctedge', 'strcoll', 'dropord',
         'sync', 'arenax', 'jsonp', 'crypto', 'http', 'soa', 'fusible')


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


def expected_of(g, cached=False):
    """g's expected output. A generator that runs its model while
    generating (a threaded model is slow) keeps it in lines_cache, which
    make_program may reuse; the reducer always runs the model again."""
    lines = getattr(g, 'lines_cache', None) if cached else None
    m = lines if lines is not None else g.model()
    e = Expected(m, errors=getattr(m, 'errors', None),
                 panic=getattr(m, 'panic', None) or getattr(g, 'expect_panic', None))
    e.leaks_ok = bool(getattr(m, 'leaks_ok', False) or getattr(g, 'leaks_ok', False)
                      or e.panic)
    # output the model does not predict (pf_soa: copy and drop hooks); the
    # program is checked by comparing its two layouts
    e.any_output = bool(getattr(g, 'any_output', False))
    # files the program reads (comptime read_file), written next to it
    e.files = dict(getattr(g, 'files', None) or {})
    return e


def make_program(area, seed):
    g = make_gen(area, seed)
    return g.render(), expected_of(g, cached=True)


# ------------------------------------------------------------ asan runtime

SAN_COMMON = ['-g', '-O1', '-fno-omit-frame-pointer', '-std=gnu17', '-fwrapv',
              '-fno-strict-aliasing', '-ffp-contract=off', '-Wno-integer-overflow', '-w']
SAN_FLAGS = {
    'plain': ['-O3'],
    'asan': ['-fsanitize=address,undefined', '-fno-sanitize-recover=undefined'],
    'tsan': ['-fsanitize=thread'],
    # helgrind / DRD: the runtime's happens-before annotations on (neither
    # tool models C11 atomics), unoptimized and not inlined so the
    # suppressions in valgrind_threads.supp can name the atomic functions
    'vg': ['-O0', '-fno-inline', '-DIRON_VALGRIND_ANNOTATE'],
}
VG_SUPP = os.path.join(HERE, 'valgrind_threads.supp')
SAN = SAN_COMMON + SAN_FLAGS['asan']


def build_asan_rt(rtdir, flavor='asan'):
    san = SAN_COMMON + SAN_FLAGS[flavor]
    lib = os.path.join(rtdir, f'libironrt_{flavor}.a')
    if os.path.exists(lib):
        return lib
    src = os.path.normpath(os.path.join(HERE, '..', '..', 'src'))
    os.makedirs(rtdir, exist_ok=True)
    files = [f for f in glob.glob(os.path.join(src, 'runtime', 'iron_*.c'))
             if 'win_crt0' not in f and 'gencheck_count' not in f]
    files += [os.path.join(src, 'stdlib', f'iron_{n}.c') for n in
              ('math', 'io', 'time', 'log', 'hint', 'net', 'http', 'tls', 'websocket',
               'encoding', 'json', 'crypto')]
    files += [os.path.join(src, 'util', n) for n in ('stb_ds_impl.c', 'arena.c', 'strbuf.c')]
    objs = []
    for f in files:
        o = os.path.join(rtdir, os.path.basename(f)[:-2] + '.o')
        subprocess.run(['clang', '-c'] + san + ssl_flags() + [f'-I{src}', f'-I{src}/vendor',
                        f'-I{src}/stdlib', f, '-o', o], check=True)
        objs.append(o)
    subprocess.run(['ar', 'rcs', lib + '.tmp'] + objs, check=True)
    os.replace(lib + '.tmp', lib)
    return lib


def ssl_flags():
    """OpenSSL for the crypto, tls and https code of the sanitizer runtime,
    when its headers are installed (as the compiler builds programs that
    import crypto or http)."""
    return ['-DIRON_HAVE_OPENSSL=1'] if os.path.exists('/usr/include/openssl/evp.h') else []


def ssl_libs():
    return ['-lssl', '-lcrypto'] if ssl_flags() else []


def asan_build(ironc, src, workdir, exe, rtlib, flavor='asan', build_args=()):
    b = subprocess.run([ironc, 'build'] + list(build_args) + ['--emit-c', os.path.abspath(src)], cwd=workdir,
                       capture_output=True, text=True)
    if b.returncode != 0:
        return b
    name = os.path.splitext(os.path.basename(src))[0]
    c = os.path.join(workdir, '.iron-build', name + '.c')
    s = os.path.normpath(os.path.join(HERE, '..', '..', 'src'))
    return subprocess.run(['clang'] + SAN_COMMON + SAN_FLAGS[flavor] +
                          ssl_flags() + [f'-I{s}', f'-I{s}/vendor', f'-I{s}/stdlib', c, rtlib,
                                         '-lm', '-lpthread'] + ssl_libs() + ['-o', exe],
                          capture_output=True, text=True)


# ----------------------------------------------------------------- running

def check(ironc, outdir, area, seed, mode='plain', rtlib=None):
    """Build and run one program. Returns (status, message)."""
    src_text, expected = make_program(area, seed)
    d = os.path.join(outdir, f'{area}_{seed}')
    st, msg, _ = check_text(ironc, d, f'{area}_{seed}', src_text, expected, mode, rtlib)
    if st == 'ok' and area == 'soa':
        # Keep the same source basename and lines, so panic sites compare
        # exactly after replacing only the differing absolute directory.
        aos = os.path.join(d, 'aos')
        aos_text = src_text.replace(', layout: soa]', ']')
        st, msg, _ = check_text(ironc, aos, f'{area}_{seed}', aos_text, expected, mode, rtlib)
        if st == 'ok':
            def outcome(path):
                with open(os.path.join(path, 'outcome.json'), encoding='utf-8') as f:
                    value = json.load(f)
                value['panic'] = value['panic'].replace(path, '<program>')
                return value
            if outcome(d) != outcome(aos):
                st, msg = 'wrong', 'SoA and AoS output, exit code or panic differs'
    if st == 'ok' and area == 'fusible':
        st, msg = check_fusible(ironc, d, f'{area}_{seed}', src_text, expected, mode, rtlib)
    if st == 'ok':
        shutil.rmtree(d, ignore_errors=True)
    return st, msg


def check_fusible(ironc, d, name, src_text, expected, mode, rtlib):
    """The checks of the fusible area (pf_fusible.py) after the program as
    written passed: the same program with its `//iron:fusible` hints removed
    and with --no-optimize must behave the same, the optimized C must fuse
    exactly the lines the source marks, and W0618 must be reported for
    exactly the marked hints."""
    sys.path.insert(0, os.path.normpath(os.path.join(HERE, '..', 'integration')))
    import fusible_parity as fp

    def outcome(path):
        with open(os.path.join(path, 'outcome.json'), encoding='utf-8') as f:
            value = json.load(f)
        value['panic'] = value['panic'].replace(path, '<program>')
        return value

    texts = {'hinted': (src_text, ()), 'plain': (fp.strip_hints(src_text), ()),
             'noopt': (src_text, ('--no-optimize',))}
    for variant in ('plain', 'noopt'):
        text, args = texts[variant]
        vd = os.path.join(d, variant)
        st, msg, _ = check_text(ironc, vd, name, text, expected, mode, rtlib, args)
        if st != 'ok':
            return st, f'{variant}: {msg}'
        if outcome(vd) != outcome(d):
            return 'wrong', f'hinted and {variant} differ in output, exit code or panic'
    want_fused = fp.expected_fusion(src_text)
    want_warn = fp.expected_warnings(src_text)
    for variant, (text, args) in texts.items():
        cg = os.path.abspath(os.path.join(d, 'c_' + variant))
        os.makedirs(cg, exist_ok=True)
        csrc = os.path.join(cg, f'{name}.iron')
        with open(csrc, 'w', encoding='utf-8') as f:
            f.write(text)
        b = subprocess.run([ironc, 'build'] + list(args) + ['--emit-c', csrc], cwd=cg,
                           capture_output=True, text=True, encoding='utf-8', errors='replace')
        cfile = os.path.join(cg, '.iron-build', f'{name}.c')
        if b.returncode != 0 or not os.path.exists(cfile):
            return 'compile', f'{variant}: no generated C\n' + (b.stdout + b.stderr)[:2000]
        with open(cfile, encoding='utf-8', errors='replace') as f:
            have = fp.actual_fusion(f.read())
        target = [] if variant == 'noopt' else want_fused
        if have != target:
            return 'wrong', (f'{variant}: fused loops (line, stages) {have}, the rules give '
                             f'{target}')
        seen = fp.warnings_of(b.stdout + b.stderr)
        target_w = {} if variant == 'plain' else want_warn
        if sorted(l for l, _ in seen) != sorted(target_w) or any(
                target_w.get(l, '') not in m for l, m in seen):
            return 'wrong', (f'{variant}: W0618 warnings {seen}, the rules give '
                             f'{sorted(target_w.items())}')
    return 'ok', ''


def check_text(ironc, d, name, src_text, expected, mode='plain', rtlib=None,
               build_args=()):
    """Returns (status, message, signature)."""
    os.makedirs(d, exist_ok=True)
    src = os.path.join(d, f'{name}.iron')
    exe = os.path.join(d, f'{name}.bin')
    with open(src, 'w', encoding='utf-8') as f:
        f.write(src_text)
    for fname, content in (getattr(expected, 'files', None) or {}).items():
        with open(os.path.join(d, fname), 'w', encoding='utf-8', newline='') as f:
            f.write(content)
    errors = getattr(expected, 'errors', None)
    panic = getattr(expected, 'panic', None)
    with open(os.path.join(d, 'expected.txt'), 'w', encoding='utf-8') as f:
        f.write('\n'.join(expected) + ('\n' if expected else ''))
        if errors:
            f.write('-- compile error: ' + ' or '.join(sorted(errors)) + '\n')
        if panic:
            f.write('-- then panics: ' + panic + '\n')
    if os.path.exists(exe):
        os.remove(exe)
    if mode in ('asan', 'tsan') or (mode == 'plain' and rtlib):
        b = asan_build(ironc, src, d, exe, rtlib, mode, build_args)
    elif mode in ('helgrind', 'drd') and rtlib:
        b = asan_build(ironc, src, d, exe, rtlib, 'vg', build_args)
    else:
        b = subprocess.run([ironc, 'build'] + list(build_args) + [src, '-o', exe], capture_output=True, text=True,
                           encoding='utf-8', errors='replace')
    out = b.stdout + b.stderr
    if errors:
        codes = set(re.findall(r'error\[(E\d{4})\]', out))
        if b.returncode != 0 and codes & errors:
            return 'ok', '', ('ok',)
        if b.returncode == 0:
            return ('wrong', 'built, but ' + ' or '.join(sorted(errors)) + ' was expected',
                    ('wrong', 'built'))
        first = next((l for l in out.splitlines() if 'error' in l), '')
        return ('compile', ' or '.join(sorted(errors)) + ' expected, got:\n' + out[:4000],
                ('compile', norm(first)))
    if b.returncode != 0:
        first = next((l for l in out.splitlines() if 'error' in l), '')
        return 'compile', out[:4000], ('compile', norm(first))
    leaks_ok = getattr(expected, 'leaks_ok', False) or bool(panic)
    cmd = [exe]
    env = dict(os.environ)
    if mode == 'valgrind':
        cmd = ['valgrind', '--quiet', '--leak-check=' + ('no' if leaks_ok else 'full'),
               '--errors-for-leak-kinds=definite,indirect', '--error-exitcode=99', exe]
    if mode in ('helgrind', 'drd'):
        cmd = ['valgrind', '--quiet', f'--tool={mode}', '--error-exitcode=99',
               f'--suppressions={VG_SUPP}', exe]
    if mode == 'asan':
        # (LeakSanitizer does not exist on macOS: asking for it ends the run)
        leaks = 0 if leaks_ok or sys.platform == 'darwin' else 1
        env['ASAN_OPTIONS'] = (f'detect_leaks={leaks}:abort_on_error=0:exitcode=99:'
                               'detect_stack_use_after_return=1')
        env['UBSAN_OPTIONS'] = 'print_stacktrace=1:halt_on_error=1'
    if mode == 'tsan':
        env['TSAN_OPTIONS'] = 'halt_on_error=1:exitcode=99:second_deadlock_stack=1'
    for _ in range(max(1, int(os.environ.get('PF_RUNS', '1')))):
        st = run_once(cmd, env, expected, panic, d, mode)
        if st[0] != 'ok':
            break
    return st


def run_once(cmd, env, expected, panic, d, mode):
    try:
        # helgrind and DRD run a threaded program 20 to 100 times slower
        slow = 4 if mode in ('helgrind', 'drd') else 1
        r = subprocess.run(cmd, capture_output=True, text=True,
                           timeout=int(os.environ.get('PF_TIMEOUT', '120')) * slow, env=env,
                           encoding='utf-8',
                           errors='replace')
    except subprocess.TimeoutExpired:
        return 'timeout', '', ('timeout', '')
    # Panic identity excludes stack traces, whose addresses vary per run.
    # Retain the panic header and source site, excluding native stack frames.
    panic_line = '\n'.join(line for line in r.stderr.splitlines()
                           if line.startswith('iron:') or line.startswith('  site:')
                           or line.startswith('{\"panic\":')) if panic else ''
    with open(os.path.join(d, 'outcome.json'), 'w', encoding='utf-8') as f:
        json.dump({'stdout': r.stdout, 'returncode': r.returncode, 'panic': panic_line}, f)
    got = r.stdout.splitlines()
    with open(os.path.join(d, 'got.txt'), 'w', encoding='utf-8') as f:
        f.write(r.stdout + ('\n-- stderr:\n' + r.stderr[:4000] if r.stderr else ''))
    err1 = next((l for l in r.stderr.splitlines()
                 if l.strip() and not l.startswith('==')), '')
    if 'Sanitizer' in r.stderr or 'definitely lost' in r.stderr:
        err1 = next((l for l in r.stderr.splitlines() if 'Sanitizer' in l or 'lost' in l), err1)
    sig = ('rc' if r.returncode else 'ok', norm(err1))
    if (panic and mode == 'asan' and 'heap-use-after-free' in r.stderr and
            'iron_check_pointer_gen' in r.stderr):
        # The stale pointer check reads the freed block's header (the
        # release allocator has the block back); AddressSanitizer stops
        # there, before the panic. Expected for a program that panics so.
        return 'ok', '', ('ok',)
    if got != expected and not getattr(expected, 'any_output', False):
        sig = ('wrong',) + sig
        for i, (x, y) in enumerate(zip(expected, got)):
            if x != y:
                return ('wrong', f'line {i + 1}: expected {x!r} got {y!r} (rc={r.returncode})\n'
                        f'{r.stderr[:1500]}', sig)
        return 'wrong', (f'{len(expected)} lines expected, {len(got)} produced '
                         f'(rc={r.returncode})\n{r.stderr[:1500]}'), sig
    if panic is not None:
        if r.returncode == 99 or 'Sanitizer' in r.stderr:
            return 'memory', f'rc={r.returncode}\n{r.stderr[:3000]}', sig
        if r.returncode == 0 or panic not in r.stderr:
            return ('nopanic', f'expected a runtime error containing {panic!r} '
                    f'(rc={r.returncode})\n{r.stderr[:1500]}', ('nopanic', sig[1]))
        return 'ok', '', ('ok',)
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
    # A candidate with a deleted send or await blocks the threaded model
    # (pf_conc) until its timeout: keep that short while reducing.
    mod = sys.modules.get('pf_' + area)
    if hasattr(mod, 'TIMEOUT'):
        mod.TIMEOUT = 2
    d = os.path.join(outdir, f'reduce_{area}_{seed}')
    name = f'{area}_{seed}'
    st, msg, base = check_text(ironc, d, name, g.render(), expected_of(g), mode, rtlib)
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
                exp = expected_of(g)
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
        # segment programs (pf_seg.py): drop whole segments
        while hasattr(g, 'segs') and len(g.segs) > 1:
            cands = [deletion(g.segs, i, 1) for i in range(len(g.segs) - 1, -1, -1)]
            k = first_failing(cands)
            if k < 0:
                break
            cands[k][0]()
            changed = True
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
            child_blocks([g.mainblock] + [fe[0].body for fe in g.funcs], blocks, set())
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
    exp = expected_of(g)
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
            for fn, out in ((f'{k}.iron', f'{k}.iron'), ('expected.txt', f'{k}.expected'),
                            ('got.txt', f'{k}.got')):
                p = os.path.join(outdir, k, fn)
                if os.path.exists(p):
                    shutil.copy(p, os.path.join(d, out))
            for p in glob.glob(os.path.join(outdir, k, 'data_*')):
                shutil.copy(p, os.path.join(d, os.path.basename(p)))
            print(f'{area} seed {seed}: {st.upper()} {os.path.join(d, k + ".iron")}\n'
                  + '\n'.join('    ' + l for l in msg.splitlines()[:30]), flush=True)
        elif not quiet:
            print(f'{area} seed {seed}: ok', flush=True)
    return bad


# The fixed seeds ctest runs (prog_fuzz): each area's programs found bugs
# when they were added, so they guard the fixes.
SUITE = {'stmt': range(1, 9), 'coll': range(1, 9), 'obj': range(1, 9), 'closure': range(1, 9),
         'conc': range(1, 9), 'arena': range(1, 9),
         'num': range(1, 9), 'generic': range(1, 9), 'ptr': (1, 2, 3, 4, 5, 22, 26, 41),
         'comptime': range(1, 9),
         # the edge case areas: their first seeds found the bugs fixed with
         # them (#466 .. #505); about a minute per ctest entry
         'numedge': range(1, 49), 'strcoll': range(1, 49), 'genedge': range(1, 49),
         'ptredge': range(1, 49), 'ctedge': range(1, 25), 'dropord': range(1, 25),
         'sync': range(1, 9), 'arenax': range(1, 9), 'jsonp': range(1, 7), 'crypto': range(1, 7),
         'http': range(1, 5), 'soa': range(1, 25), 'fusible': range(1, 13)}
# `--suite IRONC OUTDIR` runs these; the other areas have ctest entries of
# their own (`--suite IRONC OUTDIR conc`, `... num generic ptr comptime`,
# `... sync arenax`, `... jsonp crypto http`).
SUITE_DEFAULT = ('stmt', 'coll', 'obj', 'closure')


def main(argv):
    if argv[0] == '--gen':
        src, out = make_program(argv[1], int(argv[2]))
        print(src)
        print('-- expected:')
        print('\n'.join(out))
        return 0
    if argv[0] == '--suite':
        ironc, outdir = os.path.abspath(argv[1]), argv[2]
        areas = argv[3:] or SUITE_DEFAULT
        jobs = min(4, os.cpu_count() or 1)
        bad = 0
        for area in areas:
            seeds = SUITE[area]
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
        elif argv[i].startswith('-j') and argv[i][2:].isdigit():
            jobs = int(argv[i][2:])
        elif argv[i] == '--valgrind':
            mode = 'valgrind'
        elif argv[i] in ('--helgrind', '--drd'):
            # RTDIR is optional: with it, programs and runtime are built
            # unoptimized for the suppressions (see SAN_FLAGS['vg'])
            mode = argv[i][2:]
            if i + 1 < len(argv) and not argv[i + 1].startswith('-') and \
                    not argv[i + 1].isdigit() and len(args) >= 4:
                rtlib = build_asan_rt(argv[i + 1], 'vg')
                i += 1
        elif argv[i] == '--runs':
            os.environ['PF_RUNS'] = argv[i + 1]
            i += 1
        elif argv[i] == '--cached-runtime':
            rtlib = build_asan_rt(argv[i + 1], 'plain')
            i += 1
        elif argv[i] in ('--asan', '--tsan'):
            mode = argv[i][2:]
            rtlib = build_asan_rt(argv[i + 1], mode)
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
