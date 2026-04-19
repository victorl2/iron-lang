/* Phase 4 Plan 04-02 Task 02 (EDIT-01, D-01, D-17) -- completion facade
 * orchestrator implementation.
 *
 * Routes through ilsp_facade_compile_for_nav so the single
 * iron_analyze_buffer call site invariant (CORE-22) stays intact; no
 * new analyzer call sites are introduced. On cold-start (program
 * returned NULL) we fall back to parser-only identifier extraction +
 * keyword bucket so the user still gets a useful list during the
 * initial warm-up window.
 */

#include "lsp/facade/edit/complete/complete.h"
#include "lsp/facade/edit/complete/buckets.h"
#include "lsp/facade/edit/complete/context_classify.h"
#include "lsp/facade/edit/complete/auto_import.h"
#include "lsp/facade/edit/complete/snippet.h"
#include "lsp/facade/compile.h"
#include "lsp/server/server.h"
#include "lsp/store/document.h"
#include "lsp/store/line_index.h"
#include "lsp/store/utf.h"
#include "diagnostics/diagnostics.h"
#include "parser/ast.h"
#include "util/arena.h"

#include <math.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Byte math ────────────────────────────────────────────────────── */

static size_t pos_to_byte(const struct IronLsp_Document *doc,
                            IronLsp_Position pos,
                            IronLsp_PositionEncoding enc) {
    if (!doc || !doc->text) return 0;
    size_t line_start = ilsp_byte_of_line(&doc->line_idx, pos.line);
    if (line_start > doc->text_len) return doc->text_len;
    size_t line_end = line_start;
    while (line_end < doc->text_len && doc->text[line_end] != '\n') line_end++;
    const char *line = doc->text + line_start;
    size_t line_len = line_end - line_start;
    size_t byte;
    if (enc == ILSP_ENC_UTF16) {
        byte = ilsp_utf16_column_to_utf8_byte(line, line_len, pos.character);
    } else {
        byte = ilsp_utf8_column_to_utf8_byte(line, line_len, pos.character);
    }
    if (byte > line_len) byte = line_len;
    return line_start + byte;
}

/* Walk backward over identifier characters to extract the query prefix
 * the user is actively typing. Returns an arena-owned NUL-terminated
 * string (empty string if cursor is not inside an ident). */
static const char *extract_query_prefix(const struct IronLsp_Document *doc,
                                          size_t byte_off,
                                          Iron_Arena *arena) {
    if (!doc || !doc->text || byte_off > doc->text_len) return "";
    size_t end = byte_off;
    size_t start = end;
    while (start > 0) {
        unsigned char c = (unsigned char)doc->text[start - 1];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '_') { start--; continue; }
        break;
    }
    if (start == end) return "";
    return iron_arena_strdup(arena, doc->text + start, end - start);
}

/* ── Plan 04-03: auto-import + snippet wiring ─────────────────────── */

/* Find the top-level decl in `program` whose `name()` matches `label`.
 * Used to recover snippet metadata (param names / field names) for
 * candidates pushed by the bucket builder without a decl_node
 * reference. Returns NULL when no matching decl exists (e.g. for
 * stdlib / dep bucket candidates; those are handled by auto-import,
 * not by snippet rendering, except when the candidate maps to a
 * module-scoped symbol that exists in another TU -- out of scope
 * for this plan). */
static const Iron_Node *find_top_level_decl_by_name(const Iron_Program *program,
                                                      const char *name) {
    if (!program || !name || !*name) return NULL;
    for (int i = 0; i < program->decl_count; i++) {
        Iron_Node *d = program->decls[i];
        if (!d || d->kind == IRON_NODE_ERROR) continue;
        const char *dn = NULL;
        /* Only the declaring kinds above carry a `name` we care to
         * match. Every other Iron_NodeKind (statements, expressions,
         * helpers, etc.) is skipped. Cast to int to suppress
         * `-Werror=switch-enum` exhaustiveness (pattern used by
         * nav/document_symbol.c). */
        switch ((int)d->kind) {
            case IRON_NODE_FUNC_DECL:
                dn = ((const Iron_FuncDecl *)d)->name; break;
            case IRON_NODE_METHOD_DECL:
                dn = ((const Iron_MethodDecl *)d)->method_name; break;
            case IRON_NODE_OBJECT_DECL:
                dn = ((const Iron_ObjectDecl *)d)->name; break;
            case IRON_NODE_ENUM_DECL:
                dn = ((const Iron_EnumDecl *)d)->name; break;
            default:
                dn = NULL; break;
        }
        if (dn && strcmp(dn, name) == 0) return d;
    }
    return NULL;
}

