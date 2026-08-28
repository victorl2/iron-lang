#!/usr/bin/env python3
"""Validate local links, fragments, and duplicate IDs in docs/site."""

from __future__ import annotations

import argparse
from html.parser import HTMLParser
from pathlib import Path
from urllib.parse import unquote, urlsplit


# Generated Raylib reference overload groups intentionally reuse anchors.
# Keep the exception list exact so any new duplicate ID still fails Pages CI.
KNOWN_DUPLICATE_IDS = {
    ("raylib/reference/model.html", "model-draw"),
    ("raylib/reference/model.html", "mesh-draw"),
    ("raylib/reference/tex.html", "image-load"),
    ("raylib/reference/tex.html", "image-export"),
    ("raylib/reference/tex.html", "texture-load"),
    ("raylib/reference/tex.html", "texture-update"),
    ("raylib/reference/tex.html", "texture-draw"),
    ("raylib/reference/text.html", "font-load"),
    ("raylib/reference/text.html", "text-measure"),
    ("raylib/reference/types.html", "camera"),
}


class PageParser(HTMLParser):
    def __init__(self) -> None:
        super().__init__(convert_charrefs=True)
        self.ids: list[str] = []
        self.links: list[tuple[int, str]] = []

    def handle_starttag(self, tag: str, attrs: list[tuple[str, str | None]]) -> None:
        values = dict(attrs)
        if values.get("id"):
            self.ids.append(values["id"] or "")
        if tag == "a" and values.get("href") is not None:
            self.links.append((self.getpos()[0], values["href"] or ""))


def page_target(site: Path, source: Path, raw_path: str) -> Path:
    decoded = unquote(raw_path)
    if decoded == "":
        return source.resolve()
    if decoded.startswith("/"):
        target = site / decoded.lstrip("/")
    else:
        target = source.parent / decoded
    if decoded.endswith("/") or target.is_dir():
        target = target / "index.html"
    return target.resolve()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("site", type=Path)
    args = parser.parse_args()
    site = args.site.resolve()
    pages = sorted(site.rglob("*.html"))
    parsed: dict[Path, PageParser] = {}
    errors: list[str] = []
    warnings: list[str] = []

    for page in pages:
        result = PageParser()
        result.feed(page.read_text(encoding="utf-8"))
        parsed[page.resolve()] = result
        seen: set[str] = set()
        for element_id in result.ids:
            if element_id in seen:
                relative = page.relative_to(site).as_posix()
                duplicate = (relative, element_id)
                message = f"{page}: duplicate id #{element_id}"
                if duplicate in KNOWN_DUPLICATE_IDS:
                    warnings.append(message)
                else:
                    errors.append(message)
            seen.add(element_id)

    generated_paths = {"/install.sh"}
    for page, result in parsed.items():
        for line, href in result.links:
            parts = urlsplit(href)
            if parts.scheme or parts.netloc or href.startswith(("mailto:", "data:", "javascript:")):
                continue
            if parts.path in generated_paths:
                continue
            target = page_target(site, page, parts.path)
            if target not in parsed:
                errors.append(f"{page}:{line}: missing local page {href!r}")
                continue
            if parts.fragment and parts.fragment not in parsed[target].ids:
                errors.append(f"{page}:{line}: missing fragment #{parts.fragment} in {target}")

    if errors:
        print("Site validation failed:")
        for error in errors:
            print(f"- {error}")
        return 1
    if warnings:
        print(f"Site validation warnings: {len(warnings)} pre-existing duplicate IDs")
        for warning in warnings:
            print(f"- {warning}")
    print(f"Site validation passed: {len(pages)} HTML pages")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
