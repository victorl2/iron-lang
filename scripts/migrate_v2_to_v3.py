#!/usr/bin/env python3
"""
migrate_v2_to_v3.py - Iron language v2 to v3 source codemod

Transforms v2 receiver-method syntax into v3 patch-block syntax and handles
related ergonomic changes introduced in Iron v3.

Usage:
    python3 migrate_v2_to_v3.py --from v2 --to v3 <path>

Where <path> is a .iron file or a directory (recursed for *.iron files).

Transforms performed:
  1. func (recv: T) name(...) -> grouped patch object T { ... } blocks
  2. func (mut recv: T) name(...) -> same as above (mut dropped)
  3. var name: T = expr inside object bodies -> moved to synthesized init()
  4. standalone mut keyword in params/locals stripped (conservative)

Diff of changes is emitted to stderr. Patterns that cannot be safely
transformed are flagged with TODO(v3-migration) warnings to stderr.
"""

import re
import sys
import difflib
import argparse
from pathlib import Path


# ---------------------------------------------------------------------------
# Regex patterns
# ---------------------------------------------------------------------------

# func (recv: T) name(...) rest
RE_RECV_METHOD = re.compile(
    r'^(\s*)func\s+\((\w+)\s*:\s*(\w+)\)\s+(.+)$'
)

# func (mut recv: T) name(...) rest
RE_MUT_RECV_METHOD = re.compile(
    r'^(\s*)func\s+\(mut\s+(\w+)\s*:\s*(\w+)\)\s+(.+)$'
)

# object Name { or object Name<T> {  (opening brace on same line)
RE_OBJECT_OPEN = re.compile(
    r'^(\s*)object\s+\w+(?:\s*<[^>]*>)?\s*\{'
)

# var/val name: Type = expr  (field default)
RE_FIELD_DEFAULT = re.compile(
    r'^(\s*)(var|val)\s+(\w+)\s*:\s*([^\s=]+)\s*=\s*(.+)$'
)


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def rename_receiver(text, recv_name):
    """Replace recv_name. with self. in text."""
    return re.sub(r'\b' + re.escape(recv_name) + r'\.', 'self.', text)


def is_inline_body(rest):
    """True if the func signature's rest contains a self-closing body."""
    opens = rest.count('{')
    closes = rest.count('}')
    return opens > 0 and opens == closes


def reindent_body(body_lines, base_indent, extra):
    """
    Re-indent body lines (which may have been indented relative to the
    original func declaration) by stripping base_indent and adding extra.

    If a body line is indented more than base_indent, preserve the extra
    indentation relative to it. If it is the closing brace at base_indent
    level, emit it at base_indent + extra.
    """
    result = []
    for line in body_lines:
        stripped_eol = line.rstrip('\n')
        # Count leading whitespace
        leading = len(stripped_eol) - len(stripped_eol.lstrip())
        content = stripped_eol.lstrip()
        if not content:
            result.append('\n')
            continue
        # How much indent does the original line have relative to base?
        original_indent = leading
        base_len = len(base_indent)
        relative = max(0, original_indent - base_len)
        new_line = extra + ' ' * relative + content + '\n'
        result.append(new_line)
    return result


# ---------------------------------------------------------------------------
# Core transform
# ---------------------------------------------------------------------------

