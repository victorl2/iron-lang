#!/usr/bin/env python3
"""Check the EBNF grammar of docs/language_definition.md against Iron sources.

The grammar is read from the ```ebnf blocks under the "Complete syntax of
Iron" heading of the manual, turned into a recognizer, and run over every
.iron file of the positive fixture corpus (tests/integration/v4 by default).
A file the grammar rejects is reported with the position of the longest
match, which points at the missing production. Files marked
`@expected-pass-after:` are parked fixtures and are skipped.

With --negative DIR, files of a negative corpus whose .expected file names a
parse error code (E0101 to E0107, E0175, E0176) must be rejected.

Usage:
    scripts/grammar_check.py [--manual docs/language_definition.md]
                             [--corpus tests/integration/v4]
                             [--negative tests/integration/v4-fail]
                             [--verbose] [FILE ...]

Standard library only, Python 3.8+.
"""

from __future__ import annotations

import argparse
import os
import re
import sys
from functools import lru_cache
from typing import Dict, FrozenSet, List, Optional, Set, Tuple

# ---------------------------------------------------------------- lexer --

KEYWORDS = {
    "and", "await", "comptime", "copy", "defer", "drop", "elif", "else",
    "enum", "extends", "extern", "false", "for", "free", "func", "heap",
    "if", "impl", "import", "in", "init", "interface", "is", "leak",
    "match", "mut", "nocopy", "not", "null", "object", "or", "parallel",
    "patch", "pool", "private", "pub", "pure", "rc", "readonly", "return",
    "self", "spawn", "super", "true", "unchecked", "val", "var", "weak",
    "while",
}

# Longest punctuation first.
PUNCT = [
    "<<=", ">>=",
    "->", "..", "==", "!=", "<=", ">=", "<<", ">>", "+=", "-=", "*=", "/=",
    "&=", "|=", "^=",
    "+", "-", "*", "/", "%", "=", "<", ">", ".", ",", ":", "?", "&", "|",
    "^", "~", "(", ")", "[", "]", "{", "}", ";", "@",
]


class Token:
    __slots__ = ("kind", "text", "line", "col")

    def __init__(self, kind: str, text: str, line: int, col: int) -> None:
        self.kind = kind
        self.text = text
        self.line = line
        self.col = col

    def __repr__(self) -> str:
        return f"{self.kind}({self.text!r})@{self.line}:{self.col}"


class LexError(Exception):
    pass


