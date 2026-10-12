#!/usr/bin/env python3
"""Fuzzer for comments, documentation, directives, `iron fmt` and
`iron fmt --migrate-comments` (manual section 1.2).

Takes runnable fixtures of the v4 corpus (a .iron with an .expected
output), sprinkles random comments over them and checks that comments never
change what a program does:

  - trailing comments at random line ends, in every spelling (`//`, `--`,
    `///`, directives `//key:value`, `//iron:` hints, look-alikes such as
    `--json:x`, `---/`, `////`, URLs), with text full of quotes, braces,
    slashes and dashes;
  - own-line comments in place of random blank lines (so line numbers,
    which panics and diagnostics print, do not move), which also attaches
    documentation and directives to the declarations below them;

then, for each mutated program:

  1. it builds and prints its fixture's .expected output;
  2. `ironc fmt --migrate-comments` accepts it, the migrated program prints
     the same, `--check` passes on the result and a second migration
     changes nothing;
  3. `ironc fmt` accepts the mutated and the migrated program, the result
     prints the same and formatting it again changes nothing.

    comment_fuzz.py IRONC OUTDIR SEED [COUNT]
    comment_fuzz.py --suite IRONC OUTDIR     (the fixed seeds ctest runs)

Exit status 1 on any failure; failing programs are kept under OUTDIR/fail/.
"""
import os
import random
import re
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
CORPUS = os.path.join(REPO, 'tests', 'integration', 'v4')
MARKERS = re.compile(r'@(expect-panic|compile-only|expected-pass-after|file:|skip-no-optimize)')

TEXT_BITS = ['note', 'x', '"', '""', '"{x}"', '{', '}', '{{', '/', '//', '///',
             '--', '-', '->', '*/', '/*', 'http://example.com/a//b', '\\',
             '\\"', 'é', '😀', 'TODO', 'json:id', 'iron:fusible', ':', ' ',
             '\t', 'val x = 1', 'func f() {', '#']
DIRECTIVES = ['json:id', 'db:primary_key', 'k:v', 'rpc:name x y', 'a_1:',
              'iron:fusible', 'iron:fusable', 'tag:"q"', 'x:{y}']


def text(rng):
    return ''.join(rng.choice(TEXT_BITS) for _ in range(rng.randint(0, 5)))


def comment(rng):
    kind = rng.randrange(10)
    if kind == 0:
        return '//' + rng.choice(DIRECTIVES)
    if kind == 1:
        return '--' + rng.choice(DIRECTIVES)          # not a directive
    if kind == 2:
        return '///' + text(rng)
    if kind == 3:
        return '--' + text(rng)
    if kind == 4:
        return rng.choice(['//', '--', '///', '////', '---/', '--/x', '//-', '// '])
    return rng.choice(['// ', '//', '-- ', '/// ']) + text(rng)


def mutate(src, rng):
    lines = src.split('\n')
    out = []
    for line in lines:
        stripped = line.strip()
        if stripped == '' and rng.random() < 0.5:
            out.append(' ' * rng.randint(0, 8) + comment(rng))
            continue
        # A line that already ends in a comment, or has a string, keeps its
        # end: the comment would land inside the string or comment text.
        if stripped and '"' not in line and '--' not in line and '//' not in line \
                and rng.random() < 0.3:
            line = line + ' ' * rng.randint(0, 3) + comment(rng)
        out.append(line)
    return '\n'.join(out)


def run(cmd, cwd, timeout=120):
    try:
        p = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True,
                           timeout=timeout, errors='replace')
        return p.returncode, p.stdout, p.stderr
    except subprocess.TimeoutExpired:
        return -1, '', 'timeout'


def fixtures():
    found = []
    for root, _, files in os.walk(CORPUS):
        for f in sorted(files):
            if not f.endswith('.iron'):
                continue
            path = os.path.join(root, f)
            exp = path[:-5] + '.expected'
            if not os.path.exists(exp):
                continue
            src = open(path, encoding='utf-8', errors='replace').read()
            if '"""' in src or MARKERS.search(src[:2000]) or 'import' in src:
                continue
            found.append((path, exp))
    return sorted(found)


