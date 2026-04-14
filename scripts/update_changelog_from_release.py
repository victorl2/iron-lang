#!/usr/bin/env python3
"""Sync CHANGELOG.md and CMakeLists.txt with a GitHub release.

Invoked by .github/workflows/changelog-on-release.yml on every
`release: published` event, and usable locally for manual re-syncs.

Given a release tag (e.g. v1.0.0-alpha):

  1. Fetch the release name, body, and publish date via `gh release view`.
  2. If CHANGELOG.md does not already contain a `## <name> (<date>)`
     entry for this tag, prepend one using the same formatting rule
     that built the initial CHANGELOG (see docstring of `build_entry`).
  3. If CMakeLists.txt's `project(iron VERSION X.Y.Z ...)` or
     `set(IRON_VERSION_FULL "X.Y.Z[-suffix]")` do not match the tag,
     rewrite them in place.

Exit codes:
  0  something changed on disk (caller should commit+push)
  2  everything already up to date, no changes made
  1  usage or fetch error

Usage:
  scripts/update_changelog_from_release.py --tag v1.0.0-alpha
  scripts/update_changelog_from_release.py --tag v1.0.0-alpha --repo victorl2/iron-lang
  scripts/update_changelog_from_release.py --tag v1.0.0-alpha --dry-run
"""
from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
CHANGELOG_PATH = REPO_ROOT / "CHANGELOG.md"
CMAKE_PATH = REPO_ROOT / "CMakeLists.txt"

TAG_RE = re.compile(r"^v?(\d+\.\d+\.\d+)(-[A-Za-z0-9.+-]+)?$")

# Canonical release-tag scheme per docs/versioning.md (VER-01). The public
# scheme wins: MAJOR starts at 1, three numeric segments, optional -alpha
# suffix (dropped once a stable v1.x.y ships). This is stricter than TAG_RE
# above — TAG_RE still parses legacy internal tags for backwards compat in
# the parse_tag helper; _validate_tag_format below is the enforcement gate.
_CANONICAL_TAG_REGEX = re.compile(r"^v[1-9][0-9]*\.[0-9]+\.[0-9]+(-alpha)?$")

# Matches: project(iron VERSION X.Y.Z[.W] LANGUAGES C)
CMAKE_PROJECT_RE = re.compile(
    r"^(project\(\s*iron\s+VERSION\s+)(\d+\.\d+\.\d+(?:\.\d+)?)(\b[^)]*\))",
    re.MULTILINE,
)
# Matches: set(IRON_VERSION_FULL "X.Y.Z-anything")
CMAKE_FULL_RE = re.compile(
    r'^(set\(\s*IRON_VERSION_FULL\s+")([^"]*)("\s*\))',
    re.MULTILINE,
)


def parse_tag(tag: str) -> tuple[str, str]:
    """Split a release tag into (cmake_numeric, display_full).

    >>> parse_tag("v1.0.0-alpha")
    ('1.0.0', '1.0.0-alpha')
    >>> parse_tag("v0.0.8-alpha")
    ('0.0.8', '0.0.8-alpha')
    >>> parse_tag("1.2.3")
    ('1.2.3', '1.2.3')
    """
    m = TAG_RE.match(tag.strip())
    if not m:
        raise SystemExit(f"error: tag {tag!r} does not match vMAJOR.MINOR.PATCH[-suffix]")
    numeric = m.group(1)
    suffix = m.group(2) or ""
    return numeric, numeric + suffix


def _validate_tag_format(tag: str) -> None:
    """Enforce the canonical release-tag scheme from docs/versioning.md.

    Called as the first action in main() before parse_tag() and before
    fetch_release(), so the rejection path works offline and without a
    configured gh CLI. Raises SystemExit(1) on mismatch; returns None on
    success.

    >>> _validate_tag_format("v1.4.0-alpha")
    >>> _validate_tag_format("v2.0.0")
    """
    if _CANONICAL_TAG_REGEX.match(tag.strip()) is None:
        raise SystemExit(
            f"ERROR: release tag {tag!r} does not match canonical scheme "
            f"'{_CANONICAL_TAG_REGEX.pattern}'. "
            f"See docs/versioning.md for the canonical version scheme. "
            f"Reject this release or delete the tag and re-tag with the correct format."
        )


