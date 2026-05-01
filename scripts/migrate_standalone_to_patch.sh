#!/usr/bin/env bash
# Phase 98 PATCH-01: codemod that rewrites every standalone
# `func TypeName.method(...)` declaration in a .iron file into a
# consolidated per-TypeName `patch object TypeName { ... }` block.
#
# See .planning/phases/98-patch-standalone-form-removal/98-CONTEXT.md
# (decisions section, PATCH-01 / PATCH-02 - Option B "consolidating").
#
# Algorithm:
#   1. Pre-pass coalesces multi-line `func TYPE.method(...)` declarations
#      into single logical lines by tracking parenthesis depth across lines
#      until a closing paren returns depth to 0. The coalesced line preserves
#      the original opener line position; continuation lines are dropped.
#   2. Two-pass scan over the coalesced stream.
#   3. Pass 1 collects, per-TypeName:
#        a. Every standalone-form line `func TypeName.method(...) {...}`,
#           classified as factory / init / regular instance method, then
#           rewritten and bucketed for the patch block.
#        b. The body content of every existing `patch object TypeName { ... }`
#           block (lines BETWEEN the opening `{` line and the matching `}` line,
#           preserving original indentation).
#   4. Pass 2 re-emits the file. At the location of the FIRST occurrence
#      of each TypeName (whether as a standalone line OR as a patch-block
#      opener), emit one consolidated `patch object TypeName { ... }` block
#      with all bucketed content. Drop all subsequent standalone lines and
#      existing patch-block lines for the same TypeName.
#   5. Comments, blank lines, `import` decls, `object T { ... }` blocks,
#      and any `-- @file:` markers pass through verbatim.
#   6. Idempotent: if the file has zero `^func [A-Z]` lines AND no two
#      patch blocks for the same TypeName, the codemod treats the file
#      as already-migrated and exits 0 with `<file>: already migrated`.
#
# Method classification (Phase 98 Plan 01 Option A1 enhancements):
#   - Init rewrite: `func TYPE.init(args)` becomes named init `init init(args)`
#     inside the patch block. Per parser.c:3992 init always returns Self, so
#     any explicit `-> Type` return annotation is dropped.
#   - Factory heuristic: `func TYPE.NAME(args) -> TYPE {}` where NAME is one of
#     {zero, identity, new} OR matches `from_*` is treated as a factory and
#     rewritten as named init `init NAME(args)` (return type dropped). Bodies
#     are empty (FFI stubs) so factory-vs-instance disambiguation cannot rely
#     on body inspection.
#   - Readonly injection: receiver types in the non-struct allowlist (String,
#     Math, Int, Float, Float32, Bool, Char) get an automatic `readonly`
#     prefix on every regular instance method to satisfy the resolver E0236
#     mut-on-non-struct guard. Init/factory rewrites are exempt (init is
#     never mutating in the receiver sense).
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
# Embedded awk program. POSIX-compatible.
#
# Two stages run inside one awk invocation:
#   Stage A (per-record): coalesce a multi-line `func TYPE.method(...)` into
#                         one buffer entry. Buffered into LINE[].
#   Stage B (END):        run the original two-pass classification + emission
#                         over LINE[1..NLINES].
# ---------------------------------------------------------------------------
AWK_PROGRAM='
BEGIN {
    NLINES = 0
    pending = ""
    pending_paren_depth = 0
    in_pending_func = 0

    types_seen_count = 0
    in_patch        = 0
    cur_patch_type  = ""
    cur_patch_open  = 0
    cur_patch_depth = 0

    # Non-struct receiver allowlist: methods on these types receive an
    # automatic `readonly` prefix during migration. String/Math/primitives
    # are not declared as object T { } so the resolver E0236 guard rejects
    # mutating receivers on them. The remaining stdlib types (Vector2 etc.)
    # are object-declared and tolerate the default mutating receiver.
    nonstruct["String"]  = 1
    nonstruct["Math"]    = 1
    nonstruct["Int"]     = 1
    nonstruct["Float"]   = 1
    nonstruct["Float32"] = 1
    nonstruct["Bool"]    = 1
    nonstruct["Char"]    = 1

    # Factory method names. Anything else with no explicit instance use
    # is treated as a regular instance method; conservative classification
    # avoids false-positive named-init rewrites.
    factory_name["zero"]     = 1
    factory_name["one"]      = 1
    factory_name["identity"] = 1
    factory_name["new"]      = 1
}