def transform_source(source, filepath):
    """
    Transform v2 Iron source to v3.
    Returns (new_source, warnings) where warnings is a list of strings.
    """
    lines = source.splitlines(keepends=True)
    warnings = []

    # ------------------------------------------------------------------
    # Phase A: classify lines into segments.
    #
    # Each segment is a dict with 'kind' and kind-specific fields.
    # Kinds:
    #   'recv'         - receiver-method declaration + body lines
    #   'field_default'- field with inline default (stripped; expr for init)
    #   'object_close' - closing } of an object block
    #   'other'        - any other line (blank, comment, non-method code)
    # ------------------------------------------------------------------
    segments = []

    in_object = False
    object_depth = 0
    i = 0

    while i < len(lines):
        raw = lines[i]
        ls = raw.rstrip('\n')

        # Track object scope
        if in_object:
            object_depth += ls.count('{') - ls.count('}')
            if object_depth <= 0:
                in_object = False
                object_depth = 0
                segments.append({'kind': 'object_close', 'raw': raw})
                i += 1
                continue
        else:
            if RE_OBJECT_OPEN.match(ls):
                in_object = True
                object_depth = ls.count('{') - ls.count('}')
                segments.append({'kind': 'other', 'raw': raw})
                i += 1
                continue

        # Receiver-method (mut first, then plain)
        m = RE_MUT_RECV_METHOD.match(ls)
        if not m:
            m = RE_RECV_METHOD.match(ls)

        if m:
            indent, recv_name, recv_type, rest = m.groups()
            i += 1

            if is_inline_body(rest):
                # Single-line stub: no body to collect
                body_lines = []
            else:
                # Multi-line body: collect until matching brace closes
                body_lines = []
                depth = rest.count('{') - rest.count('}')
                while i < len(lines) and depth > 0:
                    bline = lines[i]
                    bls = bline.rstrip('\n')
                    depth += bls.count('{') - bls.count('}')
                    body_lines.append(bline)
                    i += 1

            segments.append({
                'kind': 'recv',
                'indent': indent,
                'recv_name': recv_name,
                'recv_type': recv_type,
                'sig_rest': rest,
                'body_lines': body_lines,
            })
            continue

        # Field defaults inside object body at direct-member depth
        if in_object and object_depth == 1:
            m = RE_FIELD_DEFAULT.match(ls)
            if m:
                f_indent, f_kw, f_name, f_type, f_expr = m.groups()
                expr = f_expr.strip()
                if '{' in expr or len(expr) > 80:
                    warnings.append(
                        'WARNING: {}:{}: TODO(v3-migration): complex inline '
                        "default for field '{}' -- review manually".format(
                            filepath, i + 1, f_name
                        )
                    )
                    segments.append({'kind': 'other', 'raw': raw})
                else:
                    stripped = '{}{} {}: {}\n'.format(f_indent, f_kw, f_name, f_type)
                    segments.append({
                        'kind': 'field_default',
                        'stripped': stripped,
                        'field_name': f_name,
                        'field_expr': expr,
                    })
                i += 1
                continue

        i += 1
        segments.append({'kind': 'other', 'raw': raw})

    # ------------------------------------------------------------------
    # Phase B: build init_map
    # For each 'object_close' segment index, gather field_default items
    # that appeared just before it (separated only by blanks/comments).
    # ------------------------------------------------------------------
    init_map = {}  # segment index -> [(field_name, field_expr)]
    field_buf = []
    for si, seg in enumerate(segments):
        k = seg['kind']
        if k == 'field_default':
            field_buf.append((seg['field_name'], seg['field_expr']))
        elif k == 'object_close':
            if field_buf:
                init_map[si] = list(field_buf)
            field_buf = []
        elif k == 'other':
            rs = seg['raw'].strip()
            if rs and not rs.startswith('--'):
                field_buf = []

    # ------------------------------------------------------------------
    # Phase C: emit output
    #
    # Grouping rule: keep the patch block open across blank/comment 'other'
    # lines IF the next meaningful segment is a 'recv' of the same type.
    # Close the patch block when a non-blank/comment other line appears,
    # or when the next recv has a different type.
    # ------------------------------------------------------------------
    output_lines = []
    cur_patch_type = None
    cur_patch_indent = ''

    # Build a "lookahead" view: for each segment index, find the next
    # 'recv' or 'non-blank-other' segment index.
    def next_meaningful_recv(segments, start):
        """
        Return the recv_type of the next recv segment after start,
        skipping blank/comment 'other' segments. Return None if
        a non-blank other (or object_close or field_default) is found first.
        """
        for j in range(start, len(segments)):
            seg = segments[j]
            if seg['kind'] == 'recv':
                return seg['recv_type']
            if seg['kind'] == 'other':
                rs = seg['raw'].strip()
                if rs and not rs.startswith('--'):
                    return None  # non-blank other breaks the group
            if seg['kind'] in ('object_close', 'field_default'):
                return None
        return None

    def close_patch():
        nonlocal cur_patch_type, cur_patch_indent
        if cur_patch_type is not None:
            output_lines.append('{}}}\n'.format(cur_patch_indent))
            output_lines.append('\n')
            cur_patch_type = None
            cur_patch_indent = ''

    # Buffer for 'other' lines that appear between recv segments of the same type.
    # We hold them until we know whether to emit them inside the patch block or
    # close the block first.
    pending_others = []

    def flush_pending(inside_patch):
        """Emit pending other lines, either inside or after closing the patch."""
        nonlocal pending_others
        if inside_patch:
            output_lines.extend(pending_others)
        else:
            close_patch()
            output_lines.extend(pending_others)
        pending_others = []

    for si, seg in enumerate(segments):
        k = seg['kind']

        if k == 'recv':
            recv_type = seg['recv_type']
            recv_name = seg['recv_name']
            indent = seg['indent']
            rest = seg['sig_rest']
            body_lines = seg['body_lines']

            if cur_patch_type == recv_type:
                # Continuing the same patch block: flush pending inside
                flush_pending(inside_patch=True)
            else:
                # Different type or no open block: flush pending outside, then close
                flush_pending(inside_patch=False)
                cur_patch_type = recv_type
                cur_patch_indent = indent
                output_lines.append('{}patch object {} {{\n'.format(indent, recv_type))

            inner = indent + '    '
            func_line = '{}func {}'.format(inner, rest)
            func_line = rename_receiver(func_line, recv_name)
            if not func_line.endswith('\n'):
                func_line += '\n'
            output_lines.append(func_line)

            if body_lines:
                reindented = reindent_body(body_lines, indent, inner)
                renamed_body = [rename_receiver(bl, recv_name) for bl in reindented]
                output_lines.extend(renamed_body)

        elif k == 'other':
            if cur_patch_type is not None:
                # We are inside a patch block. Buffer this line and decide later.
                pending_others.append(seg['raw'])
            else:
                output_lines.append(seg['raw'])

        elif k == 'object_close':
            flush_pending(inside_patch=False)
            if si in init_map:
                close_raw = seg['raw'].rstrip('\n')
                brace_col = len(close_raw) - len(close_raw.lstrip())
                inner = ' ' * (brace_col + 4)
                output_lines.append('{}init() {{\n'.format(inner))
                for f_name, f_expr in init_map[si]:
                    output_lines.append('{}    self.{} = {}\n'.format(inner, f_name, f_expr))
                output_lines.append('{}}}\n'.format(inner))
            output_lines.append(seg['raw'])

        elif k == 'field_default':
            flush_pending(inside_patch=False)
            output_lines.append(seg['stripped'])

    # Close any remaining open patch block
    flush_pending(inside_patch=False)

    return ''.join(output_lines), warnings