def fetch_release(tag: str, repo: str | None) -> dict:
    cmd = ["gh", "release", "view", tag, "--json", "name,tagName,publishedAt,body"]
    if repo:
        cmd.extend(["--repo", repo])
    try:
        out = subprocess.run(cmd, check=True, capture_output=True, text=True)
    except FileNotFoundError:
        raise SystemExit("error: gh CLI not found on PATH")
    except subprocess.CalledProcessError as e:
        raise SystemExit(f"error: gh release view {tag} failed:\n{e.stderr}")
    return json.loads(out.stdout)


# ── CHANGELOG entry builder ──────────────────────────────────────────────
# Must stay in lock-step with the bulk builder used to rewrite CHANGELOG.md
# initially (see PR #16). Any change to the formatting rule here MUST be
# mirrored in the bulk builder, otherwise re-syncing the whole file would
# produce a diff.
#
# Rule:
#   * Version header: "## <release.name> (<YYYY-MM-DD>)"
#   * If the body's first non-blank line is a "## " heading containing
#     the release tag, drop it and leave remaining headings at their
#     original depth (the stripped line implicitly played the role of
#     the version header, so descendants are already correctly nested).
#   * Otherwise, demote every markdown heading by one level so body
#     content ("##", "###", ...) nests cleanly under the prepended "##".


def normalize_body(body: str) -> str:
    body = body.replace("\r\n", "\n").replace("\r", "\n")
    body = "\n".join(line.rstrip() for line in body.split("\n"))
    body = re.sub(r"\n{3,}", "\n\n", body)
    return body.strip()


def strip_redundant_tag_heading(body: str, tag: str) -> tuple[str, bool]:
    lines = body.split("\n")
    idx = 0
    while idx < len(lines) and lines[idx].strip() == "":
        idx += 1
    if idx < len(lines):
        first = lines[idx]
        if first.startswith("## ") and tag in first:
            del lines[idx]
            if idx < len(lines) and lines[idx].strip() == "":
                del lines[idx]
            return "\n".join(lines), True
    return body, False


def demote_headings(body: str) -> str:
    return re.sub(r"^(#+)", r"#\1", body, flags=re.MULTILINE)


def build_entry(release: dict) -> str:
    name = release["name"].strip()
    tag = release["tagName"].strip()
    date = release["publishedAt"][:10]
    body = normalize_body(release["body"])
    body, stripped = strip_redundant_tag_heading(body, tag)
    if not stripped:
        body = demote_headings(body)
    return f"## {name} ({date})\n\n{body}\n"


# ── CHANGELOG.md update ──────────────────────────────────────────────────

def changelog_has_entry_for(text: str, tag: str) -> bool:
    """True if CHANGELOG.md already has a '## ... <tag> ...' heading."""
    for line in text.split("\n"):
        if line.startswith("## ") and tag in line:
            return True
    return False


def prepend_entry(current: str, entry: str) -> str:
    """Insert `entry` after the preamble and before the first existing '## '."""
    lines = current.split("\n")
    # Find the first line that starts with '## ' (existing release entry).
    insert_at = None
    for i, line in enumerate(lines):
        if line.startswith("## "):
            insert_at = i
            break
    if insert_at is None:
        # No prior entries — append after the whole preamble.
        return current.rstrip() + "\n\n" + entry.rstrip() + "\n"
    before = "\n".join(lines[:insert_at]).rstrip()
    after = "\n".join(lines[insert_at:])
    return before + "\n\n" + entry.rstrip() + "\n\n" + after


# ── CMakeLists.txt update ───────────────────────────────────────────────

