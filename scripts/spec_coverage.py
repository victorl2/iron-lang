#!/usr/bin/env python3
"""Measure how much of the language manual the test suite exercises.

Every unit counted here is read from docs/language_definition.md, so the
denominator is fixed by the manual, not chosen by the tests:

  syntax       each alternative, optional part and repetition of each
               production in section 13, used by the derivation of some
               program of the positive corpus (one derivation per file,
               rebuilt from the recognizer of scripts/grammar_check.py)
  diagnostics  each error and warning code of the section 11 table, expected
               by a negative fixture (.expected), a help check
               (.expected_help), a ctest regex or a unit test (by its
               IRON_ERR_ / IRON_WARN_ name)
  library      each function and method named in the section 9 tables,
               called by a program of the positive corpus
  examples     each ```iron example of the manual not marked doctest-skip
               (scripts/test_doc_examples.sh builds and runs it, compares
               its ```output block, or checks its doctest-expect-error code,
               which also counts for the diagnostics dimension)

Usage:
    scripts/spec_coverage.py                  summary per dimension and section
    scripts/spec_coverage.py --missing        also list every uncovered unit
    scripts/spec_coverage.py --json FILE      write the full result
    scripts/spec_coverage.py --check BASELINE fail if a unit covered in the
                                              baseline is no longer covered
    scripts/spec_coverage.py --update BASELINE  rewrite the baseline

Standard library only, Python 3.8+.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import sys
from typing import Dict, List, Set, Tuple

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import grammar_check as gc  # noqa: E402

ROOT = os.path.dirname(HERE)


def rel(p: str) -> str:
    return os.path.relpath(p, ROOT)


def read(p: str) -> str:
    return open(p, encoding="utf-8-sig", errors="replace").read()


def iter_files(roots: List[str], suffixes: Tuple[str, ...]):
    for root in roots:
        if os.path.isfile(root):
            yield root
            continue
        for dirpath, _dirs, files in sorted(os.walk(root)):
            for f in sorted(files):
                if f.endswith(suffixes):
                    yield os.path.join(dirpath, f)


def positive_sources(roots: List[str]) -> List[Tuple[str, str]]:
    out = []
    for p in iter_files(roots, (".iron",)):
        src = read(p)
        if "@expected-pass-after" in src:  # parked fixture, not run
            continue
        out.append((p, src))
    return out


# ------------------------------------------------------------- sections --

def section_index(manual: str) -> List[Tuple[int, str]]:
    """(offset, 'N.M Title') for every numbered heading."""
    out = []
    for m in re.finditer(r"^#{2,4} (\d+(?:\.\d+)*)\.? (.*)$", manual, re.M):
        out.append((m.start(), f"{m.group(1)} {m.group(2).strip()}"))
    return out


def section_at(index: List[Tuple[int, str]], offset: int) -> str:
    cur = "?"
    for off, name in index:
        if off > offset:
            break
        cur = name
    return cur


# --------------------------------------------------------------- syntax --

def label_grammar(rules: Dict[str, tuple]) -> Tuple[Dict[int, str], List[str]]:
    """A stable name for every alternative / optional / repetition node:
    rule/altN (the Nth branch of the Kth alternation is rule/aK.N),
    rule/optN, rule/repN, numbered in reading order. Returns id(node) ->
    name and the list of all unit names (each rule itself is one unit)."""
    names: Dict[int, str] = {}
    units: List[str] = []

    for rule, body in rules.items():
        units.append(rule)
        counters = {"a": 0, "opt": 0, "rep": 0}

        def walk(node):
            kind = node[0]
            if kind == "alt":
                counters["a"] += 1
                k = counters["a"]
                for i, child in enumerate(node[1], 1):
                    n = f"{rule}/a{k}.{i}"
                    names[id(child)] = n
                    units.append(n)
                    walk(child)
            elif kind == "seq":
                for c in node[1]:
                    walk(c)
            elif kind in ("opt", "rep"):
                counters[kind] += 1
                n = f"{rule}/{kind}{counters[kind]}"
                names[id(node)] = n
                units.append(n)
                walk(node[1])

        walk(body)
    return names, units


class Deriver:
    """One derivation of an accepted token stream, read off the
    recognizer's memo tables; records the grammar units it uses."""

    def __init__(self, rec: gc.Recognizer, names: Dict[int, str]) -> None:
        self.rec = rec
        self.names = names
        self.used: Set[str] = set()
        self.done: Set[Tuple[int, int, int]] = set()

    def ends(self, node, pos: int):
        return self.rec._node(node, pos)

    def derive(self, node, pos: int, end: int) -> None:
        key = (id(node), pos, end)
        if key in self.done:
            return
        self.done.add(key)
        kind = node[0]
        if kind == "term":
            return
        if kind == "nt":
            name = node[1]
            if name in gc.TOKEN_SYMBOLS:
                return
            self.used.add(name)
            self.derive(self.rec.rules[name], pos, end)
            return
        if kind == "alt":
            for child in node[1]:
                if end in self.ends(child, pos):
                    self.used.add(self.names[id(child)])
                    self.derive(child, pos, end)
                    return
            return
        if kind == "opt":
            if end != pos and end in self.ends(node[1], pos):
                self.used.add(self.names[id(node)])
                self.derive(node[1], pos, end)
            return
        if kind == "rep":
            if end == pos:
                return
            # Shortest chain of iterations pos -> ... -> end.
            prev = {pos: None}
            frontier = [pos]
            while frontier and end not in prev:
                nxt = []
                for p in frontier:
                    for q in self.ends(node[1], p):
                        if q > p and q not in prev:
                            prev[q] = p
                            nxt.append(q)
                frontier = nxt
            if end not in prev:
                return
            self.used.add(self.names[id(node)])
            q = end
            while prev[q] is not None:
                p = prev[q]
                self.derive(node[1], p, q)
                q = p
            return
        if kind == "seq":
            kids = node[1]
            reach = [{pos}]
            for c in kids:
                nxt = set()
                for p in reach[-1]:
                    nxt |= self.ends(c, p)
                reach.append(nxt)
            if end not in reach[-1]:
                return
            splits = [end]
            cur = end
            for i in range(len(kids) - 1, -1, -1):
                for p in sorted(reach[i]):
                    if cur in self.ends(kids[i], p):
                        splits.append(p)
                        cur = p
                        break
            splits.reverse()
            for i, c in enumerate(kids):
                self.derive(c, splits[i], splits[i + 1])
            return


