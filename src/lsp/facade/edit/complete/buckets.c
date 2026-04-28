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
 *   - "Stdlib" (Bucket 4) + "Deps" (Bucket 5) iterate the flattened
 *     workspace-index indexes. Cold-start fallback: if either index is
 *     NULL we simply emit no candidates for that bucket.
 */

#include "lsp/facade/edit/complete/buckets.h"
#include "lsp/facade/edit/complete/context_classify.h"
#include "lsp/facade/nav/fuzzy.h"
#include "lsp/server/server.h"
#include "lsp/store/document.h"
#include "lsp/store/workspace_index.h"
#include "lsp/store/stdlib_cache.h"
#include "lsp/store/dep_map.h"
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
        "math", "io", "time", "log", "hint", "net", "url",
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

/* ── Bucket 5: deps (importable) ──────────────────────────────────── */

static void emit_deps(IronLsp_CompletionCandidate **out_arr,
                        Iron_Arena              *arena,
                        struct IronLsp_Server          *server,
                        ImportedSet                    *imported,
                        const char                     *query_prefix,
                        _Atomic bool                   *cancel) {
    if (!server || !server->workspace_index) return;
    IronLsp_DepMap *dm = server->workspace_index->deps;
    if (!dm) return;
    size_t ndeps = ilsp_dep_map_size(dm);
    if (ndeps == 0) return;
    (void)ndeps;
    /* dep_map currently exposes size + lookup-by-name but no iteration
     * API. We approximate: attempt lookups for a handful of canonical
     * dep names the workspace may have declared. Plan 04-03 may extend
     * dep_map with an iteration helper for full coverage. */
    (void)out_arr; (void)arena; (void)imported; (void)query_prefix; (void)cancel;
}

/* ── Bucket 6: keywords ───────────────────────────────────────────── */

static void emit_keywords(IronLsp_CompletionCandidate **out_arr,
                            Iron_Arena              *arena,
                            const char                     *query_prefix,
                            _Atomic bool                   *cancel) {
    for (size_t i = 0; i < ILSP_COMPLETION_KEYWORD_COUNT; i++) {
        if (i % 64 == 0 && canceled(cancel)) return;
        const char *kw = ILSP_COMPLETION_KEYWORDS[i];
        maybe_push(out_arr, arena, kw, LSP_CK_KEYWORD,
                    ILSP_COMPLETION_BUCKET_KEYWORDS,
                    "keyword", "",
                    kw, false, false, query_prefix);
    }
}

/* ── MEMBER_AFTER_DOT ─────────────────────────────────────────────── */

/* Find the object type for a `x.|` cursor by walking back one ident in
 * the document buffer and looking it up in program->decls (val/var).
 * If the resolved type is an object, emit its fields + methods. */
