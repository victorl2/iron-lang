/* Phase 3 Plan 03 Task 03 (NAV-08, D-11) -- workspace/symbol.
 *
 * Iterates the workspace_index (+ stdlib cache), fuzzy-scores every
 * top-level decl name against the query, sorts by (score DESC, kind
 * rank ASC, canonical_path ASC, line ASC, col ASC), caps at 256.
 *
 * Query syntax per D-11:
 *   - plain subsequence match on symbol_name_path
 *   - "prefix/rest" filters entries whose canonical_path contains
 *     "prefix" as a path segment, then scores "rest"
 *   - CamelCase anchors fall out of the fuzzy matcher bonuses
 *
 * Hard limits (T-03-07 DoS):
 *   - query > ILSP_WORKSPACE_SYMBOL_QUERY_MAX -> empty result
 *   - returned list capped at ILSP_WORKSPACE_SYMBOL_MAX_RESULTS (256)
 *   - cancel polled per entry
 */

#include "lsp/facade/nav/nav_core.h"
#include "lsp/facade/nav/nav_common.h"
#include "lsp/facade/nav/fuzzy.h"
#include "lsp/facade/nav/symbol_id.h"
#include "lsp/facade/nav/visibility.h"
#include "lsp/store/workspace_index.h"
#include "lsp/server/server.h"
#include "analyzer/analyzer.h"
#include "parser/ast.h"
#include "util/arena.h"
#include "vendor/stb_ds.h"

#include <math.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#define LSP_SYMKIND_CLASS       5
#define LSP_SYMKIND_METHOD      6
#define LSP_SYMKIND_FIELD       8
#define LSP_SYMKIND_ENUM        10
#define LSP_SYMKIND_INTERFACE   11
#define LSP_SYMKIND_FUNCTION    12
#define LSP_SYMKIND_VARIABLE    13
#define LSP_SYMKIND_CONSTANT    14
#define LSP_SYMKIND_ENUMMEMBER  22
#define LSP_SYMKIND_PACKAGE     4

/* Internal candidate used during scoring + sorting. */
typedef struct {
    const char    *name;            /* arena-owned */
    const char    *container_name;  /* arena-owned */
    const char    *canonical_path;  /* arena-owned (== entry->canonical_path) */
    const char    *uri;             /* arena-owned file:// URI */
    int            lsp_kind;
    int            kind_rank;
    double         score;
    Iron_Span      span;
    Iron_Span      sel_span;
} candidate_t;

static int lsp_kind_from_node(Iron_Node *d) {
    switch ((int)d->kind) {
        case IRON_NODE_FUNC_DECL:      return LSP_SYMKIND_FUNCTION;
        case IRON_NODE_METHOD_DECL:    return LSP_SYMKIND_METHOD;
        case IRON_NODE_OBJECT_DECL:    return LSP_SYMKIND_CLASS;
        case IRON_NODE_INTERFACE_DECL: return LSP_SYMKIND_INTERFACE;
        case IRON_NODE_ENUM_DECL:      return LSP_SYMKIND_ENUM;
        case IRON_NODE_ENUM_VARIANT:   return LSP_SYMKIND_ENUMMEMBER;
        case IRON_NODE_FIELD:          return LSP_SYMKIND_FIELD;
        case IRON_NODE_VAL_DECL:       return LSP_SYMKIND_CONSTANT;
        case IRON_NODE_IMPORT_DECL:    return LSP_SYMKIND_PACKAGE;
        default:                        return LSP_SYMKIND_VARIABLE;
    }
}

static int kind_rank_for_lsp(int lsp_kind) {
    /* Mirror D-11 ranking using LSP kinds. */
    switch (lsp_kind) {
        case LSP_SYMKIND_FUNCTION:   return 0;
        case LSP_SYMKIND_METHOD:     return 1;
        case LSP_SYMKIND_CLASS:      return 2;
        case LSP_SYMKIND_INTERFACE:  return 3;
        case LSP_SYMKIND_ENUM:       return 4;
        case LSP_SYMKIND_ENUMMEMBER: return 5;
        case LSP_SYMKIND_FIELD:      return 6;
        case LSP_SYMKIND_CONSTANT:   return 7;
        case LSP_SYMKIND_VARIABLE:   return 8;
        case LSP_SYMKIND_PACKAGE:    return 9;
        default:                      return 9;
    }
}

/* Decl -> identifier string (NULL for anonymous / non-named). */
static const char *decl_name(Iron_Node *d) {
    switch ((int)d->kind) {
        case IRON_NODE_FUNC_DECL:      return ((Iron_FuncDecl *)d)->name;
        case IRON_NODE_METHOD_DECL:    return ((Iron_MethodDecl *)d)->method_name;
        case IRON_NODE_OBJECT_DECL:    return ((Iron_ObjectDecl *)d)->name;
        case IRON_NODE_INTERFACE_DECL: return ((Iron_InterfaceDecl *)d)->name;
        case IRON_NODE_ENUM_DECL:      return ((Iron_EnumDecl *)d)->name;
        case IRON_NODE_ENUM_VARIANT:   return ((Iron_EnumVariant *)d)->name;
        case IRON_NODE_FIELD:          return ((Iron_Field *)d)->name;
        case IRON_NODE_VAL_DECL:       return ((Iron_ValDecl *)d)->name;
        default:                        return NULL;
    }
}