def syntax_coverage(manual_path: str, sources) -> Tuple[List[str], Dict[str, str]]:
    sys.setrecursionlimit(100000)
    rules = gc.load_grammar(manual_path)
    rec = gc.Recognizer(rules)
    names, units = label_grammar(rules)
    covered: Dict[str, str] = {}
    for path, src in sources:
        try:
            toks = gc.lex(src)
        except gc.LexError:
            continue
        ok, _ = rec.run(toks)
        if not ok:
            continue
        d = Deriver(rec, names)
        d.used.add("program")
        d.derive(rec.rules["program"], 0, len(toks) - 1)
        for u in d.used:
            covered.setdefault(u, rel(path))
    return units, covered


# ---------------------------------------------------------- diagnostics --

def diagnostic_units(manual: str, index) -> Dict[str, str]:
    """code -> section where the manual first cites it (codes come from the
    section 11 table; 'E0238 to E0245' is a range)."""
    at = manual.find("## 11. Diagnostics")
    end = manual.find("\n## 12.", at)
    table = manual[at:end]
    codes: List[str] = []
    for row in re.findall(r"^\|([^|]*)\|", table, re.M):
        for a, b in re.findall(r"([EW]\d{4}) to ([EW]\d{4})", row):
            for n in range(int(a[1:]), int(b[1:]) + 1):
                codes.append(f"{a[0]}{n:04d}")
        codes += re.findall(r"[EW]\d{4}", row)
    out: Dict[str, str] = {}
    for c in codes:
        if c in out:
            continue
        m = re.search(r"\b" + c + r"\b", manual)
        out[c] = section_at(index, m.start()) if m and m.start() < at else "11 Diagnostics"
    return out