/* Derive a module stem from a stdlib/dep canonical path + name_path.
 * For bucket-4 (stdlib) the bucket builder sets canonical_path = ""
 * and name_path = <module-stem>, so we return name_path directly.
 * For bucket-5 (deps) the dep iteration API is currently stubbed
 * (buckets.c emits no dep candidates yet in 04-02); when deps come
 * online we expect canonical_path = `dep://<name>/<path>` and
 * name_path = <module-stem>. The fallback is name_path itself. */
static const char *derive_module_stem(const IronLsp_CompletionCandidate *c) {
    if (!c) return NULL;
    if (c->name_path && *c->name_path) return c->name_path;
    return c->label;
}

/* Extract FUNCTION / METHOD param names into an arena-owned
 * const char *const [] suitable for IronLsp_SnippetMeta.param_names. */
static const char *const *extract_param_names(const Iron_Node *decl,
                                                int *out_count,
                                                Iron_Arena *arena) {
    if (out_count) *out_count = 0;
    if (!decl || !arena) return NULL;
    Iron_Node **params = NULL;
    int count = 0;
    if (decl->kind == IRON_NODE_FUNC_DECL) {
        params = ((const Iron_FuncDecl *)decl)->params;
        count  = ((const Iron_FuncDecl *)decl)->param_count;
    } else if (decl->kind == IRON_NODE_METHOD_DECL) {
        params = ((const Iron_MethodDecl *)decl)->params;
        count  = ((const Iron_MethodDecl *)decl)->param_count;
    }
    if (count <= 0 || !params) return NULL;
    const char **arr = (const char **)iron_arena_alloc(
        arena, sizeof(const char *) * (size_t)count, _Alignof(const char *));
    if (!arr) return NULL;
    for (int i = 0; i < count; i++) {
        Iron_Node *p = params[i];
        if (p && p->kind == IRON_NODE_PARAM) {
            arr[i] = ((const Iron_Param *)p)->name;
        } else {
            arr[i] = "";
        }
    }
    if (out_count) *out_count = count;
    return (const char *const *)arr;
}

/* Extract OBJECT_DECL field names. */
static const char *const *extract_field_names(const Iron_Node *decl,
                                                int *out_count,
                                                Iron_Arena *arena) {
    if (out_count) *out_count = 0;
    if (!decl || decl->kind != IRON_NODE_OBJECT_DECL || !arena) return NULL;
    const Iron_ObjectDecl *od = (const Iron_ObjectDecl *)decl;
    if (od->field_count <= 0 || !od->fields) return NULL;
    const char **arr = (const char **)iron_arena_alloc(
        arena, sizeof(const char *) * (size_t)od->field_count,
        _Alignof(const char *));
    if (!arr) return NULL;
    for (int i = 0; i < od->field_count; i++) {
        Iron_Node *f = od->fields[i];
        if (f && f->kind == IRON_NODE_FIELD) {
            arr[i] = ((const Iron_Field *)f)->name;
        } else {
            arr[i] = "";
        }
    }
    if (out_count) *out_count = od->field_count;
    return (const char *const *)arr;
}

/* Rewrite an auto-import candidate's insert_text to use an existing
 * alias. For a stdlib/dep bucket candidate whose canonical module
 * stem is already aliased to Y in the current file, the accepted
 * completion should type `Y` rather than the bare module stem. */
static void rewrite_insert_text_for_alias(IronLsp_CompletionCandidate *c,
                                            const char *alias,
                                            Iron_Arena *arena) {
    if (!c || !alias || !*alias || !arena) return;
    /* For module-kind candidates the label IS the stem (e.g. "io").
     * We swap label / insert_text to the alias so the user's typed
     * prefix actually resolves after acceptance. filterText stays as
     * the original so fuzzy matching continues to work. */
    size_t alen = strlen(alias);
    char *buf = (char *)iron_arena_alloc(arena, alen + 1, 1);
    if (!buf) return;
    memcpy(buf, alias, alen + 1);
    c->insert_text = buf;
}

