/* Phase 4 Plan 04-07 Task 01 (EDIT-13, D-12) -- documentHighlight facade.
 *
 * Algorithm:
 *   1. Analyze current document via ilsp_facade_compile_for_nav so that
 *      resolved_sym is populated on every Iron_Ident.
 *   2. Resolve cursor -> Iron_Node via ilsp_nav_node_at. Require the
 *      hit to be an Iron_Ident with resolved_sym != NULL -- otherwise
 *      return an empty array (D-12 T-4-1 rule).
 *   3. Iterative stack walk of Iron_Program. At every Iron_Ident whose
 *      resolved_sym->decl_node matches the target's decl_node (identity
 *      match -- same Iron_Symbol), classify via classify_readwrite()
 *      using the parent node + the ident's role within that parent.
 *   4. NEVER descend into IRON_NODE_STRING_LIT. Interpolated string
 *      parts (IRON_NODE_INTERP_STRING) DO contain ident children --
 *      we walk those so idents referenced inside "${x}" are classified.
 *   5. The decl site itself (when the target's decl_node carries a name)
 *      is seeded as a Write highlight before the walk starts.
 *   6. Cancel polled once at entry (single-file walk, bounded by AST).
 *
 * The classifier is -Werror=switch-enum safe: explicit default branch
 * returns Read for any Iron_NodeKind that doesn't have a dedicated
 * classification rule (safe fallback). */

#include "lsp/facade/edit/highlight.h"

#include "lsp/facade/compile.h"
#include "lsp/facade/nav/node_at.h"
#include "lsp/facade/span.h"
#include "lsp/store/document.h"
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

/* LSP DocumentHighlightKind constants. */
#define HIGHLIGHT_READ  2
#define HIGHLIGHT_WRITE 3

/* Walk frame: the current node + its parent + the ident's role within
 * that parent. role_tag convention (matches D-12 classifier table):
 *   For IRON_NODE_ASSIGN       : 0 = target (LHS), 1 = value (RHS).
 *   For IRON_NODE_FIELD_ACCESS : 0 = object (receiver), 1 = field token.
 *                                 (Iron_FieldAccess stores field as a
 *                                  plain const char* — there is no ident
 *                                  child to walk; we never hit role 1 via
 *                                  this walker. Kept for symmetry.)
 *   For IRON_NODE_CALL         : 0 = callee, 1+ = arg positions (Read).
 *   Default                    : 0.
 *
 * parent_is_assign_lhs_chain is a boolean carried down through
 * FIELD_ACCESS chains so that writes to `obj.a.b` classify the final
 * field as Write (D-12 "field being written" rule). */
typedef struct {
    Iron_Node *node;
    Iron_Node *parent;
    int        role_tag;
    bool       parent_is_assign_lhs_chain;
} Walk_Frame;

/* Classify a single Iron_Ident hit given its parent context. Returns
 * LSP kind code (2 = Read, 3 = Write). */