def diagnostic_coverage(units: Dict[str, str], neg_roots: List[str]) -> Dict[str, str]:
    covered: Dict[str, str] = {}

    def mark(code: str, where: str) -> None:
        if code in units:
            covered.setdefault(code, where)

    # The manual's own examples that must fail with a code.
    manual = read(os.path.join(ROOT, "docs", "language_definition.md"))
    for c in re.findall(r"doctest-expect-error:\s*([EW]\d{4})", manual):
        mark(c, "docs/language_definition.md (doctest-expect-error)")
    for p in iter_files(neg_roots, (".expected", ".expected_help")):
        for c in re.findall(r"[EW]\d{4}", read(p)):
            mark(c, rel(p))
    # ctest regexes in the CMake files.
    for p in iter_files([os.path.join(ROOT, "CMakeLists.txt"),
                         os.path.join(ROOT, "tests")], ("CMakeLists.txt",)):
        for line in read(p).splitlines():
            if "REGULAR_EXPRESSION" in line:
                for c in re.findall(r"[EW]\d{4}", line):
                    mark(c, rel(p))
    # Unit tests name codes by their macros: IRON_ERR_X / IRON_WARN_X.
    macros: Dict[str, str] = {}
    hdr = read(os.path.join(ROOT, "src", "diagnostics", "diagnostics.h"))
    for name, num in re.findall(r"#define\s+(IRON_(?:ERR|WARN)_\w+)\s+(\d+)", hdr):
        macros[name] = ("W" if name.startswith("IRON_WARN") else "E") + f"{int(num):04d}"
    for p in iter_files([os.path.join(ROOT, "tests", "unit")], (".c",)):
        src = read(p)
        for name in set(re.findall(r"IRON_(?:ERR|WARN)_\w+", src)):
            if name in macros:
                mark(macros[name], rel(p))
    return covered


# -------------------------------------------------------------- library --

def library_units(manual: str, index) -> Dict[str, Tuple[str, str]]:
    """unit -> (section, search pattern). Units are the names in the first
    column of the section 9 tables: `Math.sqrt(x)` is a qualified call,
    `upper()` a method, and the section 9.1 built-ins are bare calls."""
    at = manual.find("## 9. The standard library")
    end = manual.find("\n## 10.", at)
    out: Dict[str, Tuple[str, str]] = {}
    for m in re.finditer(r"^\|([^|\n]*)\|", manual[at:end], re.M):
        sec = section_at(index, at + m.start())
        builtin = sec.startswith("9.1 ")
        for span in re.findall(r"`([^`]*)`", m.group(1)):
            for q, name in re.findall(r"(?:\b([A-Z]\w*)\.)?\b([a-z_]\w*)\(", span):
                if name == "func":  # `func [T].name(...)`: a declaration form
                    continue
                if q:
                    unit, pat = f"{q}.{name}", r"\b" + q + r"\." + name + r"\("
                elif builtin:
                    unit, pat = name, r"(?<![.\w])" + name + r"\("
                else:
                    unit, pat = f".{name}", r"\." + name + r"\("
                out.setdefault(unit, (sec, pat))
    return out


def strip_comments(src: str) -> str:
    return re.sub(r"--[^\n]*", "", src)


def library_coverage(units, sources) -> Dict[str, str]:
    covered: Dict[str, str] = {}
    bodies = [(rel(p), strip_comments(s)) for p, s in sources]
    for unit, (_sec, pat) in units.items():
        rx = re.compile(pat)
        for where, body in bodies:
            if rx.search(body):
                covered[unit] = where
                break
    return covered


# ------------------------------------------------------------- examples --

def example_units(manual: str, index) -> Tuple[Dict[str, str], Set[str]]:
    """example id ('3.3#1') -> section; and the ids the doc-example test
    runs (every block not preceded by a doctest-skip marker)."""
    units: Dict[str, str] = {}
    checked: Set[str] = set()
    count: Dict[str, int] = {}
    for m in re.finditer(r"^```iron\n.*?^```\n", manual, re.S | re.M):
        sec = section_at(index, m.start())
        num = sec.split(" ", 1)[0]
        count[num] = count.get(num, 0) + 1
        uid = f"{num}#{count[num]}"
        units[uid] = sec
        before = manual[max(0, m.start() - 300):m.start()].rstrip().splitlines()
        if not (before and "doctest-skip" in before[-1]):
            checked.add(uid)
    return units, checked


# --------------------------------------------------------------- report --