/* Post-process the bucket-builder output: for each candidate, attach
 * auto-import edits + snippet-format insertText when applicable.
 *
 *   - additional_text_edit is computed for every candidate whose
 *     `needs_auto_import` flag is true (buckets 4 + 5). Dedup /
 *     alias honor / nested-path carve-out are implemented by
 *     ilsp_auto_import_edit itself.
 *   - insert_text_format is set to 2 (Snippet) when the client
 *     advertised snippetSupport AND the candidate's kind maps to
 *     one of the 5 template shapes (FUNCTION, METHOD, CLASS (object
 *     literal), ENUMMEMBER (enum variant)); 1 (PlainText) otherwise.
 *     MATCH / IMPORT snippet wiring fires only when the corresponding
 *     keyword is accepted at STATEMENT_HEAD; for this plan we opt to
 *     emit those templates as snippets on the keyword itself when
 *     the context is STATEMENT_HEAD (context argument). */
static void attach_auto_import_and_snippets(
    struct IronLsp_Server               *server,
    const Iron_Program                  *program,
    IronLsp_CompletionContext            ctx,
    IronLsp_CompletionCandidate         *cands,
    size_t                               n,
    Iron_Arena                          *arena) {
    bool snippets_ok = server ? server->client_supports_snippet : false;
    for (size_t i = 0; i < n; i++) {
        IronLsp_CompletionCandidate *c = &cands[i];
        /* Default: PlainText. */
        c->insert_text_format = 1;
        c->additional_text_edit = NULL;

        /* Step 1 -- auto-import for buckets 4 and 5. */
        if (c->needs_auto_import) {
            const char *stem = derive_module_stem(c);
            if (stem && *stem) {
                IronLsp_AutoImportEdit edit = {0};
                const char *alias = NULL;
                ilsp_auto_import_edit(program, NULL, stem, arena,
                                        &edit, &alias);
                if (edit.new_text) {
                    IronLsp_AutoImportEdit *heap_edit =
                        (IronLsp_AutoImportEdit *)iron_arena_alloc(
                            arena, sizeof(*heap_edit),
                            _Alignof(IronLsp_AutoImportEdit));
                    if (heap_edit) {
                        *heap_edit = edit;
                        c->additional_text_edit = heap_edit;
                    }
                } else if (alias) {
                    /* Existing alias -- no new import, but rewrite
                     * the accepted insertText so it uses the alias. */
                    rewrite_insert_text_for_alias(c, alias, arena);
                }
            }
        }

        /* Step 2 -- snippet render when client supports snippets. */
        if (!snippets_ok) continue;

        IronLsp_SnippetMeta meta = {0};
        IronLsp_SnippetKind kind;
        bool do_snippet = false;

        /* LSP CompletionItemKind constants: 3=FUNCTION, 2=METHOD,
         * 7=CLASS, 20=ENUMMEMBER, 14=KEYWORD. */
        if (c->kind == 3 || c->kind == 2) {
            /* FUNCTION / METHOD: look up matching top-level decl to
             * extract param names. Only applies to bucket 2
             * (top-level same-file); bucket 4/5 stdlib modules are
             * kind=MODULE, not FUNCTION. Bucket 1 (local) has kind
             * VARIABLE. */
            const Iron_Node *d =
                find_top_level_decl_by_name(program, c->label);
            if (d) {
                int pc = 0;
                const char *const *pn = extract_param_names(d, &pc, arena);
                meta.name = c->label;
                meta.param_names = pn;
                meta.param_count = pc;
                kind = (c->kind == 2) ? ILSP_SNIPPET_METHOD
                                       : ILSP_SNIPPET_FUNCTION;
                do_snippet = true;
            }
        } else if (c->kind == 7) {
            /* CLASS -> ObjectDecl. Only render object-literal snippet
             * when the context is EXPR_HEAD (i.e. the user is
             * constructing the object, not referring to it as a type
             * somewhere else). */
            if (ctx == ILSP_CCTX_EXPR_HEAD) {
                const Iron_Node *d =
                    find_top_level_decl_by_name(program, c->label);
                if (d) {
                    int fc = 0;
                    const char *const *fn =
                        extract_field_names(d, &fc, arena);
                    meta.name = c->label;
                    meta.field_names = fn;
                    meta.field_count = fc;
                    kind = ILSP_SNIPPET_OBJECT_LITERAL;
                    do_snippet = true;
                }
            }
        } else if (c->kind == 20) {
            /* ENUMMEMBER -- payload count requires walking the
             * enclosing Iron_EnumDecl's variants. The current
             * bucket builder emits enum variants only via
             * MEMBER_AFTER_DOT; we conservatively render
             * payload-less form here (payload_count = 0). */
            meta.name = "";           /* Enum type name unknown here */
            meta.variant_name = c->label;
            meta.payload_count = 0;
            kind = ILSP_SNIPPET_ENUM_VARIANT;
            /* Skip for now -- without the enum type name we can't
             * emit a spec-compliant `EnumName.Variant$0` body. This
             * will tighten when the bucket builder starts passing
             * the parent enum name via `detail`. */
            do_snippet = false;
        } else if (c->kind == 14 && ctx == ILSP_CCTX_STATEMENT_HEAD) {
            /* Keyword bucket at STATEMENT_HEAD -- render the full
             * match / import templates when the keyword is match or
             * import. */
            if (c->label && strcmp(c->label, "match") == 0) {
                kind = ILSP_SNIPPET_MATCH;
                do_snippet = true;
            } else if (c->label && strcmp(c->label, "import") == 0) {
                kind = ILSP_SNIPPET_IMPORT;
                do_snippet = true;
            }
        }

        if (do_snippet) {
            const char *body = ilsp_snippet_render(kind, &meta, arena);
            if (body) {
                c->insert_text = body;
                c->insert_text_format = 2;  /* Snippet */
            }
        }
    }
}

