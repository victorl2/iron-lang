/* Phase 4 Plan 04-07 Task 03 (EDIT-15, D-14) -- selectionRange facade.
 *
 * Parser-only walker. For each requested Position we build a linked
 * list of rungs from the innermost node covering the cursor up to the
 * module (program) level. Each rung's .range contains its .parent's
 * .range (strict-or-equal containment invariant, validated in tests).
 *
 * Algorithm:
 *   1. Parse-only pipeline via iron_lex_all + iron_parse (zero
 *      iron_analyze_buffer calls).
 *   2. For each position -> 1-based line/col.
 *   3. Walk program->decls[] to find the covering decl. Descend into
 *      the matching sub-tree (block / stmt / expression tree) as long
 *      as a child's span still covers the position. Each descent step
 *      materializes a rung.
 *   4. The outermost rung is the program span; the innermost rung is
 *      the final node we stopped at (or a ValDecl / Ident / literal).
 *   5. EOF cursor (line > last line) -> one-rung chain at program.
 *   6. Iron_ErrorNode is included as a rung only when its span is
 *      non-empty (D-14 rule).
 *
 * T-4-2 DoS mitigation: caller-supplied positions[] is clamped to 256
 * before iteration (matches workspace/symbol precedent from Phase 3). */

#include "lsp/facade/edit/selection_range.h"

#include "lsp/store/document.h"
#include "lsp/server/server.h"
#include "lexer/lexer.h"
#include "parser/parser.h"
#include "parser/ast.h"
#include "analyzer/analyzer.h"
#include "diagnostics/diagnostics.h"
#include "util/arena.h"
#include "vendor/stb_ds.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define ILSP_SELECTION_RANGE_POSITIONS_CAP 256

/* 1-based (line, col) test: does the span cover the position? */
static bool span_covers_1b(const Iron_Span *sp, uint32_t line, uint32_t col) {
    if (!sp) return false;
    if (sp->line == 0 && sp->end_line == 0) return false;  /* unknown span */
    if (line < sp->line || line > sp->end_line) return false;
    if (line == sp->line     && col < sp->col)     return false;
    if (line == sp->end_line && col > sp->end_col) return false;
    return true;
}

/* Allocate a new rung in the caller's arena. */
static IronLsp_SelectionRange *alloc_rung(Iron_Arena *arena, Iron_Span span) {
    IronLsp_SelectionRange *r = (IronLsp_SelectionRange *)iron_arena_alloc(
        arena, sizeof(*r), _Alignof(IronLsp_SelectionRange));
    if (!r) return NULL;
    /* Convert 1-based Iron_Span to 0-based LSP Range. */
    uint32_t sl = span.line > 0 ? span.line - 1 : 0;
    uint32_t sc = span.col  > 0 ? span.col  - 1 : 0;
    uint32_t el = span.end_line > 0 ? span.end_line - 1 : sl;
    uint32_t ec = span.end_col  > 0 ? span.end_col  - 1 : sc;
    /* LSP end.character is exclusive in many conventions; Iron_Span's
     * end_col is inclusive (covers the last byte). Clients still
     * interpret the range as spanning [start, end], so pass end_col
     * directly rather than end_col-1. This matches the translation
     * used by ilsp_span_to_lsp_range in facade/span.c. */
    (void)ec;
    r->range_start_line = sl;
    r->range_start_char = sc;
    r->range_end_line   = el;
    r->range_end_char   = span.end_col;  /* LSP wants exclusive-like byte col */
    r->parent           = NULL;
    return r;
}

/* Descend into `node` as long as a child's span covers (line, col).
 * Append a new rung for each descent step. Returns the innermost
 * rung. The chain is linked via `parent`. */
