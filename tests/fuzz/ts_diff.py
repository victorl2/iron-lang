#!/usr/bin/env python3
"""Differential test: the tree-sitter grammar against the compiler's parser.

Editors that highlight with tree-sitter (Neovim, Zed) see an ERROR node
wherever the grammar does not cover what the compiler accepts. This test
feeds both the same sources and reports the disagreements in both
directions:

  ts-rejects   the compiler's lexer and parser accept the file, the grammar
               produces ERROR or MISSING nodes (an editor marks valid code);
  ts-accepts   the compiler rejects the file, the grammar parses it cleanly
               (the grammar is looser than the language; informational).

"The compiler accepts" means `ironc fmt --check` does not refuse the file
for lex or parse errors (exit 2): lexer and parser only, no type checking.

Sources: programs from every prog_fuzz generator, the integration corpus,
and mutants of both (a token deleted, duplicated, swapped with its
neighbour, a keyword or punctuation inserted, a line dropped or doubled).

Usage:
    ts_diff.py --ironc C --tree-sitter TS [--seed S] [--count N]
               [--mutants M] [-j N] [--out DIR] [--fail-on ts-rejects]
    ts_diff.py --ironc C --tree-sitter TS --reduce FILE [--keep REGEX]
        shrink FILE while the disagreement it shows persists (and REGEX
        still matches, to stay on the construct of interest)

Disagreeing files are written to OUT/<kind>/. With --fail-on ts-rejects
(the default) the exit status is 1 when the grammar rejects a generated or
corpus program the compiler accepts, outside the known gaps listed in
KNOWN; mutants are reported (and fail with --strict-mutants). Review the
mutant ts-rejects of a campaign by hand: the compiler accepting a mutant
the grammar rejects has been a compiler bug (a stray `}`, `Int[]`).
"""
from __future__ import annotations

import argparse
import concurrent.futures
import glob
import os
import random
import re
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.normpath(os.path.join(HERE, '..', '..'))
GRAMMAR = os.path.join(REPO, 'grammars', 'tree-sitter', 'iron')
sys.path.insert(0, HERE)

AREAS = ('stmt', 'coll', 'obj', 'closure', 'conc', 'arena', 'num', 'generic', 'ptr', 'comptime')

# Constructs the grammar does not cover by design (see
# tests/grammars/tree_sitter/parse_fixtures.sh): a source matching one of
# these is not counted as a ts-rejects failure.
KNOWN = [
    # backslash-escaped quotes inside an interpolation need an external scanner
    re.compile(r'\{[^}"\n]*\\"'),
]

INSERTS = ['{', '}', '(', ')', '[', ']', ',', '.', ':', '->', '=', '+', '"', '\n', 'val ', 'var ',
           'func ', 'if ', 'else ', 'return ', 'for ', 'in ', 'match ', 'object ', 'enum ', 'heap ',
           'rc ', 'defer ', '?', '!', '*', '&', '..', '@', '-- c\n', 'not ', 'and ', 'or ']

TOKEN_RE = re.compile(r'"(?:\\.|[^"\\\n])*"|--[^\n]*|[A-Za-z_][A-Za-z0-9_]*|\d+(?:\.\d+)?|->|\.\.|[=!<>]=|\S')


def read(path):
    with open(path, encoding='utf-8', errors='replace') as f:
        return f.read()


def mutate(text, rng):
    toks = [m for m in TOKEN_RE.finditer(text) if not m.group(0).startswith('--')]
    if not toks:
        return text
    k = rng.randrange(7)
    if k == 0:      # delete a token
        m = rng.choice(toks)
        return text[:m.start()] + text[m.end():]
    if k == 1:      # duplicate a token
        m = rng.choice(toks)
        return text[:m.end()] + ' ' + m.group(0) + text[m.end():]
    if k == 2:      # swap a token with the next
        i = rng.randrange(len(toks) - 1) if len(toks) > 1 else 0
        a, b = toks[i], toks[min(i + 1, len(toks) - 1)]
        if a is b:
            return text
        return text[:a.start()] + b.group(0) + text[a.end():b.start()] + a.group(0) + text[b.end():]
    if k == 3:      # insert something before a token
        m = rng.choice(toks)
        return text[:m.start()] + rng.choice(INSERTS) + text[m.start():]
    lines = text.split('\n')
    j = rng.randrange(len(lines))
    if k == 4:      # drop a line
        del lines[j]
    elif k == 5:    # double a line
        lines.insert(j, lines[j])
    else:           # join a line with the next
        if j + 1 < len(lines):
            lines[j] = lines[j] + ' ' + lines.pop(j + 1).lstrip()
    return '\n'.join(lines)


def compiler_accepts(ironc, path):
    p = subprocess.run([ironc, 'fmt', '--check', path], capture_output=True, timeout=60)
    if p.returncode < 0:
        raise RuntimeError(f'ironc fmt --check crashed on {path}: {p.stderr[-500:]!r}')
    return p.returncode != 2


def ts_rejected(ts, paths, workdir):
    """The subset of paths tree-sitter parses with ERROR or MISSING nodes."""
    lst = os.path.join(workdir, 'paths.txt')
    with open(lst, 'w') as f:
        f.write('\n'.join(paths) + '\n')
    p = subprocess.run([ts, 'parse', '--quiet', '--paths', lst], capture_output=True,
                       cwd=GRAMMAR, timeout=600)
    out = p.stdout.decode('utf-8', 'replace') + p.stderr.decode('utf-8', 'replace')
    bad = set()
    known_paths = set(paths)
    for line in out.splitlines():
        if '(ERROR' in line or '(MISSING' in line:
            tok = line.split()[0] if line.split() else ''
            if tok in known_paths:
                bad.add(tok)
    return bad