def lex(src: str) -> List[Token]:
    """Tokenize Iron source the way src/lexer/lexer.c does, minus newlines
    (the parser skips them everywhere) and doc comments."""
    toks: List[Token] = []
    i, n = 0, len(src)
    line, col = 1, 1

    def adv(k: int = 1) -> None:
        nonlocal i, line, col
        for _ in range(k):
            if i < n and src[i] == "\n":
                line += 1
                col = 1
            else:
                col += 1
            i += 1

    while i < n:
        c = src[i]
        if c in " \t\r\n":
            adv()
            continue
        if c == "-" and src.startswith("--", i):
            while i < n and src[i] != "\n":
                adv()
            continue
        if c == "/" and src.startswith("///", i):
            while i < n and src[i] != "\n":
                adv()
            continue
        sl, sc = line, col
        if c == '"':
            start = i
            multiline = src.startswith('"""', i)
            adv(3 if multiline else 1)
            depth = 0
            while True:
                if i >= n:
                    raise LexError(f"{sl}:{sc}: unterminated string")
                d = src[i]
                if not multiline and d == "\n":
                    raise LexError(f"{sl}:{sc}: unterminated string")
                if depth > 0 and d == '"':
                    # nested literal inside an interpolation expression
                    adv()
                    while i < n and src[i] not in '"\n':
                        if src[i] == "\\":
                            adv()
                        adv()
                    if i < n and src[i] == '"':
                        adv()
                    continue
                if d == "\\":
                    adv(2)
                    continue
                if d == "{":
                    depth += 1
                elif d == "}" and depth > 0:
                    depth -= 1
                if multiline and src.startswith('"""', i):
                    adv(3)
                    break
                if not multiline and d == '"':
                    adv()
                    break
                adv()
            toks.append(Token("STRING", src[start:i], sl, sc))
            continue
        if c.isdigit():
            start = i
            if c == "0" and i + 1 < n and src[i + 1] in "xX":
                adv(2)
                while i < n and src[i] in "0123456789abcdefABCDEF":
                    adv()
                toks.append(Token("INT", src[start:i], sl, sc))
                continue
            if c == "0" and i + 1 < n and src[i + 1] in "bB":
                adv(2)
                while i < n and src[i] in "01":
                    adv()
                toks.append(Token("INT", src[start:i], sl, sc))
                continue
            while i < n and src[i].isdigit():
                adv()
            kind = "INT"
            if i + 1 < n and src[i] == "." and src[i + 1].isdigit():
                adv()
                while i < n and src[i].isdigit():
                    adv()
                kind = "FLOAT"
            if i < n and (src[i].isalpha()):
                raise LexError(f"{sl}:{sc}: invalid numeric literal")
            toks.append(Token(kind, src[start:i], sl, sc))
            continue
        if c.isalpha() or c == "_":
            start = i
            while i < n and (src[i].isalnum() or src[i] == "_"):
                adv()
            text = src[start:i]
            if text == "_":
                toks.append(Token("WILDCARD", text, sl, sc))
            elif text in KEYWORDS:
                toks.append(Token("KW", text, sl, sc))
            else:
                toks.append(Token("IDENT", text, sl, sc))
            continue
        for p in PUNCT:
            if src.startswith(p, i):
                adv(len(p))
                toks.append(Token("PUNCT", p, sl, sc))
                break
        else:
            raise LexError(f"{sl}:{sc}: invalid character {c!r}")
    toks.append(Token("EOF", "", line, col))
    return toks


# ---------------------------------------------------------- EBNF parser --

# Grammar AST nodes: ("term", text) ("nt", name) ("seq", [..]) ("alt", [..])
# ("opt", node) ("rep", node)


class GrammarError(Exception):
    pass


_EBNF_TOKEN = re.compile(
    r"\s+|\(\*.*?\*\)|(?P<def>::=)|(?P<term>'(?:[^'\\]|\\.)*')|(?P<sym>[A-Za-z_][A-Za-z_0-9]*)|(?P<op>[|{}\[\]()])",
    re.S,
)


def parse_ebnf(text: str) -> Dict[str, tuple]:
    items: List[Tuple[str, str]] = []
    pos = 0
    while pos < len(text):
        m = _EBNF_TOKEN.match(text, pos)
        if not m:
            raise GrammarError(f"bad grammar text at {text[pos:pos+20]!r}")
        pos = m.end()
        if m.lastgroup is None:
            continue
        items.append((m.lastgroup, m.group(m.lastgroup)))

    # Split into rules: SYM '::=' body ... up to the next SYM '::='.
    rules: Dict[str, tuple] = {}
    idx = 0
    starts = [k for k in range(len(items) - 1) if items[k][0] == "sym" and items[k + 1][0] == "def"]
    for si, k in enumerate(starts):
        name = items[k][1]
        end = starts[si + 1] if si + 1 < len(starts) else len(items)
        body = items[k + 2:end]
        node, rest = _parse_alt(body, 0)
        if rest != len(body):
            raise GrammarError(f"rule {name}: trailing tokens {body[rest:]}")
        if name in rules:
            raise GrammarError(f"rule {name} defined twice")
        rules[name] = node
        idx = end
    if not rules:
        raise GrammarError("no rules found")
    return rules


def _parse_alt(items, i):
    alts = []
    node, i = _parse_seq(items, i)
    alts.append(node)
    while i < len(items) and items[i] == ("op", "|"):
        node, i = _parse_seq(items, i + 1)
        alts.append(node)
    return (("alt", alts) if len(alts) > 1 else alts[0]), i


