#!/usr/bin/env python3
"""
scripts/v4_acceptance_report.py
Aggregator for v4-acceptance corpus pass/XFAIL/FAIL counts per phase + per REQ-ID.

Usage:
    python3 scripts/v4_acceptance_report.py [--corpus PATH] [--negative-corpus PATH]
                                            [--ironc PATH] [--phase N]
                                            [--baseline] [--json] [--text]

Reads each .iron fixture's @expected-pass-after and @req directives from the
first 10 lines, optionally runs the harness, and prints per-phase + per-REQ
PASS/FAIL/XFAIL counts. With --baseline (no harness invocation) it just
reports fixture-count + REQ coverage for static gating.
"""
from __future__ import annotations
import argparse
import json
import re
import subprocess
import sys
from collections import defaultdict
from pathlib import Path

DIRECTIVE_RE = re.compile(r'--\s+@([\w-]+):\s*(.*)$')


def parse_directives(iron_file: Path) -> dict:
    """Extract @expected-pass-after, @req, @expect-panic from first 10 lines."""
    directives = {}
    try:
        with open(iron_file, encoding='utf-8') as f:
            for i, line in enumerate(f):
                if i >= 10:
                    break
                m = DIRECTIVE_RE.match(line.strip())
                if m:
                    directives[m.group(1)] = m.group(2).strip()
    except OSError:
        pass
    return directives


def collect_fixtures(corpus: Path) -> list[tuple[Path, dict]]:
    """Walk corpus recursively; return (path, directives) for every .iron file."""
    out = []
    for iron in sorted(corpus.rglob('*.iron')):
        out.append((iron, parse_directives(iron)))
    return out


def parse_phase(directive_value: str) -> int | None:
    """Parse 'phase-21' -> 21, 'phase-3' -> 3, anything else -> None."""
    m = re.match(r'phase-(\d+)', directive_value)
    return int(m.group(1)) if m else None


def parse_reqs(directive_value: str) -> list[str]:
    """Parse 'POL-02, PTR-04' -> ['POL-02', 'PTR-04']."""
    return [r.strip() for r in directive_value.split(',') if r.strip()]


def run_harness(category: str, ironc: Path, repo_root: Path) -> str:
    """Invoke run_tests.sh and capture its output."""
    script = repo_root / 'tests' / 'run_tests.sh'
    result = subprocess.run(
        [str(script), category, str(ironc)],
        capture_output=True, text=True, cwd=str(repo_root)
    )
    return result.stdout + result.stderr


def classify_one(directives: dict, current_phase: int) -> str:
    """Static classification only (without running harness): XFAIL if pass-after > current."""
    pass_after = directives.get('expected-pass-after', '')
    n = parse_phase(pass_after)
    if n is None:
        return 'unclassified'
    return 'xfail' if n > current_phase else 'expected-pass'


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--corpus', type=Path, default=Path('tests/integration/v4'))
    ap.add_argument('--negative-corpus', type=Path, default=Path('tests/integration/v4-fail'))
    ap.add_argument('--ironc', type=Path, default=Path('build/ironc'))
    ap.add_argument('--phase', type=int, default=15, help='current phase (default 15)')
    ap.add_argument('--baseline', action='store_true', help='static report only, no harness')
    ap.add_argument('--json', dest='emit_json', action='store_true')
    ap.add_argument('--text', dest='emit_text', action='store_true')
    args = ap.parse_args()

    if not args.emit_json and not args.emit_text:
        args.emit_text = True

    fixtures = collect_fixtures(args.corpus) if args.corpus.exists() else []
    neg_fixtures = collect_fixtures(args.negative_corpus) if args.negative_corpus.exists() else []
    all_fixtures = fixtures + neg_fixtures

    # Static classification
    per_phase: dict[int, int] = defaultdict(int)
    per_req: dict[str, int] = defaultdict(int)
    xfail_count = 0
    expected_pass_count = 0
    unclassified_count = 0

    for path, directives in all_fixtures:
        cls = classify_one(directives, args.phase)
        if cls == 'xfail':
            xfail_count += 1
        elif cls == 'expected-pass':
            expected_pass_count += 1
        else:
            unclassified_count += 1

        n = parse_phase(directives.get('expected-pass-after', ''))
        if n is not None:
            per_phase[n] += 1
        for req in parse_reqs(directives.get('req', '')):
            per_req[req] += 1

    summary = {
        'corpus': str(args.corpus),
        'negative_corpus': str(args.negative_corpus),
        'current_phase': args.phase,
        'total': len(all_fixtures),
        'pass_count': 0,         # set by harness mode below
        'fail_count': 0,
        'xfail_count': xfail_count,
        'expected_pass_count': expected_pass_count,
        'unclassified_count': unclassified_count,
        'per_phase': dict(sorted(per_phase.items())),
        'per_req': dict(sorted(per_req.items())),
    }

    if not args.baseline:
        # Run harness for live PASS/FAIL counts.
        repo_root = Path.cwd()
        out_pos = run_harness('v4', args.ironc, repo_root)
        out_neg = run_harness('v4-fail', args.ironc, repo_root)
        # Parse summary line: "PASS=N FAIL=M XFAIL=K TOTAL=T"
        summary_re = re.compile(r'PASS=(\d+)\s+FAIL=(\d+)\s+XFAIL=(\d+)\s+TOTAL=(\d+)')
        for out in (out_pos, out_neg):
            m = summary_re.search(out)
            if m:
                summary['pass_count'] += int(m.group(1))
                summary['fail_count'] += int(m.group(2))
                # XFAIL from harness overrides static count

    if args.emit_json:
        print(json.dumps(summary, indent=2, sort_keys=True))
    if args.emit_text:
        print(f"=== v4-acceptance corpus report (phase {args.phase}) ===")
        print(f"corpus       : {summary['corpus']}")
        print(f"neg corpus   : {summary['negative_corpus']}")
        print(f"total        : {summary['total']}")
        print(f"PASS         : {summary['pass_count']}")
        print(f"FAIL         : {summary['fail_count']}")
        print(f"XFAIL (static): {summary['xfail_count']}")
        print(f"unclassified : {summary['unclassified_count']}")
        print(f"")
        print(f"--- per-phase fixture counts ---")
        for phase, count in summary['per_phase'].items():
            print(f"  phase-{phase:>2}: {count}")
        print(f"")
        print(f"--- per-REQ fixture coverage ---")
        for req, count in summary['per_req'].items():
            print(f"  {req}: {count}")

    # Exit non-zero if FAIL > 0 (live mode); baseline mode is informational only
    if not args.baseline and summary['fail_count'] > 0:
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