def classify(ironc, ts, paths, workdir, jobs):
    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as ex:
        acc = dict(zip(paths, ex.map(lambda p: compiler_accepts(ironc, p), paths)))
    bad = ts_rejected(ts, paths, workdir)
    out = {}
    for p in paths:
        c, t = acc[p], p not in bad
        out[p] = 'agree' if c == t else ('ts-rejects' if c else 'ts-accepts')
    return out


def known(text):
    return any(r.search(text) for r in KNOWN)


def reduce(args):
    """Shrink a file while it shows the same disagreement."""
    work = tempfile.mkdtemp(prefix='tsred_')
    text = read(args.reduce)

    def kind_of(t):
        p = os.path.join(work, 'r.iron')
        with open(p, 'w') as f:
            f.write(t)
        return classify(args.ironc, args.tree_sitter, [p], work, 1)[p]

    keep = re.compile(args.keep) if args.keep else None
    base_kind = kind_of

    def kind_of(t):  # noqa: F811
        if keep and not keep.search(t):
            return 'dropped'
        return base_kind(t)

    want = kind_of(text)
    if want == 'agree':
        print('the file shows no disagreement')
        return 1
    for unit in ('line', 'token', 'char'):
        changed = True
        while changed:
            changed = False
            if unit == 'line':
                parts = text.split('\n')
                join = '\n'
            elif unit == 'token':
                parts = [m.group(0) for m in re.finditer(r'\s+|\S+', text)]
                join = ''
            else:
                parts = list(text)
                join = ''
            n = max(1, len(parts) // 2)
            while n >= 1:
                i = 0
                while i < len(parts):
                    cand = join.join(parts[:i] + parts[i + n:])
                    if cand != text and kind_of(cand) == want:
                        text = cand
                        parts = parts[:i] + parts[i + n:]
                        changed = True
                    else:
                        i += n
                n //= 2
    print(f'{want}:')
    print(text)
    shutil.rmtree(work, ignore_errors=True)
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--ironc', required=True)
    ap.add_argument('--tree-sitter', required=True)
    ap.add_argument('--seed', type=int, default=1)
    ap.add_argument('--count', type=int, default=200, help='generated programs')
    ap.add_argument('--corpus', type=int, default=200, help='corpus files sampled')
    ap.add_argument('--mutants', type=int, default=3, help='mutants per program')
    ap.add_argument('-j', type=int, default=4)
    ap.add_argument('--out', default='ts_diff_out')
    ap.add_argument('--fail-on', default='ts-rejects', choices=['ts-rejects', 'none', 'both'])
    ap.add_argument('--strict-mutants', action='store_true',
                    help='a mutant the compiler accepts and the grammar rejects fails too (by '
                    'default only unmutated programs fail: a mutant can be valid in a way the '
                    'grammar does not need to cover, such as an expression pattern)')
    ap.add_argument('--reduce')
    ap.add_argument('--keep', help='with --reduce: a regex the reduced file must still match')
    args = ap.parse_args(argv)
    args.ironc = os.path.abspath(args.ironc)
    args.tree_sitter = os.path.abspath(args.tree_sitter)
    if args.reduce:
        return reduce(args)

    import prog_fuzz
    rng = random.Random(args.seed)
    work = tempfile.mkdtemp(prefix='tsdiff_')
    sources = []
    for i in range(args.count):
        area = AREAS[i % len(AREAS)]
        try:
            src, _ = prog_fuzz.make_program(area, rng.randrange(1 << 30))
        except Exception:
            continue
        sources.append((f'gen_{area}_{i}', src))
    corpus = sorted(glob.glob(os.path.join(REPO, 'tests', 'integration', '**', '*.iron'), recursive=True))
    corpus = [f for f in corpus if '/v4-fail/' not in f and '/diagnostics/' not in f]
    for f in rng.sample(corpus, min(args.corpus, len(corpus))):
        sources.append((os.path.basename(f)[:-5], read(f)))
    files = []
    for name, src in sources:
        files.append((name, src, 'orig'))
        for k in range(args.mutants):
            files.append((f'{name}_m{k}', mutate(src, rng), 'mutant'))
    paths = {}
    for idx, (name, src, origin) in enumerate(files):
        p = os.path.join(work, f'{idx:05d}_{re.sub(r"[^A-Za-z0-9_]", "_", name)}.iron')
        with open(p, 'w', encoding='utf-8') as f:
            f.write(src)
        paths[p] = (name, src, origin)
    res = classify(args.ironc, args.tree_sitter, list(paths), work, args.j)
    counts = {}
    failing = 0
    for p, kind in sorted(res.items()):
        name, src, origin = paths[p]
        counts[(origin, kind)] = counts.get((origin, kind), 0) + 1
        if kind == 'agree':
            continue
        is_known = known(src)
        d = os.path.join(args.out, kind + ('-known' if is_known else ''))
        os.makedirs(d, exist_ok=True)
        shutil.copy(p, os.path.join(d, os.path.basename(p)))
        counted = not is_known and (args.fail_on == 'both' or args.fail_on == kind) and \
            (origin == 'orig' or args.strict_mutants)
        if counted:
            failing += 1
        if not is_known:
            print(f'{kind}: {name} ({origin}) -> {os.path.join(d, os.path.basename(p))}'
                  + ('' if counted else ' (reported, not failing)'))
    shutil.rmtree(work, ignore_errors=True)
    total = len(res)
    summary = ', '.join(f'{o} {k}={n}' for (o, k), n in sorted(counts.items()))
    print(f'ts_diff: {total} files ({summary}); {failing} failing')
    return 1 if failing else 0


if __name__ == '__main__':
    sys.exit(main())
