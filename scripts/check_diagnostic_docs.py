#!/usr/bin/env python3
"""Check the compiler diagnostic namespace and its documentation registry."""

from __future__ import annotations

import re
import sys
from collections import defaultdict
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
HEADER = ROOT / "src/diagnostics/diagnostics.h"
REGISTRY = ROOT / "docs/dev/diagnostic-codes.md"
DEFINE_RE = re.compile(
    r"^#define\s+(IRON_(ERR|WARN)_[A-Z0-9_]+)\s+(\d+)\b", re.MULTILINE
)
ROW_RE = re.compile(
    r"^\|\s*([EW]\d{4})\s*\|\s*`(IRON_(?:ERR|WARN)_[A-Z0-9_]+)`\s*\|"
    r"\s*(.*?)\s*\|\s*(.*?)\s*\|\s*(emitted|reserved|retired)\s*\|$",
    re.MULTILINE,
)
MENTION_RE = re.compile(r"\b([EW]\d{4})\b")


def fail(message: str, failures: list[str]) -> None:
    failures.append(message)


def main() -> int:
    failures: list[str] = []
    definitions: dict[str, tuple[str, int]] = {}
    by_number: defaultdict[int, list[str]] = defaultdict(list)
    for symbol, kind, raw_number in DEFINE_RE.findall(HEADER.read_text()):
        number = int(raw_number)
        prefix = "W" if kind == "WARN" else "E"
        code = f"{prefix}{number:04d}"
        definitions[symbol] = (code, number)
        by_number[number].append(symbol)

    for number, symbols in sorted(by_number.items()):
        if len(symbols) != 1:
            fail(f"diagnostic number {number} is assigned to {', '.join(symbols)}", failures)

    documented: defaultdict[str, list[tuple[str, str, str, str]]] = defaultdict(list)
    registry_text = REGISTRY.read_text()
    for code, symbol, meaning, example, status in ROW_RE.findall(registry_text):
        documented[symbol].append((code, meaning, example, status))

    for symbol, (expected_code, _number) in sorted(
        definitions.items(), key=lambda item: item[1][1]
    ):
        rows = documented.get(symbol, [])
        if len(rows) != 1:
            fail(f"{symbol} has {len(rows)} registry rows, expected exactly one", failures)
            continue
        code, meaning, example, _status = rows[0]
        if code != expected_code:
            fail(f"{symbol} is {expected_code} in the source but {code} in the registry", failures)
        if not meaning or not example:
            fail(f"{expected_code} needs both a description and an example", failures)

    for symbol in sorted(set(documented) - set(definitions)):
        fail(f"registry documents unknown symbol {symbol}", failures)

    valid_codes = {code for code, _number in definitions.values()}
    markdown = [ROOT / "README.md", *(ROOT / "docs").rglob("*.md")]
    for path in sorted(markdown):
        for code in sorted(set(MENTION_RE.findall(path.read_text(encoding="utf-8")))):
            if code not in valid_codes:
                fail(f"{path.relative_to(ROOT)} mentions unknown diagnostic {code}", failures)

    for message in failures:
        print(f"FAIL {message}")
    print(
        f"check_diagnostic_docs: {len(definitions)} codes, "
        f"{sum(len(rows) for rows in documented.values())} registry rows, "
        f"{len(failures)} failures"
    )
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
