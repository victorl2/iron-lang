#!/usr/bin/env python3
"""Generate the LLM-facing files for ironlang.dev (https://llmstxt.org).

Writes into the site directory (default: docs/site):

  llms.txt               index: summary, key rules, and links to the pages below
  llms-full.txt          one file with the reference manual, the guide, the
                         networking guide, stdlib API, and runnable examples,
                         for tools that load everything
  llms/reference.md      the full reference manual (docs/language_definition.md)
  llms/iron.md           language guide for LLMs (docs/llms/iron.md)
  llms/networking.md     networking guide (docs/networking.md)
  llms/examples.md       runnable examples (docs/examples/*.iron)
  llms/stdlib/<mod>.md   one page per stdlib module (src/stdlib/<mod>.iron)

Everything is generated from files in the repository, so the output cannot
drift from the sources. The pages workflow runs this before deploying; the
docs-check workflow runs it with --check.

Usage:
  scripts/build_llms.py [--site DIR]
  scripts/build_llms.py --check     # build into a temp dir, verify every link
"""

from __future__ import annotations

import argparse
import re
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
SITE_URL = "https://ironlang.dev"

GUIDE = REPO / "docs" / "llms" / "iron.md"
MANUAL = REPO / "docs" / "language_definition.md"
NETWORKING = REPO / "docs" / "networking.md"
EXAMPLES_DIR = REPO / "docs" / "examples"
STDLIB_DIR = REPO / "src" / "stdlib"

# Modules users import by name, in the order they appear in llms.txt.
PRIMARY_MODULES = {
    "io": "Files, directories, and paths: read, write, copy, move, list, file info",
    "math": "Math functions and constants on the Math object",
    "time": "Clock, sleep, timers, durations",
    "log": "Leveled logging",
    "net": "TCP, UDP, and DNS",
    "http": "HTTP/HTTPS clients and servers, REST, static files",
    "websocket": "WebSocket and secure WebSocket clients and servers",
    "url": "URL parsing, building, resolving, and percent-encoding",
}
# Built-in types and runtime surfaces that need no import.
CORE_MODULES = {
    "string": "String methods",
    "list": "List methods (map, filter, reduce, sum)",
    "int": "Integer to_string",
    "float": "Float to_string",
    "box": "Box[T]: checked single-owner heap box",
    "arena": "Arena allocation",
    "channel": "Channels for message passing between tasks",
    "mutex": "Mutex",
    "rwlock": "Read-write lock",
    "filehandle": "Low-level file handles",
    "rawptr": "Raw pointers for FFI",
    "hashable": "Hashable interface for map keys",
    "hint": "Hint.black_box to keep values from being optimized away",
    "map": "Map[K, V] (declared; not usable yet)",
    "set": "Set[T] (declared; not usable yet)",
}
# Large or specialised modules: listed under "Optional" and left out of
# llms-full.txt so it stays focused on the language.
OPTIONAL_MODULES = {
    "raylib": "Raylib bindings for graphics, input, and audio (large)",
}

SUMMARY = (
    "Iron is a general-purpose, statically typed native language that compiles "
    "to C. It favors explicit control and readable code: no garbage collector, "
    "no operator overloading, no implicit conversions, and no package manager."
)

KEY_RULES = """\
Key rules for writing Iron (the language is in alpha; older examples online
may use removed syntax):

- Comments start with `--`. Blocks use braces; no semicolons.
- `val` is immutable, `var` is mutable. A binding must be `var` to be
  reassigned, to have its fields written, or to call a mutating method.
- String interpolation: `"{name} has {hp} HP"`. Print with `println`.
- Methods are declared inside `object` blocks and use `self.`; constructors
  are `init` blocks. `match` arms use `->`.
- Multiple results use tuples: `func f() -> (Int, Int)` and `val (a, b) = f()`.
- Stdlib calls go through the capitalized module object after importing the
  lowercase module: `import io` then `IO.read_file(path)`.
- No package manager: copy third-party Iron code into `vendor/`; `iron build`
  compiles it with the project.
"""


def module_path(name: str) -> Path:
    return STDLIB_DIR / f"{name}.iron"


def stdlib_page(name: str) -> str:
    src = module_path(name).read_text()
    header = f"# Iron stdlib: {name}\n\n"
    if name in PRIMARY_MODULES:
        header += f"Import with `import {name}`. "
    header += (
        "These are the declarations the compiler uses: bodies are empty "
        "because the implementation is in the C runtime.\n\n"
    )
    return header + "```iron\n" + src.rstrip() + "\n```\n"


def examples_page() -> str:
    parts = [
        "# Iron runnable examples\n",
        "Each example compiles with the current compiler and runs with "
        "`iron run <file>`.\n",
    ]
    for path in sorted(EXAMPLES_DIR.glob("*.iron")):
        parts.append(f"\n## {path.name}\n\n```iron\n{path.read_text().rstrip()}\n```\n")
    return "".join(parts)


