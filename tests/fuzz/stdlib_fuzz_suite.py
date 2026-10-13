#!/usr/bin/env python3
"""The fixed seeds ctest runs for the standard library program fuzzers
(pf_url, pf_iofs, pf_mathlib, pf_strmore, pf_net, pf_ws), through
prog_fuzz.run. Their first seeds found the bugs fixed with them (#554 ..
#637); each group takes about a minute.

    stdlib_fuzz_suite.py IRONC OUTDIR GROUP     (GROUP: io, num, net)
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import prog_fuzz  # noqa: E402

GROUPS = {
    'io': {'iofs': range(1, 21), 'url': range(1, 25)},
    'num': {'mathlib': range(1, 21), 'strmore': range(1, 31)},
    'net': {'net': range(1, 17), 'ws': range(1, 25)},
}


def main(argv):
    ironc, outdir, group = os.path.abspath(argv[0]), argv[1], argv[2]
    bad = 0
    for area, seeds in GROUPS[group].items():
        n = prog_fuzz.run(ironc, outdir, area, seeds, quiet=True, jobs=min(4, os.cpu_count() or 1))
        print(f'{area}: {len(seeds) - n}/{len(seeds)} ok')
        bad += n
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
