/* Phase 4 Plan 04-02 Task 03 (EDIT-03, D-04) -- completionItem/resolve
 * facade implementation.
 *
 * Walks the workspace_index for the claimed canonical_path, finds the
 * decl matching name_path, and renders a hover-style markdown block:
 *   ```iron
 *   <signature>
 *   ```
 *
 *   <doc_comment>
 *
 * On any mismatch (no entry, stale content_hash, no matching decl),
 * both outputs are left NULL — the handler emits the item unchanged.
 *
 * Keywords (bucket 6) short-circuit to a generic keyword-reference
 * line in place of a true signature; stdlib (bucket 4) / deps (bucket
 * 5) that reference module-only candidates similarly produce a
 * "module <name>" detail. Future work: full cross-file symbol lookup
 * via workspace_index_lookup + program walk.
 */

#include "lsp/facade/edit/complete/resolve.h"
#include "lsp/server/server.h"
#include "lsp/store/workspace_index.h"
#include "parser/ast.h"
#include "util/arena.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

/* ── Small SB (shared pattern with hover.c) ───────────────────────── */

typedef struct {
    char       *buf;
    size_t      len;
    size_t      cap;
    Iron_Arena *arena;
} SB;

static void sb_init(SB *sb, Iron_Arena *arena) {
    sb->arena = arena;
    sb->cap   = 256;
    sb->len   = 0;
    sb->buf   = (char *)iron_arena_alloc(arena, sb->cap, 1);
    if (sb->buf) sb->buf[0] = '\0';
}

static void sb_append(SB *sb, const char *s) {
    if (!sb->buf || !s) return;
    size_t sl = strlen(s);
    if (sb->len + sl + 1 > sb->cap) {
        size_t ncap = sb->cap ? sb->cap : 1;
        while (ncap < sb->len + sl + 1) ncap *= 2;
        char *nb = (char *)iron_arena_alloc(sb->arena, ncap, 1);
        if (!nb) return;
        memcpy(nb, sb->buf, sb->len + 1);
        sb->buf = nb;
        sb->cap = ncap;
    }
    memcpy(sb->buf + sb->len, s, sl);
    sb->len += sl;
    sb->buf[sb->len] = '\0';
}

/* ── Signature rendering (trimmed copy of hover.c's dispatcher) ──── */

static const char *render_type_ann(Iron_Node *n, Iron_Arena *arena) {
    if (!n) return "Void";
    if (n->kind == IRON_NODE_TYPE_ANNOTATION) {
        Iron_TypeAnnotation *ta = (Iron_TypeAnnotation *)n;
        SB tmp; sb_init(&tmp, arena);
        if (ta->is_array) sb_append(&tmp, "[");
        sb_append(&tmp, ta->name ? ta->name : "Unknown");
        if (ta->is_array) sb_append(&tmp, "]");
        if (ta->is_nullable) sb_append(&tmp, "?");
        return tmp.buf ? tmp.buf : "Unknown";
    }
    return "Unknown";
}

static void render_params(SB *sb, Iron_Node **params, int count,
                            Iron_Arena *arena) {
    for (int i = 0; i < count; i++) {
        if (i > 0) sb_append(sb, ", ");
        Iron_Node *p = params ? params[i] : NULL;
        if (!p || p->kind != IRON_NODE_PARAM) { sb_append(sb, "_"); continue; }
        Iron_Param *pp = (Iron_Param *)p;
        sb_append(sb, pp->name ? pp->name : "_");
        sb_append(sb, ": ");
        sb_append(sb, render_type_ann(pp->type_ann, arena));
    }
}

static const char *signature_for_decl(Iron_Node *d, Iron_Arena *arena,
                                        const char **out_doc) {
    if (!d || !arena) return NULL;
    SB sb; sb_init(&sb, arena);
    switch ((int)d->kind) {
        case IRON_NODE_FUNC_DECL: {
            Iron_FuncDecl *fd = (Iron_FuncDecl *)d;
            sb_append(&sb, "func ");
            sb_append(&sb, fd->name ? fd->name : "_");
            sb_append(&sb, "(");
            render_params(&sb, fd->params, fd->param_count, arena);
            sb_append(&sb, ")");
            if (fd->return_type) {
                sb_append(&sb, " -> ");
                sb_append(&sb, render_type_ann(fd->return_type, arena));
            }
            if (out_doc) *out_doc = fd->doc_comment;
            break;
        }
        case IRON_NODE_METHOD_DECL: {
            Iron_MethodDecl *md = (Iron_MethodDecl *)d;
            sb_append(&sb, "func ");
            sb_append(&sb, md->type_name ? md->type_name : "_");
            sb_append(&sb, ".");
            sb_append(&sb, md->method_name ? md->method_name : "_");
            sb_append(&sb, "(");
            render_params(&sb, md->params, md->param_count, arena);
            sb_append(&sb, ")");
            if (md->return_type) {
                sb_append(&sb, " -> ");
                sb_append(&sb, render_type_ann(md->return_type, arena));
            }
            if (out_doc) *out_doc = md->doc_comment;
            break;
        }
        case IRON_NODE_OBJECT_DECL: {
            Iron_ObjectDecl *od = (Iron_ObjectDecl *)d;
            sb_append(&sb, "object ");
            sb_append(&sb, od->name ? od->name : "_");
            if (out_doc) *out_doc = od->doc_comment;
            break;
        }
        case IRON_NODE_INTERFACE_DECL: {
            Iron_InterfaceDecl *ifd = (Iron_InterfaceDecl *)d;
            sb_append(&sb, "interface ");
            sb_append(&sb, ifd->name ? ifd->name : "_");
            if (out_doc) *out_doc = ifd->doc_comment;
            break;
        }
        case IRON_NODE_ENUM_DECL: {
            Iron_EnumDecl *ed = (Iron_EnumDecl *)d;
            sb_append(&sb, "enum ");
            sb_append(&sb, ed->name ? ed->name : "_");
            if (out_doc) *out_doc = ed->doc_comment;
            break;
        }
        case IRON_NODE_VAL_DECL: {
            Iron_ValDecl *vd = (Iron_ValDecl *)d;
            sb_append(&sb, "val ");
            sb_append(&sb, vd->name ? vd->name : "_");
            sb_append(&sb, ": ");
            sb_append(&sb, render_type_ann(vd->type_ann, arena));
            break;
        }
        default:
            return NULL;
    }
    return sb.buf;
}

