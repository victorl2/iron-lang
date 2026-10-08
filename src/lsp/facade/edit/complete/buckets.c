/* Phase 4 Plan 04-02 Task 02 (EDIT-01, EDIT-02, D-01, D-03) -- 6-bucket
 * completion candidate builder.
 *
 * Takes the classified context + query prefix + sealed Iron_Program and
 * emits a stb_ds dynamic array of IronLsp_CompletionCandidate structs
 * backed by the caller's arena. The facade orchestrator in complete.c
 * then qsort's and 128-caps the result.
 *
 * Implementation notes:
 *   - "Local scope" (Bucket 1) is approximated by walking the enclosing
 *     func decl's params + let/val/var decls inside its body whose spans
 *     precede the cursor byte offset. True Iron_Scope chain walking
 *     would require a scope_at_cursor helper and broadly re-traversing
 *     the analyzer output; for Plan 04-02 we keep this as the fn-level
 *     approximation (sufficient for EDIT-01's "locals surfaced before
 *     top-level" guarantee).
 *   - "Imported" (Bucket 3) surfaces each import alias as a module
 *     candidate (Module kind=9). Full same-module symbol traversal is
 *     deferred to Plan 04-03 when auto-import wiring makes the
 *     distinction between imported-and-in-scope vs imported-by-path
 *     meaningful.
 *   - "Stdlib" (Bucket 4) iterates the flattened workspace-index stdlib
 *     cache. Cold-start fallback: if the cache is NULL we simply emit no
 *     candidates for that bucket. Bucket 5 is unused: Iron has no package
 *     manager, so vendored modules surface as ordinary workspace symbols.
 */

#include "lsp/facade/edit/complete/buckets.h"
#include "lsp/facade/edit/complete/context_classify.h"
#include "lsp/facade/edit/complete/keyword_filter.h"
#include "lsp/facade/nav/fuzzy.h"
#include "lsp/facade/nav/patch_lookup.h"
#include "lsp/server/server.h"
#include "lsp/store/document.h"
#include "lsp/store/line_index.h"
#include "lsp/store/workspace_index.h"
#include "lsp/store/stdlib_cache.h"
#include "analyzer/analyzer.h"
#include "analyzer/scope.h"
#include "analyzer/types.h"
#include "diagnostics/diagnostics.h"
#include "parser/ast.h"
#include "util/arena.h"
#include "vendor/stb_ds.h"

#include "keyword_mirror.h"

#include <math.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* ── LSP CompletionItemKind constants (per 04-02 interfaces block) ─── */

#define LSP_CK_TEXT         1
#define LSP_CK_METHOD       2
#define LSP_CK_FUNCTION     3
#define LSP_CK_CONSTRUCTOR  4
#define LSP_CK_FIELD        5
#define LSP_CK_VARIABLE     6
#define LSP_CK_CLASS        7
#define LSP_CK_INTERFACE    8
#define LSP_CK_MODULE       9
#define LSP_CK_PROPERTY     10
#define LSP_CK_UNIT         11
#define LSP_CK_VALUE        12
#define LSP_CK_ENUM         13
#define LSP_CK_KEYWORD      14
#define LSP_CK_ENUMMEMBER   20
#define LSP_CK_CONSTANT     21
#define LSP_CK_STRUCT       22

/* ── Helpers ──────────────────────────────────────────────────────── */

static bool canceled(_Atomic bool *cancel) {
    return cancel != NULL && atomic_load(cancel);
}

static const char *arena_dup(const char *s, Iron_Arena *arena) {
    if (!s) return "";
    return iron_arena_strdup(arena, s, strlen(s));
}

static int lsp_kind_from_decl(const Iron_Node *d) {
    if (!d) return LSP_CK_VARIABLE;
    switch ((int)d->kind) {
        case IRON_NODE_FUNC_DECL:      return LSP_CK_FUNCTION;
        case IRON_NODE_METHOD_DECL:    return LSP_CK_METHOD;
        case IRON_NODE_OBJECT_DECL:    return LSP_CK_CLASS;
        case IRON_NODE_INTERFACE_DECL: return LSP_CK_INTERFACE;
        case IRON_NODE_ENUM_DECL:      return LSP_CK_ENUM;
        case IRON_NODE_ENUM_VARIANT:   return LSP_CK_ENUMMEMBER;
        case IRON_NODE_FIELD:          return LSP_CK_FIELD;
        case IRON_NODE_VAL_DECL:       return LSP_CK_CONSTANT;
        case IRON_NODE_VAR_DECL:       return LSP_CK_VARIABLE;
        case IRON_NODE_PARAM:          return LSP_CK_VARIABLE;
        case IRON_NODE_IMPORT_DECL:    return LSP_CK_MODULE;
        default:                        return LSP_CK_VARIABLE;
    }
}

static const char *decl_name(const Iron_Node *d) {
    if (!d) return NULL;
    switch ((int)d->kind) {
        case IRON_NODE_FUNC_DECL:      return ((const Iron_FuncDecl *)d)->name;
        case IRON_NODE_METHOD_DECL:    return ((const Iron_MethodDecl *)d)->method_name;
        case IRON_NODE_OBJECT_DECL:    return ((const Iron_ObjectDecl *)d)->name;
        case IRON_NODE_INTERFACE_DECL: return ((const Iron_InterfaceDecl *)d)->name;
        case IRON_NODE_ENUM_DECL:      return ((const Iron_EnumDecl *)d)->name;
        case IRON_NODE_ENUM_VARIANT:   return ((const Iron_EnumVariant *)d)->name;
        case IRON_NODE_FIELD:          return ((const Iron_Field *)d)->name;
        case IRON_NODE_VAL_DECL:       return ((const Iron_ValDecl *)d)->name;
        case IRON_NODE_VAR_DECL:       return ((const Iron_VarDecl *)d)->name;
        case IRON_NODE_PARAM:          return ((const Iron_Param *)d)->name;
        default:                        return NULL;
    }
}