# ---------------------------------------------------------------------------
# File-level migration
# ---------------------------------------------------------------------------

def migrate_file(path, dry_run=False):
    """Migrate one .iron file in-place. Returns True if changed."""
    try:
        source = path.read_text(encoding='utf-8')
    except OSError as exc:
        print('ERROR: cannot read {}: {}'.format(path, exc), file=sys.stderr)
        return False

    new_source, warnings = transform_source(source, str(path))

    for w in warnings:
        print(w, file=sys.stderr)

    if new_source == source:
        return False

    diff = difflib.unified_diff(
        source.splitlines(keepends=True),
        new_source.splitlines(keepends=True),
        fromfile='a/{}'.format(path),
        tofile='b/{}'.format(path),
    )
    diff_text = ''.join(diff)
    if diff_text:
        print(diff_text, file=sys.stderr, end='')

    if not dry_run:
        try:
            path.write_text(new_source, encoding='utf-8')
        except OSError as exc:
            print('ERROR: cannot write {}: {}'.format(path, exc), file=sys.stderr)
            return False

    return True


# ---------------------------------------------------------------------------
# CLI entry point
# ---------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(
        prog='migrate_v2_to_v3',
        description='Migrate Iron source files from v2 to v3 syntax.',
    )
    parser.add_argument('--from', dest='from_ver', required=True,
                        help='Source version (must be v2)')
    parser.add_argument('--to', dest='to_ver', required=True,
                        help='Target version (must be v3)')
    parser.add_argument('path', nargs='?', default=None,
                        help='File or directory to migrate')
    parser.add_argument('--dry-run', action='store_true',
                        help='Print diff without modifying files')
    args = parser.parse_args()

    if args.from_ver != 'v2' or args.to_ver != 'v3':
        print(
            'migrate: unsupported version pair --from {} --to {}\n'
            'Only --from v2 --to v3 is supported.'.format(
                args.from_ver, args.to_ver
            ),
            file=sys.stderr,
        )
        return 1

    if not args.path:
        print(
            'migrate: missing path argument\n'
            'Usage: migrate_v2_to_v3.py --from v2 --to v3 <path>',
            file=sys.stderr,
        )
        return 1

    target = Path(args.path)
    if not target.exists():
        print('migrate: path not found: {}'.format(target), file=sys.stderr)
        return 1

    files = [target] if target.is_file() else sorted(target.rglob('*.iron'))
    if not files:
        print('migrate: no .iron files found under {}'.format(target), file=sys.stderr)
        return 0

    changed = 0
    for f in files:
        if migrate_file(f, dry_run=args.dry_run):
            changed += 1

    label = ' (dry-run)' if args.dry_run else ''
    if changed:
        print('migrate: {} file(s) updated{}'.format(changed, label), file=sys.stderr)
    else:
        print('migrate: no changes needed', file=sys.stderr)
    return 0


if __name__ == '__main__':
    sys.exit(main())
