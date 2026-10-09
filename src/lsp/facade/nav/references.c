/* Phase 3 Plan 04 Task 01 (NAV-06, D-09, D-16, T-03-09) --
 * textDocument/references facade.
 *
 * Flow:
 *   1. Analyze the open doc via ilsp_facade_compile_for_nav so we can
 *      resolve the cursor's ident -> resolved_sym -> identity triple.
 *   2. On the FIRST references request per server-lifetime, bulk-
 *      analyze every non-open, not-yet-analyzed workspace entry so
 *      the workspace reverse-ref index has full coverage. Subsequent
 *      requests skip this (O(1) after warm).
 *   3. Query the workspace reverse-ref index by triple; the query
 *      UNCONDITIONALLY drops stdlib/dep use-sites (D-09 LOCKED).
 *   4. If context.includeDeclaration is true, prepend the decl span.
 *   5. Cancel flag polled between per-file iterations inside the
 *      bulk-analyze helper (T-03-09 + D-16).
 */

#include "lsp/facade/nav/nav_core.h"
#include "lsp/facade/nav/nav_common.h"
#include "lsp/facade/nav/node_at.h"
#include "lsp/facade/nav/symbol_id.h"
#include "lsp/facade/nav/references_index.h"
#include "lsp/facade/nav/visibility.h"
#include "lsp/facade/nav/patch_lookup.h"
#include "lsp/facade/compile.h"
#include "lsp/facade/span.h"
#include "lsp/store/document.h"
#include "lsp/store/workspace_index.h"
#include "lsp/server/server.h"
#include "analyzer/analyzer.h"
#include "analyzer/scope.h"
#include "parser/ast.h"
#include "diagnostics/diagnostics.h"
#include "util/arena.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

/* Match decl_is_in_doc from definition.c: compare decl span.filename
 * to doc->uri with suffix tolerance. */
static bool span_file_is_doc(const char *fn, const IronLsp_Document *doc) {
    if (!doc || !doc->uri) return false;
    if (!fn || !*fn) return true;
    if (strcmp(fn, doc->uri) == 0) return true;
    size_t fl = strlen(fn);
    size_t ul = strlen(doc->uri);
    return (ul >= fl && strcmp(doc->uri + (ul - fl), fn) == 0);
}

/* Build a ref site for the decl (for context.includeDeclaration=true). */
static void build_decl_site(const Iron_Symbol              *sym,
                              IronLsp_Document              *doc,
                              IronLsp_PositionEncoding       enc,
                              Iron_Arena                    *arena,
                              IronLsp_WorkspaceIndex        *wi,
                              IronLsp_RefSite               *out) {
    memset(out, 0, sizeof(*out));
    if (!sym || !sym->decl_node) return;
    Iron_Span dspan = sym->decl_node->span;
    const char *fn = dspan.filename;
    if (span_file_is_doc(fn, doc)) {
        dspan = ilsp_nav_decl_name_span(sym->decl_node, doc->text, doc->text_len);
        out->range = ilsp_span_to_lsp_range(dspan, doc, enc);
        out->uri = doc->uri
            ? iron_arena_strdup(arena, doc->uri, strlen(doc->uri)) : "";
    } else if (wi && fn) {
        IronLsp_IndexEntry *entry = ilsp_workspace_index_lookup(wi, fn);
        if (entry) {
            dspan = ilsp_nav_decl_name_span(sym->decl_node, entry->source_bytes,
                                            entry->source_len);
            out->range = ilsp_nav_entry_span_to_range(entry, dspan, enc);
            out->uri = ilsp_nav_path_to_uri(fn, arena);
        } else {
            IronLsp_Range z = { {0,0}, {0,0} };
            out->range = z;
            out->uri = fn ? ilsp_nav_path_to_uri(fn, arena) : "";
        }
    } else {
        IronLsp_Range z = { {0,0}, {0,0} };
        out->range = z;
        out->uri = fn ? ilsp_nav_path_to_uri(fn, arena) : "";
    }
    if (!out->uri) out->uri = "";
}

/* Every identifier in the open document resolving to `decl`. */
typedef struct {
    const Iron_Node          *decl;
    IronLsp_Document         *doc;
    IronLsp_PositionEncoding  enc;
    Iron_Arena               *arena;
    IronLsp_RefSite          *sites;
    size_t                    cap, n;
} RefCollect;

