#!/usr/bin/env python3
"""Render the repository's guides as ironlang.dev pages.

Writes <site>/docs/index.html from the reference manual and the other
guide pages (networking, projects, raylib) from their markdown files in
docs/, through the page template in scripts/templates/reference.html, so
the website can never drift from the documentation: the markdown is the
single source, and every ```iron block in it is compiled by the Doc Test
workflow.

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
GITHUB_REPO = "https://github.com/victorl2/iron-lang"
GITHUB_DOCS = GITHUB_REPO + "/blob/main/docs/"

# The pages: markdown source in docs/, site path, title, description and
# the plain Markdown copy build_llms.py publishes under /llms/.
PAGES = [
    {
        "source": "language_definition.md", "path": "docs",
        "title": "Reference Manual",
        "description": "The Iron reference manual: lexical rules, types, expressions, statements, declarations, memory, concurrency, compile-time evaluation, the standard library and every diagnostic code. Generated from the compiler-verified manual in the repository.",
        "llms": "/llms/reference.md",
        "featured": "native_summary",
    },
    {
        "source": "networking.md", "path": "networking",
        "title": "Networking",
        "description": "TCP, UDP, DNS, HTTP and HTTPS clients and servers, REST, webpages, WebSocket and WSS, and binary-safe files in Iron, with the ownership and limits that keep them safe.",
        "llms": "/llms/networking.md",
    },
    {
        "source": "guide.md", "path": "guide",
        "title": "Projects",
        "description": "Iron packages: iron.toml, the source layout, build, run and test, vendoring third-party code, and the project commands.",
        "llms": "/llms/guide.md",
    },
    {
        "source": "raylib.md", "path": "raylib",
        "title": "Raylib",
        "description": "Graphics, input, audio and games in Iron with the bundled raylib binding: the frame loop, drawing, textures, text, sound and the examples.",
        "llms": "/llms/raylib.md",
    },
]

# Relative links between repository docs resolve to the site page when
# one exists, otherwise to the file on GitHub.
SITE_PAGES = {page["source"]: f"/{page['path']}/" for page in PAGES}

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
    def __init__(self, source: str, page: dict | None = None) -> None:
        self.source = source
        self.page = page or PAGES[0]
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
        body = re.sub(r'href="(?!https?://|/|#|mailto:)([A-Za-z0-9_./-]+)(#[^"]*)?"', self.doc_link, body)
        return body, self.sidebar()

    @staticmethod
    def doc_link(match: re.Match[str]) -> str:
        """Relative links from a docs/ file: the site page when the target
        is a manual with one, otherwise the file or directory on GitHub."""
        target, fragment = match.group(1), match.group(2) or ""
        parts: list[str] = ["docs"]
        for piece in target.split("/"):
            if piece == "..":
                if len(parts) > 0:
                    parts.pop()
            elif piece and piece != ".":
                parts.append(piece)
        repo_path = "/".join(parts)
        if repo_path.startswith("docs/") and repo_path[len("docs/"):] in SITE_PAGES:
            return f'href="{SITE_PAGES[repo_path[len("docs/"):]]}{fragment}"'
        kind = "tree" if target.endswith("/") or "." not in parts[-1] else "blob"
        return f'href="{GITHUB_REPO}/{kind}/main/{repo_path}{fragment}"'

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


def build_page(site: Path, page: dict) -> Path:
    source_path = REPO / "docs" / page["source"]
    source = source_path.read_text(encoding="utf-8")
    renderer = Renderer(source, page)
    content, sidebar = renderer.render()
    template = TEMPLATE.read_text(encoding="utf-8")
    callout = (
        f'This page is generated from <a href="{GITHUB_DOCS}{page["source"]}">docs/{page["source"]}</a> '
        f'on every deploy, and every Iron example in it is compiled and run in CI. A plain Markdown copy '
        f'for tools and language models is at <a href="{page["llms"]}">{page["llms"]}</a>; see '
        f'<a href="/llms.txt">/llms.txt</a> for the full index.'
    )
    out_html = (template
                .replace("{{TITLE}}", html.escape(page["title"]))
                .replace("{{DESCRIPTION}}", html.escape(page["description"], quote=True))
                .replace("{{LLMS}}", page["llms"])
                .replace("{{PATH}}", f"/{page['path']}/")
                .replace("{{CALLOUT}}", callout)
                .replace("{{SIDEBAR}}", sidebar)
                .replace("{{CONTENT}}", content))
    # The current page's nav entry is marked.
    out_html = out_html.replace(f'<a href="/{page["path"]}/">', f'<a href="/{page["path"]}/" class="active">', 1)
    out = site / page["path"] / "index.html"
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(out_html, encoding="utf-8")
    return out


def build(site: Path) -> list[tuple[dict, Path]]:
    return [(page, build_page(site, page)) for page in PAGES]


def check(page: dict, out: Path) -> list[str]:
    """Every in-page link must resolve and the source's own TOC must survive."""
    rendered = out.read_text(encoding="utf-8")
    ids = set(re.findall(r' id="([^"]+)"', rendered))
    errors = []
    for target in re.findall(r'href="#([^"]+)"', rendered):
        if target not in ids:
            errors.append(f"{out}: broken fragment #{target}")
    source_path = REPO / "docs" / page["source"]
    for target in re.findall(r"\]\(#([^)]+)\)", source_path.read_text(encoding="utf-8")):
        if target not in ids:
            errors.append(f"{source_path}: anchor #{target} not produced by the renderer")
    if "@@FENCE" in rendered or "{{" in rendered:
        errors.append(f"{out}: unrendered placeholder")
    featured = page.get("featured")
    if featured and f'data-example="{featured}"' not in rendered:
        errors.append(f"{out}: featured {featured} example missing")
    return errors


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--site", type=Path, default=REPO / "docs" / "site")
    parser.add_argument("--check", action="store_true",
                        help="render into a temporary directory and validate")
    args = parser.parse_args()
    errors: list[str] = []
    if args.check:
        with tempfile.TemporaryDirectory(prefix="iron-reference-") as tmp:
            for page, out in build(Path(tmp)):
                errors += check(page, out)
    else:
        for page, out in build(args.site):
            errors += check(page, out)
            print(f"wrote {out}")
    for error in errors:
        print(error, file=sys.stderr)
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
