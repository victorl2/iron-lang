#!/usr/bin/env python3
"""Program fuzzer for the log module (manual 9.7): Log.debug / info / warn /
error with Log.set_level at every level and at values outside them, on
messages holding zero bytes, newlines, braces, multi-byte characters and
5000 bytes, from main and from up to 8 tasks logging at once.

The output goes to standard error, which prog_fuzz does not compare, so
this driver runs the programs itself: every stderr line must be
`[YYYY-MM-DD HH:MM:SS] [LEVEL] message` (no color when stderr is not a
terminal), the messages shown must be exactly those at or above the level
in force (below DEBUG shows everything, above ERROR nothing), in program
order for one task and as a whole for several tasks, with no two lines
mixed. Modes as prog_fuzz: plain, --asan RTDIR, --tsan RTDIR, --valgrind.

    log_fuzz.py IRONC OUTDIR SEED [COUNT] [--asan RTDIR | --tsan RTDIR | --valgrind] [-j N]
    log_fuzz.py --suite IRONC OUTDIR     (the fixed seeds ctest runs)
"""
import os
import random
import re
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import prog_fuzz  # noqa: E402
from stdfz_common import lit  # noqa: E402

LEVELS = ['debug', 'info', 'warn', 'error']
NAMES = {'debug': 'DEBUG', 'info': 'INFO', 'warn': 'WARN', 'error': 'ERROR'}
TS = re.compile(r'^\[\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\] \[(DEBUG|INFO|WARN|ERROR)\] ')


def rmsg(r, tag):
    k = r.random()
    body = r.choice(['plain', 'café 日本', '{braces}', 'tab\there', 'x' * 5000, '',
                     'nul\x00inside', 'ends with nul\x00', '\U0001F600', '%s %d %n'])
    return f'{tag} {body}' if k < 0.95 else f'{tag} line1\nline2'


def generate(seed):
    r = random.Random(f'log-{seed}')
    code = ['import log', '', 'func main() {']
    expect = []        # (task, level name, message bytes) in order
    level = 1
    ntasks = r.choice([0, 0, 0, 2, 4, 8])
    for i in range(r.randint(3, 25)):
        if r.random() < 0.2:
            lv = r.choice([0, 1, 2, 3, -5, 99, -9223372036854775807, 4])
            name = {0: 'Log.DEBUG', 1: 'Log.INFO', 2: 'Log.WARN', 3: 'Log.ERROR'}.get(lv, str(lv))
            code.append(f'    Log.set_level({name})')
            level = lv
            continue
        fn = r.choice(LEVELS)
        msg = rmsg(r, f'm{i}')
        code.append(f'    Log.{fn}({lit(msg)})')
        if LEVELS.index(fn) >= level:
            expect.append(('main', NAMES[fn], msg))
    if ntasks:
        for t in range(ntasks):
            code.append(f'    val h{t} = spawn("t{t}") {{')
            for j in range(r.randint(1, 20)):
                fn = r.choice(LEVELS)
                msg = rmsg(r, f't{t}.{j}')
                code.append(f'        Log.{fn}({lit(msg)})')
                if LEVELS.index(fn) >= level:
                    expect.append((f't{t}', NAMES[fn], msg))
            code.append('        return 0')
            code.append('    }')
        for t in range(ntasks):
            code.append(f'    val r{t} = await h{t}')
    code.append('    println("done")')
    code.append('}')
    return '\n'.join(code) + '\n', expect


def lines_of(expect):
    """Expected stderr lines (after the timestamp) per task."""
    out = {}
    for task, lv, msg in expect:
        text = f'[{lv}] {msg}'
        out.setdefault(task, []).extend(text.split('\n'))
    return out


