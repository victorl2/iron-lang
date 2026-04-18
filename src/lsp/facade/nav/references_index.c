/* Phase 3 Plan 04 Task 01 (NAV-06, D-09, Pitfall 6) -- workspace-level
 * reverse reference index.
 *
 * Data structure:
 *   wi->refs is an stb_ds hmput/hmget map keyed by uint64_t (triple hash)
 *   with value = IronLsp_RefSpan *  (stb_ds dynamic array of use-sites).
 *
 * Per-entry sidemap:
 *   entry->ref_contributed_hashes is an stb_ds dynamic array of the
 *   triple-hashes this entry has contributed to. On drop, we walk this
 *   list and, for each hash, strip away any span whose canonical_path
 *   equals entry->canonical_path. On populate we always drop first
 *   (Pitfall 6 invariant).
 *
 * Filtering:
 *   ilsp_refs_query drops any span whose canonical_path begins with
 *   "stdlib://" or "dep://" -- D-09 LOCKED, always filtered.
 */

#include "lsp/facade/nav/references_index.h"

#include "lsp/facade/nav/nav_common.h"
#include "lsp/facade/nav/symbol_id.h"
#include "lsp/store/workspace_index.h"
#include "parser/ast.h"
#include "analyzer/scope.h"
#include "diagnostics/diagnostics.h"
#include "util/arena.h"
#include "vendor/stb_ds.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* ── Map entry type ──────────────────────────────────────────────── */

/* Struct-hashmap entry: key = triple_hash, value = stb_ds array of
 * IronLsp_RefSpan. stb_ds hmput/hmget operate on this exported type. */
typedef struct IronLsp_RefsMapEntry {
    uint64_t         key;
    IronLsp_RefSpan *spans;   /* stb_ds dynamic array */
} IronLsp_RefsMapEntry;

/* Local helper: look up the refs map entry for a given triple hash,
 * returning a pointer into the map's storage (or NULL on miss). The
 * stb_ds `hmgetp` family hands back a pointer so we can mutate the
 * array in place without writing back. */
static IronLsp_RefsMapEntry *get_map_entry(IronLsp_WorkspaceIndex *wi,
                                             uint64_t key) {
    if (!wi || !wi->refs) return NULL;
    ptrdiff_t idx = hmgeti(wi->refs, key);
    if (idx < 0) return NULL;
    return &wi->refs[idx];
}

/* ── Visitor state + recursive walker ─────────────────────────────── */

typedef struct {
    IronLsp_WorkspaceIndex *wi;
    IronLsp_IndexEntry     *entry;
} RefsVisitCtx;

/* Record an ident's span under its resolved_sym's identity triple. */
static void record_ident(RefsVisitCtx *ctx, Iron_Ident *id) {
    if (!id || !id->resolved_sym) return;
    Iron_Symbol *sym = id->resolved_sym;
    if (!sym->decl_node) return;

    /* The entry's arena is a safe, stable arena to intern the
     * triple's canonical_path + name_path into -- it dies with the
     * entry, which is exactly the lifetime contract we want for
     * this entry's contributions. */
    if (!ctx->entry->arena) return;

    /* Derive identity triple keyed on the DECL's owning file
     * (sym->decl_node->span.filename). That way a use-site in
     * file A referring to a decl in file B records under B's
     * triple -- which is what references/definition both expect. */
    const char *decl_path = sym->decl_node->span.filename;
    if (!decl_path || !*decl_path) decl_path = ctx->entry->canonical_path;

    /* For workspace symbols we want the canonical decl-file path.
     * workspace_index entries already use canonical paths; the AST
     * span.filename may or may not match -- match by suffix-equal
     * to the canonical_path. Use the raw decl_path for triple
     * derivation; filtering / cross-file resolution lives in the
     * query path. */
    IronLsp_SymbolId triple = ilsp_symbol_id_derive(
        sym, decl_path, ctx->entry->program, ctx->entry->arena);
    if (triple.hash == 0) return;

    /* Insert-or-append to the map. */
    IronLsp_RefsMapEntry *e = get_map_entry(ctx->wi, triple.hash);
    if (!e) {
        IronLsp_RefsMapEntry nent = { .key = triple.hash, .spans = NULL };
        hmputs(ctx->wi->refs, nent);
        e = get_map_entry(ctx->wi, triple.hash);
        if (!e) return;
    }

    IronLsp_RefSpan rs = { .span = id->span,
                            .canonical_path = ctx->entry->canonical_path };
    arrput(e->spans, rs);

    /* Track this triple hash in the entry's contributions side-map.
     * Duplicates are cheap to allow: drop does bulk filter-by-path. */
    bool already_tracked = false;
    ptrdiff_t n = arrlen(ctx->entry->ref_contributed_hashes);
    for (ptrdiff_t i = 0; i < n; i++) {
        if (ctx->entry->ref_contributed_hashes[i] == triple.hash) {
            already_tracked = true; break;
        }
    }
    if (!already_tracked) {
        arrput(ctx->entry->ref_contributed_hashes, triple.hash);
    }
}

