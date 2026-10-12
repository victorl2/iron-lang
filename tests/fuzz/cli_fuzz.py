#!/usr/bin/env python3
"""Command line fuzzer for ironc and iron.

Random argument vectors built from the subcommands, every flag the help
lists, unknown and malformed flags, missing and extra values, and inputs
that are valid, missing, directories, empty, binary, huge, oddly named or
not .iron files, run in a scratch directory (sometimes inside a package).

Oracles, for every run:
  - no crash: no signal, no sanitizer report, no timeout;
  - the exit status is 0 or 1 (2 only for usage errors of the harness);
  - a failing run says why: something on stderr or stdout that is not
    only the general help;
  - rules the manual and the help state: an unknown flag fails and names
    the flag; `--help` / `-h` after a known subcommand prints that
    subcommand's help and exits 0 without side effects; a missing input
    file fails and names it; a directory given as the file fails; an
    extra file argument fails;
  - every flag `<tool> <sub> --help` lists is accepted by that subcommand
    (checked once per run of the fuzzer, `--help-check`).

    cli_fuzz.py IRON OUTDIR SEED [COUNT] [-j N]
    cli_fuzz.py --suite IRON OUTDIR       (fixed seeds, for ctest)
    cli_fuzz.py --help-check IRON OUTDIR
"""
import os
import random
import re
import shutil
import subprocess
import sys

TIMEOUT = int(os.environ.get('CF_TIMEOUT', '120'))
SAN = re.compile(r'AddressSanitizer|LeakSanitizer|runtime error:|UndefinedBehaviorSanitizer|'
                 r'crashed \(|internal compiler error')

SUBS_IRONC = ['build', 'run', 'check', 'fmt', 'test', 'toolchain']
SUBS_IRON = ['build', 'run', 'check', 'fmt', 'test', 'init', 'toolchain']
FLAGS = ['--release', '--debug', '--debug-build', '--emit-c', '--test', '--no-optimize',
         '--strict-v3', '--no-strict-v3', '--force-comptime', '--dump-ir-passes',
         '--report-compression', '--warn-fusion-break', '--verbose', '--check', '--lib']
VALUE_FLAGS = ['-o', '--output', '--target', '--pkg-name', '--pkg-version']
BAD_FLAGS = ['--bogus', '-x', '---', '--release=1', '--target=', '--target=bogus', '--target=../up',
             '--output=', '-L', '-l', '--ü', '-', '--debug-buil', '--RELEASE', '-oo', '--target=native',
             '--target=web', '-h=1']
VALUES = ['out', 'x/y/z', '', ' ', 'native', 'web', 'linux-x86_64', 'bogus', '-', '--', '日本',
          'a b', '../escape', '/nonexistent/dir/bin']


def files(work):
    """Inputs in work/: name -> path (created on demand)."""
    big = os.path.join(work, 'huge.iron')
    if not os.path.exists(big):
        # 8 MB, mostly comments (a huge function body takes the C compiler
        # minutes, which is not what this checks)
        with open(big, 'w') as f:
            f.write(('-- ' + 'x' * 77 + '\n') * 100000)
            f.write('func main() {\n')
            f.write('    println("' + 'x' * 70 + '")\n' * 200)
            f.write('}\n')
    specs = {
        'ok.iron': 'func main() {\n    println("ok")\n}\n',
        'err.iron': 'func main() {\n    println(undefined_name)\n}\n',
        'tests.iron': 'test "a" {\n    assert(true)\n}\ntest "b" {\n    assert_eq(1, 2)\n}\n',
        'empty.iron': '',
        'nomain.iron': 'func f() {\n}\n',
        'garbage.iron': '\x00\x01\x02\xff\xfe binary \x00',
        'sp ace é.iron': 'func main() {\n    println("spaced")\n}\n',
        'unfmt.iron': 'func main(){\nprintln("x")\n}\n',
        'text.txt': 'func main() {\n}\n',
        'crlf.iron': 'func main() {\r\n    println("crlf")\r\n}\r\n',
        'bom.iron': '﻿func main() {\n    println("bom")\n}\n',
        'longline.iron': 'func main() {\n    println("' + 'y' * 5000 + '")\n}\n',
    }
    out = {}
    for name, content in specs.items():
        p = os.path.join(work, name)
        if not os.path.exists(p):
            with open(p, 'w', encoding='utf-8', errors='surrogateescape', newline='') as f:
                f.write(content)
        out[name] = name
    os.makedirs(os.path.join(work, 'adir.iron'), exist_ok=True)
    os.makedirs(os.path.join(work, 'plaindir'), exist_ok=True)
    out['adir.iron'] = 'adir.iron'
    out['plaindir'] = 'plaindir'
    out['missing.iron'] = 'missing.iron'
    out['huge.iron'] = 'huge.iron'
    out['nested/missing.iron'] = 'nested/missing.iron'
    out['long'] = ('d' * 200 + '/') * 25 + 'x.iron'
    return out