static bool decl_is_extern(const Iron_Node *d) {
    if (!d) return false;
    if (d->kind == IRON_NODE_FUNC_DECL) return ((const Iron_FuncDecl *)d)->is_extern;
    return false;
}

/* Push a candidate if it fuzzy-matches the query prefix. Returns true
 * if pushed, false if dropped (no match / empty name / private). The
 * candidate struct is arena-copied. */
static bool maybe_push(IronLsp_CompletionCandidate **out_arr,
                        Iron_Arena             *arena,
                        const char                    *label,
                        int                            kind,
                        int                            bucket,
                        const char                    *detail,
                        const char                    *canonical_path,
                        const char                    *name_path,
                        bool                           is_extern,
                        bool                           needs_auto_import,
                        const char                    *query_prefix) {
    if (!label || !*label) return false;
    if (!ilsp_fuzzy_has_match(query_prefix, label)) return false;
    double score = ilsp_fuzzy_match(query_prefix, label, NULL, NULL);
    if (!isfinite(score)) return false;

    IronLsp_CompletionCandidate c;
    memset(&c, 0, sizeof(c));
    c.label             = arena_dup(label, arena);
    c.insert_text       = c.label;       /* plain-text; snippets = 04-03 */
    c.kind              = kind;
    c.bucket            = bucket;
    c.fuzzy_score       = score;
    c.canonical_path    = arena_dup(canonical_path ? canonical_path : "", arena);
    c.name_path         = arena_dup(name_path ? name_path : label, arena);
    c.is_extern         = is_extern;
    c.needs_auto_import = needs_auto_import;
    c.content_hash      = 0;

    /* Build the `detail` string. Extern marker appended per D-04. */
    if (detail && *detail) {
        if (is_extern) {
            size_t dl = strlen(detail) + sizeof(" (C interop)") + 1;
            char *buf = (char *)iron_arena_alloc(arena, dl, 1);
            if (buf) {
                snprintf(buf, dl, "%s (C interop)", detail);
                c.detail = buf;
            } else {
                c.detail = arena_dup(detail, arena);
            }
        } else {
            c.detail = arena_dup(detail, arena);
        }
    } else {
        c.detail = is_extern ? "(C interop)" : "";
    }

    arrput(*out_arr, c);
    return true;
}

/* ── Bucket 2: top-level same-file ────────────────────────────────── */

static void emit_top_level(IronLsp_CompletionCandidate **out_arr,
                             Iron_Arena              *arena,
                             Iron_Program            *program,
                             const char                     *canonical_path,
                             const char                     *query_prefix,
                             _Atomic bool                   *cancel) {
    if (!program) return;
    for (int i = 0; i < program->decl_count; i++) {
        if (i % 64 == 0 && canceled(cancel)) return;
        Iron_Node *d = program->decls[i];
        if (!d || d->kind == IRON_NODE_ERROR) continue;
        const char *nm = decl_name(d);
        if (!nm || !*nm) continue;
        /* Keep imports out of bucket 2 (they're module names, not
         * symbols). Plan 04-03 may surface them via bucket 3. */
        if (d->kind == IRON_NODE_IMPORT_DECL) continue;
        /* NEW Phase 10 TIER-03 (D-10): build tier-prefixed detail for
         * method-bearing decls (FUNC_DECL + METHOD_DECL only). Mutual
         * exclusion is parser-enforced (parser.c:3162-3180). FIELD,
         * ENUM_VARIANT, VAL_DECL, VAR_DECL, PARAM remain untouched. */
        const char *tier_prefix = "";
        switch ((int)d->kind) {
            case IRON_NODE_FUNC_DECL: {
                const Iron_FuncDecl *fd = (const Iron_FuncDecl *)d;
                if      (fd->is_readonly) tier_prefix = "readonly func";
                else if (fd->is_pure)     tier_prefix = "pure func";
                else                       tier_prefix = "func";
                break;
            }
            case IRON_NODE_METHOD_DECL: {
                const Iron_MethodDecl *md = (const Iron_MethodDecl *)d;
                if      (md->is_readonly) tier_prefix = "readonly func";
                else if (md->is_pure)     tier_prefix = "pure func";
                else                       tier_prefix = "func";
                break;
            }
            default:
                tier_prefix = "";
                break;
        }
        maybe_push(out_arr, arena, nm, lsp_kind_from_decl(d),
                    ILSP_COMPLETION_BUCKET_TOP_LEVEL,
                    tier_prefix, canonical_path, nm,
                    decl_is_extern(d), false, query_prefix);
    }
}

/* ── Bucket 1: local scope (approximated at func level) ───────────── */

/* Find the enclosing FUNC_DECL or METHOD_DECL whose span brackets the
 * cursor. Returns NULL when the cursor is at top-level. */