def one_case(ironc, work, path, exp, rng):
    """Returns None when every check holds, else a description."""
    want = open(exp, encoding='utf-8', errors='replace').read()
    src = open(path, encoding='utf-8', errors='replace').read()
    mutated = mutate(src, rng)
    prog = os.path.join(work, 'case.iron')
    with open(prog, 'w', encoding='utf-8') as f:
        f.write(mutated)
    rc, out, err = run([ironc, 'run', 'case.iron'], work)
    if rc != 0 or out != want:
        # The fixture may print something run-dependent: skip it unless
        # the untouched fixture is stable here.
        with open(os.path.join(work, 'orig.iron'), 'w', encoding='utf-8') as f:
            f.write(src)
        rc0, out0, _ = run([ironc, 'run', 'orig.iron'], work)
        if rc0 != 0 or out0 != want:
            return None
        return 'comments changed the program (exit %d)\n%s' % (rc, err[-2000:])

    rc, _, err = run([ironc, 'fmt', '--migrate-comments', 'case.iron'], work)
    if rc != 0:
        return 'migration refused or failed\n' + err[-2000:]
    migrated = open(prog, encoding='utf-8').read()
    rc, out, err = run([ironc, 'run', 'case.iron'], work)
    if rc != 0 or out != want:
        return 'the migrated program prints something else\n' + err[-2000:]
    rc, _, err = run([ironc, 'fmt', '--migrate-comments', '--check', 'case.iron'], work)
    if rc != 0 or open(prog, encoding='utf-8').read() != migrated:
        return 'a second migration would change the file\n' + err[-2000:]

    for label, text_ in (('mutated', mutated), ('migrated', migrated)):
        with open(prog, 'w', encoding='utf-8') as f:
            f.write(text_)
        rc, _, err = run([ironc, 'fmt', 'case.iron'], work)
        if rc != 0:
            return 'fmt refused the %s program\n%s' % (label, err[-2000:])
        once = open(prog, encoding='utf-8').read()
        rc, out, err = run([ironc, 'run', 'case.iron'], work)
        if rc != 0 or out != want:
            return 'the formatted %s program prints something else\n%s' % (label, err[-2000:])
        rc, _, err = run([ironc, 'fmt', 'case.iron'], work)
        if rc != 0 or open(prog, encoding='utf-8').read() != once:
            return 'formatting the %s program twice changed it' % label
    return None


def campaign(ironc, outdir, seed, count):
    rng = random.Random(seed)
    cases = fixtures()
    if not cases:
        print('comment_fuzz: no fixtures found under', CORPUS)
        return 1
    work = os.path.join(outdir, 'work-%d' % seed)
    shutil.rmtree(work, ignore_errors=True)
    os.makedirs(work)
    failures = 0
    for i in range(count):
        path, exp = rng.choice(cases)
        case_rng = random.Random(rng.getrandbits(64))
        state = case_rng.getstate()
        why = one_case(ironc, work, path, exp, case_rng)
        if why:
            failures += 1
            case_rng.setstate(state)
            keep = os.path.join(outdir, 'fail', 'seed%d_%d.iron' % (seed, i))
            os.makedirs(os.path.dirname(keep), exist_ok=True)
            with open(keep, 'w', encoding='utf-8') as f:
                f.write(mutate(open(path, encoding='utf-8', errors='replace').read(), case_rng))
            print('FAIL seed %d case %d (%s): %s\n  kept %s'
                  % (seed, i, os.path.relpath(path, REPO), why, keep))
    shutil.rmtree(work, ignore_errors=True)
    print('comment_fuzz seed %d: %d cases, %d failures' % (seed, count, failures))
    return 1 if failures else 0


def main(argv):
    if len(argv) >= 3 and argv[0] == '--suite':
        ironc, outdir = os.path.abspath(argv[1]), argv[2]
        rc = 0
        # Small enough for a sanitizer build: each case compiles and runs
        # its program about eight times. Longer campaigns: SEED COUNT.
        for seed in (1, 2):
            rc |= campaign(ironc, outdir, seed, 6)
        return rc
    if len(argv) < 3:
        print(__doc__)
        return 2
    count = int(argv[3]) if len(argv) > 3 else 50
    return campaign(os.path.abspath(argv[0]), argv[1], int(argv[2]), count)


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
