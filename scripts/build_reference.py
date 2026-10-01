#!/usr/bin/env python3
"""Render docs/language_definition.md as the ironlang.dev reference page.

Writes <site>/docs/index.html from the reference manual and the page
template in scripts/templates/reference.html, so the website can never
drift from the manual: the manual is the single source, and every ```iron
block in it is compiled by the Doc Test workflow.

The sidebar is built from the manual's "##" sections and "###" subsections.
Heading ids follow the GitHub slug rules the manual's own table of contents
uses, so links of the form /docs/#6-memory keep working from other pages.

Requires the Python-Markdown package (pip install markdown).

Usage:
  scripts/build_reference.py [--site DIR]
  scripts/build_reference.py --check     # render to a temp dir and validate
"""

from __future__ import annotations

import argparse
import html
import re
import sys
import tempfile
from pathlib import Path

try:
    import markdown
except ImportError:  # pragma: no cover
    sys.stderr.write("build_reference.py needs Python-Markdown: pip install markdown\n")
    sys.exit(2)

REPO = Path(__file__).resolve().parent.parent
MANUAL = REPO / "docs" / "language_definition.md"
TEMPLATE = REPO / "scripts" / "templates" / "reference.html"
EXAMPLES_DIR = REPO / "docs" / "examples"
GITHUB_DOCS = "https://github.com/victorl2/iron-lang/blob/main/docs/"

# Manual files that have a page of their own on the site.
SITE_PAGES = {
    "networking.md": "/networking/",
}

# Keywords from manual section 1.3. Reserved words are included so that the
# examples showing their rejection highlight them the same way.
KEYWORDS = {
    "and", "await", "comptime", "copy", "defer", "drop", "elif", "else",
    "enum", "extends", "extern", "false", "for", "free", "func", "heap",
    "if", "impl", "import", "in", "init", "interface", "is", "leak",
    "match", "mut", "nocopy", "not", "null", "object", "or", "parallel",
    "patch", "pool", "private", "pub", "pure", "rc", "readonly", "return",
    "self", "spawn", "super", "true", "unchecked", "val", "var", "weak",
    "while",
}

TOKEN = re.compile(
    r"(?P<cm>--[^\n]*)"
    r"|(?P<st>\"(?:\\.|[^\"\\\n])*\")"
    r"|(?P<nu>\b\d[\d_]*(?:\.\d[\d_]*)?(?:[eE][+-]?\d+)?\b)"
    r"|(?P<id>\b[A-Za-z_][A-Za-z0-9_]*\b)"
)


def slugify(text: str) -> str:
    """GitHub-style heading anchors: lowercase, drop punctuation, spaces to '-'."""
    text = re.sub(r"`", "", text).strip().lower()
    text = re.sub(r"[^\w\- ]", "", text)
    return re.sub(r" ", "-", text)


def highlight_iron(code: str) -> str:
    """Wrap Iron tokens in the span classes the site stylesheet already defines."""
    out: list[str] = []
    pos = 0
    for match in TOKEN.finditer(code):
        out.append(html.escape(code[pos:match.start()]))
        text = html.escape(match.group(0))
        kind = match.lastgroup
        if kind == "id":
            word = match.group(0)
            after = code[match.end():match.end() + 1]
            if word in KEYWORDS:
                out.append(f'<span class="kw">{text}</span>')
            elif word[0].isupper() and word.isupper() and len(word) > 1:
                out.append(f'<span class="pr">{text}</span>')
            elif word[0].isupper():
                out.append(f'<span class="ty">{text}</span>')
            elif after == "(":
                out.append(f'<span class="fn">{text}</span>')
            else:
                out.append(text)
        else:
            out.append(f'<span class="{kind}">{text}</span>')
        pos = match.end()
    out.append(html.escape(code[pos:]))
    return "".join(out)


def load_examples() -> dict[str, str]:
    """Featured examples whose source the branding check expects on this page."""
    examples = {}
    for path in sorted(EXAMPLES_DIR.glob("*.iron")):
        examples[path.read_text(encoding="utf-8").strip()] = path.stem
    return examples


