#!/usr/bin/env python3
"""Check current positioning and keep featured snippets executable and in sync.

No compiler is needed for static checks. Pass --compiler to also run the examples.
Historical release notes and dedicated game guides deliberately retain game terms.
"""

from __future__ import annotations

import argparse
from html.parser import HTMLParser
from pathlib import Path
import re
import subprocess
import tempfile


EXAMPLES = {
    "native_summary": "processed=5 total=150\n",
    "native_memory": "local=1 owned=2 shared=3\n",
    "native_concurrency": "total=4950\n",
    "native_comptime": "buffer bytes=65536\n",
}
FEATURED_PAGES = {
    "index.html": set(EXAMPLES),
    "docs/index.html": {"native_summary"},
}
OLD_POSITIONING = re.compile(
    r"forged\s+for\s+games|(?:language|built|designed)\s+(?:primarily\s+)?for\s+game\s+development",
    re.IGNORECASE,
)


class Snippets(HTMLParser):
    def __init__(self) -> None:
        super().__init__(convert_charrefs=True)
        self.examples: dict[str, str] = {}
        self.duplicates: list[str] = []
        self.active: str | None = None
        self.visible: list[str] = []

    def handle_starttag(self, tag, attrs):
        values = dict(attrs)
        if tag == "pre" and values.get("data-example"):
            self.active = values["data-example"]
            if self.active in self.examples:
                self.duplicates.append(self.active)
            self.examples[self.active] = ""
        if tag == "meta" and values.get("content"):
            self.visible.append(values["content"])

    def handle_endtag(self, tag):
        if tag == "pre":
            self.active = None

    def handle_data(self, data):
        self.visible.append(data)
        if self.active is not None:
            self.examples[self.active] += data


def validate_page(content: str, expected: set[str], sources: dict[str, str]) -> list[str]:
    page = Snippets()
    page.feed(content)
    errors = []
    if OLD_POSITIONING.search(" ".join(page.visible)):
        errors.append("obsolete games-first positioning")
    if set(page.examples) != expected:
        errors.append(f"featured examples must be {sorted(expected)}, got {sorted(page.examples)}")
    for name in page.duplicates:
        errors.append(f"duplicate example {name}")
    for name, source in page.examples.items():
        if name in sources and source.strip() != sources[name].strip():
            errors.append(f"{name} differs from docs/examples/{name}.iron")
    return errors


def validate_repository(root: Path, site: Path) -> list[str]:
    errors = []
    sources = {}
    for name in EXAMPLES:
        path = root / "docs/examples" / f"{name}.iron"
        if not path.is_file():
            errors.append(f"missing example source: {path}")
        else:
            sources[name] = path.read_text(encoding="utf-8")
    for relative, expected in FEATURED_PAGES.items():
        path = site / relative
        if not path.is_file():
            errors.append(f"missing featured page: {path}")
            continue
        errors.extend(f"{path}: {error}" for error in validate_page(
            path.read_text(encoding="utf-8"), expected, sources))
    for relative in ("README.md", "docs/positioning.md", "docs/language_definition.md"):
        path = root / relative
        if not path.is_file():
            errors.append(f"missing positioning document: {path}")
            continue
        content = path.read_text(encoding="utf-8")
        if "general-purpose native programming language" not in content:
            errors.append(f"{path}: missing general-purpose introduction")
        if OLD_POSITIONING.search(content):
            errors.append(f"{path}: obsolete games-first positioning")
        if relative == "docs/language_definition.md" and "native_summary" in sources:
            blocks = re.findall(r"^```iron\n(.*?)^```", content, re.MULTILINE | re.DOTALL)
            if sources["native_summary"].strip() not in [block.strip() for block in blocks]:
                errors.append(f"{path}: native summary example differs from its source")
    return errors


def run_examples(root: Path, compiler: Path) -> list[str]:
    errors = []
    # Isolate generated artifacts and ignore any developer's local iron.toml.
    with tempfile.TemporaryDirectory(prefix="iron-branding-") as temporary:
        for name, expected in EXAMPLES.items():
            try:
                result = subprocess.run(
                    [str(compiler), "run", str(root / "docs/examples" / f"{name}.iron")],
                    cwd=temporary, capture_output=True, text=True, timeout=90,
                )
            except (OSError, subprocess.TimeoutExpired) as error:
                errors.append(f"{name}: {error}")
                continue
            if result.returncode != 0 or result.stdout != expected:
                errors.append(f"{name}: exit={result.returncode}, stdout={result.stdout!r}, stderr={result.stderr!r}")
    return errors


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", type=Path, help="iron CLI binary to execute examples")
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    errors = validate_repository(root, root / "docs/site")
    if args.compiler:
        errors.extend(run_examples(root, args.compiler.resolve()))
    for error in errors:
        print(f"FAIL: {error}")
    if not errors:
        print("Branding and featured-example checks passed" + (" (4 programs executed)" if args.compiler else ""))
    return bool(errors)


if __name__ == "__main__":
    raise SystemExit(main())