/* ── Public API ───────────────────────────────────────────────────── */

void ilsp_facade_completion_resolve(struct IronLsp_Server              *server,
                                       const IronLsp_CompletionResolveData *data,
                                       _Atomic bool                       *cancel,
                                       Iron_Arena                         *arena,
                                       const char                        **detail_out,
                                       const char                        **documentation_markdown_out) {
    if (detail_out)                 *detail_out = NULL;
    if (documentation_markdown_out) *documentation_markdown_out = NULL;
    if (!server || !data || !arena) return;
    if (cancel && atomic_load(cancel)) return;

    /* Keyword bucket: short-circuit with generic detail, no doc. */
    if (data->bucket == 6 /* KEYWORDS */) {
        if (detail_out) *detail_out = "Iron keyword";
        if (documentation_markdown_out && data->name_path) {
            SB sb; sb_init(&sb, arena);
            sb_append(&sb, "```iron\n");
            sb_append(&sb, data->name_path);
            sb_append(&sb, "\n```");
            *documentation_markdown_out = sb.buf;
        }
        return;
    }

    /* Stdlib / deps bucket: generic module detail. */
    if (data->bucket == 4 || data->bucket == 5) {
        if (detail_out) {
            const char *label = data->name_path ? data->name_path : "";
            size_t l = strlen(label) + sizeof("module ") + 1;
            char *buf = (char *)iron_arena_alloc(arena, l, 1);
            if (buf) { snprintf(buf, l, "module %s", label); *detail_out = buf; }
        }
        return;
    }

    /* Buckets 1-3: walk the workspace_index for the matching entry. */
    if (!server->workspace_index) return;
    if (!data->canonical_path || !*data->canonical_path) return;

    IronLsp_IndexEntry *entry =
        ilsp_workspace_index_lookup(server->workspace_index, data->canonical_path);
    if (!entry) return;

    /* Stale-content guard (T-4-1 mitigation): if the file has been
     * edited since the initial completion response, bail out. */
    if (data->content_hash != 0 && entry->content_hash != data->content_hash) {
        return;
    }

    Iron_Program *program = entry->program;
    if (!program || !data->name_path) return;

    /* Find the decl by name_path match. Simple 1-level lookup: we assume
     * name_path is a flat ident for top-level symbols in Plan 04-02.
     * Nested (method) name_paths are Plan 04-03 work. */
    Iron_Node *best = NULL;
    for (int i = 0; i < program->decl_count; i++) {
        Iron_Node *d = program->decls[i];
        if (!d) continue;
        const char *nm = NULL;
        switch ((int)d->kind) {
            case IRON_NODE_FUNC_DECL:      nm = ((Iron_FuncDecl *)d)->name; break;
            case IRON_NODE_METHOD_DECL:    nm = ((Iron_MethodDecl *)d)->method_name; break;
            case IRON_NODE_OBJECT_DECL:    nm = ((Iron_ObjectDecl *)d)->name; break;
            case IRON_NODE_INTERFACE_DECL: nm = ((Iron_InterfaceDecl *)d)->name; break;
            case IRON_NODE_ENUM_DECL:      nm = ((Iron_EnumDecl *)d)->name; break;
            case IRON_NODE_VAL_DECL:       nm = ((Iron_ValDecl *)d)->name; break;
            default: break;
        }
        if (nm && strcmp(nm, data->name_path) == 0) { best = d; break; }
    }
    if (!best) return;

    const char *doc_comment = NULL;
    const char *sig = signature_for_decl(best, arena, &doc_comment);
    if (!sig) return;

    /* Upgraded detail = the signature itself (short form). */
    if (detail_out) *detail_out = sig;

    /* Documentation = fenced iron block + optional doc comment. */
    if (documentation_markdown_out) {
        SB md; sb_init(&md, arena);
        sb_append(&md, "```iron\n");
        sb_append(&md, sig);
        sb_append(&md, "\n```");
        if (doc_comment && *doc_comment) {
            sb_append(&md, "\n\n");
            sb_append(&md, doc_comment);
        }
        *documentation_markdown_out = md.buf;
    }
}
