/* Phase 4 Plan 04-03 Task 02 (EDIT-04, D-15) -- snippet-body renderer
 * for completion candidates whose CompletionItemKind maps to one of
 * the 5 template shapes (function/method call, object literal, match,
 * import, enum variant).
 *
 * Implementation strategy:
 *   - Arena-backed string builder (SB struct) modeled on the one in
 *     src/lsp/facade/hover.c -- identical grow semantics minus the
 *     truncation cap (snippets are short; no truncation here).
 *   - sb_append_escaped() escapes $ -> \$, } -> \}, \ -> \\ per LSP
 *     3.17 Snippet Syntax Appendix. Applied to every user-source text
 *     region (names, param identifiers, field names) to mitigate
 *     PITFALL D (the `${USER}` variable-substitution leak).
 *   - Placeholder tab-stop numbers start at 1. The terminal tab-stop
 *     is always `$0` appended at the end.
 *
 * Templates per CONTEXT.md D-15 (LOCKED):
 *   FUNCTION / METHOD: name(${1:p1}, ${2:p2})$0  (zero-arg: name()$0)
 *   OBJECT_LITERAL:    Name { ${1:f1}: ${2:value1}, ${3:f2}: ${4:value2} }$0
 *                      (empty fields: Name {}$0)
 *   MATCH:             match ${1:expr} {\n  ${2:Pattern} -> ${3:result},\n}$0
 *   IMPORT:            import ${1:module}$0
 *   ENUM_VARIANT:      Name.Variant$0 (payload-less) or
 *                      Name.Variant(${1:payload})$0 (with payload) */

#include "lsp/facade/edit/complete/snippet.h"

#include "util/arena.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Arena-backed string builder ──────────────────────────────────── */

typedef struct {
    char        *buf;
    size_t       len;
    size_t       cap;
    Iron_Arena  *arena;
} SB;

static void sb_init(SB *sb, Iron_Arena *arena) {
    sb->arena = arena;
    sb->cap   = 128;
    sb->len   = 0;
    sb->buf   = (char *)iron_arena_alloc(arena, sb->cap, 1);
    if (sb->buf) sb->buf[0] = '\0';
}

static void sb_reserve(SB *sb, size_t need) {
    if (!sb->buf) return;
    if (sb->len + need + 1 <= sb->cap) return;
    size_t ncap = sb->cap;
    while (ncap < sb->len + need + 1) ncap *= 2;
    char *nb = (char *)iron_arena_alloc(sb->arena, ncap, 1);
    if (!nb) return;
    memcpy(nb, sb->buf, sb->len + 1);
    sb->buf = nb;
    sb->cap = ncap;
}

static void sb_append(SB *sb, const char *s) {
    if (!sb->buf || !s) return;
    size_t slen = strlen(s);
    sb_reserve(sb, slen);
    if (!sb->buf) return;
    memcpy(sb->buf + sb->len, s, slen);
    sb->len += slen;
    sb->buf[sb->len] = '\0';
}

/* Escape user-source bytes per LSP Snippet Syntax Appendix:
 *   '$'  -> `\$`
 *   '}'  -> `\}`
 *   '\\' -> `\\`
 * Any other byte is emitted verbatim. See PITFALL D in 04-RESEARCH.md
 * -- this is the security-relevant hook; every placeholder default
 * that embeds a user-source identifier, parameter name, or field
 * name MUST flow through this function. */
static void sb_append_escaped(SB *sb, const char *s) {
    if (!sb->buf || !s) return;
    for (const char *p = s; *p; p++) {
        char c = *p;
        if (c == '$' || c == '}' || c == '\\') {
            sb_reserve(sb, 2);
            if (!sb->buf) return;
            sb->buf[sb->len++] = '\\';
            sb->buf[sb->len++] = c;
            sb->buf[sb->len] = '\0';
        } else {
            sb_reserve(sb, 1);
            if (!sb->buf) return;
            sb->buf[sb->len++] = c;
            sb->buf[sb->len] = '\0';
        }
    }
}