static const char *decl_container(Iron_Node *d) {
    switch ((int)d->kind) {
        case IRON_NODE_METHOD_DECL: return ((Iron_MethodDecl *)d)->type_name;
        default: return "";
    }
}

/* Sort predicate: score DESC, kind_rank ASC, canonical_path ASC,
 * line ASC, col ASC (deterministic final order). */
static int candidate_cmp(const void *pa, const void *pb) {
    const candidate_t *a = (const candidate_t *)pa;
    const candidate_t *b = (const candidate_t *)pb;
    if (a->score != b->score) return (a->score > b->score) ? -1 : 1;
    if (a->kind_rank != b->kind_rank)
        return (a->kind_rank < b->kind_rank) ? -1 : 1;
    const char *pa_path = a->canonical_path ? a->canonical_path : "";
    const char *pb_path = b->canonical_path ? b->canonical_path : "";
    int c = strcmp(pa_path, pb_path);
    if (c != 0) return c;
    if (a->span.line != b->span.line)
        return (a->span.line < b->span.line) ? -1 : 1;
    if (a->span.col != b->span.col)
        return (a->span.col < b->span.col) ? -1 : 1;
    return 0;
}

/* Parse "prefix/rest" into (prefix, rest). If no '/' in query, prefix
 * is NULL and rest == query. */
static void split_path_prefix(const char *query,
                                const char **out_prefix,
                                size_t      *out_prefix_len,
                                const char **out_rest) {
    *out_prefix     = NULL;
    *out_prefix_len = 0;
    *out_rest       = query;
    if (!query) return;
    const char *slash = strchr(query, '/');
    if (!slash) return;
    *out_prefix     = query;
    *out_prefix_len = (size_t)(slash - query);
    *out_rest       = slash + 1;
}

static bool path_contains_prefix(const char *canonical_path,
                                   const char *prefix,
                                   size_t      prefix_len) {
    if (!prefix || prefix_len == 0) return true;
    if (!canonical_path) return false;
    const char *p = canonical_path;
    size_t pl = strlen(p);
    for (size_t i = 0; i + prefix_len <= pl; i++) {
        if (strncmp(p + i, prefix, prefix_len) == 0) return true;
    }
    return false;
}

/* Score one decl against (name_needle, prefix/rest) and, if a match,
 * push a candidate row onto `*out_arr` (stb_ds). */
static void score_decl(IronLsp_Server *server,
                        IronLsp_IndexEntry *entry,
                        Iron_Node *d,
                        const char *name_needle,
                        const char *prefix, size_t prefix_len,
                        Iron_Arena *arena,
                        candidate_t **out_arr) {
    (void)server;
    if (!d || d->kind == IRON_NODE_ERROR) return;
    const char *nm = decl_name(d);
    if (!nm || !*nm) return;

    /* NEW Phase 10 VIS-02 (Pitfall 4): drop non-pub decls BEFORE the
     * candidate gets arrput-into the score array, so the 256-result
     * cap operates over a visibility-filtered candidate list.
     *
     * workspace/symbol has no requester URI in the LSP request shape;
     * the workspace IS the requester. Every caller not from the
     * symbol's own module is cross-module by definition. Therefore
     * the filter uses ilsp_vis_is_public directly (not
     * ilsp_vis_can_see with a requester) - non-pub means hidden
     * globally. documentSymbol path is unchanged (it does not call
     * score_decl). */
    if (!ilsp_vis_is_public(d)) {
        /* Stdlib carve-out (D-08): keep stdlib decls visible since
         * stdlib .iron files lack `pub` keywords pre-Phase-14 MIG. */
        if (!entry || !ilsp_nav_path_is_stdlib(entry->canonical_path)) {
            return;
        }
    }

    if (!path_contains_prefix(entry->canonical_path, prefix, prefix_len)) return;
    if (!ilsp_fuzzy_has_match(name_needle, nm)) return;
    double sc = ilsp_fuzzy_match(name_needle, nm, arena, NULL);
    if (!isfinite(sc)) return;

    candidate_t c;
    memset(&c, 0, sizeof(c));
    c.name           = iron_arena_strdup(arena, nm, strlen(nm));
    const char *cn   = decl_container(d);
    c.container_name = cn ? iron_arena_strdup(arena, cn, strlen(cn)) : "";
    c.canonical_path = entry->canonical_path
        ? iron_arena_strdup(arena, entry->canonical_path,
                             strlen(entry->canonical_path))
        : "";
    c.uri            = ilsp_nav_path_to_uri(entry->canonical_path, arena);
    if (!c.uri) c.uri = "";
    c.lsp_kind       = lsp_kind_from_node(d);
    c.kind_rank      = kind_rank_for_lsp(c.lsp_kind);
    c.score          = sc;
    c.span           = d->span;
    c.sel_span       = d->span;
    arrput(*out_arr, c);
}