def pct(a: int, b: int) -> str:
    return f"{100.0 * a / b:5.1f}%" if b else "   n/a"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--manual", default=os.path.join(ROOT, "docs", "language_definition.md"))
    ap.add_argument("--corpus", action="append",
                    help="positive corpus (default: tests/integration/v4, multi_file, test_blocks)")
    ap.add_argument("--negative", action="append",
                    help="negative corpus (default: tests/integration/v4-fail, diagnostics)")
    ap.add_argument("--missing", action="store_true", help="list every uncovered unit")
    ap.add_argument("--json", help="write the full result to this file")
    ap.add_argument("--check", help="baseline JSON: fail if a covered unit is lost")
    ap.add_argument("--update", help="rewrite this baseline JSON")
    args = ap.parse_args()

    it = os.path.join(ROOT, "tests", "integration")
    pos_roots = args.corpus or [os.path.join(it, d) for d in ("v4", "multi_file", "test_blocks")]
    neg_roots = args.negative or [os.path.join(it, "v4-fail"), os.path.join(it, "diagnostics")]
    manual = read(args.manual)
    index = section_index(manual)
    sources = positive_sources([r for r in pos_roots if os.path.exists(r)])

    syn_units, syn_cov = syntax_coverage(args.manual, sources)
    diag_units = diagnostic_units(manual, index)
    diag_cov = diagnostic_coverage(diag_units, [r for r in neg_roots if os.path.exists(r)])
    lib_units = library_units(manual, index)
    lib_cov = library_coverage(lib_units, sources)
    ex_units, ex_checked = example_units(manual, index)

    dims = {
        "syntax": {u: "13 Complete syntax of Iron" for u in syn_units},
        "diagnostics": diag_units,
        "library": {u: s for u, (s, _p) in lib_units.items()},
        "examples": ex_units,
    }
    covered = {
        "syntax": syn_cov,
        "diagnostics": diag_cov,
        "library": lib_cov,
        "examples": {u: "scripts/test_doc_examples.sh" for u in ex_checked},
    }

    total_units = sum(len(v) for v in dims.values())
    total_cov = sum(len(covered[d]) for d in dims)
    print(f"Spec coverage of {rel(args.manual)} "
          f"({len(sources)} programs in the positive corpus)\n")
    print(f"  {'dimension':<12} {'covered':>8} {'units':>6} {'':>7}")
    for d, units in dims.items():
        print(f"  {d:<12} {len(covered[d]):>8} {len(units):>6} {pct(len(covered[d]), len(units))}")
    print(f"  {'overall':<12} {total_cov:>8} {total_units:>6} {pct(total_cov, total_units)}")

    # Per section, for the dimensions that map to one.
    per: Dict[str, List[int]] = {}
    for d in ("diagnostics", "library", "examples"):
        for u, sec in dims[d].items():
            row = per.setdefault(sec, [0, 0])
            row[1] += 1
            if u in covered[d]:
                row[0] += 1
    print("\n  by section (diagnostics, library and examples):")

    def sec_key(s: str):
        return [int(x) for x in re.findall(r"\d+", s.split(" ", 1)[0])]
    for sec in sorted(per, key=sec_key):
        c, t = per[sec]
        print(f"  {pct(c, t)}  {c:>3}/{t:<3} {sec}")

    if args.missing:
        for d, units in dims.items():
            miss = [u for u in units if u not in covered[d]]
            print(f"\n  uncovered {d} ({len(miss)}):")
            for u in miss:
                print(f"    {u}  [{units[u]}]")

    result = {
        "dimensions": {
            d: {"units": len(units), "covered": sorted(covered[d])}
            for d, units in dims.items()
        },
        "overall": {"units": total_units, "covered": total_cov},
    }
    if args.json:
        with open(args.json, "w", encoding="utf-8") as f:
            json.dump({**result, "where": covered, "sections": dims}, f, indent=1, sort_keys=True)

    rc = 0
    if args.check:
        base = json.load(open(args.check, encoding="utf-8"))
        lost = []
        for d, info in base.get("dimensions", {}).items():
            for u in info.get("covered", []):
                if u in dims.get(d, {}) and u not in covered.get(d, {}):
                    lost.append(f"{d}: {u}")
        if lost:
            print("\nFAIL: units the baseline covers are no longer covered:")
            for x in lost:
                print(f"  {x}")
            rc = 1
        gained = total_cov - base.get("overall", {}).get("covered", 0)
        if gained > 0 and not lost:
            print(f"\n{gained} unit(s) covered beyond the baseline: "
                  f"run with --update {rel(args.check)} to keep them.")
    if args.update:
        with open(args.update, "w", encoding="utf-8") as f:
            json.dump(result, f, indent=1, sort_keys=True)
            f.write("\n")
        print(f"\nbaseline written to {rel(args.update)}")
    return rc


if __name__ == "__main__":
    sys.exit(main())