class Renderer:
    def __init__(self, source: str) -> None:
        self.source = source
        self.examples = load_examples()
        self.fences: list[str] = []
        self.headings: list[tuple[int, str, str]] = []

    def protect_fences(self, text: str) -> str:
        """Replace fenced blocks with placeholders rendered by hand afterwards."""
        pattern = re.compile(r"^```([a-z]*)\n(.*?)^```[ \t]*$", re.MULTILINE | re.DOTALL)

        def repl(match: re.Match[str]) -> str:
            lang, code = match.group(1), match.group(2)
            code = code.rstrip("\n")
            attrs = ""
            if lang == "iron":
                body = highlight_iron(code)
                name = self.examples.get(code.strip())
                if name:
                    attrs = f' data-example="{name}"'
            else:
                body = html.escape(code)
            label = f' data-lang="{lang}"' if lang else ""
            block = f'<div class="code-block"{label}><pre{attrs}>{body}</pre></div>'
            self.fences.append(block)
            return f"\n\n@@FENCE{len(self.fences) - 1}@@\n\n"

        return pattern.sub(repl, text)

    def restore_fences(self, rendered: str) -> str:
        def repl(match: re.Match[str]) -> str:
            return self.fences[int(match.group(1))]

        rendered = re.sub(r"<p>@@FENCE(\d+)@@</p>", repl, rendered)
        return re.sub(r"@@FENCE(\d+)@@", repl, rendered)

    def add_heading_ids(self, rendered: str) -> str:
        seen: dict[str, int] = {}

        def repl(match: re.Match[str]) -> str:
            level = int(match.group(1))
            inner = match.group(2)
            plain = re.sub(r"<[^>]+>", "", inner)
            plain = html.unescape(plain)
            slug = slugify(plain)
            count = seen.get(slug, 0)
            seen[slug] = count + 1
            if count:
                slug = f"{slug}-{count}"
            if level > 1:
                self.headings.append((level, slug, plain))
            return f'<h{level} id="{slug}">{inner}</h{level}>'

        return re.sub(r"<h([1-6])>(.*?)</h\1>", repl, rendered, flags=re.DOTALL)

    def render(self) -> tuple[str, str]:
        text = self.protect_fences(self.source)
        body = markdown.markdown(text, extensions=["tables"], output_format="html5")
        body = self.add_heading_ids(body)
        body = self.restore_fences(body)
        body = body.replace("<table>", '<table class="doc-table">')
        body = re.sub(r'href="([A-Za-z0-9_./-]+\.md)(#[^"]*)?"', self.doc_link, body)
        return body, self.sidebar()

    @staticmethod
    def doc_link(match: re.Match[str]) -> str:
        """Relative links between repository docs: site page if one exists,
        otherwise the file on GitHub."""
        target, fragment = match.group(1), match.group(2) or ""
        page = SITE_PAGES.get(target)
        if page:
            return f'href="{page}{fragment}"'
        return f'href="{GITHUB_DOCS}{target}{fragment}"'

    def sidebar(self) -> str:
        parts: list[str] = []
        open_section = False
        for level, slug, title in self.headings:
            title_html = html.escape(title)
            if level == 2:
                if open_section:
                    parts.append("      </div>")
                parts.append('      <div class="sidebar-section">')
                parts.append(f'        <h3><a href="#{slug}">{title_html}</a></h3>')
                open_section = True
            elif level == 3:
                # Drop the "6.3 " numbering: the section header carries it.
                short = re.sub(r"^\d+\.\d+\s+", "", title)
                parts.append(f'        <a href="#{slug}">{html.escape(short)}</a>')
        if open_section:
            parts.append("      </div>")
        return "\n".join(parts)


def build(site: Path) -> Path:
    source = MANUAL.read_text(encoding="utf-8")
    renderer = Renderer(source)
    content, sidebar = renderer.render()
    template = TEMPLATE.read_text(encoding="utf-8")
    page = template.replace("{{SIDEBAR}}", sidebar).replace("{{CONTENT}}", content)
    out = site / "docs" / "index.html"
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(page, encoding="utf-8")
    return out


def check(out: Path) -> list[str]:
    """Every in-page link must resolve and the manual's own TOC must survive."""
    page = out.read_text(encoding="utf-8")
    ids = set(re.findall(r' id="([^"]+)"', page))
    errors = []
    for target in re.findall(r'href="#([^"]+)"', page):
        if target not in ids:
            errors.append(f"{out}: broken fragment #{target}")
    for target in re.findall(r"\]\(#([^)]+)\)", MANUAL.read_text(encoding="utf-8")):
        if target not in ids:
            errors.append(f"{MANUAL}: anchor #{target} not produced by the renderer")
    if "@@FENCE" in page:
        errors.append(f"{out}: unrendered code fence placeholder")
    if 'data-example="native_summary"' not in page:
        errors.append(f"{out}: featured native_summary example missing")
    return errors


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--site", type=Path, default=REPO / "docs" / "site")
    parser.add_argument("--check", action="store_true",
                        help="render into a temporary directory and validate")
    args = parser.parse_args()
    if args.check:
        with tempfile.TemporaryDirectory(prefix="iron-reference-") as tmp:
            out = build(Path(tmp))
            errors = check(out)
    else:
        out = build(args.site)
        errors = check(out)
        print(f"wrote {out}")
    for error in errors:
        print(error, file=sys.stderr)
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