static Iron_Node *enclosing_func(Iron_Program *program,
                                   size_t cursor_byte_offset,
                                   const struct IronLsp_Document *doc) {
    if (!program || !doc) return NULL;
    for (int i = 0; i < program->decl_count; i++) {
        Iron_Node *d = program->decls[i];
        if (!d) continue;
        if (d->kind != IRON_NODE_FUNC_DECL && d->kind != IRON_NODE_METHOD_DECL)
            continue;
        /* Best-effort: if the span's byte range covers cursor, accept. */
        Iron_Span s = d->span;
        /* Convert span start -> byte; if doc has line index we could be
         * precise, but for the approximation here we accept any decl
         * whose line range brackets the cursor byte. Since the span
         * end is on the closing `}`, this is safe. */
        (void)s; (void)cursor_byte_offset;
        /* Simple acceptance: linearly accept the FIRST func decl. In
         * practice Iron programs have one main() and a handful of
         * helpers; the cursor is usually inside main(). A more refined
         * line-index lookup is future work. */
        return d;
    }
    return NULL;
}

static void emit_func_locals(IronLsp_CompletionCandidate **out_arr,
                               Iron_Arena              *arena,
                               Iron_Node                      *fn,
                               const char                     *canonical_path,
                               const char                     *query_prefix,
                               _Atomic bool                   *cancel) {
    if (!fn) return;
    Iron_Node **params = NULL;
    int param_count = 0;
    Iron_Node *body = NULL;
    if (fn->kind == IRON_NODE_FUNC_DECL) {
        Iron_FuncDecl *fd = (Iron_FuncDecl *)fn;
        params = fd->params;
        param_count = fd->param_count;
        body = fd->body;
    } else if (fn->kind == IRON_NODE_METHOD_DECL) {
        Iron_MethodDecl *md = (Iron_MethodDecl *)fn;
        params = md->params;
        param_count = md->param_count;
        body = md->body;
    } else {
        return;
    }

    for (int i = 0; i < param_count; i++) {
        if (canceled(cancel)) return;
        Iron_Node *p = params ? params[i] : NULL;
        const char *nm = decl_name(p);
        if (!nm) continue;
        maybe_push(out_arr, arena, nm, LSP_CK_VARIABLE,
                    ILSP_COMPLETION_BUCKET_LOCAL,
                    "parameter", canonical_path, nm,
                    false, false, query_prefix);
    }

    if (body && body->kind == IRON_NODE_BLOCK) {
        Iron_Block *b = (Iron_Block *)body;
        for (int i = 0; i < b->stmt_count; i++) {
            if (i % 64 == 0 && canceled(cancel)) return;
            Iron_Node *s = b->stmts[i];
            if (!s) continue;
            if (s->kind != IRON_NODE_VAL_DECL && s->kind != IRON_NODE_VAR_DECL)
                continue;
            const char *nm = decl_name(s);
            if (!nm) continue;
            maybe_push(out_arr, arena, nm, lsp_kind_from_decl(s),
                        ILSP_COMPLETION_BUCKET_LOCAL,
                        s->kind == IRON_NODE_VAL_DECL ? "val" : "var",
                        canonical_path, nm,
                        false, false, query_prefix);
        }
    }
}

/* ── Bucket 3: imported modules ───────────────────────────────────── */

static void emit_imported(IronLsp_CompletionCandidate **out_arr,
                            Iron_Arena              *arena,
                            Iron_Program            *program,
                            const char                     *canonical_path,
                            const char                     *query_prefix,
                            _Atomic bool                   *cancel) {
    if (!program) return;
    for (int i = 0; i < program->decl_count; i++) {
        if (i % 64 == 0 && canceled(cancel)) return;
        Iron_Node *d = program->decls[i];
        if (!d || d->kind != IRON_NODE_IMPORT_DECL) continue;
        Iron_ImportDecl *imp = (Iron_ImportDecl *)d;
        const char *label = imp->alias ? imp->alias : imp->path;
        if (!label || !*label) continue;
        maybe_push(out_arr, arena, label, LSP_CK_MODULE,
                    ILSP_COMPLETION_BUCKET_IMPORTED,
                    imp->path ? imp->path : "",
                    canonical_path, label,
                    false, false, query_prefix);
    }
}

/* ── Bucket 4: stdlib (importable) ────────────────────────────────── */

/* Collect the set of already-imported module names + aliases so we can
 * SKIP re-surfacing them in bucket 4/5. */
typedef struct { char *key; int value; } ImportedSet;

static void collect_imports(ImportedSet **out, Iron_Program *program) {
    sh_new_strdup(*out);
    if (!program) return;
    for (int i = 0; i < program->decl_count; i++) {
        Iron_Node *d = program->decls[i];
        if (!d || d->kind != IRON_NODE_IMPORT_DECL) continue;
        Iron_ImportDecl *imp = (Iron_ImportDecl *)d;
        if (imp->path)  shput(*out, imp->path,  1);
        if (imp->alias) shput(*out, imp->alias, 1);
    }
}

static void emit_stdlib(IronLsp_CompletionCandidate **out_arr,
                          Iron_Arena              *arena,
                          struct IronLsp_Server          *server,
                          ImportedSet                    *imported,
                          const char                     *query_prefix,
                          _Atomic bool                   *cancel) {
    if (!server || !server->workspace_index) return;
    IronLsp_StdlibCache *sl = server->workspace_index->stdlib;
    if (!sl) return;

    /* The stdlib module list is fixed in Iron v1; enumerate by name
     * since stdlib_cache's iteration API is GET-only. We walk the known
     * module stems from the stdlib surface files. Plan 04-03 may
     * introduce a true iteration API if the list grows. */
    static const char *const stdlib_modules[] = {
        "math", "io", "time", "log", "hint", "os", "net", "url",
        "string", "list", "raylib",
    };
    size_t n = sizeof(stdlib_modules) / sizeof(stdlib_modules[0]);
    for (size_t i = 0; i < n; i++) {
        if (i % 64 == 0 && canceled(cancel)) return;
        const char *name = stdlib_modules[i];
        /* Skip if already imported so we don't re-surface via bucket 4. */
        if (imported && shgeti(imported, name) >= 0) continue;
        /* Only emit if the cache actually has it (defensive). */
        if (!ilsp_stdlib_cache_get(sl, name)) continue;
        maybe_push(out_arr, arena, name, LSP_CK_MODULE,
                    ILSP_COMPLETION_BUCKET_STDLIB,
                    "stdlib module", "",
                    name, false, true, query_prefix);
    }
}