def gen_argv(rng, inputs, tool):
    subs = SUBS_IRONC if tool == 'ironc' else SUBS_IRON
    argv = []
    r = rng.random()
    if r < 0.05:
        return []
    argv.append(rng.choice(subs + ['bogus', '', '--version', '-v', '--help', '-h'])
                if rng.random() < 0.15 else rng.choice(subs))
    for _ in range(rng.randint(0, 4)):
        k = rng.random()
        if k < 0.35:
            argv.append(rng.choice(FLAGS))
        elif k < 0.5:
            argv.append(rng.choice(VALUE_FLAGS))
            if rng.random() < 0.85:
                argv.append(rng.choice(VALUES))
        elif k < 0.62:
            argv.append(rng.choice(BAD_FLAGS))
        elif k < 0.95:
            argv.append(rng.choice(sorted(inputs)))
        else:
            argv.append(rng.choice(['--help', '-h', '--', 'arg', '-v']))
    return argv


def help_only(text):
    return text.lstrip().startswith(('iron ', 'ironc ')) and 'Usage:' in text and 'error' not in text


def check_run(rng, tool, path, argv, work, in_pkg):
    cwd = os.path.join(work, 'pkg') if in_pkg else work
    before = set(os.listdir(cwd))
    env = dict(os.environ)
    env['PATH'] = os.path.dirname(path) + os.pathsep + env.get('PATH', '')
    env.setdefault('ASAN_OPTIONS', 'detect_leaks=0')
    env['NO_COLOR'] = '1'
    try:
        r = subprocess.run([path] + argv, cwd=cwd, env=env, capture_output=True, timeout=TIMEOUT,
                           stdin=subprocess.DEVNULL)
    except subprocess.TimeoutExpired:
        return 'timeout', 'timed out'
    out = r.stdout.decode('utf-8', 'replace')
    err = r.stderr.decode('utf-8', 'replace')
    both = out + err
    ctx = f'rc={r.returncode}\n--- stdout:\n{out[-1500:]}\n--- stderr:\n{err[-2500:]}'
    if r.returncode < 0 or r.returncode >= 128 or SAN.search(err):
        return 'crash', ctx
    # the program a `run` builds exits with its own status; ok.iron and the
    # others the fuzzer writes exit 0 or panic (rc 1 or a signal caught above)
    # `fmt --check`: 2 for a file that does not parse, 3 for one that
    # cannot be read (manual 10.2)
    fmt_check = argv[:1] == ['fmt'] and '--check' in argv
    if r.returncode not in ((0, 1, 2, 3) if fmt_check else (0, 1)):
        return 'status', ctx
    if r.returncode != 0 and not both.strip():
        return 'silent', ctx
    if r.returncode != 0 and argv and help_only(out) and not re.search(
            r'unknown|missing|error|requires|cannot|not ', err):
        return 'helponly', ctx
    sub = argv[0] if argv else ''
    pre = argv[1:argv.index('--')] if '--' in argv else argv[1:]
    known = sub in (SUBS_IRONC if tool == 'ironc' else SUBS_IRON)
    if known and sub != 'toolchain' and any(a in ('--help', '-h') for a in pre):
        if r.returncode != 0 or 'Usage' not in out:
            return 'help', ctx
        after = set(os.listdir(cwd))
        if after - before:
            return 'help-side-effect', f'created {sorted(after - before)}\n{ctx}'
        return 'ok', ''
    if not known or sub == 'toolchain':
        return 'ok', ''
    # rules for build/run/check/fmt/test/init
    value_next = False
    unknown = None
    for i, a in enumerate(pre):
        if value_next:
            value_next = False
            continue
        if a in VALUE_FLAGS and a != '--target':
            value_next = True
            continue
        if a == '--target':
            value_next = True
            continue
        if a in BAD_FLAGS and a not in ('-', '--target=native', '--target=web') and \
                not a.startswith('--target=') and a not in ('--output=',):
            unknown = unknown or a
    if unknown and r.returncode == 0:
        return 'accepted-unknown', f'{unknown} was accepted\n{ctx}'
    # positional arguments (what is left after flags and their values)
    pos, skip = [], False
    for a in pre:
        if skip:
            skip = False
            continue
        if a in VALUE_FLAGS:
            skip = True
            continue
        if a.startswith('-') and a != '-':
            continue
        pos.append(a)
    if sub == 'init' or (tool == 'iron' and sub == 'fmt'):
        return 'ok', ''
    # any flag outside the plain known ones may be the reason a run failed
    odd = any(a.startswith('-') and a not in FLAGS + VALUE_FLAGS for a in pre) or \
        any(v in pre for v in VALUE_FLAGS)
    unknown = unknown or (odd and 'odd') or ('unknown flag' in both and 'flag')
    file_mode = tool == 'ironc' or any(a.endswith('.iron') for a in pos)
    limit = (2 if sub == 'test' else 1) if file_mode else (1 if sub == 'test' else 0)
    if len(pos) > limit and r.returncode == 0:
        return 'accepted-extra', f'extra argument {pos[limit]!r} was accepted\n{ctx}'
    if file_mode and pos and not unknown and len(pos) <= limit and r.returncode != 0:
        first = pos[0]
        full = os.path.join(cwd, first)
        if first == '-' or len(first) > 4000:
            return 'ok', ''
        if not os.path.exists(full) and first not in both and 'cannot' not in both:
            return 'unclear', f'missing {first!r} is not named\n{ctx}'
    if file_mode and pos and sub in ('build', 'run', 'check', 'fmt'):
        full = os.path.join(cwd, pos[0])
        if os.path.isdir(full):
            if r.returncode == 0:
                return 'accepted-dir', f'directory {pos[0]!r} was accepted\n{ctx}'
            if not unknown and len(pos) <= limit and 'directory' not in both:
                return 'unclear', f'directory {pos[0]!r} not reported as one\n{ctx}'
    return 'ok', ''