static bool ref_collect_visit(Iron_Visitor *v, Iron_Node *n) {
    RefCollect *rc = (RefCollect *)v->ctx;
    if (n->kind == IRON_NODE_ERROR) return false;
    if (n->kind != IRON_NODE_IDENT) return true;
    Iron_Ident *id = (Iron_Ident *)n;
    if (!id->resolved_sym || id->resolved_sym->decl_node != rc->decl) return true;
    if (id->span.filename && rc->doc->uri && strcmp(id->span.filename, rc->doc->uri) != 0)
        return true;
    for (size_t i = 0; i < rc->n; i++) {  /* generic instances repeat spans */
        if (rc->sites[i].range.start.line == id->span.line - 1 &&
            rc->sites[i].range.start.character + 1 == id->span.col) return true;
    }
    if (rc->n == rc->cap) {
        size_t ncap = rc->cap * 2;
        IronLsp_RefSite *ns = (IronLsp_RefSite *)iron_arena_alloc(
            rc->arena, ncap * sizeof(*ns), _Alignof(IronLsp_RefSite));
        if (!ns) return false;
        memcpy(ns, rc->sites, rc->n * sizeof(*ns));
        rc->sites = ns;
        rc->cap = ncap;
    }
    rc->sites[rc->n].uri = rc->doc->uri
        ? iron_arena_strdup(rc->arena, rc->doc->uri, strlen(rc->doc->uri)) : "";
    rc->sites[rc->n].range = ilsp_span_to_lsp_range(id->span, rc->doc, rc->enc);
    rc->n++;
    return true;
}

