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
        out->range = ilsp_span_to_lsp_range(dspan, doc, enc);
        out->uri = doc->uri
            ? iron_arena_strdup(arena, doc->uri, strlen(doc->uri)) : "";
    } else if (wi && fn) {
        IronLsp_IndexEntry *entry = ilsp_workspace_index_lookup(wi, fn);
        if (entry) {
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
     * sites get filtered. */
    if (raw_sites && raw_n > 0) {
        const char *requester = (doc && doc->uri) ? doc->uri : "";
        size_t kept = 0;
        for (size_t i = 0; i < raw_n; i++) {
            if (ilsp_vis_can_see(raw_sites[i].uri, requester,
                                  sym ? sym->decl_node : NULL)) {
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
        /* Walk top-level decls and bodies (minimal walker -- mirror
         * of references_index walker, simplified for self-scope). */
        /* Iterative stack-based walk avoiding a second visitor TU. */
        Iron_Node **stack = NULL;
        size_t sp = 0, sc = 64;
        stack = (Iron_Node **)malloc(sc * sizeof(*stack));
        if (!stack) goto assemble;
        for (int i = 0; i < program->decl_count; i++) {
            if (program->decls[i]) {
                if (sp >= sc) {
                    sc *= 2;
                    Iron_Node **ns = (Iron_Node **)realloc(stack, sc * sizeof(*stack));
                    if (!ns) { free(stack); goto assemble; }
                    stack = ns;
                }
                stack[sp++] = program->decls[i];
            }
        }
#define PUSH(n) do { if ((n)) { if (sp >= sc) { sc *= 2; Iron_Node **ns = (Iron_Node **)realloc(stack, sc*sizeof(*stack)); if (!ns) goto stack_done; stack = ns; } stack[sp++] = (n); } } while (0)
        while (sp > 0) {
            Iron_Node *cur = stack[--sp];
            if (!cur || cur->kind == IRON_NODE_ERROR) continue;
            switch ((int)cur->kind) {
                case IRON_NODE_IDENT: {
                    Iron_Ident *id = (Iron_Ident *)cur;
                    if (id->resolved_sym && id->resolved_sym->decl_node == sym->decl_node) {
                        if (fallback_n == cap) {
                            size_t ncap = cap * 2;
                            IronLsp_RefSite *nf = (IronLsp_RefSite *)iron_arena_alloc(
                                arena, ncap * sizeof(*fallback), _Alignof(IronLsp_RefSite));
                            if (!nf) break;
                            memcpy(nf, fallback, fallback_n * sizeof(*fallback));
                            fallback = nf; cap = ncap;
                        }
                        fallback[fallback_n].uri = doc->uri
                            ? iron_arena_strdup(arena, doc->uri, strlen(doc->uri)) : "";
                        fallback[fallback_n].range = ilsp_span_to_lsp_range(
                            id->span, doc, enc);
                        fallback_n++;
                    }
                    break;
                }
                case IRON_NODE_FUNC_DECL: { Iron_FuncDecl *fd = (Iron_FuncDecl *)cur;   PUSH(fd->body); break; }
                case IRON_NODE_METHOD_DECL:{ Iron_MethodDecl *md = (Iron_MethodDecl *)cur; PUSH(md->body); break; }
                case IRON_NODE_BLOCK: { Iron_Block *b = (Iron_Block *)cur;
                    for (int i = 0; i < b->stmt_count; i++) PUSH(b->stmts[i]); break; }
                case IRON_NODE_BINARY: { Iron_BinaryExpr *e = (Iron_BinaryExpr *)cur;
                    PUSH(e->left); PUSH(e->right); break; }
                case IRON_NODE_UNARY: { Iron_UnaryExpr *e = (Iron_UnaryExpr *)cur;
                    PUSH(e->operand); break; }
                case IRON_NODE_CALL: { Iron_CallExpr *c = (Iron_CallExpr *)cur;
                    PUSH(c->callee); for (int i = 0; i < c->arg_count; i++) PUSH(c->args[i]); break; }
                case IRON_NODE_METHOD_CALL: { Iron_MethodCallExpr *m = (Iron_MethodCallExpr *)cur;
                    PUSH(m->object); for (int i = 0; i < m->arg_count; i++) PUSH(m->args[i]); break; }
                case IRON_NODE_FIELD_ACCESS: { Iron_FieldAccess *fa = (Iron_FieldAccess *)cur;
                    PUSH(fa->object); break; }
                case IRON_NODE_INDEX: { Iron_IndexExpr *ix = (Iron_IndexExpr *)cur;
                    PUSH(ix->object); PUSH(ix->index); break; }
                case IRON_NODE_SLICE: { Iron_SliceExpr *sl = (Iron_SliceExpr *)cur;
                    PUSH(sl->object); PUSH(sl->start); PUSH(sl->end); break; }
                case IRON_NODE_ASSIGN: { Iron_AssignStmt *a = (Iron_AssignStmt *)cur;
                    PUSH(a->target); PUSH(a->value); break; }
                case IRON_NODE_RETURN: { Iron_ReturnStmt *r = (Iron_ReturnStmt *)cur; PUSH(r->value); break; }
                case IRON_NODE_IF: { Iron_IfStmt *s = (Iron_IfStmt *)cur;
                    PUSH(s->condition); PUSH(s->body);
                    for (int i = 0; i < s->elif_count; i++) { PUSH(s->elif_conds[i]); PUSH(s->elif_bodies[i]); }
                    PUSH(s->else_body); break; }
                case IRON_NODE_WHILE: { Iron_WhileStmt *w = (Iron_WhileStmt *)cur;
                    PUSH(w->condition); PUSH(w->body); break; }
                case IRON_NODE_FOR: { Iron_ForStmt *f = (Iron_ForStmt *)cur;
                    PUSH(f->iterable); PUSH(f->body); break; }
                case IRON_NODE_VAL_DECL: { Iron_ValDecl *v = (Iron_ValDecl *)cur; PUSH(v->init); break; }
                case IRON_NODE_VAR_DECL: { Iron_VarDecl *v = (Iron_VarDecl *)cur; PUSH(v->init); break; }
                case IRON_NODE_MATCH: { Iron_MatchStmt *m = (Iron_MatchStmt *)cur;
                    PUSH(m->subject); for (int i = 0; i < m->case_count; i++) PUSH(m->cases[i]);
                    PUSH(m->else_body); break; }
                case IRON_NODE_MATCH_CASE: { Iron_MatchCase *mc = (Iron_MatchCase *)cur; PUSH(mc->body); break; }
                case IRON_NODE_DEFER: { Iron_DeferStmt *d = (Iron_DeferStmt *)cur; PUSH(d->expr); break; }
                case IRON_NODE_FREE:  { Iron_FreeStmt  *d = (Iron_FreeStmt  *)cur; PUSH(d->expr); break; }
                case IRON_NODE_LEAK:  { Iron_LeakStmt  *d = (Iron_LeakStmt  *)cur; PUSH(d->expr); break; }
                case IRON_NODE_SPAWN: { Iron_SpawnStmt *d = (Iron_SpawnStmt *)cur; PUSH(d->pool_expr); PUSH(d->body); break; }
                case IRON_NODE_INTERP_STRING: { Iron_InterpString *i_s = (Iron_InterpString *)cur;
                    for (int i = 0; i < i_s->part_count; i++) PUSH(i_s->parts[i]); break; }
                default: break;
            }
        }
#undef PUSH
stack_done:
        free(stack);
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
