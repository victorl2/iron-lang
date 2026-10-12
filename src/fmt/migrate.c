/* migrate.c -- see migrate.h. */

#include "fmt/migrate.h"

#include "lexer/lexer.h"
#include "parser/parser.h"
#include "parser/ast.h"
#include "diagnostics/diagnostics.h"
#include "util/strbuf.h"
#include "vendor/stb_ds.h"

#include <stdio.h>
#include <string.h>

/* `key:` with key matching [a-z][a-z0-9_]* starts at s (directive form). */
static bool looks_like_directive(const char *s, const char *end) {
    if (s >= end || *s < 'a' || *s > 'z') return false;
    s++;
    while (s < end && ((*s >= 'a' && *s <= 'z') || (*s >= '0' && *s <= '9') || *s == '_')) s++;
    return s < end && *s == ':';
}

typedef struct {
    size_t      start, end;   /* replaced byte range */
    const char *with;
} Edit;

/* ── Verification ───────────────────────────────────────────────────────── */

/* One element of a token stream with comment spelling taken out. */
typedef struct {
    int         kind;    /* a token kind, or one of the ITEM_* below */
    const char *text;
    uint8_t     flags;   /* OWN_LINE | DIRECTIVE | MARKER, for comments */
} Item;

enum { ITEM_COMMENT = -1, ITEM_FUSIBLE = -2 };

static Item *items_of(const Iron_Token *t, int n) {
    Item *out = NULL;
    for (int i = 0; i < n; i++) {
        uint8_t keep = (uint8_t)(t[i].comment & (IRON_COMMENT_OWN_LINE |
                                                 IRON_COMMENT_DIRECTIVE |
                                                 IRON_COMMENT_MARKER));
        if (t[i].kind == IRON_TOK_AT && i + 1 < n &&
            t[i + 1].kind == IRON_TOK_IDENTIFIER && t[i + 1].value &&
            strcmp(t[i + 1].value, "fusible") == 0) {
            Item it = { ITEM_FUSIBLE, NULL, 0 };
            arrput(out, it);
            i++;
            continue;
        }
        if (t[i].kind == IRON_TOK_DOC_COMMENT) {
            Item it = { ITEM_COMMENT, t[i].value ? t[i].value : "", keep };
            arrput(out, it);
            continue;
        }
        if (t[i].kind == IRON_TOK_NEWLINE) {
            if (!(t[i].comment & IRON_COMMENT_PRESENT)) continue;
            const char *v = t[i].value ? t[i].value : "";
            if ((t[i].comment & IRON_COMMENT_DIRECTIVE) &&
                strcmp(v, "iron:fusible") == 0) {
                Item it = { ITEM_FUSIBLE, NULL, 0 };
                arrput(out, it);
                continue;
            }
            /* Documentation text: one leading space removed (the `///`
             * body comes that way from the lexer). */
            if (!(t[i].comment & IRON_COMMENT_DIRECTIVE) && v[0] == ' ') v++;
            Item it = { ITEM_COMMENT, v, keep };
            arrput(out, it);
            continue;
        }
        Item it = { (int)t[i].kind, t[i].value, 0 };
        arrput(out, it);
    }
    return out;
}

static bool same_str(const char *a, const char *b) {
    if (!a || !b) return a == b;
    return strcmp(a, b) == 0;
}

static bool same_items(const Item *a, const Item *b) {
    if (arrlen(a) != arrlen(b)) return false;
    for (ptrdiff_t i = 0; i < arrlen(a); i++) {
        if (a[i].kind != b[i].kind || a[i].flags != b[i].flags) return false;
        if (!same_str(a[i].text, b[i].text)) return false;
    }
    return true;
}

static bool same_diags(const Iron_DiagList *a, const Iron_DiagList *b) {
    if (a->count != b->count) return false;
    for (int i = 0; i < a->count; i++) {
        const Iron_Diagnostic *x = &a->items[i], *y = &b->items[i];
        if (x->level != y->level || x->code != y->code ||
            x->span.line != y->span.line || x->span.col != y->span.col ||
            !same_str(x->message, y->message))
            return false;
    }
    return true;
}

/* Documentation, directives (an `iron:fusible` directive stands for the
 * `@fusible` it replaces, so it is compared through the fusible mark) and
 * fusible marks of one node. */