void ilsp_facade_nav_references(struct IronLsp_Server         *server,
                                  struct IronLsp_Document       *doc,
                                  IronLsp_Position               pos,
                                  bool                           include_declaration,
                                  _Atomic bool                  *cancel,
                                  Iron_Arena                    *arena,
                                  IronLsp_RefSite              **out_sites,
                                  size_t                        *out_n) {
    if (out_sites) *out_sites = NULL;
    if (out_n)     *out_n     = 0;
    if (!server || !doc || !arena || !out_sites || !out_n) return;

    IronLsp_PositionEncoding enc = server->position_encoding;

    /* Step 1: analyze the cursor's doc via the nav seam to get a
     * fresh annotated program -- we need resolved_sym at the cursor. */
    Iron_Arena walk_arena = iron_arena_create(64 * 1024);
    Iron_DiagList walk_diags = iron_diaglist_create();
    IronLsp_CompileRequest req = { .version = doc->version,
                                    .cancel_flag = cancel };
    Iron_Program *program = ilsp_facade_compile_for_nav(
        doc, &req, &walk_arena, &walk_diags);
    if (!program) goto done;
    if (cancel && atomic_load(cancel)) goto done;

    /* Step 2: resolve cursor -> ident -> Iron_Symbol. */
    Iron_Node *node = ilsp_nav_node_at(doc, program, pos, enc);
    Iron_Symbol *sym = NULL;
    if (node && node->kind == IRON_NODE_IDENT) {
        Iron_Ident *id = (Iron_Ident *)node;
        sym = id->resolved_sym;
    } else if (node) {
        /* The cursor on a declaration's name: its symbol is the one any
         * use of it resolves to. */
        sym = ilsp_nav_symbol_of_decl(program, node);
    }
    /* If cursor is directly on a decl itself, synthesize a
     * "self-reference" by using the decl_node's triple and treating
     * the cursor position as the "decl span" for includeDeclaration. */
    if (!sym) goto done;  /* graceful degradation */

    /* Step 3: derive the triple using decl's canonical path. */
    const char *decl_path = (sym->decl_node && sym->decl_node->span.filename)
        ? sym->decl_node->span.filename
        : doc->uri;
    IronLsp_SymbolId triple = ilsp_symbol_id_derive(
        sym, decl_path, program, &walk_arena);
    if (triple.hash == 0) goto done;

    /* Step 4: on first references request, bulk-analyze every
     * workspace entry so the reverse-ref index is populated across
     * all user files. The helper polls cancel between files and
     * only flips bulk_analyze_done on full completion. */
    IronLsp_WorkspaceIndex *wi = server->workspace_index;
    if (wi && !wi->bulk_analyze_done) {
        ilsp_workspace_index_bulk_analyze_for_refs(wi, cancel);
        if (cancel && atomic_load(cancel)) goto done;
    }

    /* Also make sure THIS doc's contributions are recorded. The open-
     * document analyze we just ran doesn't go through workspace_index
     * (the document is the source of truth for its path). If the
     * document's canonical_path corresponds to a workspace entry,
     * invoke analyze_lazy on it so its spans show up in the reverse
     * index. */
    if (wi && doc->uri) {
        IronLsp_IndexEntry *self_entry = ilsp_workspace_index_lookup(wi, doc->uri);
        if (self_entry) {
            /* Force a fresh populate from the analyzed entry. */
            (void)ilsp_workspace_index_analyze_lazy(wi, self_entry, cancel);
        }
    }

    /* Step 5: query the reverse-ref index. UNCONDITIONAL stdlib/dep
     * filter happens inside the query helper (D-09 LOCKED). */
    IronLsp_RefSite *raw_sites = NULL;
    size_t raw_n = 0;
    if (wi) {
        ilsp_refs_query(wi, triple, arena, enc, &raw_sites, &raw_n);
    }

    /* Step 5.5 (NEW Phase 10 VIS-01): post-filter cross-file results
     * by visibility. doc->uri is the requester; sym->decl_node carries
     * the visibility bits via ilsp_vis_is_public. Same-module sites
     * short-circuit to true; stdlib sites pass via D-08 carve-out.
     *
     * In-place compaction preserves the order of remaining results.
     * Each raw_sites[i].uri is treated as the per-site decl-path
     * proxy (the predicate's same-module shortcut handles the case
     * where the site IS the decl's home file). Cross-file private
     * sites get filtered.
     *
     * PATCH-05 (Plan 11-03): when the cursor is on a patch method, the
     * visibility gate uses the enclosing patch ObjectDecl as decl_node
     * (CONTEXT D-14). Per RESEARCH Conflict 3, this is forward-compat
     * shape: today the predicate defaults-true for ObjectDecl (no
     * is_private/is_pub axis on Iron_ObjectDecl in v3 grammar). The
     * call shape activates the moment a future grammar phase adds
     * patch-level visibility. For native methods, sym->decl_node
     * remains the right argument and the existing filter applies.
     *
     * Derivation is done ONCE per request (before the filter loop) and
     * cached in a local vis_decl_node used inside the loop. */
    const Iron_Node *vis_decl_node = sym ? sym->decl_node : NULL;
    if (sym && sym->decl_node &&
        sym->decl_node->kind == IRON_NODE_METHOD_DECL) {
        Iron_ObjectDecl *patch_od = ilsp_patch_enclosing_for_method(
            program, (Iron_MethodDecl *)sym->decl_node, wi);
        if (patch_od) vis_decl_node = (const Iron_Node *)patch_od;
    }
    if (raw_sites && raw_n > 0) {
        const char *requester = (doc && doc->uri) ? doc->uri : "";
        size_t kept = 0;
        for (size_t i = 0; i < raw_n; i++) {
            if (ilsp_vis_can_see(raw_sites[i].uri, requester,
                                  vis_decl_node)) {
                if (kept != i) raw_sites[kept] = raw_sites[i];
                kept++;
            }
        }
        raw_n = kept;
    }

    /* Step 6: if raw_n is 0 and we still want to surface same-file
     * references, fall back to walking the open document's program
     * directly. This is essential because the open doc is not
     * (re-)populated into the workspace index in single-file
     * scenarios (no workspace_index entry). */
    IronLsp_RefSite *fallback = NULL;
    size_t fallback_n = 0;
    if (raw_n == 0 && program) {
        /* Gather every Iron_Ident use-site in THIS doc whose
         * resolved_sym maps to the same decl_node. We use decl_node
         * pointer equality (safe within a single analyze). */
        size_t cap = 16;
        fallback = (IronLsp_RefSite *)iron_arena_alloc(
            arena, cap * sizeof(*fallback), _Alignof(IronLsp_RefSite));
        if (!fallback) goto assemble;
        RefCollect rc = { sym->decl_node, doc, enc, arena, fallback, cap, 0 };
        Iron_Visitor v = { .ctx = &rc, .visit_node = ref_collect_visit, .post_visit = NULL };
        for (int i = 0; i < program->decl_count; i++) {
            if (program->decls[i]) iron_ast_walk(program->decls[i], &v);
        }
        fallback = rc.sites;
        fallback_n = rc.n;
    }

assemble:
    /* Step 7: assemble final array: optional decl + raw_sites or
     * fallback sites. */
    {
        IronLsp_RefSite *src = (raw_n > 0) ? raw_sites : fallback;
        size_t src_n = (raw_n > 0) ? raw_n : fallback_n;
        size_t extra = include_declaration ? 1 : 0;
        size_t total = src_n + extra;
        if (total == 0) goto done;

        IronLsp_RefSite *final = (IronLsp_RefSite *)iron_arena_alloc(
            arena, total * sizeof(*final), _Alignof(IronLsp_RefSite));
        if (!final) goto done;

        size_t w = 0;
        if (include_declaration) {
            build_decl_site(sym, doc, enc, arena, wi, &final[w]);
            w++;
        }
        if (src && src_n > 0) {
            memcpy(&final[w], src, src_n * sizeof(*final));
            w += src_n;
        }
        *out_sites = final;
        *out_n = w;
    }

done:
    iron_diaglist_free(&walk_diags);
    iron_arena_free(&walk_arena);
}