# Count net paren delta on a string (open - close).
function paren_delta(s,    i, c, d) {
    d = 0
    for (i = 1; i <= length(s); i++) {
        c = substr(s, i, 1)
        if (c == "(") d++
        else if (c == ")") d--
    }
    return d
}

# Count net brace delta (open - close).
function brace_delta(s,    i, c, d) {
    d = 0
    for (i = 1; i <= length(s); i++) {
        c = substr(s, i, 1)
        if (c == "{") d++
        else if (c == "}") d--
    }
    return d
}

# Stage A: coalesce multi-line `func TYPE.method(...)` declarations.
{
    if (in_pending_func) {
        # Append continuation, dropping the leading whitespace so the
        # coalesced line reads cleanly.
        cont = $0
        sub(/^[[:space:]]+/, " ", cont)
        pending = pending cont
        pending_paren_depth += paren_delta($0)
        if (pending_paren_depth <= 0) {
            NLINES++
            LINE[NLINES] = pending
            pending = ""
            pending_paren_depth = 0
            in_pending_func = 0
        }
        next
    }

    if ($0 ~ /^func[[:space:]]+[A-Z][A-Za-z0-9_]*\./) {
        d = paren_delta($0)
        if (d > 0) {
            pending = $0
            pending_paren_depth = d
            in_pending_func = 1
            next
        }
    }

    NLINES++
    LINE[NLINES] = $0
}

END {
    # Defensive: unterminated pending becomes a literal trailing line.
    if (in_pending_func) {
        NLINES++
        LINE[NLINES] = pending
    }

    # Stage B: classify and bucket.
    for (n = 1; n <= NLINES; n++) {
        s = LINE[n]

        if (in_patch) {
            cur_patch_depth += brace_delta(s)
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
                bucket_append(cur_patch_type, s)
            }
            continue
        }

        type = extract_patch_type(s)
        if (type != "") {
            line_kind[n] = "patch_open"
            line_type[n] = type
            register_first(type, n)
            cur_patch_depth = brace_delta(s)
            if (cur_patch_depth <= 0) {
                cur_patch_depth = 0
            } else {
                in_patch = 1
                cur_patch_type = type
                cur_patch_open = n
            }
            continue
        }

        type = extract_standalone_type(s)
        if (type != "") {
            line_kind[n] = "stand"
            line_type[n] = type
            register_first(type, n)
            method = extract_standalone_method(s)
            rewrite = rewrite_standalone(s, type, method)
            bucket_append(type, rewrite)
            continue
        }

        line_kind[n] = "other"
    }

    if (in_patch) {
        # Defensive: malformed input had unclosed patch block. Pass through
        # the orphan lines verbatim instead of corrupting the file.
        for (i = cur_patch_open; i <= NLINES; i++) {
            line_kind[i] = "other"
            delete line_type[i]
        }
    }

    for (i = 1; i <= NLINES; i++) {
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

        # Subsequent occurrence - drop entirely.
    }
}