/* Recursive walker: visits every Iron_Ident in the AST subtree.
 * Silently skips IRON_NODE_ERROR. */
static void walk_node(RefsVisitCtx *ctx, Iron_Node *n);

static void walk_nodes(RefsVisitCtx *ctx, Iron_Node **arr, int count) {
    if (!arr) return;
    for (int i = 0; i < count; i++) walk_node(ctx, arr[i]);
}

static void walk_node(RefsVisitCtx *ctx, Iron_Node *n) {
    if (!n || n->kind == IRON_NODE_ERROR) return;
    switch ((int)n->kind) {
        case IRON_NODE_IDENT:
            record_ident(ctx, (Iron_Ident *)n);
            break;

        /* Expressions with children. */
        case IRON_NODE_BINARY: {
            Iron_BinaryExpr *b = (Iron_BinaryExpr *)n;
            walk_node(ctx, b->left);
            walk_node(ctx, b->right);
            break;
        }
        case IRON_NODE_UNARY: {
            Iron_UnaryExpr *u = (Iron_UnaryExpr *)n;
            walk_node(ctx, u->operand);
            break;
        }
        case IRON_NODE_CALL: {
            Iron_CallExpr *c = (Iron_CallExpr *)n;
            walk_node(ctx, c->callee);
            walk_nodes(ctx, c->args, c->arg_count);
            break;
        }
        case IRON_NODE_METHOD_CALL: {
            Iron_MethodCallExpr *m = (Iron_MethodCallExpr *)n;
            walk_node(ctx, m->object);
            walk_nodes(ctx, m->args, m->arg_count);
            break;
        }
        case IRON_NODE_FIELD_ACCESS: {
            Iron_FieldAccess *fa = (Iron_FieldAccess *)n;
            walk_node(ctx, fa->object);
            break;
        }
        case IRON_NODE_INDEX: {
            Iron_IndexExpr *ix = (Iron_IndexExpr *)n;
            walk_node(ctx, ix->object);
            walk_node(ctx, ix->index);
            break;
        }
        case IRON_NODE_SLICE: {
            Iron_SliceExpr *sl = (Iron_SliceExpr *)n;
            walk_node(ctx, sl->object);
            walk_node(ctx, sl->start);
            walk_node(ctx, sl->end);
            break;
        }
        case IRON_NODE_INTERP_STRING: {
            Iron_InterpString *is = (Iron_InterpString *)n;
            walk_nodes(ctx, is->parts, is->part_count);
            break;
        }
        case IRON_NODE_ARRAY_LIT: {
            /* Generic layout: walk children by scanning subsequent
             * slots; rather than forcing the layout we treat array
             * literals as a leaf (identifiers inside array lits are
             * less common; completeness comes in a later plan). */
            break;
        }

        /* Statements with children. */
        case IRON_NODE_BLOCK: {
            Iron_Block *b = (Iron_Block *)n;
            walk_nodes(ctx, b->stmts, b->stmt_count);
            break;
        }
        case IRON_NODE_ASSIGN: {
            Iron_AssignStmt *a = (Iron_AssignStmt *)n;
            walk_node(ctx, a->target);
            walk_node(ctx, a->value);
            break;
        }
        case IRON_NODE_RETURN: {
            Iron_ReturnStmt *r = (Iron_ReturnStmt *)n;
            walk_node(ctx, r->value);
            break;
        }
        case IRON_NODE_IF: {
            Iron_IfStmt *ifs = (Iron_IfStmt *)n;
            walk_node(ctx, ifs->condition);
            walk_node(ctx, ifs->body);
            for (int i = 0; i < ifs->elif_count; i++) {
                walk_node(ctx, ifs->elif_conds[i]);
                walk_node(ctx, ifs->elif_bodies[i]);
            }
            walk_node(ctx, ifs->else_body);
            break;
        }
        case IRON_NODE_WHILE: {
            Iron_WhileStmt *w = (Iron_WhileStmt *)n;
            walk_node(ctx, w->condition);
            walk_node(ctx, w->body);
            break;
        }
        case IRON_NODE_FOR: {
            Iron_ForStmt *f = (Iron_ForStmt *)n;
            walk_node(ctx, f->iterable);
            walk_node(ctx, f->body);
            break;
        }
        case IRON_NODE_MATCH: {
            Iron_MatchStmt *m = (Iron_MatchStmt *)n;
            walk_node(ctx, m->subject);
            walk_nodes(ctx, m->cases, m->case_count);
            walk_node(ctx, m->else_body);
            break;
        }
        case IRON_NODE_MATCH_CASE: {
            Iron_MatchCase *mc = (Iron_MatchCase *)n;
            walk_node(ctx, mc->body);
            break;
        }
        case IRON_NODE_DEFER: {
            Iron_DeferStmt *d = (Iron_DeferStmt *)n;
            walk_node(ctx, d->expr);
            break;
        }
        case IRON_NODE_FREE: {
            Iron_FreeStmt *fr = (Iron_FreeStmt *)n;
            walk_node(ctx, fr->expr);
            break;
        }
        case IRON_NODE_LEAK: {
            Iron_LeakStmt *lk = (Iron_LeakStmt *)n;
            walk_node(ctx, lk->expr);
            break;
        }
        case IRON_NODE_SPAWN: {
            Iron_SpawnStmt *sp = (Iron_SpawnStmt *)n;
            walk_node(ctx, sp->pool_expr);
            walk_node(ctx, sp->body);
            break;
        }
        case IRON_NODE_VAL_DECL: {
            Iron_ValDecl *v = (Iron_ValDecl *)n;
            walk_node(ctx, v->init);
            break;
        }
        case IRON_NODE_VAR_DECL: {
            Iron_VarDecl *v = (Iron_VarDecl *)n;
            walk_node(ctx, v->init);
            break;
        }

        /* Top-level decls: walk bodies. */
        case IRON_NODE_FUNC_DECL: {
            Iron_FuncDecl *fd = (Iron_FuncDecl *)n;
            walk_node(ctx, fd->body);
            break;
        }
        case IRON_NODE_METHOD_DECL: {
            Iron_MethodDecl *md = (Iron_MethodDecl *)n;
            walk_node(ctx, md->body);
            break;
        }

        /* Leaves / unsupported (literals, imports, object/interface/
         * enum decls handled at the top level): ignore. */
        default:
            break;
    }
}