def update_cmake(text: str, numeric: str, display_full: str) -> tuple[str, bool]:
    """Rewrite project(VERSION) and IRON_VERSION_FULL. Returns (new_text, changed)."""
    changed = False

    def replace_project(m: re.Match[str]) -> str:
        nonlocal changed
        if m.group(2) != numeric:
            changed = True
            return m.group(1) + numeric + m.group(3)
        return m.group(0)

    def replace_full(m: re.Match[str]) -> str:
        nonlocal changed
        if m.group(2) != display_full:
            changed = True
            return m.group(1) + display_full + m.group(3)
        return m.group(0)

    if not CMAKE_PROJECT_RE.search(text):
        raise SystemExit("error: could not find 'project(iron VERSION ...)' in CMakeLists.txt")
    if not CMAKE_FULL_RE.search(text):
        raise SystemExit("error: could not find 'set(IRON_VERSION_FULL ...)' in CMakeLists.txt")

    text = CMAKE_PROJECT_RE.sub(replace_project, text, count=1)
    text = CMAKE_FULL_RE.sub(replace_full, text, count=1)
    return text, changed


# ── Main ────────────────────────────────────────────────────────────────

def main() -> int:
    p = argparse.ArgumentParser()
    p.add_argument("--tag", required=True, help="Release tag, e.g. v1.0.0-alpha")
    p.add_argument("--repo", default=None, help="owner/name (default: current repo)")
    p.add_argument("--dry-run", action="store_true", help="Print diff instead of writing")
    args = p.parse_args()

    _validate_tag_format(args.tag)
    numeric, display_full = parse_tag(args.tag)
    release = fetch_release(args.tag, args.repo)

    # --- CHANGELOG ---
    changelog_text = CHANGELOG_PATH.read_text()
    changelog_changed = False
    if changelog_has_entry_for(changelog_text, args.tag):
        print(f"CHANGELOG.md already has entry for {args.tag}, skipping.")
    else:
        entry = build_entry(release)
        new_changelog = prepend_entry(changelog_text, entry)
        changelog_changed = new_changelog != changelog_text
        if changelog_changed:
            if args.dry_run:
                print(f"--- CHANGELOG.md would gain a new entry for {args.tag} ---")
            else:
                CHANGELOG_PATH.write_text(new_changelog)
                print(f"CHANGELOG.md: prepended entry for {args.tag}")

    # --- CMakeLists.txt ---
    cmake_text = CMAKE_PATH.read_text()
    new_cmake, cmake_changed = update_cmake(cmake_text, numeric, display_full)
    if cmake_changed:
        if args.dry_run:
            print(f"--- CMakeLists.txt would update to VERSION {numeric} / FULL {display_full} ---")
        else:
            CMAKE_PATH.write_text(new_cmake)
            print(f"CMakeLists.txt: updated VERSION={numeric} IRON_VERSION_FULL={display_full}")
    else:
        print(f"CMakeLists.txt already at VERSION {numeric} / FULL {display_full}, skipping.")

    # Expose change flags to the workflow via $GITHUB_OUTPUT when available,
    # falling back to stderr for local runs. The workflow reads these to
    # decide whether to commit and whether to re-trigger the release build
    # (CMake change → stale binaries → rebuild needed).
    outputs = {
        "changelog_changed": "true" if changelog_changed else "false",
        "cmake_changed": "true" if cmake_changed else "false",
        "any_changed": "true" if (changelog_changed or cmake_changed) else "false",
    }
    gh_output = os.environ.get("GITHUB_OUTPUT")
    if gh_output:
        with open(gh_output, "a") as fh:
            for k, v in outputs.items():
                fh.write(f"{k}={v}\n")
    else:
        for k, v in outputs.items():
            print(f"[output] {k}={v}", file=sys.stderr)

    return 0 if (changelog_changed or cmake_changed) else 2


if __name__ == "__main__":
    sys.exit(main())