static int classify_readwrite(Iron_Node *parent,
                                int        role_tag,
                                bool       parent_is_assign_lhs_chain) {
    if (!parent) return HIGHLIGHT_READ;
    switch ((int)parent->kind) {
        case IRON_NODE_VAL_DECL:
        case IRON_NODE_VAR_DECL:
        case IRON_NODE_PARAM:
        case IRON_NODE_FIELD:
        case IRON_NODE_ENUM_VARIANT:
        case IRON_NODE_FOR:
            /* Decl / binding sites — the ident IS the name slot. */
            return HIGHLIGHT_WRITE;
        case IRON_NODE_ASSIGN: {
            /* role 0 = target, 1 = value. Plain = and compound +=/-=/
             * etc. all map LHS -> Write, RHS -> Read per LSP spec
             * (compound-assign LHS is traditionally surfaced as Write
             * even though it reads-then-writes). */
            if (role_tag == 0) return HIGHLIGHT_WRITE;
            return HIGHLIGHT_READ;
        }
        case IRON_NODE_FIELD_ACCESS: {
            /* receiver is always Read. Field tokens are not ident
             * children on Iron_FieldAccess (field is a const char*), so
             * role_tag=0 is the only case that resolves a child ident
             * through this parent. */
            return HIGHLIGHT_READ;
        }
        case IRON_NODE_CALL:
        case IRON_NODE_METHOD_CALL:
            return HIGHLIGHT_READ;
        case IRON_NODE_METHOD_DECL:
        case IRON_NODE_FUNC_DECL:
            /* Func/Method decl's params + body idents are walked under
             * their own inner parents; a bare ident whose parent is the
             * decl itself is the name slot. */
            return HIGHLIGHT_WRITE;
        case IRON_NODE_OBJECT_DECL:
        case IRON_NODE_INTERFACE_DECL:
        case IRON_NODE_ENUM_DECL:
            return HIGHLIGHT_WRITE;
        default:
            /* Interp string parts, binary/unary expressions, if/while/
             * match conditions, return values, etc. all fall through
             * to Read. parent_is_assign_lhs_chain lets a future
             * enhancement promote these to Write if needed -- today,
             * we stick to the D-12 spec. */
            (void)parent_is_assign_lhs_chain;
            return HIGHLIGHT_READ;
    }
}

/* Emit one highlight row into a caller-maintained growing array. */
static bool emit_highlight(IronLsp_DocumentHighlight **arr,
                             size_t                     *n,
                             size_t                     *cap,
                             Iron_Arena                 *arena,
                             Iron_Span                   span,
                             const IronLsp_Document     *doc,
                             IronLsp_PositionEncoding    enc,
                             int                         kind) {
    if (*n == *cap) {
        size_t new_cap = (*cap == 0) ? 8 : (*cap * 2);
        IronLsp_DocumentHighlight *next = (IronLsp_DocumentHighlight *)
            iron_arena_alloc(arena,
                              new_cap * sizeof(**arr),
                              _Alignof(IronLsp_DocumentHighlight));
        if (!next) return false;
        if (*arr && *n > 0) memcpy(next, *arr, (*n) * sizeof(**arr));
        *arr = next;
        *cap = new_cap;
    }
    IronLsp_Range r = ilsp_span_to_lsp_range(span, doc, enc);
    (*arr)[*n].range_start_line = r.start.line;
    (*arr)[*n].range_start_char = r.start.character;
    (*arr)[*n].range_end_line   = r.end.line;
    (*arr)[*n].range_end_char   = r.end.character;
    (*arr)[*n].kind             = kind;
    (*n)++;
    return true;
}

/* Push a Walk_Frame onto the stack, growing as needed. */
static bool stack_push(Walk_Frame **stack, size_t *sp, size_t *sc,
                        Iron_Node *node, Iron_Node *parent,
                        int role_tag, bool lhs_chain) {
    if (!node) return true;
    if (*sp >= *sc) {
        size_t new_sc = (*sc == 0) ? 64 : (*sc * 2);
        Walk_Frame *nf = (Walk_Frame *)realloc(*stack, new_sc * sizeof(**stack));
        if (!nf) return false;
        *stack = nf;
        *sc = new_sc;
    }
    (*stack)[*sp].node      = node;
    (*stack)[*sp].parent    = parent;
    (*stack)[*sp].role_tag  = role_tag;
    (*stack)[*sp].parent_is_assign_lhs_chain = lhs_chain;
    (*sp)++;
    return true;
}

#define PUSH(n, p, r, l) do { \
    if (!stack_push(&stack, &sp, &sc, (Iron_Node *)(n), (Iron_Node *)(p), (r), (l))) goto done; \
} while (0)