/* Emit `${<n>:<escaped-default>}`. If `default_text` is NULL/empty,
 * emits `${<n>:pN}` using the ordinal as a safe fallback (keeps the
 * grammar happy and gives the user a visible placeholder). */
static void sb_append_placeholder(SB *sb, int n, const char *default_text) {
    char head[32];
    snprintf(head, sizeof(head), "${%d:", n);
    sb_append(sb, head);
    if (default_text && *default_text) {
        sb_append_escaped(sb, default_text);
    } else {
        char fb[16];
        snprintf(fb, sizeof(fb), "p%d", n);
        sb_append(sb, fb);
    }
    sb_append(sb, "}");
}

/* ── Per-kind renderers ──────────────────────────────────────────── */

static void render_function(SB *sb, const IronLsp_SnippetMeta *meta) {
    const char *nm = (meta && meta->name) ? meta->name : "";
    sb_append_escaped(sb, nm);
    sb_append(sb, "(");
    if (meta && meta->param_names && meta->param_count > 0) {
        for (int i = 0; i < meta->param_count; i++) {
            if (i > 0) sb_append(sb, ", ");
            const char *pn = meta->param_names[i] ? meta->param_names[i] : "";
            sb_append_placeholder(sb, i + 1, pn);
        }
    }
    sb_append(sb, ")$0");
}

static void render_object_literal(SB *sb, const IronLsp_SnippetMeta *meta) {
    const char *nm = (meta && meta->name) ? meta->name : "";
    sb_append_escaped(sb, nm);
    if (!meta || !meta->field_names || meta->field_count <= 0) {
        sb_append(sb, " {}$0");
        return;
    }
    sb_append(sb, " { ");
    int stop = 1;
    for (int i = 0; i < meta->field_count; i++) {
        if (i > 0) sb_append(sb, ", ");
        const char *fn = meta->field_names[i] ? meta->field_names[i] : "";
        sb_append_placeholder(sb, stop++, fn);
        sb_append(sb, ": ");
        char value_default[32];
        snprintf(value_default, sizeof(value_default), "value%d", i + 1);
        sb_append_placeholder(sb, stop++, value_default);
    }
    sb_append(sb, " }$0");
}

static void render_match(SB *sb) {
    /* Literal template from D-15. No user-source text, so no escape
     * needed. Multi-line; LF only. */
    sb_append(sb, "match ${1:expr} {\n  ${2:Pattern} -> ${3:result},\n}$0");
}

static void render_import(SB *sb) {
    sb_append(sb, "import ${1:module}$0");
}

static void render_enum_variant(SB *sb, const IronLsp_SnippetMeta *meta) {
    const char *enm = (meta && meta->name) ? meta->name : "";
    const char *var = (meta && meta->variant_name) ? meta->variant_name : "";
    sb_append_escaped(sb, enm);
    sb_append(sb, ".");
    sb_append_escaped(sb, var);
    if (meta && meta->payload_count > 0) {
        sb_append(sb, "(");
        sb_append_placeholder(sb, 1, "payload");
        sb_append(sb, ")");
    }
    sb_append(sb, "$0");
}

/* ── Public API ──────────────────────────────────────────────────── */

const char *ilsp_snippet_render(IronLsp_SnippetKind        kind,
                                 const IronLsp_SnippetMeta *meta,
                                 Iron_Arena                *arena) {
    if (!arena) return NULL;
    SB sb;
    sb_init(&sb, arena);
    if (!sb.buf) return NULL;

    switch (kind) {
        case ILSP_SNIPPET_FUNCTION:
        case ILSP_SNIPPET_METHOD:
            render_function(&sb, meta);
            break;
        case ILSP_SNIPPET_OBJECT_LITERAL:
            render_object_literal(&sb, meta);
            break;
        case ILSP_SNIPPET_MATCH:
            render_match(&sb);
            break;
        case ILSP_SNIPPET_IMPORT:
            render_import(&sb);
            break;
        case ILSP_SNIPPET_ENUM_VARIANT:
            render_enum_variant(&sb, meta);
            break;
        default:
            /* Unknown kind: fall back to empty snippet body. */
            break;
    }

    return sb.buf;
}
