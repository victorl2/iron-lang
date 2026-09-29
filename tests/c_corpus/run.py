#!/usr/bin/env python3
"""Build, run, and check the C reference corpus.

Every program under tests/c_corpus/<topic>/<slug>.c is compiled on its own,
run with no arguments and no stdin inside a fresh scratch directory, and its
stdout is compared byte for byte against <slug>.expected. A program passes
when it exits 0, prints nothing to stderr, and matches its expected output.

Usage:
  run.py                         build and run everything
  run.py algorithms networking   only these topics (or paths to .c files)
  run.py -k heap                 only programs whose slug contains "heap"
  run.py --sanitize              build with ASan + UBSan
  run.py --update                (re)write .expected files from actual output
  run.py --lint                  check headers and slug uniqueness only
  run.py --manifest              regenerate MANIFEST.md
"""

import argparse
import concurrent.futures
import os
import platform
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.abspath(__file__))
TOPICS = [
    "algorithms",
    "data_structures",
    "memory",
    "concurrency",
    "io_files",
    "networking",
    "unix",
    "statistics",
    "other",
]
DEPS = {"libc", "libm", "pthread", "posix", "sockets"}
HEADER_KEYS = ("title", "topic", "covers", "deps")
BASE_FLAGS = [
    "-std=gnu11",
    "-O1",
    "-Wall",
    "-Wextra",
    "-Wpedantic",
    "-Werror",
    "-Wno-unused-parameter",
    "-pthread",
]
SAN_FLAGS = [
    "-fsanitize=address,undefined",
    "-fno-sanitize-recover=undefined",
    "-fno-omit-frame-pointer",
]
SLUG_RE = re.compile(r"^[a-z][a-z0-9_]*$")


def discover(selectors, keyword):
    progs = []
    for topic in TOPICS:
        d = os.path.join(ROOT, topic)
        if not os.path.isdir(d):
            continue
        for name in sorted(os.listdir(d)):
            if name.endswith(".c"):
                progs.append(os.path.join(d, name))
    if selectors:
        wanted = []
        for p in progs:
            topic = os.path.basename(os.path.dirname(p))
            for s in selectors:
                if s == topic or os.path.abspath(s) == p:
                    wanted.append(p)
                    break
        progs = wanted
    if keyword:
        progs = [p for p in progs if keyword in os.path.basename(p)]
    return progs


def parse_header(path):
    with open(path, encoding="utf-8") as f:
        text = f.read(4096)
    m = re.match(r"\s*/\*(.*?)\*/", text, re.S)
    if not m:
        return None
    fields = {}
    for line in m.group(1).splitlines():
        line = line.strip().lstrip("*").strip()
        km = re.match(r"^(title|topic|covers|deps):\s*(.*)$", line)
        if km:
            fields[km.group(1)] = km.group(2).strip()
    return fields


def lint(progs):
    errors = []
    seen = {}
    for p in progs:
        topic = os.path.basename(os.path.dirname(p))
        slug = os.path.basename(p)[:-2]
        rel = os.path.relpath(p, ROOT)
        if not SLUG_RE.match(slug) or slug.startswith("test_"):
            errors.append(f"{rel}: bad slug (lowercase snake case, not test_*)")
        if slug in seen:
            errors.append(f"{rel}: slug also used by {seen[slug]}")
        seen[slug] = rel
        h = parse_header(p)
        if h is None:
            errors.append(f"{rel}: missing leading header comment")
            continue
        for k in HEADER_KEYS:
            if not h.get(k):
                errors.append(f"{rel}: header missing '{k}'")
        if h.get("topic") and h["topic"] != topic:
            errors.append(f"{rel}: header topic '{h['topic']}' != dir '{topic}'")
        for d in (x.strip() for x in h.get("deps", "").split(",")):
            if d and d not in DEPS:
                errors.append(f"{rel}: unknown dep '{d}' (allowed: {sorted(DEPS)})")
        if not os.path.exists(p[:-2] + ".expected"):
            errors.append(f"{rel}: missing .expected")
    return errors


def compiler():
    return os.environ.get("CC", "cc")