def setup(work, iron):
    os.makedirs(work, exist_ok=True)
    inputs = files(work)
    pkg = os.path.join(work, 'pkg')
    if not os.path.isdir(pkg):
        os.makedirs(os.path.join(pkg, 'src'))
        with open(os.path.join(pkg, 'iron.toml'), 'w') as f:
            f.write('[package]\nname = "clipkg"\nversion = "0.1.0"\n')
        with open(os.path.join(pkg, 'src', 'main.iron'), 'w') as f:
            f.write('func main() {\n    println("pkg")\n}\n')
        for name in inputs:
            src = os.path.join(work, name)
            if os.path.isfile(src):
                shutil.copy(src, os.path.join(pkg, name))
        os.makedirs(os.path.join(pkg, 'adir.iron'), exist_ok=True)
        os.makedirs(os.path.join(pkg, 'plaindir'), exist_ok=True)
    return inputs


def one(a):
    iron, outdir, seed = a
    rng = random.Random(f'cli-{seed}')
    work = os.path.join(outdir, f'w{os.getpid()}')   # one scratch directory per worker
    inputs = setup(work, iron)
    ironc = os.path.join(os.path.dirname(iron), 'ironc' + ('.exe' if os.name == 'nt' else ''))
    tool = rng.choice(('iron', 'ironc'))
    path = iron if tool == 'iron' else ironc
    argv = gen_argv(rng, inputs, tool)
    in_pkg = tool == 'iron' and rng.random() < 0.6
    # fmt rewrites files: give it a private copy of the inputs
    if argv and argv[0] in ('fmt', 'init'):
        priv = os.path.join(outdir, f'p{seed}')
        shutil.rmtree(priv, ignore_errors=True)
        shutil.copytree(work, priv, symlinks=True)
        work = priv
    st, msg = check_run(rng, tool, path, argv, work, in_pkg)
    if argv and argv[0] in ('fmt', 'init'):
        shutil.rmtree(work, ignore_errors=True)
    return seed, tool, argv, in_pkg, st, msg