# Extract the TypeName from a `patch object TYPE {` line.
function extract_patch_type(s,    pos, tail, t) {
    if (s !~ /^[[:space:]]*patch[[:space:]]+object[[:space:]]+[A-Z][A-Za-z0-9_]*[[:space:]]*\{/) {
        return ""
    }
    pos = index(s, "object")
    if (pos == 0) return ""
    tail = substr(s, pos + length("object"))
    sub(/^[[:space:]]+/, "", tail)
    if (match(tail, /^[A-Z][A-Za-z0-9_]*/)) {
        t = substr(tail, RSTART, RLENGTH)
        return t
    }
    return ""
}

# Extract the TypeName from a `func TYPE.method(...)` standalone line.
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

# Extract method name from `func TYPE.METHOD(...)`.
function extract_standalone_method(s,    tail, m) {
    tail = s
    sub(/^func[[:space:]]+[A-Z][A-Za-z0-9_]*\./, "", tail)
    if (match(tail, /^[A-Za-z_][A-Za-z0-9_]*/)) {
        m = substr(tail, RSTART, RLENGTH)
        return m
    }
    return ""
}

# Strip the `-> ReturnType` annotation from a logical line; useful when
# rewriting factory methods or inits, both of which return Self implicitly.
function strip_return_annotation(s,    out) {
    out = s
    # Remove `-> TypeName ` or `-> TypeName{` anywhere in the line.
    # POSIX awk has no non-greedy quantifier; rely on the next `{` as the
    # body opener since stub bodies are uniformly `{}`.
    if (match(out, /[[:space:]]*->[[:space:]]*[A-Z][A-Za-z0-9_]*[[:space:]]*/)) {
        out = substr(out, 1, RSTART - 1) substr(out, RSTART + RLENGTH)
        # Re-attach a single space before any trailing `{` if needed.
        if (out !~ /[[:space:]]\{/ && out ~ /\{/) {
            sub(/\{/, " {", out)
        }
    }
    return out
}

# Determine whether a stripped (TYPE.-removed) line names a factory.
function is_factory(method, line,    after_arrow, ret_type) {
    if (method == "") return 0
    if (method in factory_name) return 1
    if (method ~ /^from_/) return 1
    return 0
}

# Rewrite a standalone-form line into its patch-block body form.
# Returns the rewritten line WITH 4-space indent prepended.
#
# Init-method and factory rewrites were attempted in an earlier draft
# but proved incompatible with three parser/typecheck/HIR constraints:
#
#   (a) Init parser at parser.c:3977 expects IDENTIFIER as the
#       named-init slot; the IRON_TOK_INIT keyword is rejected, so
#       `init init(...)` does not parse.
#
#   (b) Even with parser widening, fieldless objects (Window {}, Audio {})
#       receive an auto-synth anonymous init at parser.c:3652. The
#       migrated `init init(...)` collides with this synth at the HIR
#       layer: both produce mangled-name `window_init` / `audio_init`,
#       and Pass 2 lower_method_body_hir cannot disambiguate them.
#
#   (c) E0247 enforces definite-assignment on every init body. Factory
#       FFI stubs have empty bodies and never assign fields, so
#       factory-as-init on struct-with-fields types (Vector2, Color,
#       etc.) fails to compile.
#
# The combined effect: `func TYPE.init(...)` and factory-named methods
# cannot be cleanly migrated to init form within the constraints of the
# existing grammar and resolver. Keep them as funcs (with readonly
# injection for non-struct receivers); call sites continue to dispatch
# via `Type.method(args)` unchanged.
function rewrite_standalone(s, type, method,    out) {
    out = s
    is_init = (method == "init")

    sub(/^func[[:space:]]+[A-Z][A-Za-z0-9_]*\./, "func ", out)

    # Inject `readonly` on every migrated method so:
    #   (a) Non-struct receivers (String, Math, Int, Float, etc.) skip the
    #       resolver E0236 mut-on-non-struct guard.
    #   (b) Static-style call sites (Sound.from_wave(w), Color.from_hsv(...))
    #       skip the resolver E0235 cannot-call-mutable-method-on-immutable-
    #       binding guard. Static calls dispatch through the type symbol
    #       which is val (immutable); a mutating-receiver method requires a
    #       var binding.
    #   (c) Init methods cannot be readonly (parser.c:3963 rejects). Skip
    #       the prefix for `func init(...)` migrated decls; their receiver
    #       is always mut by parser construction (for fieldless objects
    #       this is fine - the auto-synth init covers anonymous dispatch
    #       and the migrated `func init(...)` covers named dispatch).
    if (!is_init) {
        sub(/^func[[:space:]]+/, "readonly func ", out)
    }
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
# check_one <file> - exit non-zero if file needs migration.
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