/* Recurse one level into compound decls so methods / fields / variants
 * are searchable. */
static void score_program(IronLsp_Server *server,
                            IronLsp_IndexEntry *entry,
                            Iron_Program *program,
                            const char *name_needle,
                            const char *prefix, size_t prefix_len,
                            Iron_Arena *arena,
                            candidate_t **out_arr) {
    if (!program) return;
    for (int i = 0; i < program->decl_count; i++) {
        Iron_Node *d = program->decls[i];
        if (!d || d->kind == IRON_NODE_ERROR) continue;
        score_decl(server, entry, d, name_needle, prefix, prefix_len,
                    arena, out_arr);

        switch ((int)d->kind) {
            case IRON_NODE_OBJECT_DECL: {
                Iron_ObjectDecl *o = (Iron_ObjectDecl *)d;
                for (int j = 0; j < o->field_count; j++) {
                    score_decl(server, entry, o->fields[j], name_needle,
                                prefix, prefix_len, arena, out_arr);
                }
                break; }
            case IRON_NODE_INTERFACE_DECL: {
                Iron_InterfaceDecl *ifc = (Iron_InterfaceDecl *)d;
                for (int j = 0; j < ifc->method_count; j++) {
                    score_decl(server, entry, ifc->method_sigs[j],
                                name_needle, prefix, prefix_len,
                                arena, out_arr);
                }
                break; }
            case IRON_NODE_ENUM_DECL: {
                Iron_EnumDecl *e = (Iron_EnumDecl *)d;
                for (int j = 0; j < e->variant_count; j++) {
                    score_decl(server, entry, e->variants[j], name_needle,
                                prefix, prefix_len, arena, out_arr);
                }
                break; }
            default:
                break;
        }
    }
}

void ilsp_facade_nav_workspace_symbol(IronLsp_Server         *server,
                                        const char             *query,
                                        _Atomic bool           *cancel,
                                        Iron_Arena             *arena,
                                        IronLsp_WorkspaceSymbol **out,
                                        size_t                 *out_n) {
    if (out)   *out   = NULL;
    if (out_n) *out_n = 0;
    if (!server || !arena || !out || !out_n) return;
    if (!server->workspace_index) return;

    if (!query) query = "";
    size_t qlen = strlen(query);
    /* T-03-07: reject oversized queries. */
    if (qlen > ILSP_WORKSPACE_SYMBOL_QUERY_MAX) return;

    const char *prefix;
    size_t      prefix_len;
    const char *rest;
    split_path_prefix(query, &prefix, &prefix_len, &rest);

    candidate_t *cands = NULL;

    /* Iterate workspace_index entries. Coarse read -- the workspace
     * index owns its own mutex, but we take a snapshot under our own
     * discipline by reading the entries array once. */
    IronLsp_WorkspaceIndex *wi = server->workspace_index;
    size_t n_entries = (size_t)hmlenu(wi->entries);
    for (size_t i = 0; i < n_entries; i++) {
        if (cancel && atomic_load(cancel)) break;
        IronLsp_IndexEntry *entry = wi->entries[i].value;
        if (!entry || !entry->program) continue;
        score_program(server, entry, entry->program, rest,
                       prefix, prefix_len, arena, &cands);
    }

    size_t total = (size_t)arrlenu(cands);
    if (total == 0) {
        arrfree(cands);
        return;
    }

    qsort(cands, total, sizeof(candidate_t), candidate_cmp);

    /* Cap at 256. */
    if (total > ILSP_WORKSPACE_SYMBOL_MAX_RESULTS) {
        total = ILSP_WORKSPACE_SYMBOL_MAX_RESULTS;
    }

    IronLsp_WorkspaceSymbol *arr = (IronLsp_WorkspaceSymbol *)iron_arena_alloc(
        arena, sizeof(IronLsp_WorkspaceSymbol) * total,
        _Alignof(IronLsp_WorkspaceSymbol));
    if (!arr) { arrfree(cands); return; }

    /* For range conversion we need each entry's line index. Re-resolve
     * the entry per candidate (O(1) shmap lookup). */
    IronLsp_PositionEncoding enc = server->position_encoding;
    for (size_t i = 0; i < total; i++) {
        candidate_t *c = &cands[i];
        IronLsp_WorkspaceSymbol ws;
        memset(&ws, 0, sizeof(ws));
        ws.name           = c->name;
        ws.container_name = c->container_name;
        ws.kind           = c->lsp_kind;
        ws.uri            = c->uri;
        ws.fuzzy_score    = c->score;
        ws.kind_rank      = c->kind_rank;

        IronLsp_IndexEntry *entry =
            ilsp_workspace_index_lookup(wi, c->canonical_path);
        if (entry) {
            ws.range           = ilsp_nav_entry_span_to_range(entry, c->span,     enc);
            ws.selection_range = ilsp_nav_entry_span_to_range(entry, c->sel_span, enc);
        }
        arr[i] = ws;
    }

    *out   = arr;
    *out_n = total;
    arrfree(cands);
}