/* ── Public API ──────────────────────────────────────────────────── */

void ilsp_refs_drop_for_entry(IronLsp_WorkspaceIndex *wi,
                                IronLsp_IndexEntry     *entry) {
    if (!wi || !entry) return;
    if (!entry->ref_contributed_hashes) return;

    ptrdiff_t nh = arrlen(entry->ref_contributed_hashes);
    for (ptrdiff_t i = 0; i < nh; i++) {
        uint64_t h = entry->ref_contributed_hashes[i];
        IronLsp_RefsMapEntry *e = get_map_entry(wi, h);
        if (!e || !e->spans) continue;
        /* Remove every span whose canonical_path matches this entry.
         * In-place filter: copy survivors forward. */
        ptrdiff_t n = arrlen(e->spans);
        ptrdiff_t w = 0;
        for (ptrdiff_t r = 0; r < n; r++) {
            const char *ep = e->spans[r].canonical_path;
            if (ep && entry->canonical_path &&
                strcmp(ep, entry->canonical_path) == 0) {
                continue;  /* drop */
            }
            if (w != r) e->spans[w] = e->spans[r];
            w++;
        }
        /* Shrink the stb_ds array. */
        while (arrlen(e->spans) > w) {
            (void)arrpop(e->spans);
        }
        /* If the array emptied, leave the empty slot in place --
         * hmdel churn would invalidate indices into other entries. */
    }
    arrfree(entry->ref_contributed_hashes);
    entry->ref_contributed_hashes = NULL;
}