def build_and_run(path, build_dir, sanitize, timeout, update):
    slug = os.path.basename(path)[:-2]
    exe = os.path.join(build_dir, slug)
    cmd = [compiler()] + BASE_FLAGS + (SAN_FLAGS if sanitize else []) + [path, "-o", exe, "-lm"]
    if platform.system() == "Linux":
        cmd.append("-lrt")
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        return path, "compile", r.stderr[-4000:]
    scratch = tempfile.mkdtemp(prefix=slug + ".", dir=build_dir)
    env = dict(os.environ, TMPDIR=scratch, LC_ALL="C")
    if sanitize:
        env.setdefault("ASAN_OPTIONS", "abort_on_error=0:halt_on_error=1")
        env.setdefault("UBSAN_OPTIONS", "halt_on_error=1:print_stacktrace=1")
    try:
        r = subprocess.run(
            [exe], cwd=scratch, env=env, stdin=subprocess.DEVNULL,
            capture_output=True, timeout=timeout,
        )
    except subprocess.TimeoutExpired:
        return path, "timeout", f"exceeded {timeout}s"
    finally:
        shutil.rmtree(scratch, ignore_errors=True)
    out = r.stdout
    if r.returncode != 0:
        return path, "exit", f"exit {r.returncode}\n" + r.stderr.decode(errors="replace")[-4000:]
    stderr = r.stderr.decode(errors="replace")
    if sanitize:
        # macOS ASan can emit allocator notices that are not findings.
        stderr = "\n".join(l for l in stderr.splitlines() if "malloc: nano zone" not in l).strip()
    if stderr:
        return path, "stderr", stderr[-4000:]
    if not out:
        return path, "empty", "program printed nothing"
    exp_path = path[:-2] + ".expected"
    if update:
        with open(exp_path, "wb") as f:
            f.write(out)
        return path, "ok", ""
    if not os.path.exists(exp_path):
        return path, "missing", "no .expected file (run with --update)"
    with open(exp_path, "rb") as f:
        exp = f.read()
    if exp != out:
        return path, "diff", diff_text(exp, out)
    return path, "ok", ""


def diff_text(exp, out):
    import difflib
    lines = difflib.unified_diff(
        exp.decode(errors="replace").splitlines(),
        out.decode(errors="replace").splitlines(),
        "expected", "actual", lineterm="", n=2,
    )
    return "\n".join(list(lines)[:60])


def write_manifest(progs):
    by_topic = {t: [] for t in TOPICS}
    for p in progs:
        topic = os.path.basename(os.path.dirname(p))
        h = parse_header(p) or {}
        by_topic[topic].append((os.path.basename(p)[:-2], h))
    lines = [
        "# C reference corpus manifest",
        "",
        "Generated by `python3 tests/c_corpus/run.py --manifest`. Do not edit by hand.",
        "",
        f"Total programs: {len(progs)}",
        "",
        "| Topic | Programs |",
        "|---|---|",
    ]
    for t in TOPICS:
        lines.append(f"| [{t}](#{t}) | {len(by_topic[t])} |")
    for t in TOPICS:
        lines += ["", f"## {t}", "", "| Program | Title | Covers | Deps |", "|---|---|---|---|"]
        for slug, h in by_topic[t]:
            cell = lambda s: (s or "").replace("|", "\\|")
            lines.append(
                f"| [`{slug}`]({t}/{slug}.c) | {cell(h.get('title'))} | "
                f"{cell(h.get('covers'))} | {cell(h.get('deps'))} |"
            )
    with open(os.path.join(ROOT, "MANIFEST.md"), "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("select", nargs="*", help="topics or .c paths")
    ap.add_argument("-k", dest="keyword", help="substring filter on slug")
    ap.add_argument("-j", "--jobs", type=int, default=4)
    ap.add_argument("--sanitize", action="store_true", help="build with ASan + UBSan")
    ap.add_argument("--update", action="store_true", help="write .expected from actual output")
    ap.add_argument("--lint", action="store_true", help="only check headers and slugs")
    ap.add_argument("--manifest", action="store_true", help="regenerate MANIFEST.md")
    ap.add_argument("--timeout", type=float, default=30.0)
    ap.add_argument("--build-dir", help="where binaries go (default: a temp dir)")
    args = ap.parse_args()

    progs = discover(args.select, args.keyword)
    if args.manifest:
        write_manifest(discover([], None))
        print("wrote MANIFEST.md")
        return 0
    errors = lint(discover([], None) if not args.select and not args.keyword else progs)
    if args.update:
        errors = [e for e in errors if "missing .expected" not in e]
    if errors:
        print("\n".join(errors))
        print(f"lint: {len(errors)} problem(s)")
        if args.lint:
            return 1
    elif args.lint:
        print(f"lint: {len(progs)} programs OK")
        return 0
    if not progs:
        print("no programs selected")
        return 1

    build_dir = args.build_dir or tempfile.mkdtemp(prefix="c_corpus.")
    os.makedirs(build_dir, exist_ok=True)
    failures = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as ex:
        futs = [ex.submit(build_and_run, p, build_dir, args.sanitize, args.timeout, args.update) for p in progs]
        for fut in concurrent.futures.as_completed(futs):
            path, status, detail = fut.result()
            if status != "ok":
                failures.append((path, status, detail))
    if not args.build_dir:
        shutil.rmtree(build_dir, ignore_errors=True)
    for path, status, detail in sorted(failures):
        print(f"FAIL [{status}] {os.path.relpath(path, ROOT)}")
        if detail:
            print("    " + detail.replace("\n", "\n    "))
    print(f"{len(progs) - len(failures)}/{len(progs)} passed")
    return 1 if failures or errors else 0


if __name__ == "__main__":
    sys.exit(main())