def _parse_seq(items, i):
    seq = []
    while i < len(items):
        kind, text = items[i]
        if kind == "term":
            seq.append(("term", bytes(text[1:-1], "utf-8").decode("unicode_escape")))
            i += 1
        elif kind == "sym":
            seq.append(("nt", text))
            i += 1
        elif kind == "op" and text == "{":
            node, i = _parse_alt(items, i + 1)
            if i >= len(items) or items[i] != ("op", "}"):
                raise GrammarError("unclosed {")
            seq.append(("rep", node))
            i += 1
        elif kind == "op" and text == "[":
            node, i = _parse_alt(items, i + 1)
            if i >= len(items) or items[i] != ("op", "]"):
                raise GrammarError("unclosed [")
            seq.append(("opt", node))
            i += 1
        elif kind == "op" and text == "(":
            node, i = _parse_alt(items, i + 1)
            if i >= len(items) or items[i] != ("op", ")"):
                raise GrammarError("unclosed (")
            seq.append(node)
            i += 1
        else:
            break
    if not seq:
        raise GrammarError(f"empty sequence at {items[i:i+3]}")
    return (("seq", seq) if len(seq) > 1 else seq[0]), i


# ------------------------------------------------------------ recognizer --

TOKEN_SYMBOLS = {"IDENT", "INT", "FLOAT", "STRING"}


class Recognizer:
    """Set-of-positions recognizer for the EBNF (general CFG, memoized)."""

    def __init__(self, rules: Dict[str, tuple]) -> None:
        self.rules = rules
        for name, body in rules.items():
            self._check_refs(body, name)

    def _check_refs(self, node, rule):
        kind = node[0]
        if kind == "nt":
            if node[1] not in self.rules and node[1] not in TOKEN_SYMBOLS:
                raise GrammarError(f"rule {rule} references undefined {node[1]}")
        elif kind in ("seq", "alt"):
            for c in node[1]:
                self._check_refs(c, rule)
        elif kind in ("opt", "rep"):
            self._check_refs(node[1], rule)

    def run(self, toks: List[Token], start: str = "program") -> Tuple[bool, int]:
        self.toks = toks
        self.memo: Dict[Tuple[str, int], FrozenSet[int]] = {}
        self.active: Set[Tuple[str, int]] = set()
        self.furthest = 0
        ends = self._nt(start, 0)
        eof = len(toks) - 1
        return (eof in ends), self.furthest

    def _nt(self, name: str, pos: int) -> FrozenSet[int]:
        key = (name, pos)
        if key in self.memo:
            return self.memo[key]
        if key in self.active:
            return frozenset()  # left recursion guard (grammar has none)
        self.active.add(key)
        if name in TOKEN_SYMBOLS:
            tok = self.toks[pos]
            res = frozenset({pos + 1}) if tok.kind == name else frozenset()
        else:
            res = self._node(self.rules[name], pos)
        self.active.discard(key)
        self.memo[key] = res
        return res

    def _node(self, node, pos: int) -> FrozenSet[int]:
        kind = node[0]
        if kind == "term":
            tok = self.toks[pos]
            if tok.kind != "EOF" and tok.text == node[1] and (
                tok.kind != "IDENT" or node[1] not in KEYWORDS
            ):
                if pos + 1 > self.furthest:
                    self.furthest = pos + 1
                return frozenset({pos + 1})
            return frozenset()
        if kind == "nt":
            res = self._nt(node[1], pos)
            if res:
                m = max(res)
                if m > self.furthest:
                    self.furthest = m
            return res
        if kind == "seq":
            frontier: Set[int] = {pos}
            for child in node[1]:
                nxt: Set[int] = set()
                for p in frontier:
                    nxt |= self._node(child, p)
                if not nxt:
                    return frozenset()
                frontier = nxt
            return frozenset(frontier)
        if kind == "alt":
            out: Set[int] = set()
            for child in node[1]:
                out |= self._node(child, pos)
            return frozenset(out)
        if kind == "opt":
            return frozenset({pos}) | self._node(node[1], pos)
        if kind == "rep":
            seen: Set[int] = {pos}
            frontier = {pos}
            while frontier:
                nxt: Set[int] = set()
                for p in frontier:
                    for q in self._node(node[1], p):
                        if q not in seen and q > p:
                            nxt.add(q)
                seen |= nxt
                frontier = nxt
            return frozenset(seen)
        raise GrammarError(f"unknown node {kind}")