def link(path: str, title: str, desc: str) -> str:
    return f"- [{title}]({SITE_URL}/{path}): {desc}"


def llms_txt() -> str:
    lines = [
        "# Iron",
        "",
        f"> {SUMMARY}",
        "",
        KEY_RULES.rstrip(),
        "",
        "## Docs",
        "",
        link("llms/reference.md", "Iron reference manual",
             "the complete language definition: lexical rules, types, "
             "expressions, statements, declarations, memory, concurrency, "
             "comptime, the standard library and every diagnostic code; "
             "every example is compiled in CI (HTML version at /docs/)"),
        link("llms/iron.md", "Iron language guide for LLMs",
             "syntax, objects, enums, memory, concurrency, stdlib usage, and "
             "known gaps; every example is compiled in CI"),
        link("llms/networking.md", "Networking guide",
             "TCP, UDP, DNS, HTTP/HTTPS, REST servers, WebSocket"),
        link("llms/examples.md", "Runnable examples", "small complete programs"),
        link("guide/", "Project guide",
             "iron init/build/run/check/test, iron.toml, vendoring"),
        "",
        "## Standard library",
        "",
    ]
    for name, desc in PRIMARY_MODULES.items():
        lines.append(link(f"llms/stdlib/{name}.md", name, desc))
    for name, desc in CORE_MODULES.items():
        lines.append(link(f"llms/stdlib/{name}.md", name, desc))
    lines += ["", "## Optional", ""]
    lines.append(link("llms-full.txt", "llms-full.txt",
                      "the reference manual, guide, networking guide, stdlib "
                      "declarations, and examples in one file"))
    for name, desc in OPTIONAL_MODULES.items():
        lines.append(link(f"llms/stdlib/{name}.md", name, desc))
    lines.append(link("raylib/", "Raylib guide and reference", "HTML docs"))
    return "\n".join(lines) + "\n"


def llms_full_txt(pages: dict[str, str]) -> str:
    order = ["llms/reference.md", "llms/iron.md", "llms/networking.md"]
    order += [f"llms/stdlib/{n}.md" for n in (*PRIMARY_MODULES, *CORE_MODULES)]
    order += ["llms/examples.md"]
    parts = [f"# Iron\n\n> {SUMMARY}\n\n{KEY_RULES}"]
    for key in order:
        parts.append(f"\n\n<!-- source: {SITE_URL}/{key} -->\n\n{pages[key].rstrip()}\n")
    return "".join(parts)


def build(site: Path) -> list[str]:
    pages: dict[str, str] = {
        "llms/reference.md": MANUAL.read_text(),
        "llms/iron.md": GUIDE.read_text(),
        "llms/networking.md": NETWORKING.read_text(),
        "llms/examples.md": examples_page(),
    }
    for name in (*PRIMARY_MODULES, *CORE_MODULES, *OPTIONAL_MODULES):
        pages[f"llms/stdlib/{name}.md"] = stdlib_page(name)

    written = []
    for rel, text in pages.items():
        out = site / rel
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_text(text)
        written.append(rel)
    (site / "llms.txt").write_text(llms_txt())
    (site / "llms-full.txt").write_text(llms_full_txt(pages))
    written += ["llms.txt", "llms-full.txt"]
    return written


def check_links(site: Path) -> list[str]:
    """Every ironlang.dev link in llms.txt must resolve to a built file or an
    existing site page."""
    errors = []
    for url in re.findall(r"\]\((https://ironlang\.dev/[^)]*)\)", (site / "llms.txt").read_text()):
        rel = url[len(SITE_URL) + 1:]
        target = site / rel
        if rel.endswith("/"):
            target = target / "index.html"
        if not target.exists() and not (REPO / "docs" / "site" / rel / "index.html").exists():
            errors.append(f"broken link in llms.txt: {url}")
    return errors


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--site", type=Path, default=REPO / "docs" / "site")
    parser.add_argument("--check", action="store_true",
                        help="build into a temporary directory and verify links")
    args = parser.parse_args()

    missing = [p for p in (MANUAL, GUIDE, NETWORKING, EXAMPLES_DIR)
               if not p.exists()]
    missing += [module_path(n) for n in (*PRIMARY_MODULES, *CORE_MODULES, *OPTIONAL_MODULES)
                if not module_path(n).exists()]
    if missing:
        for p in missing:
            print(f"error: missing source {p.relative_to(REPO)}", file=sys.stderr)
        return 1

    if args.check:
        with tempfile.TemporaryDirectory() as tmp:
            site = Path(tmp)
            written = build(site)
            errors = check_links(site)
            size = (site / "llms-full.txt").stat().st_size
    else:
        written = build(args.site)
        errors = check_links(args.site)
        size = (args.site / "llms-full.txt").stat().st_size

    for e in errors:
        print(f"error: {e}", file=sys.stderr)
    if errors:
        return 1
    print(f"llms: wrote {len(written)} files (llms-full.txt {size // 1024} KiB)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