static bool same_doc(const char *da, const Iron_Directive *xa, int na,
                     const char *db, const Iron_Directive *xb, int nb) {
    if (!same_str(da, db)) return false;
    int i = 0, j = 0;
    for (;;) {
        while (i < na && strcmp(xa[i].key, "iron") == 0 && strcmp(xa[i].value, "fusible") == 0) i++;
        while (j < nb && strcmp(xb[j].key, "iron") == 0 && strcmp(xb[j].value, "fusible") == 0) j++;
        if (i >= na || j >= nb) return i >= na && j >= nb;
        if (strcmp(xa[i].key, xb[j].key) != 0 || strcmp(xa[i].value, xb[j].value) != 0 ||
            xa[i].trailing != xb[j].trailing)
            return false;
        i++; j++;
    }
}

#define SAME_DOC(A, B) same_doc((A)->doc_comment, (A)->directives, (A)->directive_count, \
                                (B)->doc_comment, (B)->directives, (B)->directive_count)

static bool same_node_docs(Iron_Node *a, Iron_Node *b) {
    if (!a || !b) return a == b;
    if (a->kind != b->kind) return false;
    switch ((int)a->kind) {
        case IRON_NODE_IMPORT_DECL:
            return SAME_DOC((Iron_ImportDecl *)a, (Iron_ImportDecl *)b);
        case IRON_NODE_FUNC_DECL: {
            Iron_FuncDecl *x = (Iron_FuncDecl *)a, *y = (Iron_FuncDecl *)b;
            return SAME_DOC(x, y) && x->is_fusible == y->is_fusible;
        }
        case IRON_NODE_METHOD_DECL: {
            Iron_MethodDecl *x = (Iron_MethodDecl *)a, *y = (Iron_MethodDecl *)b;
            return SAME_DOC(x, y) && x->is_fusible == y->is_fusible;
        }
        case IRON_NODE_VAL_DECL:
            return SAME_DOC((Iron_ValDecl *)a, (Iron_ValDecl *)b);
        case IRON_NODE_VAR_DECL:
            return SAME_DOC((Iron_VarDecl *)a, (Iron_VarDecl *)b);
        case IRON_NODE_OBJECT_DECL: {
            Iron_ObjectDecl *x = (Iron_ObjectDecl *)a, *y = (Iron_ObjectDecl *)b;
            if (!SAME_DOC(x, y) || x->field_count != y->field_count) return false;
            for (int i = 0; i < x->field_count; i++)
                if (!same_node_docs(x->fields[i], y->fields[i])) return false;
            return true;
        }
        case IRON_NODE_FIELD:
            return SAME_DOC((Iron_Field *)a, (Iron_Field *)b);
        case IRON_NODE_ENUM_DECL: {
            Iron_EnumDecl *x = (Iron_EnumDecl *)a, *y = (Iron_EnumDecl *)b;
            if (!SAME_DOC(x, y) || x->variant_count != y->variant_count) return false;
            for (int i = 0; i < x->variant_count; i++)
                if (!same_node_docs(x->variants[i], y->variants[i])) return false;
            return true;
        }
        case IRON_NODE_ENUM_VARIANT:
            return SAME_DOC((Iron_EnumVariant *)a, (Iron_EnumVariant *)b);
        case IRON_NODE_INTERFACE_DECL: {
            Iron_InterfaceDecl *x = (Iron_InterfaceDecl *)a, *y = (Iron_InterfaceDecl *)b;
            if (!SAME_DOC(x, y) || x->method_count != y->method_count) return false;
            for (int i = 0; i < x->method_count; i++)
                if (!same_node_docs(x->method_sigs[i], y->method_sigs[i])) return false;
            return true;
        }
        default:
            return true;
    }
}

typedef struct {
    Iron_Token    *tokens;
    int            count;
    Iron_DiagList  diags;
    Iron_Node     *program;
} Parsed;

static Parsed parse_text(const char *text, const char *filename, Iron_Arena *arena) {
    Parsed r;
    r.diags = iron_diaglist_create();
    Iron_Lexer lx = iron_lexer_create(text, filename, arena, &r.diags);
    r.tokens = iron_lex_all(&lx);
    r.count = (int)arrlen(r.tokens);
    Iron_Parser p = iron_parser_create(r.tokens, r.count, text, filename, arena, &r.diags);
    r.program = iron_parse(&p);
    return r;
}

static void parsed_free(Parsed *r) {
    arrfree(r->tokens);
    iron_diaglist_free(&r->diags);
}

static const char *verify(Parsed *a, Parsed *b) {
    Item *ia = items_of(a->tokens, a->count);
    Item *ib = items_of(b->tokens, b->count);
    bool tokens_ok = same_items(ia, ib);
    arrfree(ia);
    arrfree(ib);
    if (!tokens_ok) return "the rewritten text lexes to different tokens";
    if (!same_diags(&a->diags, &b->diags))
        return "the rewritten text reports different diagnostics";
    Iron_Program *pa = (Iron_Program *)a->program, *pb = (Iron_Program *)b->program;
    if (!pa || !pb || pa->decl_count != pb->decl_count)
        return "the rewritten text parses to different declarations";
    for (int i = 0; i < pa->decl_count; i++)
        if (!same_node_docs(pa->decls[i], pb->decls[i]))
            return "the rewritten text attaches different documentation or directives";
    return NULL;
}