static IronLsp_SelectionRange *descend_chain(Iron_Node              *node,
                                                uint32_t                line,
                                                uint32_t                col,
                                                IronLsp_SelectionRange *parent,
                                                Iron_Arena             *arena) {
    if (!node) return parent;
    /* Only include this node as a rung if its span covers the cursor.
     * ErrorNode: include only when span is non-empty (D-14 rule). */
    if (node->kind == IRON_NODE_ERROR) {
        Iron_Span es = node->span;
        if (es.line == 0 && es.end_line == 0) return parent;
        if (!span_covers_1b(&es, line, col)) return parent;
    }

    IronLsp_SelectionRange *cur = alloc_rung(arena, node->span);
    if (!cur) return parent;
    cur->parent = parent;

    /* Try each child class. Stop at first child whose span covers. */
    Iron_Node *child = NULL;
    switch ((int)node->kind) {
        case IRON_NODE_FUNC_DECL: {
            Iron_FuncDecl *fd = (Iron_FuncDecl *)node;
            for (int i = 0; i < fd->param_count; i++) {
                if (fd->params[i] && span_covers_1b(&fd->params[i]->span, line, col)) {
                    child = fd->params[i]; break;
                }
            }
            if (!child && fd->body && span_covers_1b(&fd->body->span, line, col))
                child = fd->body;
            break;
        }
        case IRON_NODE_METHOD_DECL: {
            Iron_MethodDecl *md = (Iron_MethodDecl *)node;
            for (int i = 0; i < md->param_count; i++) {
                if (md->params[i] && span_covers_1b(&md->params[i]->span, line, col)) {
                    child = md->params[i]; break;
                }
            }
            if (!child && md->body && span_covers_1b(&md->body->span, line, col))
                child = md->body;
            break;
        }
        case IRON_NODE_OBJECT_DECL: {
            Iron_ObjectDecl *od = (Iron_ObjectDecl *)node;
            for (int i = 0; i < od->field_count; i++) {
                if (od->fields[i] && span_covers_1b(&od->fields[i]->span, line, col)) {
                    child = od->fields[i]; break;
                }
            }
            break;
        }
        case IRON_NODE_INTERFACE_DECL: {
            Iron_InterfaceDecl *id_n = (Iron_InterfaceDecl *)node;
            for (int i = 0; i < id_n->method_count; i++) {
                if (id_n->method_sigs[i] && span_covers_1b(&id_n->method_sigs[i]->span, line, col)) {
                    child = id_n->method_sigs[i]; break;
                }
            }
            break;
        }
        case IRON_NODE_ENUM_DECL: {
            Iron_EnumDecl *ed = (Iron_EnumDecl *)node;
            for (int i = 0; i < ed->variant_count; i++) {
                if (ed->variants[i] && span_covers_1b(&ed->variants[i]->span, line, col)) {
                    child = ed->variants[i]; break;
                }
            }
            break;
        }
        case IRON_NODE_BLOCK: {
            Iron_Block *b = (Iron_Block *)node;
            for (int i = 0; i < b->stmt_count; i++) {
                if (b->stmts[i] && span_covers_1b(&b->stmts[i]->span, line, col)) {
                    child = b->stmts[i]; break;
                }
            }
            break;
        }
        case IRON_NODE_VAL_DECL: {
            Iron_ValDecl *v = (Iron_ValDecl *)node;
            if (v->init && span_covers_1b(&v->init->span, line, col)) child = v->init;
            break;
        }
        case IRON_NODE_VAR_DECL: {
            Iron_VarDecl *v = (Iron_VarDecl *)node;
            if (v->init && span_covers_1b(&v->init->span, line, col)) child = v->init;
            break;
        }
        case IRON_NODE_ASSIGN: {
            Iron_AssignStmt *a = (Iron_AssignStmt *)node;
            if (a->target && span_covers_1b(&a->target->span, line, col)) child = a->target;
            else if (a->value && span_covers_1b(&a->value->span, line, col)) child = a->value;
            break;
        }
        case IRON_NODE_RETURN: {
            Iron_ReturnStmt *r = (Iron_ReturnStmt *)node;
            if (r->value && span_covers_1b(&r->value->span, line, col)) child = r->value;
            break;
        }
        case IRON_NODE_IF: {
            Iron_IfStmt *s = (Iron_IfStmt *)node;
            if (s->condition && span_covers_1b(&s->condition->span, line, col)) child = s->condition;
            else if (s->body && span_covers_1b(&s->body->span, line, col)) child = s->body;
            else {
                for (int i = 0; i < s->elif_count && !child; i++) {
                    if (s->elif_conds[i] && span_covers_1b(&s->elif_conds[i]->span, line, col))
                        child = s->elif_conds[i];
                    else if (s->elif_bodies[i] && span_covers_1b(&s->elif_bodies[i]->span, line, col))
                        child = s->elif_bodies[i];
                }
                if (!child && s->else_body && span_covers_1b(&s->else_body->span, line, col))
                    child = s->else_body;
            }
            break;
        }
        case IRON_NODE_WHILE: {
            Iron_WhileStmt *w = (Iron_WhileStmt *)node;
            if (w->condition && span_covers_1b(&w->condition->span, line, col)) child = w->condition;
            else if (w->body && span_covers_1b(&w->body->span, line, col)) child = w->body;
            break;
        }
        case IRON_NODE_FOR: {
            Iron_ForStmt *f = (Iron_ForStmt *)node;
            if (f->iterable && span_covers_1b(&f->iterable->span, line, col)) child = f->iterable;
            else if (f->body && span_covers_1b(&f->body->span, line, col)) child = f->body;
            break;
        }
        case IRON_NODE_MATCH: {
            Iron_MatchStmt *m = (Iron_MatchStmt *)node;
            if (m->subject && span_covers_1b(&m->subject->span, line, col)) child = m->subject;
            else {
                for (int i = 0; i < m->case_count && !child; i++) {
                    if (m->cases[i] && span_covers_1b(&m->cases[i]->span, line, col))
                        child = m->cases[i];
                }
                if (!child && m->else_body && span_covers_1b(&m->else_body->span, line, col))
                    child = m->else_body;
            }
            break;
        }
        case IRON_NODE_MATCH_CASE: {
            Iron_MatchCase *mc = (Iron_MatchCase *)node;
            if (mc->body && span_covers_1b(&mc->body->span, line, col)) child = mc->body;
            break;
        }
        case IRON_NODE_DEFER: {
            Iron_DeferStmt *d = (Iron_DeferStmt *)node;
            if (d->expr && span_covers_1b(&d->expr->span, line, col)) child = d->expr;
            break;
        }
        case IRON_NODE_FREE: {
            Iron_FreeStmt *d = (Iron_FreeStmt *)node;
            if (d->expr && span_covers_1b(&d->expr->span, line, col)) child = d->expr;
            break;
        }
        case IRON_NODE_LEAK: {
            Iron_LeakStmt *d = (Iron_LeakStmt *)node;
            if (d->expr && span_covers_1b(&d->expr->span, line, col)) child = d->expr;
            break;
        }
        case IRON_NODE_SPAWN: {
            Iron_SpawnStmt *sn = (Iron_SpawnStmt *)node;
            if (sn->pool_expr && span_covers_1b(&sn->pool_expr->span, line, col)) child = sn->pool_expr;
            else if (sn->body && span_covers_1b(&sn->body->span, line, col)) child = sn->body;
            break;
        }
        case IRON_NODE_BINARY: {
            Iron_BinaryExpr *e = (Iron_BinaryExpr *)node;
            if (e->left && span_covers_1b(&e->left->span, line, col)) child = e->left;
            else if (e->right && span_covers_1b(&e->right->span, line, col)) child = e->right;
            break;
        }
        case IRON_NODE_UNARY: {
            Iron_UnaryExpr *e = (Iron_UnaryExpr *)node;
            if (e->operand && span_covers_1b(&e->operand->span, line, col)) child = e->operand;
            break;
        }
        case IRON_NODE_CALL: {
            Iron_CallExpr *c = (Iron_CallExpr *)node;
            if (c->callee && span_covers_1b(&c->callee->span, line, col)) child = c->callee;
            else {
                for (int i = 0; i < c->arg_count && !child; i++) {
                    if (c->args[i] && span_covers_1b(&c->args[i]->span, line, col))
                        child = c->args[i];
                }
            }
            break;
        }
        case IRON_NODE_METHOD_CALL: {
            Iron_MethodCallExpr *m = (Iron_MethodCallExpr *)node;
            if (m->object && span_covers_1b(&m->object->span, line, col)) child = m->object;
            else {
                for (int i = 0; i < m->arg_count && !child; i++) {
                    if (m->args[i] && span_covers_1b(&m->args[i]->span, line, col))
                        child = m->args[i];
                }
            }
            break;
        }
        case IRON_NODE_FIELD_ACCESS: {
            Iron_FieldAccess *fa = (Iron_FieldAccess *)node;
            if (fa->object && span_covers_1b(&fa->object->span, line, col)) child = fa->object;
            break;
        }
        case IRON_NODE_INDEX: {
            Iron_IndexExpr *ix = (Iron_IndexExpr *)node;
            if (ix->object && span_covers_1b(&ix->object->span, line, col)) child = ix->object;
            else if (ix->index && span_covers_1b(&ix->index->span, line, col)) child = ix->index;
            break;
        }
        case IRON_NODE_SLICE: {
            Iron_SliceExpr *sl = (Iron_SliceExpr *)node;
            if (sl->object && span_covers_1b(&sl->object->span, line, col)) child = sl->object;
            else if (sl->start && span_covers_1b(&sl->start->span, line, col)) child = sl->start;
            else if (sl->end && span_covers_1b(&sl->end->span, line, col)) child = sl->end;
            break;
        }
        case IRON_NODE_LAMBDA: {
            Iron_LambdaExpr *la = (Iron_LambdaExpr *)node;
            for (int i = 0; i < la->param_count && !child; i++) {
                if (la->params[i] && span_covers_1b(&la->params[i]->span, line, col))
                    child = la->params[i];
            }
            if (!child && la->body && span_covers_1b(&la->body->span, line, col))
                child = la->body;
            break;
        }
        case IRON_NODE_HEAP: {
            Iron_HeapExpr *he = (Iron_HeapExpr *)node;
            if (he->inner && span_covers_1b(&he->inner->span, line, col)) child = he->inner;
            break;
        }
        case IRON_NODE_RC: {
            Iron_RcExpr *re = (Iron_RcExpr *)node;
            if (re->inner && span_covers_1b(&re->inner->span, line, col)) child = re->inner;
            break;
        }
        case IRON_NODE_COMPTIME: {
            Iron_ComptimeExpr *cte = (Iron_ComptimeExpr *)node;
            if (cte->inner && span_covers_1b(&cte->inner->span, line, col)) child = cte->inner;
            break;
        }
        case IRON_NODE_IS: {
            Iron_IsExpr *ie = (Iron_IsExpr *)node;
            if (ie->expr && span_covers_1b(&ie->expr->span, line, col)) child = ie->expr;
            break;
        }
        case IRON_NODE_AWAIT: {
            Iron_AwaitExpr *ae = (Iron_AwaitExpr *)node;
            if (ae->handle && span_covers_1b(&ae->handle->span, line, col)) child = ae->handle;
            break;
        }
        case IRON_NODE_CONSTRUCT: {
            Iron_ConstructExpr *ce = (Iron_ConstructExpr *)node;
            for (int i = 0; i < ce->arg_count && !child; i++) {
                if (ce->args[i] && span_covers_1b(&ce->args[i]->span, line, col))
                    child = ce->args[i];
            }
            break;
        }
        case IRON_NODE_ARRAY_LIT: {
            Iron_ArrayLit *al = (Iron_ArrayLit *)node;
            for (int i = 0; i < al->element_count && !child; i++) {
                if (al->elements[i] && span_covers_1b(&al->elements[i]->span, line, col))
                    child = al->elements[i];
            }
            break;
        }
        case IRON_NODE_INTERP_STRING: {
            Iron_InterpString *is = (Iron_InterpString *)node;
            for (int i = 0; i < is->part_count && !child; i++) {
                if (is->parts[i] && span_covers_1b(&is->parts[i]->span, line, col))
                    child = is->parts[i];
            }
            break;
        }
        /* Leaf-like expressions / decls without deeper rungs. */
        case IRON_NODE_IDENT:
        case IRON_NODE_INT_LIT:
        case IRON_NODE_FLOAT_LIT:
        case IRON_NODE_STRING_LIT:
        case IRON_NODE_BOOL_LIT:
        case IRON_NODE_NULL_LIT:
        case IRON_NODE_IMPORT_DECL:
        case IRON_NODE_PARAM:
        case IRON_NODE_FIELD:
        case IRON_NODE_ENUM_VARIANT:
        case IRON_NODE_TYPE_ANNOTATION:
        case IRON_NODE_PATTERN:
        case IRON_NODE_ENUM_CONSTRUCT:
        case IRON_NODE_ERROR:
        case IRON_NODE_PROGRAM:
        case IRON_NODE_COUNT:
            break;
    }

    if (child) return descend_chain(child, line, col, cur, arena);
    return cur;
}