# ---------------------------------------------------------------- driver --

PARSE_ERROR_CODES = re.compile(r"\bE010[1-7]\b|\bE0175\b|\bE0176\b")


def load_grammar(manual_path: str) -> Dict[str, tuple]:
    text = open(manual_path, encoding="utf-8").read()
    marker = "## 13. Complete syntax of Iron"
    at = text.find(marker)
    if at < 0:
        raise GrammarError(f"{manual_path}: section '{marker}' not found")
    blocks = re.findall(r"^```ebnf\n(.*?)^```", text[at:], re.S | re.M)
    if not blocks:
        raise GrammarError("no ```ebnf block in the syntax section")
    return parse_ebnf("\n".join(blocks))


def iter_iron_files(root: str):
    for dirpath, _dirs, files in sorted(os.walk(root)):
        for f in sorted(files):
            if f.endswith(".iron"):
                yield os.path.join(dirpath, f)


def check_file(rec: Recognizer, path: str) -> Tuple[Optional[bool], str]:
    src = open(path, encoding="utf-8-sig", errors="replace").read()
    try:
        toks = lex(src)
    except LexError as e:
        return None, f"lex error: {e}"
    ok, furthest = rec.run(toks)
    if ok:
        return True, ""
    tok = toks[min(furthest, len(toks) - 1)]
    return False, f"rejected near {tok.line}:{tok.col} ({tok.kind} {tok.text!r})"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--manual", default="docs/language_definition.md")
    ap.add_argument("--corpus", default="tests/integration/v4")
    ap.add_argument("--negative", default=None,
                    help="negative corpus whose parse-error fixtures must be rejected")
    ap.add_argument("--verbose", "-v", action="store_true")
    ap.add_argument("files", nargs="*", help="check only these files")
    args = ap.parse_args()

    sys.setrecursionlimit(20000)
    rules = load_grammar(args.manual)
    rec = Recognizer(rules)
    if args.verbose:
        print(f"grammar: {len(rules)} productions")

    failures = 0
    checked = 0
    skipped = 0

    files = args.files or list(iter_iron_files(args.corpus))
    for path in files:
        src = open(path, encoding="utf-8-sig", errors="replace").read()
        if "@expected-pass-after" in src:
            skipped += 1
            continue
        ok, why = check_file(rec, path)
        checked += 1
        if ok:
            if args.verbose:
                print(f"ok   {path}")
        else:
            failures += 1
            print(f"FAIL {path}: {why}")

    neg_checked = 0
    if args.negative:
        for path in iter_iron_files(args.negative):
            expected = os.path.splitext(path)[0] + ".expected"
            if not os.path.exists(expected):
                continue
            if not PARSE_ERROR_CODES.search(open(expected, encoding="utf-8").read()):
                continue
            src = open(path, encoding="utf-8-sig", errors="replace").read()
            if "@expected-pass-after" in src:
                continue
            ok, why = check_file(rec, path)
            neg_checked += 1
            if ok:
                failures += 1
                print(f"FAIL {path}: grammar accepts a program the parser rejects")
            elif args.verbose:
                print(f"ok   {path}: {why}")

    print(f"grammar_check: {checked} files accepted-checked, {skipped} parked fixtures skipped, "
          f"{neg_checked} negative fixtures checked, {failures} failures")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