/* ── Rewrite ─────────────────────────────────────────────────────────────── */

IronMigrateResult iron_migrate_comments(const char *source, size_t len,
                                        const char *filename, Iron_Arena *arena) {
    IronMigrateResult r = { false, NULL, source, len, false, 0, 0 };
    if (!source || !arena) { r.error = "no input"; return r; }
    if (strlen(source) != len) { r.error = "the file contains a NUL byte"; return r; }
    const char *end = source + len;

    Parsed before = parse_text(source, filename, arena);
    Edit *edits = NULL;
    const Iron_Token *t = before.tokens;
    for (int i = 0; i < before.count; i++) {
        size_t o = t[i].offset;
        if (t[i].kind == IRON_TOK_DOC_COMMENT && (t[i].comment & IRON_COMMENT_TRIPLE)) {
            /* `///rest` -> `//rest`, or `// rest` when rest would read as a
             * nested `///` or a directive. */
            const char *rest = source + o + 3;
            bool space = rest < end && (*rest == '/' || looks_like_directive(rest, end));
            Edit e = { o, o + 3, space ? "// " : "//" };
            arrput(edits, e);
            r.comments++;
        } else if (t[i].kind == IRON_TOK_NEWLINE && (t[i].comment & IRON_COMMENT_DASH)) {
            const char *rest = source + o + 2;
            bool space = rest < end && (*rest == '/' || looks_like_directive(rest, end));
            Edit e = { o, o + 2, space ? "// " : "//" };
            arrput(edits, e);
            r.comments++;
        } else if (t[i].kind == IRON_TOK_AT && i + 2 < before.count &&
                   t[i + 1].kind == IRON_TOK_IDENTIFIER && t[i + 1].value &&
                   strcmp(t[i + 1].value, "fusible") == 0 &&
                   t[i + 1].line == t[i].line) {
            size_t fend = t[i + 1].offset + 7;
            const Iron_Token *next = &t[i + 2];
            if (next->kind == IRON_TOK_NEWLINE && next->comment == 0) {
                Edit e = { o, fend, "//iron:fusible" };
                arrput(edits, e);
            } else if (next->kind != IRON_TOK_NEWLINE && next->kind != IRON_TOK_DOC_COMMENT) {
                /* `@fusible func f()`: the hint goes on a line of its own,
                 * with the indent of this one. */
                size_t ls = o;
                while (ls > 0 && source[ls - 1] != '\n') ls--;
                size_t ie = ls;
                while (ie < o && (source[ie] == ' ' || source[ie] == '\t')) ie++;
                Iron_StrBuf sb = iron_strbuf_create(32 + (ie - ls));
                iron_strbuf_appendf(&sb, "//iron:fusible\n%.*s", (int)(ie - ls), source + ls);
                const char *with = iron_arena_strdup(arena, iron_strbuf_get(&sb), sb.len);
                iron_strbuf_free(&sb);
                Edit e = { o, next->offset, with ? with : "//iron:fusible\n" };
                arrput(edits, e);
            } else {
                arrfree(edits);
                parsed_free(&before);
                r.error = "`@fusible` is followed by a comment on its line; "
                          "move the comment and run the migration again";
                return r;
            }
            r.hints++;
            i++;
        }
    }

    if (arrlen(edits) == 0) {
        arrfree(edits);
        parsed_free(&before);
        r.ok = true;
        return r;
    }

    Iron_StrBuf sb = iron_strbuf_create(len + 64);
    size_t at = 0;
    for (ptrdiff_t k = 0; k < arrlen(edits); k++) {
        iron_strbuf_appendf(&sb, "%.*s%s", (int)(edits[k].start - at), source + at,
                            edits[k].with);
        at = edits[k].end;
    }
    iron_strbuf_appendf(&sb, "%.*s", (int)(len - at), source + at);
    arrfree(edits);
    char *text = iron_arena_strdup(arena, iron_strbuf_get(&sb), sb.len);
    size_t text_len = sb.len;
    iron_strbuf_free(&sb);
    if (!text) { parsed_free(&before); r.error = "out of memory"; return r; }

    Parsed after = parse_text(text, filename, arena);
    const char *why = verify(&before, &after);
    parsed_free(&before);
    parsed_free(&after);
    if (why) { r.error = why; return r; }

    r.ok = true;
    r.text = text;
    r.len = text_len;
    r.changed = true;
    return r;
}