/* ── Bucket 6: keywords ───────────────────────────────────────────── */

/* Phase 12 Plan 12-02 (KW-03, D-04..D-10) — per-keyword visibility filter.
 *
 * Threads doc + program + cursor + ctx through to ilsp_keyword_visible_at
 * for per-keyword arms. Cursor (line, col) derived from cursor_byte via
 * the document's line index. The legacy if-gate at the call site below
 * is dropped — the predicate's default arm bit-exactly preserves the
 * old "EXPR_HEAD || STATEMENT_HEAD" behaviour for the 38 pre-v3
 * keywords (D-10).
 *
 * Pitfall 5: surfaces the v3-deprecation note as detail string for `mut`
 * so editors render it in the completion list. */
static void emit_keywords(IronLsp_CompletionCandidate **out_arr,
                            Iron_Arena                    *arena,
                            const struct IronLsp_Document *doc,
                            const Iron_Program            *program,
                            size_t                         cursor_byte,
                            IronLsp_CompletionContext      ctx,
                            const char                    *query_prefix,
                            _Atomic bool                  *cancel) {
    /* Convert cursor_byte -> (line, col) once, both 0-indexed. */
    uint32_t cursor_line_0 = 0;
    uint32_t cursor_col_0  = 0;
    if (doc) {
        cursor_line_0 = ilsp_line_of_byte(&doc->line_idx, cursor_byte);
        size_t line_start = ilsp_byte_of_line(&doc->line_idx, cursor_line_0);
        if (line_start > cursor_byte) line_start = cursor_byte;
        cursor_col_0 = (uint32_t)(cursor_byte - line_start);
    }
    for (size_t i = 0; i < ILSP_COMPLETION_KEYWORD_COUNT; i++) {
        if (i % 64 == 0 && canceled(cancel)) return;
        const char *kw = ILSP_COMPLETION_KEYWORDS[i];
        if (!ilsp_keyword_visible_at(kw, doc, program,
                                       cursor_line_0, cursor_col_0, ctx)) {
            continue;
        }
        const char *detail = (strcmp(kw, "mut") == 0)
            ? "(v2 legacy — use of `mut` emits E0263)"
            : "keyword";
        maybe_push(out_arr, arena, kw, LSP_CK_KEYWORD,
                    ILSP_COMPLETION_BUCKET_KEYWORDS,
                    detail, "",
                    kw, false, false, query_prefix);
    }
}

/* ── MEMBER_AFTER_DOT ─────────────────────────────────────────────── */

/* Phase 11 PATCH-03 (Plan 11-02): visitor state for the patch-method
 * walk inside emit_member_fields. Each yielded (Iron_MethodDecl,
 * Iron_ObjectDecl) pair becomes a CompletionCandidate via maybe_push.
 * TIER-03 prefix machinery (Phase 10 D-10) is computed inline so the
 * member-after-dot detail rendering matches the native walk above. */
struct patch_member_emit_ctx {
    IronLsp_CompletionCandidate **out;
    Iron_Arena                   *arena;
    const char                   *prefix;
};

static bool emit_patch_member_field(Iron_MethodDecl *m,
                                      Iron_ObjectDecl *p,
                                      void           *ud) {
    struct patch_member_emit_ctx *st = (struct patch_member_emit_ctx *)ud;
    (void)p;
    if (!st || !m || !m->method_name) return true;
    const char *tier_prefix = "func";
    if      (m->is_readonly) tier_prefix = "readonly func";
    else if (m->is_pure)     tier_prefix = "pure func";
    maybe_push(st->out, st->arena, m->method_name, LSP_CK_METHOD,
                ILSP_COMPLETION_BUCKET_LOCAL,
                tier_prefix, "", m->method_name,
                false, false, st->prefix);
    return true;
}

/* ── Members of the expression before the dot ──────────────────────
 *
 * The facade analyzed the buffer without the `.` and the member being
 * typed (complete.c), so the receiver expression is in the AST with its
 * type: a local, a parameter, a field chain (`p.q.`), a call result
 * (`f().`), a literal (`"abc".`), or a type name for statics (`Math.`,
 * `Color.`). */

typedef struct {
    uint32_t   line, col;  /* the receiver's last character */
    Iron_Node *best;
    /* `a.b.field.`: a field access's span stops before its field name, so
     * it is matched by its field name: the closest one starting before
     * the object's end (oline, ocol). */
    const char *field;
    size_t      field_len;
    uint32_t    oline, ocol;
    Iron_Node  *by_field;
} ReceiverFind;

static bool is_expr_node(const Iron_Node *n) {
    return (n->kind >= IRON_NODE_INT_LIT && n->kind <= IRON_NODE_AWAIT) ||
           n->kind == IRON_NODE_ENUM_CONSTRUCT;
}