void ilsp_refs_populate_for_entry(IronLsp_WorkspaceIndex *wi,
                                    IronLsp_IndexEntry     *entry) {
    if (!wi || !entry || !entry->program) return;

    /* Ensure the map is initialized lazily: hmput on NULL first is
     * fine for stb_ds hmget, but we ensure consistent state here by
     * calling drop first (which tolerates NULL). */
    /* Pitfall 6: drop BEFORE populate. UNCONDITIONAL. */
    ilsp_refs_drop_for_entry(wi, entry);

    RefsVisitCtx ctx = { .wi = wi, .entry = entry };
    Iron_Program *p = entry->program;
    /* Walk top-level decls; for each, descend into bodies. */
    for (int i = 0; i < p->decl_count; i++) {
        walk_node(&ctx, p->decls[i]);
    }
}

/* Return true if the canonical_path is stdlib:// or dep:// (must be
 * filtered per D-09 LOCKED policy). */
static bool is_filtered_path(const char *path) {
    if (!path) return true;  /* malformed -- drop defensively */
    if (strncmp(path, "stdlib://", 9) == 0) return true;
    if (strncmp(path, "dep://",    6) == 0) return true;
    return false;
}

void ilsp_refs_query(IronLsp_WorkspaceIndex *wi,
                      IronLsp_SymbolId         triple,
                      Iron_Arena              *arena,
                      IronLsp_PositionEncoding enc,
                      IronLsp_RefSite        **out_sites,
                      size_t                  *out_n) {
    if (out_sites) *out_sites = NULL;
    if (out_n)     *out_n     = 0;
    if (!wi || !arena || !out_sites || !out_n) return;

    IronLsp_RefsMapEntry *e = get_map_entry(wi, triple.hash);
    if (!e || !e->spans) return;

    ptrdiff_t n = arrlen(e->spans);
    if (n <= 0) return;

    /* Two passes: count kept, then allocate + fill. UNCONDITIONAL
     * stdlib/dep filter per D-09. */
    size_t kept = 0;
    for (ptrdiff_t i = 0; i < n; i++) {
        if (!is_filtered_path(e->spans[i].canonical_path)) kept++;
    }
    if (kept == 0) return;

    IronLsp_RefSite *arr = (IronLsp_RefSite *)iron_arena_alloc(
        arena, kept * sizeof(*arr), _Alignof(IronLsp_RefSite));
    if (!arr) return;

    size_t w = 0;
    for (ptrdiff_t i = 0; i < n; i++) {
        const char *path = e->spans[i].canonical_path;
        if (is_filtered_path(path)) continue;  /* D-09 UNCONDITIONAL */

        /* Find the owning index entry to get its line_idx + source
         * bytes. The entry outlives the query (wi owns both). */
        IronLsp_IndexEntry *owner = ilsp_workspace_index_lookup(wi, path);
        const char *uri = ilsp_nav_path_to_uri(path, arena);
        arr[w].uri = uri ? uri : "";
        if (owner) {
            arr[w].range = ilsp_nav_entry_span_to_range(
                owner, e->spans[i].span, enc);
        } else {
            /* Owner missing (e.g. invalidated) -- emit zero range. */
            IronLsp_Range z = { {0,0}, {0,0} };
            arr[w].range = z;
        }
        w++;
    }
    *out_sites = arr;
    *out_n = w;
}

size_t ilsp_refs_index_total_sites(IronLsp_WorkspaceIndex *wi) {
    if (!wi || !wi->refs) return 0;
    size_t total = 0;
    ptrdiff_t n = hmlen(wi->refs);
    for (ptrdiff_t i = 0; i < n; i++) {
        total += (size_t)arrlen(wi->refs[i].spans);
    }
    return total;
}

void ilsp_refs_index_destroy(IronLsp_WorkspaceIndex *wi) {
    if (!wi || !wi->refs) return;
    ptrdiff_t n = hmlen(wi->refs);
    for (ptrdiff_t i = 0; i < n; i++) {
        arrfree(wi->refs[i].spans);
    }
    hmfree(wi->refs);
    wi->refs = NULL;
}