void ilsp_facade_selection_range(struct IronLsp_Server      *server,
                                   struct IronLsp_Document    *doc,
                                   const IronLsp_Position     *positions,
                                   size_t                      n_positions,
                                   _Atomic bool               *cancel,
                                   Iron_Arena                 *arena,
                                   IronLsp_SelectionRange   ***out,
                                   size_t                     *out_n) {
    if (out)   *out   = NULL;
    if (out_n) *out_n = 0;
    if (!doc || !arena || !out || !out_n) return;
    (void)server;

    if (cancel && atomic_load(cancel)) return;
    if (n_positions == 0 || !positions) return;

    /* T-4-2 DoS mitigation: clamp position count. */
    size_t effective_n = n_positions;
    if (effective_n > ILSP_SELECTION_RANGE_POSITIONS_CAP) {
        effective_n = ILSP_SELECTION_RANGE_POSITIONS_CAP;
    }

    /* Parser-only pipeline. */
    Iron_Arena    parse_arena = iron_arena_create(128 * 1024);
    Iron_DiagList diags       = iron_diaglist_create();
    Iron_Token   *tokens      = NULL;

    Iron_Program *program = NULL;
    if (doc->text && doc->text_len > 0) {
        Iron_Lexer lex = iron_lexer_create(doc->text,
                                             doc->uri ? doc->uri : "<doc>",
                                             &parse_arena, &diags);
        iron_lexer_set_cancel_flag(&lex, cancel);
        tokens = iron_lex_all(&lex);
        if (tokens) {
            int tok_count = (int)arrlen(tokens);
            Iron_Parser p = iron_parser_create(tokens, tok_count, doc->text,
                                                 doc->uri ? doc->uri : "<doc>",
                                                 &parse_arena, &diags);
            iron_parser_set_mode(&p, IRON_ANALYSIS_MODE_LSP);
            iron_parser_set_cancel_flag(&p, cancel);
            Iron_Node *root = iron_parse(&p);
            if (root && root->kind == IRON_NODE_PROGRAM) {
                program = (Iron_Program *)root;
            }
        }
    }

    /* Allocate out[] array. */
    IronLsp_SelectionRange **arr = (IronLsp_SelectionRange **)iron_arena_alloc(
        arena, effective_n * sizeof(*arr), _Alignof(IronLsp_SelectionRange *));
    if (!arr) goto cleanup;
    memset(arr, 0, effective_n * sizeof(*arr));

    for (size_t i = 0; i < effective_n; i++) {
        if (cancel && atomic_load(cancel)) break;

        uint32_t line = positions[i].line + 1;       /* 1-based */
        uint32_t col  = positions[i].character + 1;

        /* Build the module rung regardless. */
        Iron_Span prog_span;
        if (program) {
            prog_span = program->span;
        } else {
            /* Graceful fallback when parse failed: whole doc. */
            memset(&prog_span, 0, sizeof(prog_span));
            prog_span.line = 1; prog_span.col = 1;
            prog_span.end_line = 1; prog_span.end_col = 1;
        }
        IronLsp_SelectionRange *module_rung = alloc_rung(arena, prog_span);
        if (!module_rung) continue;
        module_rung->parent = NULL;

        /* If parse failed, program is NULL -> module-only chain. */
        if (!program) { arr[i] = module_rung; continue; }

        /* Locate the covering top-level decl. If none covers (EOF or
         * whitespace) -> module-only chain. */
        Iron_Node *covering = NULL;
        for (int j = 0; j < program->decl_count; j++) {
            Iron_Node *d = program->decls[j];
            if (!d) continue;
            if (d->kind == IRON_NODE_ERROR) {
                /* D-14: ErrorNode rung only if span is non-empty. */
                Iron_Span es = d->span;
                if (es.line == 0 && es.end_line == 0) continue;
            }
            if (span_covers_1b(&d->span, line, col)) {
                covering = d; break;
            }
        }
        if (!covering) { arr[i] = module_rung; continue; }

        IronLsp_SelectionRange *innermost = descend_chain(
            covering, line, col, module_rung, arena);
        arr[i] = innermost ? innermost : module_rung;
    }

    *out   = arr;
    *out_n = effective_n;

cleanup:
    if (tokens) arrfree(tokens);
    iron_diaglist_free(&diags);
    iron_arena_free(&parse_arena);
}