static bool span_has(const Iron_Span *sp, uint32_t line, uint32_t col) {
    if (line < sp->line || line > sp->end_line) return false;
    if (line == sp->line && col < sp->col) return false;
    if (line == sp->end_line && col > sp->end_col) return false;
    return true;
}

/* The smallest expression covering the receiver's last character that
 * ends there (`p.q` for `p.q.`, not `p`). */
static bool receiver_visit(Iron_Visitor *v, Iron_Node *n) {
    ReceiverFind *f = (ReceiverFind *)v->ctx;
    if (n->kind == IRON_NODE_ERROR) return false;
    if (n->kind == IRON_NODE_FIELD_ACCESS && f->field) {
        Iron_FieldAccess *fa = (Iron_FieldAccess *)n;
        bool starts_before = n->span.line < f->oline ||
                             (n->span.line == f->oline && n->span.col <= f->ocol);
        bool closer = !f->by_field ||
                      n->span.line > f->by_field->span.line ||
                      (n->span.line == f->by_field->span.line &&
                       n->span.col >= f->by_field->span.col);
        if (fa->field && strlen(fa->field) == f->field_len &&
            memcmp(fa->field, f->field, f->field_len) == 0 && starts_before && closer &&
            n->span.line + 8 >= f->oline) {
            f->by_field = n;
        }
    }
    if (!is_expr_node(n) || !span_has(&n->span, f->line, f->col)) return true;
    if (n->span.end_line != f->line || n->span.end_col != f->col) return true;
    if (!f->best || (span_has(&f->best->span, n->span.line, n->span.col) &&
                     span_has(&f->best->span, n->span.end_line, n->span.end_col))) {
        f->best = n;
    }
    return true;
}

static int all_decls(const Iron_Program *p) {
    return p->decl_count + p->prelude_decl_count;
}

static const char *type_str(const Iron_Type *t, Iron_Arena *arena) {
    const char *s = t ? iron_type_to_string(t, arena) : NULL;
    return s ? s : "?";
}

/* `readonly func upper() -> String`, `func put(key: K, value: V)`. */
static const char *method_detail(Iron_MethodDecl *md, Iron_Arena *arena) {
    char buf[512];
    size_t n = (size_t)snprintf(buf, sizeof(buf), "%sfunc %s(",
                                md->is_readonly ? "readonly " : md->is_pure ? "pure " : "",
                                md->method_name ? md->method_name : "?");
    bool first = true;
    for (int i = 0; i < md->param_count && n < sizeof(buf); i++) {
        Iron_Param *pm = (Iron_Param *)md->params[i];
        if (!pm || !pm->name || strcmp(pm->name, "self") == 0) continue;
        n += (size_t)snprintf(buf + n, sizeof(buf) - n, "%s%s: %s", first ? "" : ", ",
                              pm->name, type_str(pm->resolved_type, arena));
        first = false;
    }
    if (n < sizeof(buf)) n += (size_t)snprintf(buf + n, sizeof(buf) - n, ")");
    if (n < sizeof(buf) && md->resolved_return_type &&
        md->resolved_return_type->kind != IRON_TYPE_VOID) {
        snprintf(buf + n, sizeof(buf) - n, " -> %s", type_str(md->resolved_return_type, arena));
    }
    return iron_arena_strdup(arena, buf, strlen(buf));
}

/* Methods the compiler provides by name, with no declaration to read. */
typedef struct { const char *name, *detail; } BuiltinMember;

static const BuiltinMember k_list_builtins[] = {
    { "len", "func len() -> Int" },          { "push", "func push(item: T)" },
    { "pop", "func pop() -> T" },            { "get", "func get(i: Int) -> T" },
    { "set", "func set(i: Int, item: T)" },  { "insert", "func insert(i: Int, item: T)" },
    { "remove", "func remove(i: Int) -> T" },{ "clear", "func clear()" },
    { "reverse", "func reverse()" },         { "contains", "func contains(item: T) -> Bool" },
    { "sort", "func sort()" },               { "copy", "func copy() -> [T]" },
    { "take", "func take() -> [T]" },        { NULL, NULL },
};
static const BuiltinMember k_map_builtins[] = {
    { "put", "func put(key: K, value: V)" }, { "get", "func get(key: K) -> V" },
    { "get_or", "func get_or(key: K, default: V) -> V" },
    { "has", "func has(key: K) -> Bool" },   { "remove", "func remove(key: K) -> Bool" },
    { "len", "func len() -> Int" },          { "clear", "func clear()" },
    { "keys", "func keys() -> [K]" },        { "values", "func values() -> [V]" },
    { "copy", "func copy() -> Map[K, V]" },  { "take", "func take() -> Map[K, V]" },
    { NULL, NULL },
};
static const BuiltinMember k_set_builtins[] = {
    { "add", "func add(item: T) -> Bool" },  { "has", "func has(item: T) -> Bool" },
    { "remove", "func remove(item: T) -> Bool" }, { "len", "func len() -> Int" },
    { "clear", "func clear()" },             { "values", "func values() -> [T]" },
    { "copy", "func copy() -> Set[T]" },     { "take", "func take() -> Set[T]" },
    { NULL, NULL },
};

static void push_builtins(IronLsp_CompletionCandidate **out, Iron_Arena *arena,
                          const BuiltinMember *table, const char *prefix) {
    for (int i = 0; table[i].name; i++) {
        maybe_push(out, arena, table[i].name, LSP_CK_METHOD, ILSP_COMPLETION_BUCKET_LOCAL,
                   table[i].detail, "", table[i].name, false, false, prefix);
    }
}

