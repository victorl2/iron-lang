/* Phase 3 Plan 04 Task 01 (NAV-06, D-09, D-16, T-03-09) --
 * textDocument/references facade.
 *
 * Flow:
 *   1. Analyze the open doc via ilsp_facade_compile_for_nav.
 *   2. Collect every written name of the document with what it names
 *      (ilsp_nav_name_occurrences): identifiers, member names (`p.x`,
 *      `p.get()`), enum variants, type names, and declarations' own
 *      names, `for` variables and match bindings included. The one under
 *      the cursor gives the (declaration, name) pair to match.
 *   3. If context.includeDeclaration is true, the declaration comes
 *      first: its name in this document, or the declaration site in
 *      another file.
 *   4. The uses in this document come from its buffer (step 2).
 *   5. For a symbol other files can name, the workspace reverse-ref
 *      index adds the other files' uses. On the FIRST request per
 *      server lifetime every workspace entry is bulk-analyzed so the
 *      index has full coverage; the query UNCONDITIONALLY drops
 *      stdlib/dep use-sites (D-09 LOCKED), and cross-file sites of a
 *      private symbol are hidden (VIS-01). The cancel flag is polled
 *      between files (T-03-09 + D-16).
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

/* Append a site, growing the arena array. */
static void push_site(IronLsp_RefSite **arr, size_t *n, size_t *cap, Iron_Arena *arena,
                      const char *uri, IronLsp_Range range) {
    for (size_t i = 0; i < *n; i++) {
        if ((*arr)[i].range.start.line == range.start.line &&
            (*arr)[i].range.start.character == range.start.character &&
            strcmp((*arr)[i].uri, uri) == 0) return;
    }
    if (*n == *cap) {
        size_t ncap = *cap ? *cap * 2 : 16;
        IronLsp_RefSite *ns = (IronLsp_RefSite *)iron_arena_alloc(
            arena, ncap * sizeof(*ns), _Alignof(IronLsp_RefSite));
        if (!ns) return;
        if (*n) memcpy(ns, *arr, *n * sizeof(*ns));
        *arr = ns;
        *cap = ncap;
    }
    (*arr)[*n].uri = uri;
    (*arr)[*n].range = range;
    (*n)++;
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

    /* Step 2: what the name under the cursor names. Every written name
     * of the document (uses, member names, type names, declarations'
     * own names) is an occurrence; the one under the cursor gives the
     * (declaration, name) pair the others are matched against. */
    size_t occ_n = 0;
    IronLsp_NameOcc *occs = ilsp_nav_name_occurrences(doc, program, &walk_arena, &occ_n);
    const IronLsp_NameOcc *at = ilsp_nav_occurrence_at(occs, occ_n, doc, pos, enc);
    Iron_Node *node = ilsp_nav_node_at(doc, program, pos, enc);
    Iron_Symbol *sym = NULL;
    if (node && node->kind == IRON_NODE_IDENT) {
        sym = ((Iron_Ident *)node)->resolved_sym;
    } else if (node) {
        /* The cursor on a declaration's name: its symbol is the one any
         * use of it resolves to. */
        sym = ilsp_nav_symbol_of_decl(program, node);
    }
    const Iron_Node *key_decl = at ? at->decl : (sym ? sym->decl_node : NULL);
    const char *key_name = at ? at->name : (sym ? sym->name : NULL);
    if (!key_decl || !key_name) goto done;  /* graceful degradation */
    /* A symbol for another declaration than the occurrence's (a type
     * name under a constructor call...) is not the one to look up. */
    if (sym && sym->decl_node != key_decl) sym = NULL;

    const char *doc_uri = doc->uri
        ? iron_arena_strdup(arena, doc->uri, strlen(doc->uri)) : "";
    IronLsp_RefSite *final = NULL;
    size_t fn = 0, fcap = 0;

    /* Step 3: the declaration first, when asked for. */
    bool decl_in_doc = false;
    for (size_t i = 0; i < occ_n; i++) {
        if (occs[i].is_decl && ilsp_nav_occ_same(&occs[i], key_decl, key_name)) {
            decl_in_doc = true;
            if (include_declaration) {
                push_site(&final, &fn, &fcap, arena, doc_uri,
                          ilsp_span_to_lsp_range(occs[i].span, doc, enc));
            }
        }
    }
    IronLsp_WorkspaceIndex *wi = server->workspace_index;
    if (include_declaration && !decl_in_doc && sym) {
        IronLsp_RefSite ds;
        build_decl_site(sym, doc, enc, arena, wi, &ds);
        if (ds.uri && *ds.uri) push_site(&final, &fn, &fcap, arena, ds.uri, ds.range);
    }

    /* Step 4: the uses in this document. */
    for (size_t i = 0; i < occ_n; i++) {
        if (!occs[i].is_decl && ilsp_nav_occ_same(&occs[i], key_decl, key_name)) {
            push_site(&final, &fn, &fcap, arena, doc_uri,
                      ilsp_span_to_lsp_range(occs[i].span, doc, enc));
        }
    }

    /* Step 5: the uses in the other workspace files, for a symbol other
     * files can name. The open document's own sites come from step 4
     * (its buffer, not the file on disk). */
    if (sym && wi) {
        const char *decl_path = (sym->decl_node && sym->decl_node->span.filename)
            ? sym->decl_node->span.filename : doc->uri;
        IronLsp_SymbolId triple = ilsp_symbol_id_derive(sym, decl_path, program, &walk_arena);
        if (triple.hash != 0) {
            /* On the first references request, bulk-analyze every
             * workspace entry so the reverse-ref index covers all user
             * files. The helper polls cancel between files. */
            if (!wi->bulk_analyze_done) {
                ilsp_workspace_index_bulk_analyze_for_refs(wi, cancel);
                if (cancel && atomic_load(cancel)) goto done;
            }
            IronLsp_RefSite *raw = NULL;
            size_t raw_n = 0;
            /* stdlib / dep use-sites are dropped by the query (D-09). */
            ilsp_refs_query(wi, triple, arena, enc, &raw, &raw_n);
            /* Visibility (VIS-01): a cross-file site of a private symbol
             * is hidden. For a patch method the gate is the enclosing
             * patch object (PATCH-05). */
            const Iron_Node *vis_decl_node = sym->decl_node;
            if (sym->decl_node && sym->decl_node->kind == IRON_NODE_METHOD_DECL) {
                Iron_ObjectDecl *patch_od = ilsp_patch_enclosing_for_method(
                    program, (Iron_MethodDecl *)sym->decl_node, wi);
                if (patch_od) vis_decl_node = (const Iron_Node *)patch_od;
            }
            for (size_t i = 0; i < raw_n; i++) {
                if (!raw[i].uri || span_file_is_doc(raw[i].uri, doc) ||
                    (doc->uri && strcmp(raw[i].uri, doc->uri) == 0)) continue;
                if (!ilsp_vis_can_see(raw[i].uri, doc->uri ? doc->uri : "", vis_decl_node))
                    continue;
                push_site(&final, &fn, &fcap, arena, raw[i].uri, raw[i].range);
            }
        }
    }

    *out_sites = final;
    *out_n = fn;

done:
    iron_diaglist_free(&walk_diags);
    iron_arena_free(&walk_arena);
}
