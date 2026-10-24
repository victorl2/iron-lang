#!/usr/bin/env python3
"""Check relative Markdown links and heading anchors in repository docs."""

from __future__ import annotations

import re
import sys
from pathlib import Path
from urllib.parse import unquote

ROOT = Path(__file__).resolve().parents[1]
LINK_RE = re.compile(r"(?<!!)\[[^\]]*\]\(([^)]+)\)")
HEADING_RE = re.compile(r"^#{1,6}\s+(.+?)\s*#*$")
FENCE_RE = re.compile(r"^\s*(```|~~~)")


def markdown_files() -> list[Path]:
    paths = [ROOT / "README.md"]
    paths.extend((ROOT / "docs").rglob("*.md"))
    paths.extend((ROOT / "examples").rglob("README.md"))
    paths.extend((ROOT / "editors").rglob("README.md"))
    return sorted(set(paths))


def github_slug(text: str) -> str:
    text = re.sub(r"<[^>]+>", "", text.strip().lower())
    text = re.sub(r"[`*_~]", "", text)
    text = re.sub(r"[^\w\- ]", "", text)
    return text.replace(" ", "-")


def anchors(path: Path) -> set[str]:
    result: set[str] = set()
    counts: dict[str, int] = {}
    in_fence = False
    for line in path.read_text(encoding="utf-8").splitlines():
        if FENCE_RE.match(line):
            in_fence = not in_fence
            continue
        if in_fence:
            continue
        match = HEADING_RE.match(line)
        if not match:
            continue
        base = github_slug(match.group(1))
        count = counts.get(base, 0)
        counts[base] = count + 1
        result.add(base if count == 0 else f"{base}-{count}")
    return result


def main() -> int:
    failures: list[str] = []
    anchor_cache: dict[Path, set[str]] = {}
    links = 0
    for source in markdown_files():
        in_fence = False
        for lineno, original in enumerate(
            source.read_text(encoding="utf-8").splitlines(), 1
        ):
            if FENCE_RE.match(original):
                in_fence = not in_fence
                continue
            if in_fence:
                continue
            line = re.sub(r"`[^`]*`", "", original)
            for raw in LINK_RE.findall(line):
                destination = raw.split(maxsplit=1)[0].strip("<>")
                if destination.startswith(("http://", "https://", "mailto:")):
                    continue
                links += 1
                relative, separator, fragment = destination.partition("#")
                target = (
                    (source.parent / unquote(relative)).resolve()
                    if relative
                    else source.resolve()
                )
                if relative and not target.exists():
                    failures.append(
                        f"{source.relative_to(ROOT)}:{lineno}: missing target {destination}"
                    )
                    continue
                if separator and target.suffix.lower() == ".md":
                    known = anchor_cache.setdefault(target, anchors(target))
                    if unquote(fragment).lower() not in known:
                        failures.append(
                            f"{source.relative_to(ROOT)}:{lineno}: missing anchor {destination}"
                        )
    for failure in failures:
        print(f"FAIL {failure}")
    print(f"check_doc_links: {links} relative links, {len(failures)} failures")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
