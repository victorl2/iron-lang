#!/usr/bin/env python3
"""Regressions for comment handling in scripts/spec_coverage.py."""

import os
import sys


ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "scripts"))
import spec_coverage as coverage  # noqa: E402


def main() -> int:
    manual = os.path.join(ROOT, "docs", "language_definition.md")

    _units, slash = coverage.syntax_coverage(
        manual, [("slash.iron", "//iron:fusible\nfunc main() {}\n")]
    )
    assert "func_decl/opt1" in slash, slash

    _units, trailing = coverage.syntax_coverage(
        manual, [("trailing.iron", "func main() {} //iron:fusible\n")]
    )
    assert "func_decl/opt1" not in trailing, trailing

    multiline = 'val text = """\n//iron:fusible\n"""\nfunc main() {}\n'
    _units, string = coverage.syntax_coverage(manual, [("string.iron", multiline)])
    assert "func_decl/opt1" not in string, string

    units = {".fake": ("test", r"\.fake\(")}
    assert not coverage.library_coverage(units, [("slash.iron", "// x.fake()\n")])
    assert not coverage.library_coverage(units, [("dash.iron", "-- x.fake()\n")])
    assert coverage.library_coverage(units, [("string.iron", 'val s = "//"\nx.fake()\n')])

    print("spec coverage comment regressions: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