/* ── qsort comparator ─────────────────────────────────────────────── */

static int candidate_cmp(const void *pa, const void *pb) {
    const IronLsp_CompletionCandidate *a = (const IronLsp_CompletionCandidate *)pa;
    const IronLsp_CompletionCandidate *b = (const IronLsp_CompletionCandidate *)pb;
    if (a->bucket != b->bucket) return a->bucket - b->bucket;
    if (a->fuzzy_score != b->fuzzy_score)
        return (b->fuzzy_score > a->fuzzy_score) ? 1 : -1;
    const char *la = a->label ? a->label : "";
    const char *lb = b->label ? b->label : "";
    return strcmp(la, lb);
}

/* ── Public API ───────────────────────────────────────────────────── */

#define ILSP_COMPLETION_MAX_RESULTS 128

void ilsp_facade_complete(struct IronLsp_Server          *server,
                            struct IronLsp_Document        *doc,
                            IronLsp_Position                pos,
                            _Atomic bool                   *cancel,
                            Iron_Arena                     *arena,
                            IronLsp_CompletionCandidate   **out,
                            size_t                         *out_n,
                            bool                           *out_is_incomplete) {
    if (out)               *out = NULL;
    if (out_n)             *out_n = 0;
    if (out_is_incomplete) *out_is_incomplete = false;
    if (!server || !doc || !arena || !out || !out_n) return;

    IronLsp_PositionEncoding enc = server->position_encoding;

    Iron_Arena    walk_arena = iron_arena_create(64 * 1024);
    Iron_DiagList diags      = iron_diaglist_create();
    IronLsp_CompileRequest req = { .version = doc->version,
                                    .cancel_flag = cancel };
    Iron_Program *program = ilsp_facade_compile_for_nav(
        doc, &req, &walk_arena, &diags);
    /* program may be NULL on cold-start / parse-fatal; the bucket
     * builder accepts NULL program and simply emits empty top-level
     * + empty local + keyword bucket (for EXPR_HEAD / STATEMENT_HEAD). */
    if (cancel && atomic_load(cancel)) goto done;

    size_t cursor_byte = pos_to_byte(doc, pos, enc);
    const char *qp = extract_query_prefix(doc, cursor_byte, arena);
    IronLsp_CompletionContext ctx =
        ilsp_completion_context_classify(doc, cursor_byte);

    IronLsp_CompletionCandidate *cands = NULL;
    size_t n = 0;
    ilsp_complete_buckets_build(server, doc, program, cursor_byte,
                                  ctx, qp, cancel, arena, &cands, &n);
    if (cancel && atomic_load(cancel)) goto done;
    if (n == 0) goto done;

    qsort(cands, n, sizeof(*cands), candidate_cmp);

    if (n > ILSP_COMPLETION_MAX_RESULTS) {
        n = ILSP_COMPLETION_MAX_RESULTS;
    }

    /* Phase 4 Plan 04-03 Task 03: attach additionalTextEdits (auto-
     * import) + snippet-format insertText (when client supports it)
     * to each candidate. Runs after the qsort + cap so we only
     * touch items that will actually ship on the wire. */
    attach_auto_import_and_snippets(server, program, ctx, cands, n, arena);

    *out = cands;
    *out_n = n;

done:
    iron_diaglist_free(&diags);
    iron_arena_free(&walk_arena);
}
