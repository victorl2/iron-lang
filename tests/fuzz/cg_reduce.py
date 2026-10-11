#!/usr/bin/env python3
"""Shrink an Iron program while ironc keeps failing on it the same way.

    cg_reduce.py IRONC FILE [OUT] [-j N] [--flags "ironc build flags"]

The failure is what tests/fuzz/pf_mutate.py classifies (a crash, a hang,
C the C compiler rejects or warns on, a failure without a diagnostic code,
a program dying of a signal), keyed on its first message line with the
program-specific parts (numbers, temporary names) taken out. Whole lines,
blocks (a line ending in `{` through its closing line) and top-level
declarations are deleted while the key stays the same; the environment
(IRON_LIR_SKIP, IRON_CC_FLAGS) passes through to ironc. The result goes
to OUT (default FILE with .reduced.iron).
"""
import os
import re
import shutil
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import pf_mutate  # noqa: E402


def key_of(ironc, text, flags, workdir):
    d = tempfile.mkdtemp(dir=workdir)
    try:
        src = os.path.join(d, 'r.iron')
        with open(src, 'w', encoding='utf-8') as f:
            f.write(text)
        if flags:
            exe = os.path.join(d, 'r.bin')
            b, to, log = pf_mutate.run_ironc(ironc, ['build'] + flags + [src, '-o', exe], d)
            bad = pf_mutate.classify(b, to, log)
        else:
            bad = pf_mutate.check_mutant(ironc, d, 'r', text)
        if not bad:
            return None
        kind, msg = bad
        first = next((l for l in msg.splitlines() if l.strip()), '')
        first = re.sub(r'/\S+\.(c|iron)', 'F', first)
        first = re.sub(r'_v\d+|\d+', 'N', first)
        return kind, first[:160]
    finally:
        shutil.rmtree(d, ignore_errors=True)


def candidates(lines):
    """(start, end) line ranges to try deleting, largest first."""
    out = []
    for i, l in enumerate(lines):
        if l.rstrip().endswith('{'):
            out.append((i, pf_mutate.block_end(lines, i) + 1))
    out.sort(key=lambda r: r[0] - r[1])
    out += [(i, i + 1) for i in range(len(lines)) if lines[i].strip()]
    return out


def reduce(ironc, text, flags, jobs, workdir):
    base = key_of(ironc, text, flags, workdir)
    if not base:
        print('does not fail')
        return None
    print('failure:', base, flush=True)
    lines = text.split('\n')
    pool = ThreadPoolExecutor(jobs)
    changed = True
    while changed:
        changed = False
        cands = candidates(lines)
        k = 0
        while k < len(cands):
            batch = cands[k:k + jobs]
            texts = ['\n'.join(lines[:a] + lines[b:]) for a, b in batch]
            keys = list(pool.map(lambda t: key_of(ironc, t, flags, workdir), texts))
            hit = next((j for j, kk in enumerate(keys) if kk == base), None)
            if hit is not None:
                a, b = batch[hit]
                lines = lines[:a] + lines[b:]
                changed = True
                cands = candidates(lines)
                k = 0
                print(f'  {len(lines)} lines', flush=True)
                continue
            k += jobs
    pool.shutdown()
    return '\n'.join(lines)


def main(argv):
    jobs, flags, args = 4, [], []
    i = 0
    while i < len(argv):
        if argv[i] == '-j':
            jobs = int(argv[i + 1])
            i += 1
        elif argv[i] == '--flags':
            flags = argv[i + 1].split()
            i += 1
        else:
            args.append(argv[i])
        i += 1
    ironc, path = os.path.abspath(args[0]), args[1]
    out = args[2] if len(args) > 2 else re.sub(r'\.iron$', '', path) + '.reduced.iron'
    with open(path, encoding='utf-8') as f:
        text = f.read()
    workdir = tempfile.mkdtemp(prefix='cgred')
    try:
        r = reduce(ironc, text, flags, jobs, workdir)
    finally:
        shutil.rmtree(workdir, ignore_errors=True)
    if r is None:
        return 1
    with open(out, 'w', encoding='utf-8') as f:
        f.write(r)
    print(r)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