def help_check(iron, outdir):
    """Every flag a subcommand's --help lists is accepted by it."""
    work = os.path.join(outdir, 'help')
    setup(work, iron)
    bp = os.path.join(work, 'badpkg', 'src')
    os.makedirs(bp, exist_ok=True)
    with open(os.path.join(work, 'badpkg', 'iron.toml'), 'w') as f:
        f.write('[package]\nname = "badpkg"\nversion = "0.1.0"\n')
    with open(os.path.join(bp, 'main.iron'), 'w') as f:
        f.write('func main( {\n')
    ironc = os.path.join(os.path.dirname(iron), 'ironc')
    bad = 0
    for tool, path, subs in (('ironc', ironc, ('build', 'run', 'check', 'fmt', 'test')),
                             ('iron', iron, ('build', 'run', 'check', 'fmt', 'test', 'init'))):
        for sub in subs:
            h = subprocess.run([path, sub, '--help'], capture_output=True, text=True).stdout
            flags = re.findall(r'^\s+(--?[a-z][\w-]*)', h, re.M)
            for fl in flags:
                if fl in ('--help', '-h', '--version', '-v'):
                    continue
                argv = [sub, fl]
                if fl in ('--output', '-o'):
                    argv.append('outbin')
                if fl == '--target':
                    argv[-1] = '--target=native'
                for in_pkg in ((True, False) if tool == 'iron' else (False,)):
                    a = list(argv)
                    # inputs that fail fast after the flags are parsed
                    if not in_pkg and sub not in ('init',):
                        a.append('missing.iron')
                    if sub == 'init':
                        a.append('newpkg_' + fl.strip('-').replace('-', '_'))
                    cwd = os.path.join(work, 'badpkg') if in_pkg else work
                    r = subprocess.run([path] + a, cwd=cwd, capture_output=True, text=True,
                                       stdin=subprocess.DEVNULL, timeout=TIMEOUT)
                    if 'unknown flag' in r.stderr or 'unexpected argument' in r.stderr:
                        print(f'HELP {tool} {" ".join(a)} ({"package" if in_pkg else "file"}): '
                              f'listed in --help but rejected: {r.stderr.strip()[:200]}')
                        bad += 1
    return bad


def main(argv):
    if argv and argv[0] == '--help-check':
        return 1 if help_check(os.path.abspath(argv[1]), os.path.abspath(argv[2])) else 0
    suite = argv and argv[0] == '--suite'
    if suite:
        iron, outdir, seed, count, jobs = argv[1], argv[2], 1, 400, 4
    else:
        args, jobs, i = [], 1, 0
        while i < len(argv):
            if argv[i] == '-j':
                jobs = int(argv[i + 1])
                i += 1
            else:
                args.append(argv[i])
            i += 1
        if len(args) < 3:
            print(__doc__)
            return 2
        iron, outdir, seed = args[0], args[1], int(args[2])
        count = int(args[3]) if len(args) > 3 else 100
    iron, outdir = os.path.abspath(iron), os.path.abspath(outdir)
    os.makedirs(outdir, exist_ok=True)
    work = [(iron, outdir, s) for s in range(seed, seed + count)]
    if jobs > 1:
        from concurrent.futures import ProcessPoolExecutor
        results = ProcessPoolExecutor(jobs).map(one, work, chunksize=8)
    else:
        results = map(one, work)
    bad = 0
    for seed, tool, av, in_pkg, st, msg in results:
        if st != 'ok':
            bad += 1
            print(f'cli seed {seed}: {st.upper()}: {tool} {" ".join(repr(a) for a in av)}'
                  f'{" (in a package)" if in_pkg else ""}\n' +
                  '\n'.join('    ' + l for l in msg.splitlines()[:30]), flush=True)
    hb = help_check(iron, outdir) if suite else 0
    print(f'cli: {count - bad}/{count} ok' + (f', help check: {hb} rejected' if suite else ''))
    return 1 if bad or hb else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