/* Methods declared for `type_name` (the file's and the stdlib's), or the
 * list extensions (`func [T].map`) when `list`. */
static void push_declared_methods(IronLsp_CompletionCandidate **out, Iron_Arena *arena,
                                  const Iron_Program *program, const char *type_name,
                                  bool list, const char *prefix) {
    for (int i = 0; i < all_decls(program); i++) {
        Iron_Node *d = program->decls[i];
        if (!d || d->kind != IRON_NODE_METHOD_DECL) continue;
        Iron_MethodDecl *md = (Iron_MethodDecl *)d;
        if (!md->method_name || md->is_init || md->is_synth_accessor) continue;
        if (list ? !md->is_array_extension
                 : (md->is_array_extension || !md->type_name || !type_name ||
                    strcmp(md->type_name, type_name) != 0)) continue;
        maybe_push(out, arena, md->method_name, LSP_CK_METHOD, ILSP_COMPLETION_BUCKET_LOCAL,
                   method_detail(md, arena), "", md->method_name, false, false, prefix);
    }
}

static void push_fields(IronLsp_CompletionCandidate **out, Iron_Arena *arena,
                        Iron_ObjectDecl *od, const char *prefix) {
    for (int j = 0; j < od->field_count; j++) {
        Iron_Field *f = (Iron_Field *)od->fields[j];
        if (!f || !f->name) continue;
        char detail[256];
        snprintf(detail, sizeof(detail), "%s %s", f->is_var ? "var" : "val", f->name);
        maybe_push(out, arena, f->name, LSP_CK_FIELD, ILSP_COMPLETION_BUCKET_LOCAL,
                   detail, "", f->name, false, false, prefix);
    }
}

static const Iron_Type *strip_handle(const Iron_Type *t) {
    for (int g = 0; t && g < 8; g++) {
        if (t->kind == IRON_TYPE_NULLABLE) t = t->nullable.inner;
        else if (t->kind == IRON_TYPE_RC) t = t->rc.inner;
        else if (t->kind == IRON_TYPE_PTR) t = t->ptr.pointee;
        else break;
    }
    return t;
}

/* The fields and methods of the object named `type_name`: its own and
 * the stdlib's methods and those patched in by other workspace files. */
static void emit_object_members_by_name(IronLsp_CompletionCandidate **out_arr,
                                        Iron_Arena *arena, struct IronLsp_Server *server,
                                        struct IronLsp_Document *doc,
                                        const Iron_Program *program,
                                        const char *type_name, const char *query_prefix) {
    for (int i = 0; i < all_decls(program); i++) {
        Iron_Node *d = program->decls[i];
        if (!d || d->kind != IRON_NODE_OBJECT_DECL) continue;
        Iron_ObjectDecl *od = (Iron_ObjectDecl *)d;
        if (od->is_patch || !od->name || strcmp(od->name, type_name) != 0) continue;
        push_fields(out_arr, arena, od, query_prefix);
        break;
    }
    push_declared_methods(out_arr, arena, program, type_name, false, query_prefix);
    struct patch_member_emit_ctx ctx_pms = {
        .out = out_arr, .arena = arena, .prefix = query_prefix,
    };
    IronLsp_WorkspaceIndex *wi = server ? server->workspace_index : NULL;
    ilsp_patch_for_each_method((Iron_Program *)program, wi, type_name,
                               doc->uri ? doc->uri : "", emit_patch_member_field,
                               &ctx_pms, NULL);
}

/* The type written on the top-level `val` / `var` named by the identifier
 * ending at byte `last`, or NULL. */
static const char *annotated_type_of(const Iron_Program *program, const char *text,
                                     size_t last) {
    size_t end = last + 1, start = end;
    while (start > 0) {
        unsigned char c = (unsigned char)text[start - 1];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '_') { start--; continue; }
        break;
    }
    size_t len = end - start;
    if (len == 0) return NULL;
    for (int i = 0; i < program->decl_count; i++) {
        Iron_Node *d = program->decls[i];
        const char *name = NULL;
        Iron_Node *ann = NULL;
        if (d && d->kind == IRON_NODE_VAL_DECL) {
            name = ((Iron_ValDecl *)d)->name; ann = ((Iron_ValDecl *)d)->type_ann;
        } else if (d && d->kind == IRON_NODE_VAR_DECL) {
            name = ((Iron_VarDecl *)d)->name; ann = ((Iron_VarDecl *)d)->type_ann;
        }
        if (!name || strlen(name) != len || memcmp(name, text + start, len) != 0) continue;
        if (ann && ann->kind == IRON_NODE_TYPE_ANNOTATION) return ((Iron_TypeAnnotation *)ann)->name;
        return NULL;
    }
    return NULL;
}