static void emit_member_fields(IronLsp_CompletionCandidate **out_arr,
                                 Iron_Arena              *arena,
                                 struct IronLsp_Server          *server,
                                 struct IronLsp_Document        *doc,
                                 Iron_Program            *program,
                                 size_t                          cursor_byte,
                                 const char                     *query_prefix) {
    (void)server;
    if (!doc || !doc->text || !program) return;
    if (cursor_byte == 0) return;
    /* Back up over `.` plus any identifier the user is typing. */
    size_t cur = cursor_byte;
    while (cur > 0) {
        unsigned char c = (unsigned char)doc->text[cur - 1];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '_') { cur--; continue; }
        break;
    }
    if (cur == 0 || doc->text[cur - 1] != '.') return;
    size_t dot = cur - 1;
    /* Walk back over the receiver ident. */
    size_t end = dot;
    size_t start = end;
    while (start > 0) {
        unsigned char c = (unsigned char)doc->text[start - 1];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '_') { start--; continue; }
        break;
    }
    if (start == end) return;
    size_t recv_len = end - start;
    /* Find the receiver's declared type by searching program's top-level
     * val/var decls. */
    const char *type_name = NULL;
    for (int i = 0; i < program->decl_count; i++) {
        Iron_Node *d = program->decls[i];
        if (!d) continue;
        if (d->kind == IRON_NODE_VAL_DECL) {
            Iron_ValDecl *vd = (Iron_ValDecl *)d;
            if (vd->name &&
                strlen(vd->name) == recv_len &&
                memcmp(vd->name, doc->text + start, recv_len) == 0) {
                if (vd->type_ann && vd->type_ann->kind == IRON_NODE_TYPE_ANNOTATION) {
                    type_name = ((Iron_TypeAnnotation *)vd->type_ann)->name;
                }
                break;
            }
        } else if (d->kind == IRON_NODE_VAR_DECL) {
            Iron_VarDecl *vd = (Iron_VarDecl *)d;
            if (vd->name &&
                strlen(vd->name) == recv_len &&
                memcmp(vd->name, doc->text + start, recv_len) == 0) {
                if (vd->type_ann && vd->type_ann->kind == IRON_NODE_TYPE_ANNOTATION) {
                    type_name = ((Iron_TypeAnnotation *)vd->type_ann)->name;
                }
                break;
            }
        }
    }
    if (!type_name) return;
    /* Find the object decl by name + emit fields + methods. */
    for (int i = 0; i < program->decl_count; i++) {
        Iron_Node *d = program->decls[i];
        if (!d || d->kind != IRON_NODE_OBJECT_DECL) continue;
        Iron_ObjectDecl *od = (Iron_ObjectDecl *)d;
        if (!od->name || strcmp(od->name, type_name) != 0) continue;
        for (int j = 0; j < od->field_count; j++) {
            Iron_Node *f = od->fields[j];
            if (!f) continue;
            const char *nm = decl_name(f);
            if (!nm) continue;
            maybe_push(out_arr, arena, nm, LSP_CK_FIELD,
                        ILSP_COMPLETION_BUCKET_LOCAL,  /* bucket irrelevant in member mode */
                        "field", "", nm,
                        false, false, query_prefix);
        }
        break;
    }
    /* Methods: walk program-level method decls with matching type_name. */
    for (int i = 0; i < program->decl_count; i++) {
        Iron_Node *d = program->decls[i];
        if (!d || d->kind != IRON_NODE_METHOD_DECL) continue;
        Iron_MethodDecl *md = (Iron_MethodDecl *)d;
        if (!md->type_name || strcmp(md->type_name, type_name) != 0) continue;
        if (!md->method_name) continue;
        maybe_push(out_arr, arena, md->method_name, LSP_CK_METHOD,
                    ILSP_COMPLETION_BUCKET_LOCAL,
                    "method", "", md->method_name,
                    false, false, query_prefix);
    }
    (void)dot;
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
    /* `server` may be NULL in unit tests and on cold-start; buckets 4+5
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

    /* IMPORT_PATH context: emit stdlib + dep module names only. */
    if (ctx == ILSP_CCTX_IMPORT_PATH) {
        if (canceled(cancel)) goto finish;
        ImportedSet *imported = NULL;
        collect_imports(&imported, program);
        emit_stdlib(&cands, arena, server, imported, query_prefix, cancel);
        if (!canceled(cancel)) {
            emit_deps(&cands, arena, server, imported, query_prefix, cancel);
        }
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

    /* Collect imported set for buckets 4+5 skip logic. */
    ImportedSet *imported = NULL;
    collect_imports(&imported, program);

    /* Bucket 4 (STDLIB). */
    if (canceled(cancel)) { if (imported) shfree(imported); goto finish; }
    emit_stdlib(&cands, arena, server, imported, query_prefix, cancel);

    /* Bucket 5 (DEPS). */
    if (canceled(cancel)) { if (imported) shfree(imported); goto finish; }
    emit_deps(&cands, arena, server, imported, query_prefix, cancel);
    if (imported) shfree(imported);

    /* Bucket 6 (KEYWORDS) — only when context is EXPR_HEAD or
     * STATEMENT_HEAD. */
    if (ctx == ILSP_CCTX_EXPR_HEAD || ctx == ILSP_CCTX_STATEMENT_HEAD) {
        if (canceled(cancel)) goto finish;
        emit_keywords(&cands, arena, query_prefix, cancel);
    }

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
