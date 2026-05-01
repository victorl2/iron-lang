#!/usr/bin/env bash
# Phase 98 PATCH-01: codemod that rewrites every standalone
# `func TypeName.method(...)` declaration in a .iron file into a
# consolidated per-TypeName `patch object TypeName { ... }` block.
#
# See .planning/phases/98-patch-standalone-form-removal/98-CONTEXT.md
# (decisions section, PATCH-01 / PATCH-02 — Option B "consolidating").
#
# Algorithm:
#   1. Two-pass scan over the input file.
#   2. Pass 1 collects, per-TypeName:
#        a. Every standalone-form line `func TypeName.method(...) {...}`,
#           stripped of the `TypeName.` prefix, indented 4 spaces.
#        b. The body content of every existing `patch object TypeName { ... }`
#           block (lines BETWEEN the opening `{` line and the matching `}` line,
#           preserving original indentation).
#   3. Pass 2 re-emits the file. At the location of the FIRST occurrence
#      of each TypeName (whether as a standalone line OR as a patch-block
#      opener), emit one consolidated `patch object TypeName { ... }` block
#      with all bucketed content. Drop all subsequent standalone lines and
#      existing patch-block lines for the same TypeName.
#   4. Comments, blank lines, `import` decls, `object T { ... }` blocks,
#      and any `-- @file:` markers pass through verbatim.
#   5. Idempotent: if the file has zero `^func [A-Z]` lines AND no two
#      patch blocks for the same TypeName, the codemod treats the file
#      as already-migrated and exits 0 with `<file>: already migrated`.
#
# Implementation note: written in POSIX awk (no gawk extensions; works on
# macOS /usr/bin/awk and Linux mawk). Uses 2-arg match() + substr/RSTART/
# RLENGTH for capture-group equivalents. No gensub.
#
# Usage:
#   scripts/migrate_standalone_to_patch.sh <file.iron> [<file.iron> ...]
#   scripts/migrate_standalone_to_patch.sh --check <file.iron>
#       Exit 1 if any input file still has standalone-form lines OR has
#       multiple patch blocks for the same TypeName; exit 0 otherwise.
#
# Tested invocation for stdlib:
#   scripts/migrate_standalone_to_patch.sh \
#       src/stdlib/string.iron src/stdlib/math.iron src/stdlib/raylib.iron

set -euo pipefail

# ---------------------------------------------------------------------------
# Embedded awk program. POSIX-compatible. Two-pass via END block.
# ---------------------------------------------------------------------------
AWK_PROGRAM='
BEGIN {
    types_seen_count = 0
    in_patch        = 0
    cur_patch_type  = ""
    cur_patch_open  = 0
    cur_patch_depth = 0
}

# Count net brace delta (open - close) on a line.
function brace_delta(s,    i, c, d) {
    d = 0
    for (i = 1; i <= length(s); i++) {
        c = substr(s, i, 1)
        if (c == "{") d++
        else if (c == "}") d--
    }
    return d
}