def check(ironc, outdir, seed, mode, rtlib):
    src, expect = generate(seed)
    d = os.path.join(outdir, f'log_{seed}')
    os.makedirs(d, exist_ok=True)
    path = os.path.join(d, f'log_{seed}.iron')
    exe = os.path.join(d, f'log_{seed}.bin')
    with open(path, 'w', encoding='utf-8') as f:
        f.write(src)
    if mode in ('asan', 'tsan'):
        b = prog_fuzz.asan_build(ironc, path, d, exe, rtlib, mode)
    else:
        b = subprocess.run([ironc, 'build', path, '-o', exe], capture_output=True, text=True)
    if b.returncode != 0:
        return 'compile', (b.stdout + b.stderr)[:3000]
    cmd = [exe]
    env = dict(os.environ)
    if mode == 'valgrind':
        cmd = ['valgrind', '--quiet', '--leak-check=full', '--errors-for-leak-kinds=definite,indirect',
               '--error-exitcode=99', exe]
    if mode == 'asan':
        env['ASAN_OPTIONS'] = 'detect_leaks=1:exitcode=99'
    if mode == 'tsan':
        env['TSAN_OPTIONS'] = 'halt_on_error=1:exitcode=99'
    p = subprocess.run(cmd, capture_output=True, timeout=120, env=env)
    if p.returncode != 0:
        return 'crash', f'rc={p.returncode}\n' + p.stderr.decode('utf-8', 'replace')[-3000:]
    if p.stdout.decode() != 'done\n':
        return 'wrong', 'stdout: ' + p.stdout.decode('utf-8', 'replace')[:200]
    err = p.stderr.decode('utf-8', 'surrogateescape')
    got_lines = [l for l in err.split('\n')[:-1] if not l.startswith('==')]
    # strip timestamps; a continuation line (a message with a newline) has none
    stripped = []
    for l in got_lines:
        m = TS.match(l)
        stripped.append(l[len(m.group(0)) - len(f'[{m.group(1)}] '):] if m else l)
    exp = lines_of(expect)
    want_total = sum(len(v) for v in exp.values())
    if len(stripped) != want_total:
        return 'wrong', f'{want_total} lines expected, {len(stripped)} logged'
    if set(exp) == {'main'} or not exp:
        if stripped != exp.get('main', []):
            for i, (a, b) in enumerate(zip(exp.get('main', []), stripped)):
                if a != b:
                    return 'wrong', f'line {i + 1}: expected {a[:120]!r} got {b[:120]!r}'
        return 'ok', ''
    # several tasks: every task's lines in its order, the whole a permutation
    if sorted(stripped) != sorted(sum(exp.values(), [])):
        return 'wrong', 'the lines logged are not the lines expected (or two lines mixed)'
    for task, want in exp.items():
        # the first line of each message carries its task's tag
        tag = 'm' if task == 'main' else task + '.'
        first = [l for l in want if re.match(r'\[\w+\] ' + re.escape(tag), l)]
        fs = set(first)
        mine = [l for l in stripped if l in fs]
        if mine != first:
            return 'wrong', f'{task}: its lines are out of order'
    return 'ok', ''


SUITE = range(1, 41)


def run(ironc, outdir, seeds, mode, rtlib, quiet):
    from concurrent.futures import ThreadPoolExecutor
    bad = 0
    pool = ThreadPoolExecutor(min(4, os.cpu_count() or 1))
    results = pool.map(lambda s: (s, check(ironc, outdir, s, mode, rtlib)), seeds)
    for s, (st, msg) in results:
        if st != 'ok':
            bad += 1
            print(f'log seed {s}: {st.upper()} {os.path.join(outdir, f"log_{s}")}\n    ' +
                  '\n    '.join(msg.splitlines()[:20]), flush=True)
        else:
            shutil.rmtree(os.path.join(outdir, f'log_{s}'), ignore_errors=True)
            if not quiet:
                print(f'log seed {s}: ok', flush=True)
    return bad


def main(argv):
    if argv[0] == '--suite':
        ironc, outdir = os.path.abspath(argv[1]), argv[2]
        bad = run(ironc, outdir, SUITE, 'plain', None, True)
        print(f'log: {len(SUITE) - bad}/{len(SUITE)} ok')
        return 1 if bad else 0
    mode, rtlib, args = 'plain', None, []
    i = 0
    while i < len(argv):
        if argv[i] in ('--asan', '--tsan'):
            mode = argv[i][2:]
            rtlib = prog_fuzz.build_asan_rt(argv[i + 1], mode)
            i += 1
        elif argv[i] == '--valgrind':
            mode = 'valgrind'
        else:
            args.append(argv[i])
        i += 1
    ironc, outdir, seed = os.path.abspath(args[0]), args[1], int(args[2])
    count = int(args[3]) if len(args) > 3 else 1
    bad = run(ironc, outdir, range(seed, seed + count), mode, rtlib, count > 20)
    print(f'log: {count - bad}/{count} ok ({mode})')
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
