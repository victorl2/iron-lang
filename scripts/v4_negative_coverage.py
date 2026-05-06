#!/usr/bin/env python3
"""
scripts/v4_negative_coverage.py
Drift guard for v4-acceptance negative corpus.

Enumerates the 23 §3-§10 compile-error categories from the spec (rows 1-22 + 24-25; row 23 is a runtime panic excluded from the manifest because it is covered by tests/integration/v4/4.3-unchecked-ptr/panic_box_unwrap_null.iron) and verifies
that tests/integration/v4-fail/ contains at least one .iron fixture exercising
each category. Categories are matched by REQ-ID (each fixture's `-- @req:` header
must include at least one of the category's expected REQ-IDs).

Usage: python3 scripts/v4_negative_coverage.py --corpus tests/integration/v4-fail
                                                [--strict]
"""
from __future__ import annotations
import argparse
import re
import sys
from pathlib import Path

# Source of truth: 15-RESEARCH.md "Complete Compile Error Enumeration" table.
# Row 23 (UNCK-03 null Box unwrap) is excluded — runtime panic, not compile error;
# covered by tests/integration/v4/4.3-unchecked-ptr/panic_box_unwrap_null.iron.
# Each entry: (category-id, §-section, expected REQ-ID prefix, recommended substring).
V4_COMPILE_ERROR_CATEGORIES = [
    ('heap-in-type',              '3.2', ['POL-02'], 'heap is not part of the type'),
    ('heap-as-binding-modifier',  '3.2', ['POL-02'], 'heap is not a binding modifier'),
    ('heap-as-param-qualifier',   '3.2', ['POL-02'], 'heap is not a parameter qualifier'),
    ('address-of-rc',             '3.3', ['POL-07'], 'cannot take address of rc value'),
    ('rc-in-arena',               '3.7', ['ARENA-01', 'POL-06'], 'rc cannot be allocated in an arena'),
    ('address-of-rvalue',         '4.2', ['PTR-01'], 'cannot take address of rvalue'),
    ('checked-to-unchecked',      '4.4', ['PTR-04', 'UNCK-01'], 'cannot convert'),
    ('unchecked-to-checked',      '4.4', ['PTR-04', 'UNCK-01'], 'cannot convert'),
    ('regime-mismatch-param',     '4.4', ['PTR-04'], 'type mismatch'),
    ('pointer-arithmetic',        '4.8', ['PTR-04'], 'no pointer arithmetic'),
    ('mutate-immutable-pointee',  '4.10', ['PTR-06'], 'pointee is immutable'),
    ('reassign-val-pointer',      '4.10', ['VAL-01'], 'cannot reassign val binding'),
    ('missing-val-var-local',     '5.1', ['VAL-01'], 'must specify val or var'),
    ('missing-val-var-field',     '5.2', ['VAL-01'], 'must specify val or var'),
    ('mutate-readonly-param',     '5.3', ['PARM-01'], 'parameter is read-only'),
    ('readonly-param-to-var',     '5.3', ['PARM-01'], 'cannot pass read-only to var'),
    ('readonly-mutates-self',     '6.1', ['READ-01'], 'readonly forbids mutation'),
    ('readonly-calls-non-readonly', '6.1', ['READ-01'], 'readonly may not call'),
    ('readonly-does-io',          '6.1', ['READ-01'], 'readonly forbids I/O'),
    ('readonly-mutates-var-param', '6.3', ['READ-01'], 'readonly forbids mutation'),
    ('readonly-bad-return',       '6.4', ['READ-04'], 'return type not readonly-compatible'),
    ('copy-nocopy',               '7.3', ['DROP-04'], 'nocopy type cannot be copied'),
    # row 23 (UNCK-03 null Box unwrap) is a RUNTIME panic, NOT a compile error.
    # It is covered by tests/integration/v4/4.3-unchecked-ptr/panic_box_unwrap_null.iron
    # (positive-corpus @expect-panic fixture authored by Plan 03). The drift guard
    # therefore enforces 23 categories, not 24.
    ('double-free-warning',       '10.2', ['POL-04'], 'unreachable free'),
]

DIRECTIVE_RE = re.compile(r'--\s+@([\w-]+):\s*(.*)$')


def parse_reqs(iron_file: Path) -> list[str]:
    """Return REQ-IDs from `-- @req: ...` directive in first 10 lines."""
    try:
        with open(iron_file, encoding='utf-8') as f:
            for i, line in enumerate(f):
                if i >= 10:
                    break
                m = DIRECTIVE_RE.match(line.strip())
                if m and m.group(1) == 'req':
                    return [r.strip() for r in m.group(2).split(',') if r.strip()]
    except OSError:
        pass
    return []


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--corpus', type=Path, default=Path('tests/integration/v4-fail'))
    ap.add_argument('--strict', action='store_true', help='exit non-zero on missing category')
    args = ap.parse_args()

    if not args.corpus.exists():
        print(f"error: corpus directory not found: {args.corpus}", file=sys.stderr)
        return 2

    # Map REQ -> [fixture paths]
    fixtures_by_req: dict[str, list[Path]] = {}
    for iron in args.corpus.rglob('*.iron'):
        for req in parse_reqs(iron):
            fixtures_by_req.setdefault(req, []).append(iron)

    print(f"=== v4 negative-corpus coverage report ===")
    print(f"corpus: {args.corpus}")
    print(f"")

    missing = []
    for cat_id, section, expected_reqs, substring in V4_COMPILE_ERROR_CATEGORIES:
        # A category is covered if any of its expected REQ-IDs has >= 1 fixture.
        covered = any(req in fixtures_by_req for req in expected_reqs)
        if covered:
            req_hits = ', '.join(f"{r}({len(fixtures_by_req.get(r, []))})" for r in expected_reqs if r in fixtures_by_req)
            print(f"[OK]   §{section:<6} {cat_id:<30} {req_hits}")
        else:
            print(f"[MISS] §{section:<6} {cat_id:<30} (expected: {','.join(expected_reqs)})")
            missing.append(cat_id)

    print(f"")
    print(f"covered: {len(V4_COMPILE_ERROR_CATEGORIES) - len(missing)}/{len(V4_COMPILE_ERROR_CATEGORIES)}")
    if missing:
        print(f"missing: {', '.join(missing)}")
        if args.strict:
            return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