void ilsp_facade_document_highlight(struct IronLsp_Server      *server,
                                      struct IronLsp_Document    *doc,
                                      IronLsp_Position            pos,
                                      _Atomic bool               *cancel,
                                      Iron_Arena                 *arena,
                                      IronLsp_DocumentHighlight **out,
                                      size_t                     *out_n) {
    if (out)   *out   = NULL;
    if (out_n) *out_n = 0;
    if (!server || !doc || !arena || !out || !out_n) return;

    IronLsp_PositionEncoding enc = server->position_encoding;

    /* Step 1: analyze current doc via the nav seam. */
    Iron_Arena    walk_arena = iron_arena_create(64 * 1024);
    Iron_DiagList walk_diags = iron_diaglist_create();
    IronLsp_CompileRequest req = { .version = doc->version,
                                    .cancel_flag = cancel };
    Iron_Program *program = ilsp_facade_compile_for_nav(
        doc, &req, &walk_arena, &walk_diags);
    if (!program) goto cleanup;
    if (cancel && atomic_load(cancel)) goto cleanup;

    /* Step 2: resolve cursor -> Iron_Ident -> Iron_Symbol. */
    Iron_Node *node = ilsp_nav_node_at(doc, program, pos, enc);
    if (!node) goto cleanup;
    /* Walk up: Plan 3 node_at returns the covering decl; we need an
     * ident specifically. For v1 we support cursor directly on an
     * ident hit via a tight secondary walk — the cursor descends with
     * node_at to decl granularity, so we sweep the decl for the
     * innermost ident containing the position. */

    Iron_Symbol *target_sym = NULL;
    Iron_Node   *target_decl_node = NULL;
    if (node->kind == IRON_NODE_IDENT) {
        Iron_Ident *id = (Iron_Ident *)node;
        target_sym = id->resolved_sym;
    }
    /* Step 3: if node_at didn't resolve to an ident (Plan 3 node_at
     * descends only to decl level), sweep program for the ident whose
     * span strictly covers (pos.line+1, byte_col). We do this by
     * walking the program once collecting every ident whose span
     * contains the cursor's 1-based line/col — the innermost (last)
     * one wins. */
    if (!target_sym) {
        /* Derive 1-based line/col as the walker does. */
        uint32_t cur_line = pos.line + 1;
        /* byte-column conversion: for simplicity on M3 (UTF-8 path is
         * the majority) use the raw character column + 1. This matches
         * the ident spans which are byte-based. For UTF-16 negotiation
         * the cursor may be off-by-bytes on multibyte lines; the fallback
         * returns empty and the UI doesn't highlight — acceptable for
         * M3 and consistent with node_at's encoding contract. */
        uint32_t cur_col = pos.character + 1;
        for (int i = 0; i < program->decl_count; i++) {
            Iron_Node *d = program->decls[i];
            if (!d || d->kind == IRON_NODE_ERROR) continue;
            /* Small per-decl walk gathering idents covering cursor. */
            Walk_Frame *s = NULL; size_t sp = 0, sc = 0;
            (void)stack_push(&s, &sp, &sc, d, NULL, 0, false);
            while (sp > 0) {
                Walk_Frame f = s[--sp];
                Iron_Node *cur = f.node;
                if (!cur || cur->kind == IRON_NODE_ERROR) continue;
                if (cur->kind == IRON_NODE_IDENT) {
                    Iron_Ident *id = (Iron_Ident *)cur;
                    Iron_Span sp_n = cur->span;
                    bool covers = true;
                    if (cur_line < sp_n.line || cur_line > sp_n.end_line) covers = false;
                    else if (cur_line == sp_n.line && cur_col < sp_n.col) covers = false;
                    else if (cur_line == sp_n.end_line && cur_col > sp_n.end_col) covers = false;
                    if (covers && id->resolved_sym) {
                        target_sym = id->resolved_sym;
                        /* keep sweeping for a narrower match (inner ident
                         * would typically be last pushed); let later hits
                         * overwrite. */
                    }
                }
                switch ((int)cur->kind) {
                    case IRON_NODE_FUNC_DECL: { Iron_FuncDecl *fd = (Iron_FuncDecl *)cur;
                        (void)stack_push(&s, &sp, &sc, fd->body, cur, 0, false); break; }
                    case IRON_NODE_METHOD_DECL:{ Iron_MethodDecl *md = (Iron_MethodDecl *)cur;
                        (void)stack_push(&s, &sp, &sc, md->body, cur, 0, false); break; }
                    case IRON_NODE_BLOCK: { Iron_Block *b = (Iron_Block *)cur;
                        for (int j = 0; j < b->stmt_count; j++)
                            (void)stack_push(&s, &sp, &sc, b->stmts[j], cur, 0, false);
                        break; }
                    case IRON_NODE_BINARY: { Iron_BinaryExpr *e = (Iron_BinaryExpr *)cur;
                        (void)stack_push(&s, &sp, &sc, e->left,  cur, 0, false);
                        (void)stack_push(&s, &sp, &sc, e->right, cur, 0, false); break; }
                    case IRON_NODE_UNARY: { Iron_UnaryExpr *e = (Iron_UnaryExpr *)cur;
                        (void)stack_push(&s, &sp, &sc, e->operand, cur, 0, false); break; }
                    case IRON_NODE_CALL: { Iron_CallExpr *c = (Iron_CallExpr *)cur;
                        (void)stack_push(&s, &sp, &sc, c->callee, cur, 0, false);
                        for (int j = 0; j < c->arg_count; j++)
                            (void)stack_push(&s, &sp, &sc, c->args[j], cur, 1 + j, false);
                        break; }
                    case IRON_NODE_METHOD_CALL: { Iron_MethodCallExpr *m = (Iron_MethodCallExpr *)cur;
                        (void)stack_push(&s, &sp, &sc, m->object, cur, 0, false);
                        for (int j = 0; j < m->arg_count; j++)
                            (void)stack_push(&s, &sp, &sc, m->args[j], cur, 1 + j, false);
                        break; }
                    case IRON_NODE_FIELD_ACCESS: { Iron_FieldAccess *fa = (Iron_FieldAccess *)cur;
                        (void)stack_push(&s, &sp, &sc, fa->object, cur, 0, false); break; }
                    case IRON_NODE_INDEX: { Iron_IndexExpr *ix = (Iron_IndexExpr *)cur;
                        (void)stack_push(&s, &sp, &sc, ix->object, cur, 0, false);
                        (void)stack_push(&s, &sp, &sc, ix->index,  cur, 1, false); break; }
                    case IRON_NODE_ASSIGN: { Iron_AssignStmt *a = (Iron_AssignStmt *)cur;
                        (void)stack_push(&s, &sp, &sc, a->target, cur, 0, true);
                        (void)stack_push(&s, &sp, &sc, a->value,  cur, 1, false); break; }
                    case IRON_NODE_VAL_DECL: { Iron_ValDecl *v = (Iron_ValDecl *)cur;
                        (void)stack_push(&s, &sp, &sc, v->init, cur, 1, false); break; }
                    case IRON_NODE_VAR_DECL: { Iron_VarDecl *v = (Iron_VarDecl *)cur;
                        (void)stack_push(&s, &sp, &sc, v->init, cur, 1, false); break; }
                    case IRON_NODE_IF: { Iron_IfStmt *st = (Iron_IfStmt *)cur;
                        (void)stack_push(&s, &sp, &sc, st->condition, cur, 0, false);
                        (void)stack_push(&s, &sp, &sc, st->body,      cur, 1, false);
                        for (int j = 0; j < st->elif_count; j++) {
                            (void)stack_push(&s, &sp, &sc, st->elif_conds[j],  cur, 0, false);
                            (void)stack_push(&s, &sp, &sc, st->elif_bodies[j], cur, 1, false);
                        }
                        (void)stack_push(&s, &sp, &sc, st->else_body, cur, 1, false); break; }
                    case IRON_NODE_WHILE: { Iron_WhileStmt *w = (Iron_WhileStmt *)cur;
                        (void)stack_push(&s, &sp, &sc, w->condition, cur, 0, false);
                        (void)stack_push(&s, &sp, &sc, w->body, cur, 1, false); break; }
                    case IRON_NODE_FOR: { Iron_ForStmt *ff = (Iron_ForStmt *)cur;
                        (void)stack_push(&s, &sp, &sc, ff->iterable, cur, 1, false);
                        (void)stack_push(&s, &sp, &sc, ff->body, cur, 2, false); break; }
                    case IRON_NODE_MATCH: { Iron_MatchStmt *m = (Iron_MatchStmt *)cur;
                        (void)stack_push(&s, &sp, &sc, m->subject, cur, 0, false);
                        for (int j = 0; j < m->case_count; j++)
                            (void)stack_push(&s, &sp, &sc, m->cases[j], cur, 1, false);
                        (void)stack_push(&s, &sp, &sc, m->else_body, cur, 1, false); break; }
                    case IRON_NODE_MATCH_CASE: { Iron_MatchCase *mc = (Iron_MatchCase *)cur;
                        (void)stack_push(&s, &sp, &sc, mc->body, cur, 1, false); break; }
                    case IRON_NODE_RETURN: { Iron_ReturnStmt *r = (Iron_ReturnStmt *)cur;
                        (void)stack_push(&s, &sp, &sc, r->value, cur, 0, false); break; }
                    case IRON_NODE_DEFER: { Iron_DeferStmt *ds = (Iron_DeferStmt *)cur;
                        (void)stack_push(&s, &sp, &sc, ds->expr, cur, 0, false); break; }
                    case IRON_NODE_FREE:  { Iron_FreeStmt  *ds = (Iron_FreeStmt  *)cur;
                        (void)stack_push(&s, &sp, &sc, ds->expr, cur, 0, false); break; }
                    case IRON_NODE_LEAK:  { Iron_LeakStmt  *ds = (Iron_LeakStmt  *)cur;
                        (void)stack_push(&s, &sp, &sc, ds->expr, cur, 0, false); break; }
                    case IRON_NODE_SPAWN: { Iron_SpawnStmt *sp_n = (Iron_SpawnStmt *)cur;
                        (void)stack_push(&s, &sp, &sc, sp_n->pool_expr, cur, 0, false);
                        (void)stack_push(&s, &sp, &sc, sp_n->body, cur, 1, false); break; }
                    case IRON_NODE_INTERP_STRING: { Iron_InterpString *is = (Iron_InterpString *)cur;
                        for (int j = 0; j < is->part_count; j++)
                            (void)stack_push(&s, &sp, &sc, is->parts[j], cur, j, false);
                        break; }
                    /* Explicitly skip STRING_LIT — do not walk into string contents. */
                    case IRON_NODE_STRING_LIT: break;
                    default: break;
                }
            }
            free(s);
            if (target_sym) break;
        }
    }
    if (!target_sym) goto cleanup;
    target_decl_node = target_sym->decl_node;

    if (cancel && atomic_load(cancel)) goto cleanup;

    /* Step 4: outer walk — collect every ident whose resolved_sym->decl_node
     * matches target_decl_node. Each match is classified by its parent. */
    IronLsp_DocumentHighlight *arr = NULL;
    size_t arr_n = 0, arr_cap = 0;

    /* Seed: the decl site itself is a Write. Only emit if decl_node
     * lives in this document (span filename matches or is empty). */
    if (target_decl_node) {
        Iron_Span dspan = target_decl_node->span;
        bool same_doc = true;
        const char *fn = dspan.filename;
        if (fn && *fn && doc->uri && strcmp(fn, doc->uri) != 0) {
            /* Cross-file decl — skip, the current doc may reference it
             * but the decl site is elsewhere. */
            size_t fl = strlen(fn);
            size_t ul = strlen(doc->uri);
            same_doc = (ul >= fl && strcmp(doc->uri + (ul - fl), fn) == 0);
        }
        if (same_doc) {
            /* Narrow the decl span to just the name where we can. */
            Iron_Span name_span = dspan;
            switch ((int)target_decl_node->kind) {
                case IRON_NODE_FUNC_DECL: {
                    Iron_FuncDecl *fd = (Iron_FuncDecl *)target_decl_node;
                    (void)fd; /* name span reconstruction would need token
                               * tracking; keep full span as seed. */
                    break;
                }
                default: break;
            }
            (void)emit_highlight(&arr, &arr_n, &arr_cap, arena,
                                   name_span, doc, enc, HIGHLIGHT_WRITE);
        }
    }

    /* Outer walker: identical shape to the cursor-sweep above but
     * collects idents matching target_decl_node. */
    Walk_Frame *stack = NULL;
    size_t sp = 0, sc = 0;
    for (int i = 0; i < program->decl_count; i++) {
        if (program->decls[i])
            PUSH(program->decls[i], NULL, 0, false);
    }
    while (sp > 0) {
        Walk_Frame f = stack[--sp];
        Iron_Node *cur = f.node;
        if (!cur || cur->kind == IRON_NODE_ERROR) continue;

        if (cur->kind == IRON_NODE_IDENT) {
            Iron_Ident *id = (Iron_Ident *)cur;
            if (id->resolved_sym &&
                id->resolved_sym->decl_node == target_decl_node) {
                /* Skip re-emitting the decl-site ident if the seed
                 * already covered it. Identity check on span equality
                 * is cheap and good enough for M3. */
                bool already_decl_seed = false;
                if (arr_n > 0 && target_decl_node) {
                    Iron_Span cs = cur->span;
                    IronLsp_Range r0 = ilsp_span_to_lsp_range(cs, doc, enc);
                    if (arr[0].range_start_line == r0.start.line &&
                        arr[0].range_start_char == r0.start.character &&
                        arr[0].range_end_line   == r0.end.line &&
                        arr[0].range_end_char   == r0.end.character) {
                        already_decl_seed = true;
                    }
                }
                if (!already_decl_seed) {
                    int k = classify_readwrite(f.parent, f.role_tag,
                                                 f.parent_is_assign_lhs_chain);
                    if (!emit_highlight(&arr, &arr_n, &arr_cap, arena,
                                          cur->span, doc, enc, k)) goto done;
                }
            }
        }

        switch ((int)cur->kind) {
            case IRON_NODE_FUNC_DECL: { Iron_FuncDecl *fd = (Iron_FuncDecl *)cur;
                for (int j = 0; j < fd->param_count; j++)
                    PUSH(fd->params[j], cur, 0, false);
                PUSH(fd->body, cur, 0, false); break; }
            case IRON_NODE_METHOD_DECL:{ Iron_MethodDecl *md = (Iron_MethodDecl *)cur;
                for (int j = 0; j < md->param_count; j++)
                    PUSH(md->params[j], cur, 0, false);
                PUSH(md->body, cur, 0, false); break; }
            case IRON_NODE_OBJECT_DECL:{ Iron_ObjectDecl *od = (Iron_ObjectDecl *)cur;
                for (int j = 0; j < od->field_count; j++)
                    PUSH(od->fields[j], cur, 0, false); break; }
            case IRON_NODE_INTERFACE_DECL:{ Iron_InterfaceDecl *id_n = (Iron_InterfaceDecl *)cur;
                for (int j = 0; j < id_n->method_count; j++)
                    PUSH(id_n->method_sigs[j], cur, 0, false); break; }
            case IRON_NODE_ENUM_DECL: { Iron_EnumDecl *ed = (Iron_EnumDecl *)cur;
                for (int j = 0; j < ed->variant_count; j++)
                    PUSH(ed->variants[j], cur, 0, false); break; }
            case IRON_NODE_BLOCK: { Iron_Block *b = (Iron_Block *)cur;
                for (int j = 0; j < b->stmt_count; j++)
                    PUSH(b->stmts[j], cur, 0, false); break; }
            case IRON_NODE_BINARY: { Iron_BinaryExpr *e = (Iron_BinaryExpr *)cur;
                PUSH(e->left,  cur, 0, f.parent_is_assign_lhs_chain);
                PUSH(e->right, cur, 1, false); break; }
            case IRON_NODE_UNARY: { Iron_UnaryExpr *e = (Iron_UnaryExpr *)cur;
                PUSH(e->operand, cur, 0, false); break; }
            case IRON_NODE_CALL: { Iron_CallExpr *c = (Iron_CallExpr *)cur;
                PUSH(c->callee, cur, 0, false);
                for (int j = 0; j < c->arg_count; j++)
                    PUSH(c->args[j], cur, 1 + j, false);
                break; }
            case IRON_NODE_METHOD_CALL: { Iron_MethodCallExpr *m = (Iron_MethodCallExpr *)cur;
                PUSH(m->object, cur, 0, false);
                for (int j = 0; j < m->arg_count; j++)
                    PUSH(m->args[j], cur, 1 + j, false);
                break; }
            case IRON_NODE_FIELD_ACCESS: { Iron_FieldAccess *fa = (Iron_FieldAccess *)cur;
                /* Propagate assign-lhs-chain so `a.b.c = 1` marks `a`
                 * as Read (receiver) but the final ident (if any)
                 * classification can use the chain flag. */
                PUSH(fa->object, cur, 0, f.parent_is_assign_lhs_chain); break; }
            case IRON_NODE_INDEX: { Iron_IndexExpr *ix = (Iron_IndexExpr *)cur;
                PUSH(ix->object, cur, 0, f.parent_is_assign_lhs_chain);
                PUSH(ix->index,  cur, 1, false); break; }
            case IRON_NODE_SLICE: { Iron_SliceExpr *sl = (Iron_SliceExpr *)cur;
                PUSH(sl->object, cur, 0, false);
                PUSH(sl->start,  cur, 1, false);
                PUSH(sl->end,    cur, 1, false); break; }
            case IRON_NODE_ASSIGN: { Iron_AssignStmt *a = (Iron_AssignStmt *)cur;
                PUSH(a->target, cur, 0, true);
                PUSH(a->value,  cur, 1, false); break; }
            case IRON_NODE_VAL_DECL: { Iron_ValDecl *v = (Iron_ValDecl *)cur;
                PUSH(v->init, cur, 1, false); break; }
            case IRON_NODE_VAR_DECL: { Iron_VarDecl *v = (Iron_VarDecl *)cur;
                PUSH(v->init, cur, 1, false); break; }
            case IRON_NODE_IF: { Iron_IfStmt *st = (Iron_IfStmt *)cur;
                PUSH(st->condition, cur, 0, false);
                PUSH(st->body,      cur, 1, false);
                for (int j = 0; j < st->elif_count; j++) {
                    PUSH(st->elif_conds[j],  cur, 0, false);
                    PUSH(st->elif_bodies[j], cur, 1, false);
                }
                PUSH(st->else_body, cur, 1, false); break; }
            case IRON_NODE_WHILE: { Iron_WhileStmt *w = (Iron_WhileStmt *)cur;
                PUSH(w->condition, cur, 0, false);
                PUSH(w->body, cur, 1, false); break; }
            case IRON_NODE_FOR: { Iron_ForStmt *ff = (Iron_ForStmt *)cur;
                PUSH(ff->iterable, cur, 1, false);
                PUSH(ff->body, cur, 2, false); break; }
            case IRON_NODE_MATCH: { Iron_MatchStmt *m = (Iron_MatchStmt *)cur;
                PUSH(m->subject, cur, 0, false);
                for (int j = 0; j < m->case_count; j++)
                    PUSH(m->cases[j], cur, 1, false);
                PUSH(m->else_body, cur, 1, false); break; }
            case IRON_NODE_MATCH_CASE: { Iron_MatchCase *mc = (Iron_MatchCase *)cur;
                PUSH(mc->body, cur, 1, false); break; }
            case IRON_NODE_RETURN: { Iron_ReturnStmt *r = (Iron_ReturnStmt *)cur;
                PUSH(r->value, cur, 0, false); break; }
            case IRON_NODE_DEFER: { Iron_DeferStmt *ds = (Iron_DeferStmt *)cur;
                PUSH(ds->expr, cur, 0, false); break; }
            case IRON_NODE_FREE:  { Iron_FreeStmt  *ds = (Iron_FreeStmt  *)cur;
                PUSH(ds->expr, cur, 0, false); break; }
            case IRON_NODE_LEAK:  { Iron_LeakStmt  *ds = (Iron_LeakStmt  *)cur;
                PUSH(ds->expr, cur, 0, false); break; }
            case IRON_NODE_SPAWN: { Iron_SpawnStmt *sn = (Iron_SpawnStmt *)cur;
                PUSH(sn->pool_expr, cur, 0, false);
                PUSH(sn->body, cur, 1, false); break; }
            case IRON_NODE_INTERP_STRING: { Iron_InterpString *is = (Iron_InterpString *)cur;
                for (int j = 0; j < is->part_count; j++)
                    PUSH(is->parts[j], cur, j, false);
                break; }
            case IRON_NODE_ARRAY_LIT: { Iron_ArrayLit *al = (Iron_ArrayLit *)cur;
                for (int j = 0; j < al->element_count; j++)
                    PUSH(al->elements[j], cur, j, false); break; }
            case IRON_NODE_CONSTRUCT: { Iron_ConstructExpr *ce = (Iron_ConstructExpr *)cur;
                for (int j = 0; j < ce->arg_count; j++)
                    PUSH(ce->args[j], cur, j, false); break; }
            case IRON_NODE_LAMBDA: { Iron_LambdaExpr *la = (Iron_LambdaExpr *)cur;
                for (int j = 0; j < la->param_count; j++)
                    PUSH(la->params[j], cur, 0, false);
                PUSH(la->body, cur, 0, false); break; }
            case IRON_NODE_HEAP: { Iron_HeapExpr *he = (Iron_HeapExpr *)cur;
                PUSH(he->inner, cur, 0, false); break; }
            case IRON_NODE_RC: { Iron_RcExpr *re = (Iron_RcExpr *)cur;
                PUSH(re->inner, cur, 0, false); break; }
            case IRON_NODE_COMPTIME: { Iron_ComptimeExpr *cte = (Iron_ComptimeExpr *)cur;
                PUSH(cte->inner, cur, 0, false); break; }
            case IRON_NODE_IS: { Iron_IsExpr *ie = (Iron_IsExpr *)cur;
                PUSH(ie->expr, cur, 0, false); break; }
            case IRON_NODE_AWAIT: { Iron_AwaitExpr *ae = (Iron_AwaitExpr *)cur;
                PUSH(ae->handle, cur, 0, false); break; }
            /* Explicitly NEVER walk into string content. */
            case IRON_NODE_STRING_LIT: break;
            /* Terminal leaves: no children to push. */
            case IRON_NODE_IDENT:
            case IRON_NODE_INT_LIT:
            case IRON_NODE_FLOAT_LIT:
            case IRON_NODE_BOOL_LIT:
            case IRON_NODE_NULL_LIT:
            case IRON_NODE_PARAM:
            case IRON_NODE_FIELD:
            case IRON_NODE_ENUM_VARIANT:
            case IRON_NODE_TYPE_ANNOTATION:
            case IRON_NODE_PATTERN:
            case IRON_NODE_ENUM_CONSTRUCT:
            case IRON_NODE_IMPORT_DECL:
            case IRON_NODE_ERROR:
            case IRON_NODE_PROGRAM:
            case IRON_NODE_COUNT:
                break;
        }
    }

done:
    free(stack);

    *out   = (arr_n > 0) ? arr : NULL;
    *out_n = arr_n;

cleanup:
    iron_diaglist_free(&walk_diags);
    iron_arena_free(&walk_arena);
}

#undef PUSH
