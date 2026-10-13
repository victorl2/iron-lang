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
#include "util/os.h"
#include <limits.h>
#include <stdlib.h>

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

/* The canonical file system path of a span's file: a document's
 * declarations carry its URI, the other files' their canonical path. */
static const char *canonical_file(const char *fn, char *buf, Iron_Arena *arena) {
    if (!fn) return NULL;
    if (strncmp(fn, "file://", 7) == 0) {
        const char *fs = ilsp_nav_uri_to_path(fn, arena);
        if (!fs) return fn;
        if (realpath(fs, buf) != NULL) {
            for (char *c = buf; *c; c++) if (*c == '\\') *c = '/';
            return buf;
        }
        return fs;
    }
    return fn;
}

static bool has_word(const char *text, size_t len, const char *word) {
    size_t wl = strlen(word);
    if (wl == 0 || !text) return false;
    for (size_t i = 0; i + wl <= len; i++) {
        if (text[i] == word[0] && memcmp(text + i, word, wl) == 0) return true;
    }
    return false;
}

void ilsp_nav_package_occurrences(struct IronLsp_Server    *server,
                                  struct IronLsp_Document  *doc,
                                  const Iron_Node          *key_decl,
                                  const char               *key_name,
                                  _Atomic bool             *cancel,
                                  Iron_Arena               *arena,
                                  IronLsp_RefSite         **out_sites,
                                  bool                    **out_is_decl,
                                  size_t                   *out_n) {
    if (out_sites) *out_sites = NULL;
    if (out_is_decl) *out_is_decl = NULL;
    if (out_n) *out_n = 0;
    if (!server || !doc || !key_decl || !key_name || !arena || !out_sites || !out_n) return;
    IronLsp_WorkspaceIndex *wi = server->workspace_index;
    if (!wi) return;
    IronLsp_PositionEncoding enc = server->position_encoding;

    Iron_Arena scratch = iron_arena_create(64 * 1024);
    char key_buf[PATH_MAX], doc_buf[PATH_MAX];
    const char *key_file = canonical_file(
        key_decl->span.filename ? key_decl->span.filename : doc->uri, key_buf, &scratch);
    const char *doc_file = canonical_file(doc->uri, doc_buf, &scratch);
    if (!key_file || ilsp_nav_path_is_stdlib(key_file)) { iron_arena_free(&scratch); return; }

    size_t path_n = 0;
    char **paths = ilsp_workspace_index_snapshot_paths(wi, &path_n);
    IronLsp_RefSite *sites = NULL;
    bool *is_decl = NULL;
    size_t n = 0, cap = 0;
    for (size_t pi = 0; paths && pi < path_n; pi++) {
        if (cancel && atomic_load(cancel)) break;
        const char *path = paths[pi];
        if (!path || ilsp_nav_path_is_stdlib(path)) continue;
        if (strncmp(path, "dep://", 6) == 0 || strstr(path, "/.iron/deps/") != NULL) continue;
        if (doc_file && strcmp(path, doc_file) == 0) continue;
        IronLsp_IndexEntry *e = ilsp_workspace_index_lookup(wi, path);
        if (!e || !e->source_bytes || !has_word(e->source_bytes, e->source_len, key_name)) continue;
        Iron_Program *prog = ilsp_workspace_index_analyze_lazy(wi, e, cancel);
        if (!prog) continue;

        IronLsp_Document edoc = {0};
        edoc.text     = e->source_bytes;
        edoc.text_len = e->source_len;
        edoc.uri      = e->canonical_path;
        edoc.line_idx = e->line_idx;   /* borrowed: names are found by line */
        size_t occ_n = 0;
        IronLsp_NameOcc *occs = ilsp_nav_name_occurrences(&edoc, prog, &scratch, &occ_n);
        const char *uri = NULL;
        for (size_t i = 0; i < occ_n; i++) {
            const Iron_Node *d = occs[i].decl;
            if (!d || !occs[i].name || strcmp(occs[i].name, key_name) != 0) continue;
            if (d->kind != key_decl->kind || d->span.line != key_decl->span.line ||
                d->span.col != key_decl->span.col) continue;
            const char *df = d->span.filename ? d->span.filename : e->canonical_path;
            if (strcmp(df, key_file) != 0) continue;
            if (n == cap) {
                size_t ncap = cap ? cap * 2 : 16;
                IronLsp_RefSite *ns = (IronLsp_RefSite *)iron_arena_alloc(
                    arena, ncap * sizeof(*ns), _Alignof(IronLsp_RefSite));
                bool *nd = (bool *)iron_arena_alloc(arena, ncap * sizeof(*nd), _Alignof(bool));
                if (!ns || !nd) break;
                if (n) { memcpy(ns, sites, n * sizeof(*ns)); memcpy(nd, is_decl, n * sizeof(*nd)); }
                sites = ns; is_decl = nd; cap = ncap;
            }
            if (!uri) uri = ilsp_nav_path_to_uri(e->canonical_path, arena);
            if (!uri) break;
            sites[n].uri   = uri;
            sites[n].range = ilsp_nav_entry_span_to_range(e, occs[i].span, enc);
            is_decl[n]     = occs[i].is_decl;
            n++;
        }
    }
    for (size_t i = 0; paths && i < path_n; i++) free(paths[i]);
    free(paths);
    iron_arena_free(&scratch);
    *out_sites = sites;
    if (out_is_decl) *out_is_decl = is_decl;
    *out_n = n;
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
        /* The index names a declaring file by its canonical path; the
         * open document's declarations carry its URI. */
        char decl_canon[PATH_MAX];
        if (strncmp(decl_path, "file://", 7) == 0) {
            const char *fs = ilsp_nav_uri_to_path(decl_path, &walk_arena);
            if (fs && realpath(fs, decl_canon) != NULL) {
                for (char *c = decl_canon; *c; c++) if (*c == '\\') *c = '/';
                decl_path = decl_canon;
            } else if (fs) {
                decl_path = fs;
            }
        }
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

    /* Step 6: the names in the other files that the reverse-ref index
     * does not hold: a type in an annotation, a field or a method in a
     * member access (and the declaration itself, when it is in another
     * file and no symbol led to it). push_site drops what step 5 found. */
    {
        IronLsp_RefSite *pkg = NULL;
        bool *pkg_is_decl = NULL;
        size_t pkg_n = 0;
        ilsp_nav_package_occurrences(server, doc, key_decl, key_name, cancel, arena,
                                     &pkg, &pkg_is_decl, &pkg_n);
        for (size_t i = 0; i < pkg_n; i++) {
            if (pkg_is_decl[i] && !include_declaration) continue;
            push_site(&final, &fn, &fcap, arena, pkg[i].uri, pkg[i].range);
        }
    }

    *out_sites = final;
    *out_n = fn;

done:
    iron_diaglist_free(&walk_diags);
    iron_arena_free(&walk_arena);
}
