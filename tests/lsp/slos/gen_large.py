#!/usr/bin/env python3
"""Phase 7 Plan 07-04 Task 01 (HARD-18, D-06) -- committed generator for
the large (5000 LoC) SLO fixture.

The committed large_5000loc.iron MUST be byte-identical to the output of
this script at seed=20260423 -- regenerating produces the same bytes so
drift is caught by a CI diff. The large fixture is measured but
informational only (p50 thresholds apply to medium_500loc.iron per D-06).

Usage:
    python3 tests/lsp/slos/gen_large.py > tests/lsp/slos/fixtures/large_5000loc.iron
"""
from __future__ import annotations

import random
import sys

SEED = 20260423
TARGET_LINES = 5000


def main() -> int:
    rng = random.Random(SEED)
    out: list[str] = []
    out.append("// Phase 7 Plan 07-04 Task 01 (HARD-18, D-06) -- SLO fixture (large 5000 LoC).")
    out.append("// Deterministic output of tests/lsp/slos/gen_large.py at SEED=20260423.")
    out.append("// Informational fixture: p50 thresholds are enforced on the medium")
    out.append("// (500 LoC) fixture, not this one. A 5000 LoC file exercises the")
    out.append("// p95/p99 tail samples at a size approaching the Iron stdlib.iron")
    out.append("// upper bound; p95/p99 are recorded but not enforced (D-06).")
    out.append("")

    # 40 module-level constants.
    for i in range(40):
        out.append("val CONST_" + str(i).zfill(3) + ": Int = " + str(rng.randint(1, 1009)))
    out.append("")

    # Number of (object, method) pairs needed to reach ~5000 lines:
    # object block = 5 lines (decl + id + counter + close + blank)
    # method = 8 lines (func + 5 body + close + blank)
    # So one (object + 12 methods) = 5 + 12*8 = 101 lines.
    # 40 const + 7 header + 2 blank ~ 49, plus main block ~ 60.
    # (5000 - 109) / 101 ≈ 48 objects.
    obj_count = 48
    methods_per_obj = 12

    obj_names = ["Mod" + str(i).zfill(3) for i in range(obj_count)]

    for obj_idx, obj_name in enumerate(obj_names):
        sibling = obj_names[(obj_idx + 7) % obj_count]
        out.append("object " + obj_name + " {")
        out.append("  val id: Int")
        out.append("  var counter: Int = " + str(rng.randint(1, 999)))
        out.append("}")
        out.append("")
        for m in range(methods_per_obj):
            out.append("func " + obj_name + ".op_" + str(m) + "(a: Int) -> Int {")
            out.append("  var acc: Int = " + str(rng.randint(1, 97)))
            out.append("  acc = acc + a")
            out.append("  acc = acc * " + str(rng.randint(2, 9)))
            out.append("  acc = acc + CONST_" + str(rng.randint(0, 39)).zfill(3))
            out.append("  return acc")
            out.append("}")
            out.append("")

    # main block: one constructor + a couple of calls per object.
    out.append("func main() {")
    for obj_idx, obj_name in enumerate(obj_names):
        out.append("  val x" + str(obj_idx) + ": " + obj_name + " = " +
                   obj_name + " { id: " + str(obj_idx) + ", counter: " + str(obj_idx * 3) + " }")
    # At this point we are near 5000; add a handful of op_0 calls then
    # pad to exactly TARGET_LINES with comment lines to keep the final
    # count stable at `TARGET_LINES` for the CTest invariant lock.
    for obj_idx in range(obj_count):
        if len(out) + 4 >= TARGET_LINES:
            break
        out.append("  val _r" + str(obj_idx) + ": Int = x" + str(obj_idx) +
                   ".op_0(" + str(obj_idx + 1) + ")")
    out.append("  return")
    out.append("}")

    # Pad to exactly TARGET_LINES via trailing comments so wc -l is stable.
    while len(out) < TARGET_LINES:
        out.append("// slo-large-pad line " + str(len(out)).zfill(5))

    # Truncate if we overshot by one or two lines.
    out = out[:TARGET_LINES]

    text = "\n".join(out) + "\n"
    sys.stdout.write(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