# Extract the TypeName from a `patch object TYPE {` line.
# Returns the captured TYPE on success, "" on no match.
function extract_patch_type(s,    pos, tail, t) {
    if (s !~ /^[[:space:]]*patch[[:space:]]+object[[:space:]]+[A-Z][A-Za-z0-9_]*[[:space:]]*\{/) {
        return ""
    }
    # Find "object" keyword and skip it + whitespace; then read identifier.
    pos = index(s, "object")
    if (pos == 0) return ""
    tail = substr(s, pos + length("object"))
    # Skip leading whitespace.
    sub(/^[[:space:]]+/, "", tail)
    # Capture identifier prefix.
    if (match(tail, /^[A-Z][A-Za-z0-9_]*/)) {
        t = substr(tail, RSTART, RLENGTH)
        return t
    }
    return ""
}

# Extract the TypeName from a `func TYPE.method(...)` standalone line.
# Returns "" on no match. Standalone form requires column-0 `func`.
function extract_standalone_type(s,    tail, t) {
    if (s !~ /^func[[:space:]]+[A-Z][A-Za-z0-9_]*\./) {
        return ""
    }
    tail = s
    sub(/^func[[:space:]]+/, "", tail)
    if (match(tail, /^[A-Z][A-Za-z0-9_]*/)) {
        t = substr(tail, RSTART, RLENGTH)
        return t
    }
    return ""
}

# Strip the `func TYPE.` prefix off a standalone line, leaving `func method(...)`.
# Returns the stripped string, with a 4-space indent prepended.
function strip_standalone(s,    out) {
    out = s
    # Remove "func ws+TYPE." prefix; replace with "func ".
    sub(/^func[[:space:]]+[A-Z][A-Za-z0-9_]*\./, "func ", out)
    return "    " out
}

function register_first(type, lineno) {
    if (!(type in firsts)) {
        firsts[type] = lineno
        types_seen_count++
        types_order[types_seen_count] = type
    }
}

function bucket_append(type, body_line) {
    if (type in bodies) {
        bodies[type] = bodies[type] "\x1f" body_line
    } else {
        bodies[type] = body_line
    }
}

# Pass 1: read every line, classify it, build buckets.
{
    LINE[NR] = $0
    n = NR

    if (in_patch) {
        cur_patch_depth += brace_delta($0)
        if (cur_patch_depth <= 0) {
            line_kind[n] = "patch_close"
            line_type[n] = cur_patch_type
            in_patch = 0
            cur_patch_type = ""
            cur_patch_open = 0
            cur_patch_depth = 0
        } else {
            line_kind[n] = "patch_body"
            line_type[n] = cur_patch_type
            bucket_append(cur_patch_type, $0)
        }
        next
    }

    type = extract_patch_type($0)
    if (type != "") {
        line_kind[n] = "patch_open"
        line_type[n] = type
        register_first(type, n)
        cur_patch_depth = brace_delta($0)
        if (cur_patch_depth <= 0) {
            # Degenerate `patch object T {}` on one line.
            cur_patch_depth = 0
        } else {
            in_patch = 1
            cur_patch_type = type
            cur_patch_open = n
        }
        next
    }

    type = extract_standalone_type($0)
    if (type != "") {
        line_kind[n] = "stand"
        line_type[n] = type
        register_first(type, n)
        bucket_append(type, strip_standalone($0))
        next
    }

    line_kind[n] = "other"
}

END {
    if (in_patch) {
        # Defensive: malformed input had unclosed patch block. Pass through
        # the orphan lines verbatim instead of corrupting the file.
        for (i = cur_patch_open; i <= NR; i++) {
            line_kind[i] = "other"
            delete line_type[i]
        }
    }

    for (i = 1; i <= NR; i++) {
        kind = (i in line_kind) ? line_kind[i] : "other"

        if (kind == "other") {
            print LINE[i]
            continue
        }

        type = line_type[i]
        if (firsts[type] == i) {
            printf("patch object %s {\n", type)
            n_body = split(bodies[type], body_arr, "\x1f")
            for (j = 1; j <= n_body; j++) {
                if (length(body_arr[j]) > 0) {
                    print body_arr[j]
                }
            }
            print "}"
            firsts[type] = -1
            continue
        }

        # Subsequent occurrence — drop entirely.
    }
}
'

# ---------------------------------------------------------------------------
# migrate_one <file>
# Detects already-migrated state, runs the awk program, asserts post-condition.
# ---------------------------------------------------------------------------
migrate_one() {
    local f="$1"
    local stand_count
    stand_count=$(grep -c '^func [A-Z][A-Za-z0-9_]*\.' "$f" || true)
    local dup_patch_types
    dup_patch_types=$(grep -oE '^patch object [A-Z][A-Za-z0-9_]*' "$f" \
        | awk '{print $3}' | sort | uniq -d | wc -l | tr -d ' ')

    if [ "$stand_count" -eq 0 ] && [ "$dup_patch_types" -eq 0 ]; then
        echo "$f: already migrated (0 standalone-form lines, no duplicate patch blocks)"
        return 0
    fi

    echo "$f: migrating $stand_count standalone declarations and consolidating $dup_patch_types duplicate patch typenames..."

    local tmp
    tmp=$(mktemp -t migrate_codemod.XXXXXX)
    if awk "$AWK_PROGRAM" "$f" > "$tmp"; then
        mv "$tmp" "$f"
    else
        rm -f "$tmp"
        echo "ERROR: awk codemod failed for $f" >&2
        return 1
    fi

    local after
    after=$(grep -c '^func [A-Z][A-Za-z0-9_]*\.' "$f" || true)
    if [ "$after" -ne 0 ]; then
        echo "ERROR: $f still has $after standalone-form lines after migration" >&2
        return 1
    fi
    local after_dup
    after_dup=$(grep -oE '^patch object [A-Z][A-Za-z0-9_]*' "$f" \
        | awk '{print $3}' | sort | uniq -d | wc -l | tr -d ' ')
    if [ "$after_dup" -ne 0 ]; then
        echo "ERROR: $f still has $after_dup duplicate patch typenames after migration" >&2
        return 1
    fi
    echo "$f: migration complete (0 standalone-form lines, no duplicate patch blocks)"
}

# ---------------------------------------------------------------------------
# check_one <file> — exit non-zero if file needs migration.
# ---------------------------------------------------------------------------
check_one() {
    local f="$1"
    local stand_count dup_patch_types
    stand_count=$(grep -c '^func [A-Z][A-Za-z0-9_]*\.' "$f" || true)
    dup_patch_types=$(grep -oE '^patch object [A-Z][A-Za-z0-9_]*' "$f" \
        | awk '{print $3}' | sort | uniq -d | wc -l | tr -d ' ')
    if [ "$stand_count" -gt 0 ] || [ "$dup_patch_types" -gt 0 ]; then
        echo "$f: needs migration ($stand_count standalone-form lines, $dup_patch_types duplicate patch typenames)" >&2
        return 1
    fi
    echo "$f: already migrated"
    return 0
}

# ---------------------------------------------------------------------------
# Main loop.
# ---------------------------------------------------------------------------
CHECK_ONLY=0
if [ "${1:-}" = "--check" ]; then
    CHECK_ONLY=1
    shift
fi

if [ $# -lt 1 ]; then
    echo "usage: $0 [--check] <file.iron> [<file.iron> ...]" >&2
    exit 2
fi

rc=0
for f in "$@"; do
    if [ ! -f "$f" ]; then
        echo "ERROR: $f not found" >&2
        exit 1
    fi
    if [ "$CHECK_ONLY" -eq 1 ]; then
        check_one "$f" || rc=1
    else
        migrate_one "$f" || rc=1
    fi
done

exit $rc