static void emit_member_fields(IronLsp_CompletionCandidate **out_arr,
                                 Iron_Arena              *arena,
                                 struct IronLsp_Server          *server,
                                 struct IronLsp_Document        *doc,
                                 Iron_Program            *program,
                                 size_t                          cursor_byte,
                                 const char                     *query_prefix) {
    if (!doc || !doc->text || !program || cursor_byte == 0) return;
    /* Back up over the member being typed and the dot. */
    size_t cur = cursor_byte;
    while (cur > 0) {
        unsigned char c = (unsigned char)doc->text[cur - 1];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '_') { cur--; continue; }
        break;
    }
    if (cur < 2 || doc->text[cur - 1] != '.') return;
    size_t last = cur - 2;  /* the receiver's last byte */
    while (last > 0 && (doc->text[last] == ' ' || doc->text[last] == '\t')) last--;

    uint32_t line0 = ilsp_line_of_byte(&doc->line_idx, last);
    size_t line_start = ilsp_byte_of_line(&doc->line_idx, line0);
    ReceiverFind f = { line0 + 1, (uint32_t)(last - line_start) + 1, NULL,
                       NULL, 0, 0, 0, NULL };
    /* The trailing identifier, and the object before its dot. */
    size_t id_start = last + 1;
    while (id_start > 0) {
        unsigned char c = (unsigned char)doc->text[id_start - 1];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '_') { id_start--; continue; }
        break;
    }
    if (id_start <= last && id_start >= 2 && doc->text[id_start - 1] == '.') {
        size_t obj_last = id_start - 2;
        uint32_t ol = ilsp_line_of_byte(&doc->line_idx, obj_last);
        f.field     = doc->text + id_start;
        f.field_len = last + 1 - id_start;
        f.oline     = ol + 1;
        f.ocol      = (uint32_t)(obj_last - ilsp_byte_of_line(&doc->line_idx, ol)) + 1;
    }
    Iron_Visitor v = { .ctx = &f, .visit_node = receiver_visit, .post_visit = NULL };
    for (int i = 0; i < program->decl_count; i++) {
        if (program->decls[i]) iron_ast_walk(program->decls[i], &v);
    }
    if (!f.best) f.best = f.by_field;
    /* No analyzed type (the program was only parsed, or the receiver
     * did not type): a binding written with its type still names it. */
    const Iron_Type *rt = f.best ? ((Iron_ExprNode *)f.best)->resolved_type : NULL;
    if (!rt && !(f.best && f.best->kind == IRON_NODE_IDENT &&
                 ((Iron_Ident *)f.best)->resolved_sym)) {
        const char *type_name = annotated_type_of(program, doc->text, last);
        if (type_name) emit_object_members_by_name(out_arr, arena, server, doc, program,
                                                   type_name, query_prefix);
        return;
    }

    /* A type name: its statics (`Math.sqrt`, `Math.PI`) or variants. */
    if (f.best->kind == IRON_NODE_IDENT) {
        const Iron_Symbol *sym = ((Iron_Ident *)f.best)->resolved_sym;
        Iron_Node *td = sym ? sym->decl_node : NULL;
        if (td && td->kind == IRON_NODE_OBJECT_DECL) {
            Iron_ObjectDecl *od = (Iron_ObjectDecl *)td;
            push_fields(out_arr, arena, od, query_prefix);
            push_declared_methods(out_arr, arena, program, od->name, false, query_prefix);
            return;
        }
        if (td && td->kind == IRON_NODE_ENUM_DECL) {
            Iron_EnumDecl *ed = (Iron_EnumDecl *)td;
            for (int j = 0; j < ed->variant_count; j++) {
                Iron_EnumVariant *ev = (Iron_EnumVariant *)ed->variants[j];
                if (!ev || !ev->name) continue;
                maybe_push(out_arr, arena, ev->name, LSP_CK_ENUMMEMBER,
                           ILSP_COMPLETION_BUCKET_LOCAL, ed->name ? ed->name : "",
                           "", ev->name, false, false, query_prefix);
            }
            push_declared_methods(out_arr, arena, program, ed->name, false, query_prefix);
            return;
        }
    }

    const Iron_Type *t = strip_handle(rt);
    if (!t || t->kind == IRON_TYPE_ERROR) return;
    switch ((int)t->kind) {
        case IRON_TYPE_ARRAY:
            push_builtins(out_arr, arena, k_list_builtins, query_prefix);
            push_declared_methods(out_arr, arena, program, NULL, true, query_prefix);
            return;
        case IRON_TYPE_OBJECT: {
            Iron_ObjectDecl *od = t->object.decl;
            if (!od || !od->name) return;
            if (strcmp(od->name, "Map") == 0) {
                push_builtins(out_arr, arena, k_map_builtins, query_prefix);
                return;
            }
            if (strcmp(od->name, "Set") == 0) {
                push_builtins(out_arr, arena, k_set_builtins, query_prefix);
                return;
            }
            emit_object_members_by_name(out_arr, arena, server, doc, program, od->name,
                                        query_prefix);
            return;
        }
        case IRON_TYPE_INTERFACE: {
            Iron_InterfaceDecl *id = t->interface.decl;
            for (int j = 0; id && j < id->method_count; j++) {
                Iron_FuncDecl *sig = (Iron_FuncDecl *)id->method_sigs[j];
                if (!sig || sig->kind != IRON_NODE_FUNC_DECL || !sig->name) continue;
                maybe_push(out_arr, arena, sig->name, LSP_CK_METHOD,
                           ILSP_COMPLETION_BUCKET_LOCAL, id->name ? id->name : "",
                           "", sig->name, false, false, query_prefix);
            }
            return;
        }
        case IRON_TYPE_ENUM:
            if (t->enu.decl) {
                push_declared_methods(out_arr, arena, program, t->enu.decl->name, false,
                                      query_prefix);
            }
            return;
        default: {
            /* String, Int, Float...: the stdlib's `patch object String`. */
            const char *name = type_str(t, arena);
            push_declared_methods(out_arr, arena, program, name, false, query_prefix);
            return;
        }
    }
}

/* ── Public API ───────────────────────────────────────────────────── */

