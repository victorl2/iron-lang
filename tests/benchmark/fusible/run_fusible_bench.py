#!/usr/bin/env python3
"""Time fused list pipelines against the same pipelines run stage by stage,
and count the allocations fusion saves.

    run_fusible_bench.py path/to/iron [runs]

Builds fusible_bench.iron as `iron build` does and runs it `runs` times
(default 3); prints, per kernel, the best time of the fused and of the
staged variant and their ratio. Then checks the `// @fused:` marks of the
source against the generated C (the fused variants are one loop each, the
staged variants none) and links that C with the counting allocation
functions of alloc_count.c: each variant runs once on its own and the
allocations and bytes it requested are printed.

Exit status 1 when a fused and a staged variant disagree, a mark does not
match the generated C, or a fused variant allocates as much as its staged
one.
"""
import os
import re
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..', '..'))
sys.path.insert(0, os.path.join(ROOT, 'tests', 'integration'))
sys.path.insert(0, os.path.join(ROOT, 'tests', 'fuzz'))
import fusible_parity as fp  # noqa: E402
import prog_fuzz  # noqa: E402

KERNELS = ('sum', 'fold', 'float', 'point', 'collect', 'soa')


def main(argv):
    iron = os.path.abspath(argv[0])
    ironc = os.path.join(os.path.dirname(iron), 'ironc')
    runs = int(argv[1]) if len(argv) > 1 else 3
    work = tempfile.mkdtemp(prefix='iron_fusible_bench_')
    try:
        return bench(iron, ironc, runs, work)
    finally:
        shutil.rmtree(work, ignore_errors=True)


def bench(iron, ironc, runs, work):
    src = os.path.join(work, 'fusible_bench.iron')
    shutil.copy(os.path.join(HERE, 'fusible_bench.iron'), src)
    b = subprocess.run([iron, 'build', 'fusible_bench.iron', '-o', 'bench'], cwd=work,
                       capture_output=True, text=True)
    if b.returncode != 0:
        print(b.stdout + b.stderr)
        return 1
    best = {}
    for _ in range(runs):
        out = subprocess.run([os.path.join(work, 'bench')], capture_output=True, text=True,
                             check=True).stdout
        for line in out.splitlines():
            name, fused, staged, same = line.split()
            if same != 'true':
                print(f'FAIL: {name}: the fused and the staged pipeline disagree')
                return 1
            f0, s0 = best.get(name, (1 << 62, 1 << 62))
            best[name] = (min(f0, int(fused)), min(s0, int(staged)))

    # The marks of the source against the generated C.
    e = subprocess.run([ironc, 'build', '--emit-c', 'fusible_bench.iron'], cwd=work,
                       capture_output=True, text=True)
    cfile = os.path.join(work, '.iron-build', 'fusible_bench.c')
    if e.returncode != 0 or not os.path.exists(cfile):
        print(e.stdout + e.stderr)
        return 1
    with open(src, encoding='utf-8') as f:
        want = fp.expected_fusion(f.read())
    with open(cfile, encoding='utf-8') as f:
        have = fp.actual_fusion(f.read())
    if have != want:
        print(f'FAIL: fused loops {have}, the source marks {want}')
        return 1

    # Allocations: the same C linked with counting allocation functions.
    # iron_os.c defines the real ones under another name; given as an
    # object, it keeps the archive's copy out of the link.
    rtlib = fp.runtime_lib(os.path.join(work, 'rt'))
    inc = os.path.join(ROOT, 'src')
    flags = ['-O3', '-std=gnu17', '-fwrapv', '-fno-strict-aliasing', '-ffp-contract=off', '-w',
             f'-I{inc}', f'-I{inc}/vendor', f'-I{inc}/stdlib'] + prog_fuzz.ssl_flags()
    os_real = os.path.join(work, 'iron_os_real.o')
    counted = os.path.join(work, 'bench_counted')
    steps = [['clang', '-c'] + flags + ['-Diron_mem_alloc=iron_mem_alloc_real',
                                         '-Diron_mem_calloc=iron_mem_calloc_real',
                                         '-Diron_mem_realloc=iron_mem_realloc_real',
                                         os.path.join(inc, 'runtime', 'iron_os.c'), '-o', os_real],
             ['clang'] + flags + [cfile, os.path.join(HERE, 'alloc_count.c'), os_real, rtlib,
                                  '-lm', '-lpthread'] + prog_fuzz.ssl_libs() + ['-o', counted]]
    for step in steps:
        cc = subprocess.run(step, capture_output=True, text=True)
        if cc.returncode != 0:
            print(cc.stdout + cc.stderr)
            return 1

    def allocations(which):
        r = subprocess.run([counted], capture_output=True, text=True,
                           env=dict(os.environ, FUSIBLE_BENCH_ONLY=which))
        m = re.search(r'allocations (\d+) bytes (\d+)', r.stderr)
        return (int(m.group(1)), int(m.group(2))) if m else (0, 0)

    base = allocations('none')
    print(f'{"kernel":<9}{"fused_us":>10}{"staged_us":>11}{"staged/fused":>14}'
          f'{"fused allocs":>14}{"staged allocs":>15}{"fused MB":>10}{"staged MB":>11}')
    status = 0
    for k in KERNELS:
        fused_us, staged_us = best[k]
        fa = [x - y for x, y in zip(allocations(k + '_fused'), base)]
        sa = [x - y for x, y in zip(allocations(k + '_staged'), base)]
        ratio = f'{staged_us / fused_us:.2f}' if fused_us > 0 else '-'
        print(f'{k:<9}{fused_us:>10}{staged_us:>11}{ratio:>14}{fa[0]:>14}{sa[0]:>15}'
              f'{fa[1] / 1e6:>10.1f}{sa[1] / 1e6:>11.1f}')
        if fa[0] >= sa[0]:
            print(f'FAIL: {k}: the fused pipeline allocates as often as the staged one')
            status = 1
    print('(times: best of %d runs of 20 passes over 1,000,000 elements; allocations: one pass)'
          % runs)
    return status


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