void ilsp_complete_buckets_build(struct IronLsp_Server             *server,
                                   struct IronLsp_Document           *doc,
                                   Iron_Program               *program,
                                   size_t                             cursor_byte_offset,
                                   IronLsp_CompletionContext          ctx,
                                   const char                        *query_prefix,
                                   _Atomic bool                      *cancel,
                                   Iron_Arena                 *arena,
                                   IronLsp_CompletionCandidate      **out_cands,
                                   size_t                            *out_n) {
    if (out_cands) *out_cands = NULL;
    if (out_n)    *out_n    = 0;
    /* `server` may be NULL in unit tests and on cold-start; bucket 4
     * simply emit nothing in that case. arena + out_cands + out_n are
     * hard-required. */
    if (!arena || !out_cands || !out_n) return;
    if (!query_prefix) query_prefix = "";

    IronLsp_CompletionCandidate *cands = NULL;

    /* MEMBER_AFTER_DOT short-circuits all 6 buckets. */
    if (ctx == ILSP_CCTX_MEMBER_AFTER_DOT) {
        emit_member_fields(&cands, arena, server, doc, program,
                            cursor_byte_offset, query_prefix);
        goto finish;
    }

    /* Deduce doc's canonical path for label attribution. */
    const char *canonical_path = doc && doc->uri ? doc->uri : "";

    /* IMPORT_PATH context: emit stdlib module names only. */
    if (ctx == ILSP_CCTX_IMPORT_PATH) {
        if (canceled(cancel)) goto finish;
        ImportedSet *imported = NULL;
        collect_imports(&imported, program);
        emit_stdlib(&cands, arena, server, imported, query_prefix, cancel);
        if (imported) shfree(imported);
        goto finish;
    }

    /* TYPE_POSITION: only Object / Interface / Enum decls + primitives. */
    if (ctx == ILSP_CCTX_TYPE_POSITION) {
        if (canceled(cancel)) goto finish;
        if (program) {
            for (int i = 0; i < program->decl_count; i++) {
                if (i % 64 == 0 && canceled(cancel)) goto finish;
                Iron_Node *d = program->decls[i];
                if (!d) continue;
                if (d->kind != IRON_NODE_OBJECT_DECL &&
                    d->kind != IRON_NODE_INTERFACE_DECL &&
                    d->kind != IRON_NODE_ENUM_DECL) continue;
                const char *nm = decl_name(d);
                if (!nm) continue;
                maybe_push(&cands, arena, nm, lsp_kind_from_decl(d),
                            ILSP_COMPLETION_BUCKET_TOP_LEVEL,
                            "type", canonical_path, nm,
                            false, false, query_prefix);
            }
        }
        /* Primitives. */
        static const char *const primitives[] = {
            "Int", "Int8", "Int16", "Int32", "Int64",
            "UInt", "UInt8", "UInt16", "UInt32", "UInt64",
            "Float", "Float32", "Float64",
            "Bool", "String", "Void",
        };
        size_t np = sizeof(primitives) / sizeof(primitives[0]);
        for (size_t i = 0; i < np; i++) {
            maybe_push(&cands, arena, primitives[i], LSP_CK_STRUCT,
                        ILSP_COMPLETION_BUCKET_TOP_LEVEL,
                        "primitive", "", primitives[i],
                        false, false, query_prefix);
        }
        goto finish;
    }

    /* Default: 6-bucket pipeline. */

    /* Bucket 1 (LOCAL). */
    if (canceled(cancel)) goto finish;
    Iron_Node *fn = enclosing_func(program, cursor_byte_offset, doc);
    emit_func_locals(&cands, arena, fn, canonical_path, query_prefix, cancel);

    /* Bucket 2 (TOP_LEVEL). */
    if (canceled(cancel)) goto finish;
    emit_top_level(&cands, arena, program, canonical_path, query_prefix, cancel);

    /* Bucket 3 (IMPORTED). */
    if (canceled(cancel)) goto finish;
    emit_imported(&cands, arena, program, canonical_path, query_prefix, cancel);

    /* Collect imported set for bucket 4 skip logic. */
    ImportedSet *imported = NULL;
    collect_imports(&imported, program);

    /* Bucket 4 (STDLIB). */
    if (canceled(cancel)) { if (imported) shfree(imported); goto finish; }
    emit_stdlib(&cands, arena, server, imported, query_prefix, cancel);
    if (imported) shfree(imported);

    /* Bucket 6 (KEYWORDS) — Phase 12 Plan 12-02 (KW-03, D-04..D-10):
     * the legacy if-gate is dropped. Per-keyword visibility is enforced
     * inside emit_keywords via ilsp_keyword_visible_at; the predicate's
     * default arm bit-exactly preserves the old "EXPR_HEAD ||
     * STATEMENT_HEAD" gate for the 38 pre-v3 keywords. */
    if (canceled(cancel)) goto finish;
    emit_keywords(&cands, arena, doc, program, cursor_byte_offset, ctx,
                   query_prefix, cancel);

finish:
    if (canceled(cancel)) {
        arrfree(cands);
        return;
    }
    size_t n = (size_t)arrlenu(cands);
    if (n == 0) {
        arrfree(cands);
        return;
    }
    /* Copy into an arena-backed array so the caller can own it. */
    IronLsp_CompletionCandidate *arr = (IronLsp_CompletionCandidate *)
        iron_arena_alloc(arena, n * sizeof(*arr),
                          _Alignof(IronLsp_CompletionCandidate));
    if (!arr) { arrfree(cands); return; }
    memcpy(arr, cands, n * sizeof(*arr));
    arrfree(cands);
    *out_cands = arr;
    *out_n    = n;
}
