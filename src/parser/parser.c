#include "parser/parser.h"
#include "parser/ast.h"
#include "lexer/lexer.h"
#include "diagnostics/diagnostics.h"
#include "util/arena.h"
#include "stb_ds.h"

#include <string.h>
#include <stddef.h>
#include <stdlib.h>

/* ── Precedence levels (Pratt) ────────────────────────────────────────────── */

typedef enum {
    PREC_NONE       = 0,
    PREC_ASSIGN     = 1,
    PREC_IS         = 2,
    PREC_OR         = 3,
    PREC_AND        = 4,
    PREC_EQUALITY   = 5,
    PREC_COMPARISON = 6,
    PREC_TERM       = 7,
    PREC_FACTOR     = 8,
    PREC_UNARY      = 9,
    PREC_CALL       = 10
} Precedence;

/* ── Forward declarations ────────────────────────────────────────────────── */

static Iron_Node *iron_parse_expr_prec(Iron_Parser *p, int min_prec);
static Iron_Node *iron_parse_expr(Iron_Parser *p);
static Iron_Node *iron_parse_stmt(Iron_Parser *p);
static Iron_Node *iron_parse_block(Iron_Parser *p);
static Iron_Node *iron_parse_type_annotation(Iron_Parser *p);
static Iron_Node *iron_parse_decl(Iron_Parser *p, bool is_private);
static Iron_Node *iron_parse_func_or_method(Iron_Parser *p, bool is_private);
static Iron_Node *iron_parse_object_decl(Iron_Parser *p, bool is_private);
static Iron_Node *iron_parse_interface_decl(Iron_Parser *p, bool is_private);
static Iron_Node *iron_parse_enum_decl(Iron_Parser *p, bool is_private);
static Iron_Node *iron_parse_import_decl(Iron_Parser *p);
static Iron_Node *iron_parse_val_decl(Iron_Parser *p);
static Iron_Node *iron_parse_var_decl(Iron_Parser *p);
static Iron_Node **iron_parse_generic_params(Iron_Parser *p, int *out_count, Iron_Arena *arena);
static Iron_Node **iron_parse_param_list(Iron_Parser *p, int *out_count);
static Iron_Node **iron_parse_call_args(Iron_Parser *p, int *out_count);
static Iron_Node *iron_parse_lambda(Iron_Parser *p);
static Iron_Node *iron_parse_if_stmt(Iron_Parser *p);
static Iron_Node *iron_parse_while_stmt(Iron_Parser *p);
static Iron_Node *iron_parse_for_stmt(Iron_Parser *p);
static Iron_Node *iron_parse_match_stmt(Iron_Parser *p);
static Iron_Node *iron_parse_spawn_stmt(Iron_Parser *p);
static Iron_Node *iron_parse_interp_string(Iron_Parser *p, const char *raw_value, Iron_Span span);

/* ── Parser creation ─────────────────────────────────────────────────────── */

Iron_Parser iron_parser_create(Iron_Token *tokens, int token_count,
                                const char *source, const char *filename,
                                Iron_Arena *arena, Iron_DiagList *diags) {
    Iron_Parser p;
    p.tokens            = tokens;
    p.token_count       = token_count;
    p.pos               = 0;
    p.arena             = arena;
    p.diags             = diags;
    p.filename          = filename;
    p.source            = source;
    p.in_error_recovery = false;
    return p;
}

/* ── Low-level helpers ───────────────────────────────────────────────────── */

static Iron_Token *iron_current(Iron_Parser *p) {
    if (p->pos >= p->token_count) return &p->tokens[p->token_count - 1];
    return &p->tokens[p->pos];
}

static Iron_TokenKind iron_peek(Iron_Parser *p) {
    return iron_current(p)->kind;
}

/* Advance past newlines without consuming the current token */
static void iron_skip_newlines(Iron_Parser *p) {
    while (p->pos < p->token_count && p->tokens[p->pos].kind == IRON_TOK_NEWLINE)
        p->pos++;
}

/* Return current token and advance; automatically skips newlines after */
static Iron_Token *iron_advance(Iron_Parser *p) {
    Iron_Token *t = iron_current(p);
    if (p->pos < p->token_count - 1) p->pos++;
    /* skip newlines that immediately follow so callers don't have to */
    iron_skip_newlines(p);
    return t;
}

static bool iron_check(Iron_Parser *p, Iron_TokenKind kind) {
    return iron_peek(p) == kind;
}

/* True if the current token can appear as a function/method name.
 * Allows keywords like 'draw' that are common method names. */
static bool iron_check_name(Iron_Parser *p) {
    Iron_TokenKind k = iron_peek(p);
    return k == IRON_TOK_IDENTIFIER || k == IRON_TOK_DRAW;
}

static bool iron_match(Iron_Parser *p, Iron_TokenKind kind) {
    if (iron_check(p, kind)) { iron_advance(p); return true; }
    return false;
}

/* Build an Iron_Span from a single token */
static Iron_Span iron_token_span(Iron_Parser *p, Iron_Token *t) {
    return iron_span_make(p->filename,
                          t->line, t->col,
                          t->line, t->col + (t->len > 0 ? t->len - 1 : 0));
}

/* Emit a diagnostic only if not currently suppressing cascading errors */
static void iron_emit_diag(Iron_Parser *p, int code, Iron_Span sp, const char *msg) {
    if (!p->in_error_recovery) {
        iron_diag_emit(p->diags, p->arena, IRON_DIAG_ERROR, code, sp, msg, NULL);
    }
}

/* Emit a diagnostic and return NULL; used by iron_expect on failure */
static Iron_Token *iron_expect(Iron_Parser *p, Iron_TokenKind kind) {
    if (iron_check(p, kind)) return iron_advance(p);
    Iron_Token *cur = iron_current(p);
    Iron_Span   sp  = iron_token_span(p, cur);
    /* Choose an appropriate error code */
    int code = IRON_ERR_UNEXPECTED_TOKEN;
    if (kind == IRON_TOK_RBRACE) code = IRON_ERR_EXPECTED_RBRACE;
    if (kind == IRON_TOK_RPAREN) code = IRON_ERR_EXPECTED_RPAREN;
    if (kind == IRON_TOK_COLON)  code = IRON_ERR_EXPECTED_COLON;
    if (kind == IRON_TOK_ARROW)  code = IRON_ERR_EXPECTED_ARROW;
    iron_emit_diag(p, code, sp, "unexpected token");
    return NULL;
}

/* Create an ErrorNode at the current position */
static Iron_Node *iron_make_error(Iron_Parser *p) {
    Iron_ErrorNode *n = ARENA_ALLOC(p->arena, Iron_ErrorNode);
    n->span = iron_token_span(p, iron_current(p));
    n->kind = IRON_NODE_ERROR;
    return (Iron_Node *)n;
}

/* Synchronize to a top-level declaration boundary after an error */
static void iron_parser_sync_toplevel(Iron_Parser *p) {
    while (iron_peek(p) != IRON_TOK_EOF) {
        Iron_TokenKind k = iron_peek(p);
        if (k == IRON_TOK_FUNC      ||
            k == IRON_TOK_OBJECT    ||
            k == IRON_TOK_INTERFACE ||
            k == IRON_TOK_ENUM      ||
            k == IRON_TOK_IMPORT    ||
            k == IRON_TOK_VAL       ||
            k == IRON_TOK_VAR       ||
            k == IRON_TOK_PRIVATE) return;
        p->pos++;
        iron_skip_newlines(p);
    }
}

/* Synchronize to a statement boundary after an error */
static void iron_parser_sync_stmt(Iron_Parser *p) {
    while (iron_peek(p) != IRON_TOK_EOF) {
        Iron_TokenKind k = iron_peek(p);
        if (k == IRON_TOK_RBRACE ||
            k == IRON_TOK_VAL    ||
            k == IRON_TOK_VAR    ||
            k == IRON_TOK_IF     ||
            k == IRON_TOK_WHILE  ||
            k == IRON_TOK_FOR    ||
            k == IRON_TOK_MATCH  ||
            k == IRON_TOK_RETURN ||
            k == IRON_TOK_DEFER  ||
            k == IRON_TOK_FREE   ||
            k == IRON_TOK_LEAK   ||
            k == IRON_TOK_SPAWN) return;
        p->pos++;
        iron_skip_newlines(p);
    }
}

/* ── Type annotation ─────────────────────────────────────────────────────── */

/* Parse: TypeName[?][GenericArgs] or [TypeName; Size] or [TypeName] */
static Iron_Node *iron_parse_type_annotation(Iron_Parser *p) {
    Iron_Token *start = iron_current(p);

    /* Array type: [T] or [T; N] */
    if (iron_match(p, IRON_TOK_LBRACKET)) {
        Iron_TypeAnnotation *ann = ARENA_ALLOC(p->arena, Iron_TypeAnnotation);
        ann->kind              = IRON_NODE_TYPE_ANNOTATION;
        ann->is_array          = true;
        ann->is_nullable       = false;
        ann->generic_args      = NULL;
        ann->generic_arg_count = 0;
        ann->array_size        = NULL;

        /* element type name */
        if (!iron_check(p, IRON_TOK_IDENTIFIER)) {
            iron_diag_emit(p->diags, p->arena, IRON_DIAG_ERROR,
                           IRON_ERR_UNEXPECTED_TOKEN,
                           iron_token_span(p, iron_current(p)),
                           "expected type name in array type", NULL);
            ann->name = "<error>";
        } else {
            Iron_Token *name_tok = iron_advance(p);
            ann->name = iron_arena_strdup(p->arena, name_tok->value,
                                          strlen(name_tok->value));
        }

        /* optional [T; Size] */
        if (iron_match(p, IRON_TOK_SEMICOLON)) {
            ann->array_size = iron_parse_expr(p);
        }

        iron_expect(p, IRON_TOK_RBRACKET);
        ann->span = iron_span_merge(iron_token_span(p, start),
                                    iron_token_span(p, iron_current(p)));
        return (Iron_Node *)ann;
    }

    /* Named type */
    if (!iron_check(p, IRON_TOK_IDENTIFIER)) {
        iron_diag_emit(p->diags, p->arena, IRON_DIAG_ERROR,
                       IRON_ERR_UNEXPECTED_TOKEN,
                       iron_token_span(p, iron_current(p)),
                       "expected type name", NULL);
        return iron_make_error(p);
    }

    Iron_Token *name_tok = iron_advance(p);
    Iron_TypeAnnotation *ann = ARENA_ALLOC(p->arena, Iron_TypeAnnotation);
    ann->kind              = IRON_NODE_TYPE_ANNOTATION;
    ann->is_array          = false;
    ann->array_size        = NULL;
    ann->name = iron_arena_strdup(p->arena, name_tok->value,
                                  strlen(name_tok->value));

    /* nullable? */
    ann->is_nullable = iron_match(p, IRON_TOK_QUESTION);

    /* generic args: [T, U] */
    if (iron_check(p, IRON_TOK_LBRACKET)) {
        ann->generic_args = iron_parse_generic_params(p, &ann->generic_arg_count,
                                                      p->arena);
    } else {
        ann->generic_args      = NULL;
        ann->generic_arg_count = 0;
    }

    ann->span = iron_span_merge(iron_token_span(p, start),
                                iron_token_span(p, iron_current(p)));
    return (Iron_Node *)ann;
}

/* ── Generic parameter list: [T, U, ...] ────────────────────────────────── */

static Iron_Node **iron_parse_generic_params(Iron_Parser *p, int *out_count,
                                              Iron_Arena *arena) {
    (void)arena;  /* we use p->arena directly */
    Iron_Node **arr = NULL;
    *out_count = 0;

    if (!iron_match(p, IRON_TOK_LBRACKET)) return NULL;
    iron_skip_newlines(p);

    while (!iron_check(p, IRON_TOK_RBRACKET) && !iron_check(p, IRON_TOK_EOF)) {
        iron_skip_newlines(p);
        if (iron_check(p, IRON_TOK_IDENTIFIER)) {
            Iron_Token *t  = iron_advance(p);
            Iron_Ident *id = ARENA_ALLOC(p->arena, Iron_Ident);
            id->span       = iron_token_span(p, t);
            id->kind       = IRON_NODE_IDENT;
            id->name       = iron_arena_strdup(p->arena, t->value, strlen(t->value));
            arrput(arr, (Iron_Node *)id);
            (*out_count)++;
        } else {
            break;
        }
        iron_skip_newlines(p);
        if (!iron_match(p, IRON_TOK_COMMA)) break;
        iron_skip_newlines(p);
    }
    iron_expect(p, IRON_TOK_RBRACKET);
    return arr;
}

/* ── Parameter list: (param, ...) ────────────────────────────────────────── */

static Iron_Node **iron_parse_param_list(Iron_Parser *p, int *out_count) {
    Iron_Node **arr = NULL;
    *out_count = 0;

    iron_expect(p, IRON_TOK_LPAREN);
    iron_skip_newlines(p);

    while (!iron_check(p, IRON_TOK_RPAREN) && !iron_check(p, IRON_TOK_EOF)) {
        iron_skip_newlines(p);

        bool is_var = false;
        if (iron_check(p, IRON_TOK_VAR)) {
            iron_advance(p);
            is_var = true;
        } else if (iron_check(p, IRON_TOK_VAL)) {
            iron_advance(p);
            /* val is default, just consume */
        }

        if (!iron_check(p, IRON_TOK_IDENTIFIER)) {
            iron_emit_diag(p, IRON_ERR_UNEXPECTED_TOKEN,
                           iron_token_span(p, iron_current(p)),
                           "expected parameter name");
            p->in_error_recovery = true;
            iron_parser_sync_stmt(p);
            break;
        }

        Iron_Token *name_tok = iron_advance(p);
        Iron_Param *param    = ARENA_ALLOC(p->arena, Iron_Param);
        param->kind          = IRON_NODE_PARAM;
        param->span          = iron_token_span(p, name_tok);
        param->is_var        = is_var;
        param->name = iron_arena_strdup(p->arena, name_tok->value,
                                        strlen(name_tok->value));

        /* optional type annotation: : Type */
        if (iron_match(p, IRON_TOK_COLON)) {
            param->type_ann = iron_parse_type_annotation(p);
        } else {
            param->type_ann = NULL;
        }

        arrput(arr, (Iron_Node *)param);
        (*out_count)++;

        iron_skip_newlines(p);
        if (!iron_match(p, IRON_TOK_COMMA)) break;
        iron_skip_newlines(p);
    }

    iron_expect(p, IRON_TOK_RPAREN);
    return arr;
}

/* ── Block: { stmt* } ────────────────────────────────────────────────────── */

static Iron_Node *iron_parse_block(Iron_Parser *p) {
    Iron_Token *start = iron_current(p);
    if (!iron_expect(p, IRON_TOK_LBRACE)) {
        p->in_error_recovery = true;
        return iron_make_error(p);
    }

    iron_skip_newlines(p);

    Iron_Node **stmts = NULL;
    int stmt_count = 0;

    while (!iron_check(p, IRON_TOK_RBRACE) && !iron_check(p, IRON_TOK_EOF)) {
        iron_skip_newlines(p);
        if (iron_check(p, IRON_TOK_RBRACE)) break;
        Iron_Node *s = iron_parse_stmt(p);
        arrput(stmts, s);
        stmt_count++;
        iron_skip_newlines(p);
    }

    Iron_Token *end = iron_current(p);
    if (!iron_expect(p, IRON_TOK_RBRACE)) {
        /* incomplete block, return what we have */
    }

    Iron_Block *blk  = ARENA_ALLOC(p->arena, Iron_Block);
    blk->kind        = IRON_NODE_BLOCK;
    blk->span        = iron_span_merge(iron_token_span(p, start),
                                       iron_token_span(p, end));
    blk->stmts       = stmts;
    blk->stmt_count  = stmt_count;
    return (Iron_Node *)blk;
}

/* ── Call argument list: (expr, ...) ─────────────────────────────────────── */

static Iron_Node **iron_parse_call_args(Iron_Parser *p, int *out_count) {
    Iron_Node **arr = NULL;
    *out_count = 0;

    iron_expect(p, IRON_TOK_LPAREN);
    iron_skip_newlines(p);

    while (!iron_check(p, IRON_TOK_RPAREN) && !iron_check(p, IRON_TOK_EOF)) {
        iron_skip_newlines(p);
        Iron_Node *arg = iron_parse_expr(p);
        arrput(arr, arg);
        (*out_count)++;
        iron_skip_newlines(p);
        if (!iron_match(p, IRON_TOK_COMMA)) break;
        iron_skip_newlines(p);
    }

    iron_expect(p, IRON_TOK_RPAREN);
    return arr;
}

/* ── Lambda: func(params) [-> Type] { body } ─────────────────────────────── */

static Iron_Node *iron_parse_lambda(Iron_Parser *p) {
    Iron_Token *start = iron_current(p);
    iron_advance(p);  /* consume 'func' */

    int        param_count = 0;
    Iron_Node **params     = iron_parse_param_list(p, &param_count);

    Iron_Node *ret = NULL;
    if (iron_match(p, IRON_TOK_ARROW)) {
        ret = iron_parse_type_annotation(p);
    }

    Iron_Node *body = iron_parse_block(p);

    Iron_LambdaExpr *lam = ARENA_ALLOC(p->arena, Iron_LambdaExpr);
    lam->kind            = IRON_NODE_LAMBDA;
    lam->span            = iron_span_merge(iron_token_span(p, start), body->span);
    lam->params          = params;
    lam->param_count     = param_count;
    lam->return_type     = ret;
    lam->body            = body;
    return (Iron_Node *)lam;
}

/* ── Expression: Pratt parser ────────────────────────────────────────────── */

/* Return the infix precedence of the current token, or PREC_NONE */
static int iron_infix_prec(Iron_TokenKind k) {
    switch (k) {
        case IRON_TOK_OR:         return PREC_OR;
        case IRON_TOK_AND:        return PREC_AND;
        case IRON_TOK_EQUALS:
        case IRON_TOK_NOT_EQUALS: return PREC_EQUALITY;
        case IRON_TOK_LESS:
        case IRON_TOK_GREATER:
        case IRON_TOK_LESS_EQ:
        case IRON_TOK_GREATER_EQ: return PREC_COMPARISON;
        case IRON_TOK_PLUS:
        case IRON_TOK_MINUS:      return PREC_TERM;
        case IRON_TOK_STAR:
        case IRON_TOK_SLASH:
        case IRON_TOK_PERCENT:    return PREC_FACTOR;
        case IRON_TOK_DOT:        return PREC_CALL;
        case IRON_TOK_LBRACKET:   return PREC_CALL;
        case IRON_TOK_LPAREN:     return PREC_CALL;
        case IRON_TOK_IS:         return PREC_IS;
        default:                  return PREC_NONE;
    }
}

/* Parse a primary (prefix) expression */
static Iron_Node *iron_parse_primary(Iron_Parser *p) {
    iron_skip_newlines(p);
    Iron_Token *t = iron_current(p);

    switch (t->kind) {
        /* Integer literal */
        case IRON_TOK_INTEGER: {
            iron_advance(p);
            Iron_IntLit *n = ARENA_ALLOC(p->arena, Iron_IntLit);
            n->kind  = IRON_NODE_INT_LIT;
            n->span  = iron_token_span(p, t);
            n->value = iron_arena_strdup(p->arena, t->value, strlen(t->value));
            return (Iron_Node *)n;
        }
        /* Float literal */
        case IRON_TOK_FLOAT: {
            iron_advance(p);
            Iron_FloatLit *n = ARENA_ALLOC(p->arena, Iron_FloatLit);
            n->kind  = IRON_NODE_FLOAT_LIT;
            n->span  = iron_token_span(p, t);
            n->value = iron_arena_strdup(p->arena, t->value, strlen(t->value));
            return (Iron_Node *)n;
        }
        /* String literal */
        case IRON_TOK_STRING: {
            iron_advance(p);
            Iron_StringLit *n = ARENA_ALLOC(p->arena, Iron_StringLit);
            n->kind  = IRON_NODE_STRING_LIT;
            n->span  = iron_token_span(p, t);
            n->value = iron_arena_strdup(p->arena, t->value, strlen(t->value));
            return (Iron_Node *)n;
        }
        /* Interpolated string — parse into alternating literal/expr segments */
        case IRON_TOK_INTERP_STRING: {
            iron_advance(p);
            return iron_parse_interp_string(p, t->value, iron_token_span(p, t));
        }
        /* true */
        case IRON_TOK_TRUE: {
            iron_advance(p);
            Iron_BoolLit *n = ARENA_ALLOC(p->arena, Iron_BoolLit);
            n->kind  = IRON_NODE_BOOL_LIT;
            n->span  = iron_token_span(p, t);
            n->value = true;
            return (Iron_Node *)n;
        }
        /* false */
        case IRON_TOK_FALSE: {
            iron_advance(p);
            Iron_BoolLit *n = ARENA_ALLOC(p->arena, Iron_BoolLit);
            n->kind  = IRON_NODE_BOOL_LIT;
            n->span  = iron_token_span(p, t);
            n->value = false;
            return (Iron_Node *)n;
        }
        /* null */
        case IRON_TOK_NULL_KW: {
            iron_advance(p);
            Iron_NullLit *n = ARENA_ALLOC(p->arena, Iron_NullLit);
            n->kind = IRON_NODE_NULL_LIT;
            n->span = iron_token_span(p, t);
            return (Iron_Node *)n;
        }
        /* Unary minus */
        case IRON_TOK_MINUS: {
            iron_advance(p);
            Iron_Node *operand = iron_parse_expr_prec(p, PREC_UNARY);
            Iron_UnaryExpr *n  = ARENA_ALLOC(p->arena, Iron_UnaryExpr);
            n->kind            = IRON_NODE_UNARY;
            n->span            = iron_span_merge(iron_token_span(p, t), operand->span);
            n->op              = (Iron_OpKind)IRON_TOK_MINUS;
            n->operand         = operand;
            return (Iron_Node *)n;
        }
        /* Unary not */
        case IRON_TOK_NOT: {
            iron_advance(p);
            Iron_Node *operand = iron_parse_expr_prec(p, PREC_UNARY);
            Iron_UnaryExpr *n  = ARENA_ALLOC(p->arena, Iron_UnaryExpr);
            n->kind            = IRON_NODE_UNARY;
            n->span            = iron_span_merge(iron_token_span(p, t), operand->span);
            n->op              = (Iron_OpKind)IRON_TOK_NOT;
            n->operand         = operand;
            return (Iron_Node *)n;
        }
        /* Grouped expression */
        case IRON_TOK_LPAREN: {
            iron_advance(p);
            iron_skip_newlines(p);
            Iron_Node *inner = iron_parse_expr(p);
            iron_skip_newlines(p);
            iron_expect(p, IRON_TOK_RPAREN);
            return inner;
        }
        /* heap expr */
        case IRON_TOK_HEAP: {
            iron_advance(p);
            /* Use PREC_UNARY (9) so PREC_CALL infix operators (., [], ()) are
             * captured as part of the inner expression (e.g. heap Enemy(args)) */
            Iron_Node *inner   = iron_parse_expr_prec(p, PREC_UNARY);
            Iron_HeapExpr *n   = ARENA_ALLOC(p->arena, Iron_HeapExpr);
            n->kind            = IRON_NODE_HEAP;
            n->span            = iron_span_merge(iron_token_span(p, t), inner->span);
            n->inner           = inner;
            n->resolved_type   = NULL;   /* set by type checker */
            n->auto_free       = false;  /* set by escape analyzer */
            n->escapes         = false;  /* set by escape analyzer */
            return (Iron_Node *)n;
        }
        /* rc expr */
        case IRON_TOK_RC: {
            iron_advance(p);
            Iron_Node *inner = iron_parse_expr_prec(p, PREC_UNARY);
            Iron_RcExpr *n   = ARENA_ALLOC(p->arena, Iron_RcExpr);
            n->kind          = IRON_NODE_RC;
            n->span          = iron_span_merge(iron_token_span(p, t), inner->span);
            n->inner         = inner;
            return (Iron_Node *)n;
        }
        /* comptime expr */
        case IRON_TOK_COMPTIME: {
            iron_advance(p);
            Iron_Node *inner      = iron_parse_expr_prec(p, PREC_UNARY);
            Iron_ComptimeExpr *n  = ARENA_ALLOC(p->arena, Iron_ComptimeExpr);
            n->kind               = IRON_NODE_COMPTIME;
            n->span               = iron_span_merge(iron_token_span(p, t), inner->span);
            n->inner              = inner;
            return (Iron_Node *)n;
        }
        /* await expr */
        case IRON_TOK_AWAIT: {
            iron_advance(p);
            Iron_Node *handle  = iron_parse_expr_prec(p, PREC_UNARY);
            Iron_AwaitExpr *n  = ARENA_ALLOC(p->arena, Iron_AwaitExpr);
            n->kind            = IRON_NODE_AWAIT;
            n->span            = iron_span_merge(iron_token_span(p, t), handle->span);
            n->handle          = handle;
            return (Iron_Node *)n;
        }
        /* Lambda: func(...) [-> T] { body } */
        case IRON_TOK_FUNC: {
            return iron_parse_lambda(p);
        }
        /* Array literal: [Type; Size] or [elem, elem, ...] */
        case IRON_TOK_LBRACKET: {
            Iron_Token *lb = iron_current(p);
            iron_advance(p);
            iron_skip_newlines(p);

            Iron_ArrayLit *arr = ARENA_ALLOC(p->arena, Iron_ArrayLit);
            arr->kind         = IRON_NODE_ARRAY_LIT;
            arr->type_ann     = NULL;
            arr->size         = NULL;
            arr->elements     = NULL;
            arr->element_count = 0;

            /* Check for [Type; Size] form */
            if (iron_check(p, IRON_TOK_IDENTIFIER)) {
                /* Peek: if identifier followed by semicolon, it's [Type; Size] */
                int saved_pos = p->pos;
                iron_advance(p);  /* consume identifier */
                if (iron_check(p, IRON_TOK_SEMICOLON)) {
                    /* restore and parse as [TypeAnnotation; Size] */
                    p->pos = saved_pos;
                    iron_skip_newlines(p);
                    arr->type_ann = iron_parse_type_annotation(p);
                    iron_expect(p, IRON_TOK_SEMICOLON);
                    arr->size = iron_parse_expr(p);
                    iron_expect(p, IRON_TOK_RBRACKET);
                    arr->span = iron_span_merge(iron_token_span(p, lb),
                                                iron_token_span(p, iron_current(p)));
                    return (Iron_Node *)arr;
                } else {
                    /* restore and parse as element list */
                    p->pos = saved_pos;
                }
            }

            /* Element list */
            Iron_Node **elems = NULL;
            int elem_count    = 0;
            while (!iron_check(p, IRON_TOK_RBRACKET) && !iron_check(p, IRON_TOK_EOF)) {
                iron_skip_newlines(p);
                if (iron_check(p, IRON_TOK_RBRACKET)) break;
                Iron_Node *elem = iron_parse_expr(p);
                arrput(elems, elem);
                elem_count++;
                iron_skip_newlines(p);
                if (!iron_match(p, IRON_TOK_COMMA)) break;
                iron_skip_newlines(p);
            }
            iron_expect(p, IRON_TOK_RBRACKET);
            arr->elements      = elems;
            arr->element_count = elem_count;
            arr->span          = iron_span_merge(iron_token_span(p, lb),
                                                 iron_token_span(p, iron_current(p)));
            return (Iron_Node *)arr;
        }
        /* Identifier: name, or TypeName(...) construct */
        case IRON_TOK_IDENTIFIER: {
            iron_advance(p);
            Iron_Ident *id = ARENA_ALLOC(p->arena, Iron_Ident);
            id->kind         = IRON_NODE_IDENT;
            id->span         = iron_token_span(p, t);
            id->name         = iron_arena_strdup(p->arena, t->value, strlen(t->value));
            id->resolved_sym = NULL;
            id->resolved_type = NULL;
            return (Iron_Node *)id;
        }
        /* self and super are expression keywords that resolve to idents */
        case IRON_TOK_SELF: {
            iron_advance(p);
            Iron_Ident *id = ARENA_ALLOC(p->arena, Iron_Ident);
            id->kind         = IRON_NODE_IDENT;
            id->span         = iron_token_span(p, t);
            id->name         = "self";
            id->resolved_sym = NULL;
            id->resolved_type = NULL;
            return (Iron_Node *)id;
        }
        case IRON_TOK_SUPER: {
            iron_advance(p);
            Iron_Ident *id = ARENA_ALLOC(p->arena, Iron_Ident);
            id->kind         = IRON_NODE_IDENT;
            id->span         = iron_token_span(p, t);
            id->name         = "super";
            id->resolved_sym = NULL;
            id->resolved_type = NULL;
            return (Iron_Node *)id;
        }
        default:
            break;
    }

    /* Unexpected token in expression position */
    iron_emit_diag(p, IRON_ERR_EXPECTED_EXPR,
                   iron_token_span(p, t),
                   "expected expression");
    /* Do not advance — let the caller handle recovery */
    return iron_make_error(p);
}

/* Main Pratt expression parser */
static Iron_Node *iron_parse_expr_prec(Iron_Parser *p, int min_prec) {
    Iron_Node *left = iron_parse_primary(p);

    for (;;) {
        iron_skip_newlines(p);
        Iron_TokenKind cur = iron_peek(p);
        int prec = iron_infix_prec(cur);
        if (prec <= min_prec && cur != IRON_TOK_IS) break;
        if (cur == IRON_TOK_IS && PREC_IS <= min_prec) break;

        /* Field access / method call: expr.name or expr.name(args) */
        if (cur == IRON_TOK_DOT) {
            iron_advance(p);
            if (!iron_check(p, IRON_TOK_IDENTIFIER)) {
                iron_diag_emit(p->diags, p->arena, IRON_DIAG_ERROR,
                               IRON_ERR_UNEXPECTED_TOKEN,
                               iron_token_span(p, iron_current(p)),
                               "expected field or method name after '.'", NULL);
                left = iron_make_error(p);
                continue;
            }
            Iron_Token *name_tok = iron_advance(p);
            const char *name = iron_arena_strdup(p->arena, name_tok->value,
                                                  strlen(name_tok->value));

            if (iron_check(p, IRON_TOK_LPAREN)) {
                /* Method call: obj.method(args) */
                int arg_count = 0;
                Iron_Node **args = iron_parse_call_args(p, &arg_count);
                Iron_MethodCallExpr *mc = ARENA_ALLOC(p->arena, Iron_MethodCallExpr);
                mc->kind      = IRON_NODE_METHOD_CALL;
                mc->span      = iron_span_merge(left->span,
                                                iron_token_span(p, iron_current(p)));
                mc->object    = left;
                mc->method    = name;
                mc->args      = args;
                mc->arg_count = arg_count;
                left = (Iron_Node *)mc;
            } else {
                /* Field access: obj.field */
                Iron_FieldAccess *fa = ARENA_ALLOC(p->arena, Iron_FieldAccess);
                fa->kind   = IRON_NODE_FIELD_ACCESS;
                fa->span   = iron_span_merge(left->span,
                                             iron_token_span(p, iron_current(p)));
                fa->object = left;
                fa->field  = name;
                left = (Iron_Node *)fa;
            }
            continue;
        }

        /* Index / slice: expr[idx] or expr[start..end] */
        if (cur == IRON_TOK_LBRACKET) {
            iron_advance(p);
            iron_skip_newlines(p);
            Iron_Node *idx = iron_parse_expr(p);
            iron_skip_newlines(p);

            if (iron_match(p, IRON_TOK_DOTDOT)) {
                /* Slice */
                Iron_Node *end_expr = NULL;
                if (!iron_check(p, IRON_TOK_RBRACKET)) {
                    end_expr = iron_parse_expr(p);
                }
                iron_expect(p, IRON_TOK_RBRACKET);
                Iron_SliceExpr *sl = ARENA_ALLOC(p->arena, Iron_SliceExpr);
                sl->kind   = IRON_NODE_SLICE;
                sl->span   = iron_span_merge(left->span,
                                             iron_token_span(p, iron_current(p)));
                sl->object = left;
                sl->start  = idx;
                sl->end    = end_expr;
                left = (Iron_Node *)sl;
            } else {
                iron_expect(p, IRON_TOK_RBRACKET);
                Iron_IndexExpr *ix = ARENA_ALLOC(p->arena, Iron_IndexExpr);
                ix->kind   = IRON_NODE_INDEX;
                ix->span   = iron_span_merge(left->span,
                                             iron_token_span(p, iron_current(p)));
                ix->object = left;
                ix->index  = idx;
                left = (Iron_Node *)ix;
            }
            continue;
        }

        /* Function call: expr(args) */
        if (cur == IRON_TOK_LPAREN) {
            int arg_count = 0;
            Iron_Node **args = iron_parse_call_args(p, &arg_count);

            /* If callee is an Ident, may be construct or call.
             * We emit a CallExpr regardless; semantic analysis disambiguates. */
            Iron_CallExpr *call = ARENA_ALLOC(p->arena, Iron_CallExpr);
            call->kind      = IRON_NODE_CALL;
            call->span      = iron_span_merge(left->span,
                                              iron_token_span(p, iron_current(p)));
            call->callee    = left;
            call->args      = args;
            call->arg_count = arg_count;
            left = (Iron_Node *)call;
            continue;
        }

        /* is expression */
        if (cur == IRON_TOK_IS) {
            iron_advance(p);
            if (!iron_check(p, IRON_TOK_IDENTIFIER)) {
                iron_diag_emit(p->diags, p->arena, IRON_DIAG_ERROR,
                               IRON_ERR_UNEXPECTED_TOKEN,
                               iron_token_span(p, iron_current(p)),
                               "expected type name after 'is'", NULL);
                left = iron_make_error(p);
                continue;
            }
            Iron_Token *type_tok = iron_advance(p);
            const char *type_name = iron_arena_strdup(p->arena, type_tok->value,
                                                       strlen(type_tok->value));
            Iron_IsExpr *is_n = ARENA_ALLOC(p->arena, Iron_IsExpr);
            is_n->kind      = IRON_NODE_IS;
            is_n->span      = iron_span_merge(left->span,
                                              iron_token_span(p, iron_current(p)));
            is_n->expr      = left;
            is_n->type_name = type_name;
            left = (Iron_Node *)is_n;
            continue;
        }

        /* Binary operators */
        Iron_Token *op_tok = iron_advance(p);
        iron_skip_newlines(p);
        Iron_Node *right = iron_parse_expr_prec(p, prec);

        Iron_BinaryExpr *bin = ARENA_ALLOC(p->arena, Iron_BinaryExpr);
        bin->kind  = IRON_NODE_BINARY;
        bin->span  = iron_span_merge(left->span, right->span);
        bin->left  = left;
        bin->op    = (Iron_OpKind)op_tok->kind;
        bin->right = right;
        left = (Iron_Node *)bin;
    }

    return left;
}

static Iron_Node *iron_parse_expr(Iron_Parser *p) {
    return iron_parse_expr_prec(p, PREC_ASSIGN);
}

/* ── Statements ──────────────────────────────────────────────────────────── */

static Iron_Node *iron_parse_if_stmt(Iron_Parser *p) {
    Iron_Token *start = iron_current(p);
    iron_advance(p);  /* consume 'if' */

    Iron_Node *cond = iron_parse_expr(p);
    Iron_Node *body = iron_parse_block(p);

    Iron_Node **elif_conds  = NULL;
    Iron_Node **elif_bodies = NULL;
    int         elif_count  = 0;
    Iron_Node  *else_body   = NULL;

    iron_skip_newlines(p);
    while (iron_check(p, IRON_TOK_ELIF)) {
        iron_advance(p);
        Iron_Node *ec = iron_parse_expr(p);
        Iron_Node *eb = iron_parse_block(p);
        arrput(elif_conds, ec);
        arrput(elif_bodies, eb);
        elif_count++;
        iron_skip_newlines(p);
    }

    if (iron_check(p, IRON_TOK_ELSE)) {
        iron_advance(p);
        else_body = iron_parse_block(p);
    }

    Iron_IfStmt *n  = ARENA_ALLOC(p->arena, Iron_IfStmt);
    n->kind         = IRON_NODE_IF;
    n->span         = iron_span_merge(iron_token_span(p, start),
                                       else_body ? else_body->span : body->span);
    n->condition    = cond;
    n->body         = body;
    n->elif_conds   = elif_conds;
    n->elif_bodies  = elif_bodies;
    n->elif_count   = elif_count;
    n->else_body    = else_body;
    return (Iron_Node *)n;
}

static Iron_Node *iron_parse_while_stmt(Iron_Parser *p) {
    Iron_Token *start = iron_current(p);
    iron_advance(p);  /* consume 'while' */

    Iron_Node *cond = iron_parse_expr(p);
    Iron_Node *body = iron_parse_block(p);

    Iron_WhileStmt *n = ARENA_ALLOC(p->arena, Iron_WhileStmt);
    n->kind           = IRON_NODE_WHILE;
    n->span           = iron_span_merge(iron_token_span(p, start), body->span);
    n->condition      = cond;
    n->body           = body;
    return (Iron_Node *)n;
}

static Iron_Node *iron_parse_for_stmt(Iron_Parser *p) {
    Iron_Token *start = iron_current(p);
    iron_advance(p);  /* consume 'for' */

    /* var_name */
    if (!iron_check(p, IRON_TOK_IDENTIFIER)) {
        iron_diag_emit(p->diags, p->arena, IRON_DIAG_ERROR,
                       IRON_ERR_UNEXPECTED_TOKEN,
                       iron_token_span(p, iron_current(p)),
                       "expected loop variable name after 'for'", NULL);
        return iron_make_error(p);
    }
    Iron_Token *var_tok  = iron_advance(p);
    const char *var_name = iron_arena_strdup(p->arena, var_tok->value,
                                              strlen(var_tok->value));

    /* 'in' */
    if (!iron_expect(p, IRON_TOK_IN)) return iron_make_error(p);

    Iron_Node *iterable = iron_parse_expr(p);

    /* optional: parallel [( pool )] */
    bool       is_parallel = false;
    Iron_Node *pool_expr   = NULL;

    iron_skip_newlines(p);
    if (iron_check(p, IRON_TOK_PARALLEL)) {
        iron_advance(p);
        is_parallel = true;
        if (iron_check(p, IRON_TOK_LPAREN)) {
            iron_advance(p);
            pool_expr = iron_parse_expr(p);
            iron_expect(p, IRON_TOK_RPAREN);
        }
    }

    Iron_Node *body = iron_parse_block(p);

    Iron_ForStmt *n = ARENA_ALLOC(p->arena, Iron_ForStmt);
    n->kind         = IRON_NODE_FOR;
    n->span         = iron_span_merge(iron_token_span(p, start), body->span);
    n->var_name     = var_name;
    n->iterable     = iterable;
    n->body         = body;
    n->is_parallel  = is_parallel;
    n->pool_expr    = pool_expr;
    return (Iron_Node *)n;
}

static Iron_Node *iron_parse_match_stmt(Iron_Parser *p) {
    Iron_Token *start = iron_current(p);
    iron_advance(p);  /* consume 'match' */

    Iron_Node *subject = iron_parse_expr(p);

    if (!iron_expect(p, IRON_TOK_LBRACE)) return iron_make_error(p);
    iron_skip_newlines(p);

    Iron_Node **cases = NULL;
    int case_count    = 0;
    Iron_Node *else_body = NULL;

    while (!iron_check(p, IRON_TOK_RBRACE) && !iron_check(p, IRON_TOK_EOF)) {
        iron_skip_newlines(p);
        if (iron_check(p, IRON_TOK_RBRACE)) break;

        if (iron_check(p, IRON_TOK_ELSE)) {
            iron_advance(p);
            else_body = iron_parse_block(p);
            iron_skip_newlines(p);
            break;
        }

        /* Pattern is an expression (typically a qualified Ident like Enum.VARIANT) */
        Iron_Node *pattern = iron_parse_expr(p);
        Iron_Node *cbody   = iron_parse_block(p);

        Iron_MatchCase *mc = ARENA_ALLOC(p->arena, Iron_MatchCase);
        mc->kind           = IRON_NODE_MATCH_CASE;
        mc->span           = iron_span_merge(pattern->span, cbody->span);
        mc->pattern        = pattern;
        mc->body           = cbody;
        arrput(cases, (Iron_Node *)mc);
        case_count++;
        iron_skip_newlines(p);
    }

    Iron_Token *end = iron_current(p);
    iron_expect(p, IRON_TOK_RBRACE);

    Iron_MatchStmt *n = ARENA_ALLOC(p->arena, Iron_MatchStmt);
    n->kind           = IRON_NODE_MATCH;
    n->span           = iron_span_merge(iron_token_span(p, start),
                                         iron_token_span(p, end));
    n->subject        = subject;
    n->cases          = cases;
    n->case_count     = case_count;
    n->else_body      = else_body;
    return (Iron_Node *)n;
}

static Iron_Node *iron_parse_spawn_stmt(Iron_Parser *p) {
    Iron_Token *start = iron_current(p);
    iron_advance(p);  /* consume 'spawn' */

    /* spawn("name" [, pool]) { body }
     * We only parse the basic form here; handle_name is set by caller if needed */
    iron_expect(p, IRON_TOK_LPAREN);

    const char *spawn_name = NULL;
    if (iron_check(p, IRON_TOK_STRING)) {
        Iron_Token *nt = iron_advance(p);
        spawn_name = iron_arena_strdup(p->arena, nt->value, strlen(nt->value));
    } else {
        iron_diag_emit(p->diags, p->arena, IRON_DIAG_ERROR,
                       IRON_ERR_UNEXPECTED_TOKEN,
                       iron_token_span(p, iron_current(p)),
                       "expected string name as first argument to spawn", NULL);
        spawn_name = "<error>";
    }

    Iron_Node *pool_expr = NULL;
    if (iron_match(p, IRON_TOK_COMMA)) {
        pool_expr = iron_parse_expr(p);
    }
    iron_expect(p, IRON_TOK_RPAREN);

    Iron_Node *body = iron_parse_block(p);

    Iron_SpawnStmt *n = ARENA_ALLOC(p->arena, Iron_SpawnStmt);
    n->kind           = IRON_NODE_SPAWN;
    n->span           = iron_span_merge(iron_token_span(p, start), body->span);
    n->name           = spawn_name;
    n->pool_expr      = pool_expr;
    n->body           = body;
    n->handle_name    = NULL;
    return (Iron_Node *)n;
}

/* ── String interpolation parsing ────────────────────────────────────────── */

/* Parse an interpolated string token value into an InterpString node.
 * raw_value is the token text (already stripped of outer quotes by the lexer).
 * Splits on { } boundaries:
 *   literal text → Iron_StringLit node
 *   {expr}       → re-lex and parse as expression
 */
static Iron_Node *iron_parse_interp_string(Iron_Parser *p, const char *raw_value,
                                            Iron_Span span) {
    Iron_InterpString *n = ARENA_ALLOC(p->arena, Iron_InterpString);
    n->kind       = IRON_NODE_INTERP_STRING;
    n->span       = span;
    n->parts      = NULL;
    n->part_count = 0;

    if (!raw_value) {
        return (Iron_Node *)n;
    }

    const char *s   = raw_value;
    size_t      len = strlen(s);

    /* Temporary buffer for accumulating literal segments */
    char *lit_buf = (char *)malloc(len + 1);
    if (!lit_buf) return (Iron_Node *)n;

    size_t lit_len = 0;

    size_t i = 0;
    while (i < len) {
        if (s[i] == '{') {
            /* Flush accumulated literal segment */
            if (lit_len > 0) {
                lit_buf[lit_len] = '\0';
                Iron_StringLit *sl = ARENA_ALLOC(p->arena, Iron_StringLit);
                sl->kind  = IRON_NODE_STRING_LIT;
                sl->span  = span;
                sl->value = iron_arena_strdup(p->arena, lit_buf, lit_len);
                arrput(n->parts, (Iron_Node *)sl);
                n->part_count++;
                lit_len = 0;
            }

            /* Find matching closing brace, respecting nested parens/brackets */
            i++;  /* skip '{' */
            size_t expr_start = i;
            int depth = 1;
            while (i < len && depth > 0) {
                if (s[i] == '{') depth++;
                else if (s[i] == '}') depth--;
                if (depth > 0) i++;
            }
            /* s[i] is now the closing '}' (or end of string) */
            size_t expr_len = i - expr_start;
            if (i < len) i++;  /* skip '}' */

            /* Extract expression text */
            char *expr_buf = (char *)malloc(expr_len + 1);
            if (expr_buf) {
                memcpy(expr_buf, s + expr_start, expr_len);
                expr_buf[expr_len] = '\0';

                /* Re-lex the expression */
                Iron_DiagList expr_diags = iron_diaglist_create();
                Iron_Lexer    el         = iron_lexer_create(expr_buf, p->filename,
                                                              p->arena, &expr_diags);
                Iron_Token   *expr_toks  = iron_lex_all(&el);
                int           tok_count  = 0;
                while (expr_toks[tok_count].kind != IRON_TOK_EOF) tok_count++;
                tok_count++;  /* include EOF */

                /* Parse the expression using a sub-parser */
                Iron_Parser sub = iron_parser_create(expr_toks, tok_count,
                                                      expr_buf, p->filename,
                                                      p->arena, p->diags);
                Iron_Node *expr_node = iron_parse_expr_prec(&sub, PREC_NONE);
                iron_diaglist_free(&expr_diags);
                free(expr_buf);

                if (expr_node && expr_node->kind != IRON_NODE_ERROR) {
                    arrput(n->parts, expr_node);
                    n->part_count++;
                } else {
                    /* Failed to parse expression: emit diagnostic and insert ErrorNode */
                    iron_diag_emit(p->diags, p->arena, IRON_DIAG_ERROR,
                                   IRON_ERR_EXPECTED_EXPR, span,
                                   "failed to parse interpolated expression", NULL);
                    arrput(n->parts, iron_make_error(p));
                    n->part_count++;
                }
            }
        } else {
            lit_buf[lit_len++] = s[i++];
        }
    }

    /* Flush trailing literal */
    if (lit_len > 0) {
        lit_buf[lit_len] = '\0';
        Iron_StringLit *sl = ARENA_ALLOC(p->arena, Iron_StringLit);
        sl->kind  = IRON_NODE_STRING_LIT;
        sl->span  = span;
        sl->value = iron_arena_strdup(p->arena, lit_buf, lit_len);
        arrput(n->parts, (Iron_Node *)sl);
        n->part_count++;
        lit_len = 0;
    }
    (void)lit_len;

    free(lit_buf);
    return (Iron_Node *)n;
}

static Iron_Node *iron_parse_val_decl(Iron_Parser *p) {
    Iron_Token *start = iron_current(p);
    iron_advance(p);  /* consume 'val' */

    if (!iron_check(p, IRON_TOK_IDENTIFIER)) {
        iron_diag_emit(p->diags, p->arena, IRON_DIAG_ERROR,
                       IRON_ERR_UNEXPECTED_TOKEN,
                       iron_token_span(p, iron_current(p)),
                       "expected variable name after 'val'", NULL);
        return iron_make_error(p);
    }
    Iron_Token *name_tok = iron_advance(p);

    Iron_Node *type_ann = NULL;
    if (iron_match(p, IRON_TOK_COLON)) {
        type_ann = iron_parse_type_annotation(p);
    }

    Iron_Node *init = NULL;
    if (iron_match(p, IRON_TOK_ASSIGN)) {
        init = iron_parse_expr(p);
    }

    Iron_ValDecl *n = ARENA_ALLOC(p->arena, Iron_ValDecl);
    n->kind          = IRON_NODE_VAL_DECL;
    n->span          = iron_span_merge(iron_token_span(p, start),
                                       init ? init->span : iron_token_span(p, name_tok));
    n->name          = iron_arena_strdup(p->arena, name_tok->value,
                                         strlen(name_tok->value));
    n->type_ann      = type_ann;
    n->init          = init;
    n->declared_type = NULL;  /* set by type checker */
    return (Iron_Node *)n;
}

static Iron_Node *iron_parse_var_decl(Iron_Parser *p) {
    Iron_Token *start = iron_current(p);
    iron_advance(p);  /* consume 'var' */

    if (!iron_check(p, IRON_TOK_IDENTIFIER)) {
        iron_diag_emit(p->diags, p->arena, IRON_DIAG_ERROR,
                       IRON_ERR_UNEXPECTED_TOKEN,
                       iron_token_span(p, iron_current(p)),
                       "expected variable name after 'var'", NULL);
        return iron_make_error(p);
    }
    Iron_Token *name_tok = iron_advance(p);

    Iron_Node *type_ann = NULL;
    if (iron_match(p, IRON_TOK_COLON)) {
        type_ann = iron_parse_type_annotation(p);
    }

    Iron_Node *init = NULL;
    if (iron_match(p, IRON_TOK_ASSIGN)) {
        init = iron_parse_expr(p);
    }

    Iron_VarDecl *n = ARENA_ALLOC(p->arena, Iron_VarDecl);
    n->kind          = IRON_NODE_VAR_DECL;
    n->span          = iron_span_merge(iron_token_span(p, start),
                                       init ? init->span : iron_token_span(p, name_tok));
    n->name          = iron_arena_strdup(p->arena, name_tok->value,
                                         strlen(name_tok->value));
    n->type_ann      = type_ann;
    n->init          = init;
    n->declared_type = NULL;  /* set by type checker */
    return (Iron_Node *)n;
}

/* Parse a single statement */
static Iron_Node *iron_parse_stmt(Iron_Parser *p) {
    iron_skip_newlines(p);
    Iron_Token *t = iron_current(p);

    switch (t->kind) {
        case IRON_TOK_VAL:
            return iron_parse_val_decl(p);
        case IRON_TOK_VAR:
            return iron_parse_var_decl(p);
        case IRON_TOK_RETURN: {
            iron_advance(p);
            Iron_Node *val = NULL;
            /* Return without value if next is } or newline/eof */
            if (!iron_check(p, IRON_TOK_RBRACE) && !iron_check(p, IRON_TOK_EOF)) {
                val = iron_parse_expr(p);
            }
            Iron_ReturnStmt *n = ARENA_ALLOC(p->arena, Iron_ReturnStmt);
            n->kind  = IRON_NODE_RETURN;
            n->span  = iron_span_merge(iron_token_span(p, t),
                                        val ? val->span : iron_token_span(p, t));
            n->value = val;
            return (Iron_Node *)n;
        }
        case IRON_TOK_IF:
            return iron_parse_if_stmt(p);
        case IRON_TOK_WHILE:
            return iron_parse_while_stmt(p);
        case IRON_TOK_FOR:
            return iron_parse_for_stmt(p);
        case IRON_TOK_MATCH:
            return iron_parse_match_stmt(p);
        case IRON_TOK_DEFER: {
            iron_advance(p);
            Iron_Node *expr  = iron_parse_expr(p);
            Iron_DeferStmt *n = ARENA_ALLOC(p->arena, Iron_DeferStmt);
            n->kind           = IRON_NODE_DEFER;
            n->span           = iron_span_merge(iron_token_span(p, t), expr->span);
            n->expr           = expr;
            return (Iron_Node *)n;
        }
        case IRON_TOK_FREE: {
            iron_advance(p);
            Iron_Node *expr = iron_parse_expr(p);
            Iron_FreeStmt *n = ARENA_ALLOC(p->arena, Iron_FreeStmt);
            n->kind          = IRON_NODE_FREE;
            n->span          = iron_span_merge(iron_token_span(p, t), expr->span);
            n->expr          = expr;
            return (Iron_Node *)n;
        }
        case IRON_TOK_LEAK: {
            iron_advance(p);
            Iron_Node *expr = iron_parse_expr(p);
            Iron_LeakStmt *n = ARENA_ALLOC(p->arena, Iron_LeakStmt);
            n->kind          = IRON_NODE_LEAK;
            n->span          = iron_span_merge(iron_token_span(p, t), expr->span);
            n->expr          = expr;
            return (Iron_Node *)n;
        }
        case IRON_TOK_SPAWN:
            return iron_parse_spawn_stmt(p);
        case IRON_TOK_DRAW: {
            Iron_Token *draw_tok = iron_current(p);
            iron_advance(p);  /* consume 'draw' */
            Iron_Node *body = iron_parse_block(p);
            Iron_DrawBlock *db = ARENA_ALLOC(p->arena, Iron_DrawBlock);
            db->kind = IRON_NODE_DRAW;
            db->span = iron_span_merge(iron_token_span(p, draw_tok), body->span);
            db->body = body;
            return (Iron_Node *)db;
        }
        case IRON_TOK_LBRACE:
            return iron_parse_block(p);
        default: {
            /* Expression statement, or assignment */
            Iron_Node *expr = iron_parse_expr(p);

            iron_skip_newlines(p);

            /* Check for assignment operators */
            Iron_TokenKind op = iron_peek(p);
            if (op == IRON_TOK_ASSIGN      ||
                op == IRON_TOK_PLUS_ASSIGN  ||
                op == IRON_TOK_MINUS_ASSIGN ||
                op == IRON_TOK_STAR_ASSIGN  ||
                op == IRON_TOK_SLASH_ASSIGN) {
                iron_advance(p);
                iron_skip_newlines(p);
                Iron_Node *val = iron_parse_expr(p);
                Iron_AssignStmt *a = ARENA_ALLOC(p->arena, Iron_AssignStmt);
                a->kind   = IRON_NODE_ASSIGN;
                a->span   = iron_span_merge(expr->span, val->span);
                a->target = expr;
                a->value  = val;
                a->op     = (Iron_OpKind)op;
                return (Iron_Node *)a;
            }

            return expr;
        }
    }
}

/* ── Top-level declarations ──────────────────────────────────────────────── */

static Iron_Node *iron_parse_import_decl(Iron_Parser *p) {
    Iron_Token *start = iron_current(p);
    iron_advance(p);  /* consume 'import' */

    /* path: one or more identifier separated by dots */
    if (!iron_check(p, IRON_TOK_IDENTIFIER)) {
        iron_diag_emit(p->diags, p->arena, IRON_DIAG_ERROR,
                       IRON_ERR_UNEXPECTED_TOKEN,
                       iron_token_span(p, iron_current(p)),
                       "expected module path after 'import'", NULL);
        return iron_make_error(p);
    }

    /* Build path string: "a.b.c" */
    char   path_buf[256];
    size_t path_len = 0;
    Iron_Token *first = iron_advance(p);
    size_t flen = strlen(first->value);
    memcpy(path_buf, first->value, flen);
    path_len = flen;

    while (iron_check(p, IRON_TOK_DOT)) {
        /* lookahead: next must be identifier */
        if (p->pos + 1 < p->token_count &&
            p->tokens[p->pos + 1].kind == IRON_TOK_IDENTIFIER) {
            iron_advance(p);  /* consume '.' */
            Iron_Token *seg = iron_advance(p);
            if (path_len < sizeof(path_buf) - 2) {
                path_buf[path_len++] = '.';
                size_t slen = strlen(seg->value);
                if (path_len + slen < sizeof(path_buf) - 1) {
                    memcpy(path_buf + path_len, seg->value, slen);
                    path_len += slen;
                }
            }
        } else {
            break;
        }
    }
    path_buf[path_len] = '\0';
    const char *path = iron_arena_strdup(p->arena, path_buf, path_len);

    /* optional: as alias */
    const char *alias = NULL;
    if (iron_check(p, IRON_TOK_IDENTIFIER) &&
        strcmp(iron_current(p)->value, "as") == 0) {
        iron_advance(p);  /* consume 'as' */
        if (iron_check(p, IRON_TOK_IDENTIFIER)) {
            Iron_Token *at = iron_advance(p);
            alias = iron_arena_strdup(p->arena, at->value, strlen(at->value));
        }
    }

    Iron_ImportDecl *n = ARENA_ALLOC(p->arena, Iron_ImportDecl);
    n->kind            = IRON_NODE_IMPORT_DECL;
    n->span            = iron_span_merge(iron_token_span(p, start),
                                          iron_token_span(p, iron_current(p)));
    n->path            = path;
    n->alias           = alias;
    return (Iron_Node *)n;
}

/* ── Helper: convert Iron snake_case name to C CamelCase ─────────────────── */

/* e.g. "init_window" -> "InitWindow", "draw_text" -> "DrawText" */
static const char *iron_snake_to_camel(Iron_Arena *arena, const char *name) {
    if (!name) return name;
    size_t len = strlen(name);
    /* Output can be at most len bytes (we remove underscores, add nothing) */
    char *buf = (char *)iron_arena_alloc(arena, len + 1, 1);
    if (!buf) return name;

    size_t out = 0;
    bool capitalize_next = true;  /* capitalize first letter */

    for (size_t i = 0; i < len; i++) {
        char c = name[i];
        if (c == '_') {
            capitalize_next = true;
        } else {
            if (capitalize_next && c >= 'a' && c <= 'z') {
                buf[out++] = (char)(c - 'a' + 'A');
            } else {
                buf[out++] = c;
            }
            capitalize_next = false;
        }
    }
    buf[out] = '\0';
    return buf;
}

/* ── Parse extern func declaration ───────────────────────────────────────── */

static Iron_Node *iron_parse_extern_func(Iron_Parser *p, bool is_private) {
    Iron_Token *start = iron_current(p);
    iron_advance(p);  /* consume 'extern' */

    if (!iron_check(p, IRON_TOK_FUNC)) {
        iron_emit_diag(p, IRON_ERR_UNEXPECTED_TOKEN,
                       iron_token_span(p, iron_current(p)),
                       "expected 'func' after 'extern'");
        p->in_error_recovery = true;
        iron_parser_sync_toplevel(p);
        return iron_make_error(p);
    }
    iron_advance(p);  /* consume 'func' */

    if (!iron_check(p, IRON_TOK_IDENTIFIER)) {
        iron_emit_diag(p, IRON_ERR_UNEXPECTED_TOKEN,
                       iron_token_span(p, iron_current(p)),
                       "expected function name after 'extern func'");
        p->in_error_recovery = true;
        iron_parser_sync_toplevel(p);
        return iron_make_error(p);
    }
    Iron_Token *name_tok = iron_advance(p);
    const char *iron_name = iron_arena_strdup(p->arena, name_tok->value,
                                               strlen(name_tok->value));
    /* Derive C name: snake_case -> CamelCase */
    const char *c_name = iron_snake_to_camel(p->arena, iron_name);

    /* Parse parameter list (may be empty) */
    int         param_count = 0;
    Iron_Node **params      = iron_parse_param_list(p, &param_count);

    /* Optional return type */
    Iron_Node *ret = NULL;
    if (iron_match(p, IRON_TOK_ARROW)) {
        ret = iron_parse_type_annotation(p);
    }

    /* No body for extern funcs */
    Iron_FuncDecl *f        = ARENA_ALLOC(p->arena, Iron_FuncDecl);
    f->kind                 = IRON_NODE_FUNC_DECL;
    f->span                 = iron_span_merge(iron_token_span(p, start),
                                              ret ? ret->span
                                                  : iron_token_span(p, name_tok));
    f->name                 = iron_name;
    f->params               = params;
    f->param_count          = param_count;
    f->return_type          = ret;
    f->body                 = NULL;        /* extern funcs have no body */
    f->is_private           = is_private;
    f->is_extern            = true;
    f->extern_c_name        = c_name;
    f->generic_params       = NULL;
    f->generic_param_count  = 0;
    f->resolved_return_type = NULL;
    return (Iron_Node *)f;
}

static Iron_Node *iron_parse_func_or_method(Iron_Parser *p, bool is_private) {
    Iron_Token *start = iron_current(p);
    iron_advance(p);  /* consume 'func' */

    /* Check for generic method: Pool[T].method or just name.
     * Accept both identifiers and 'draw' keyword (common method name). */
    if (!iron_check_name(p)) {
        iron_emit_diag(p, IRON_ERR_UNEXPECTED_TOKEN,
                       iron_token_span(p, iron_current(p)),
                       "expected function name");
        p->in_error_recovery = true;
        iron_parser_sync_toplevel(p);
        return iron_make_error(p);
    }
    Iron_Token *name_tok = iron_advance(p);

    /* Generic params on the name itself: func find[T](...) */
    int         generic_count  = 0;
    Iron_Node **generic_params = NULL;
    if (iron_check(p, IRON_TOK_LBRACKET)) {
        generic_params = iron_parse_generic_params(p, &generic_count, p->arena);
    }

    /* Check for method: TypeName.method_name */
    if (iron_check(p, IRON_TOK_DOT)) {
        iron_advance(p);  /* consume '.' */
        if (!iron_check_name(p)) {
            iron_diag_emit(p->diags, p->arena, IRON_DIAG_ERROR,
                           IRON_ERR_UNEXPECTED_TOKEN,
                           iron_token_span(p, iron_current(p)),
                           "expected method name after '.'", NULL);
            return iron_make_error(p);
        }
        Iron_Token *method_tok = iron_advance(p);

        /* Method may also have generic params after its name */
        if (iron_check(p, IRON_TOK_LBRACKET) && generic_count == 0) {
            generic_params = iron_parse_generic_params(p, &generic_count, p->arena);
        }

        int param_count = 0;
        Iron_Node **params = iron_parse_param_list(p, &param_count);

        Iron_Node *ret = NULL;
        if (iron_match(p, IRON_TOK_ARROW)) {
            ret = iron_parse_type_annotation(p);
        }

        Iron_Node *body = iron_parse_block(p);

        Iron_MethodDecl *m      = ARENA_ALLOC(p->arena, Iron_MethodDecl);
        m->kind                 = IRON_NODE_METHOD_DECL;
        m->span                 = iron_span_merge(iron_token_span(p, start), body->span);
        m->type_name            = iron_arena_strdup(p->arena, name_tok->value,
                                                     strlen(name_tok->value));
        m->method_name          = iron_arena_strdup(p->arena, method_tok->value,
                                                     strlen(method_tok->value));
        m->params               = params;
        m->param_count          = param_count;
        m->return_type          = ret;
        m->body                 = body;
        m->is_private           = is_private;
        m->generic_params       = generic_params;
        m->generic_param_count  = generic_count;
        m->resolved_return_type = NULL;  /* set by type checker */
        m->owner_sym            = NULL;  /* set by resolver */
        return (Iron_Node *)m;
    }

    /* Regular function */
    int param_count = 0;
    Iron_Node **params = iron_parse_param_list(p, &param_count);

    Iron_Node *ret = NULL;
    if (iron_match(p, IRON_TOK_ARROW)) {
        ret = iron_parse_type_annotation(p);
    }

    Iron_Node *body = iron_parse_block(p);

    Iron_FuncDecl *f        = ARENA_ALLOC(p->arena, Iron_FuncDecl);
    f->kind                 = IRON_NODE_FUNC_DECL;
    f->span                 = iron_span_merge(iron_token_span(p, start), body->span);
    f->name                 = iron_arena_strdup(p->arena, name_tok->value,
                                               strlen(name_tok->value));
    f->params               = params;
    f->param_count          = param_count;
    f->return_type          = ret;
    f->body                 = body;
    f->is_private           = is_private;
    f->is_extern            = false;
    f->extern_c_name        = NULL;
    f->generic_params       = generic_params;
    f->generic_param_count  = generic_count;
    f->resolved_return_type = NULL;  /* set by type checker */
    return (Iron_Node *)f;
}

static Iron_Node *iron_parse_object_decl(Iron_Parser *p, bool is_private) {
    Iron_Token *start = iron_current(p);
    iron_advance(p);  /* consume 'object' */

    if (!iron_check(p, IRON_TOK_IDENTIFIER)) {
        iron_emit_diag(p, IRON_ERR_UNEXPECTED_TOKEN,
                       iron_token_span(p, iron_current(p)),
                       "expected object name");
        p->in_error_recovery = true;
        iron_parser_sync_toplevel(p);
        return iron_make_error(p);
    }
    Iron_Token *name_tok = iron_advance(p);

    /* Optional generic params */
    int generic_count = 0;
    Iron_Node **generic_params = NULL;
    if (iron_check(p, IRON_TOK_LBRACKET)) {
        generic_params = iron_parse_generic_params(p, &generic_count, p->arena);
    }

    /* Optional: extends ParentName */
    const char *extends_name = NULL;
    if (iron_check(p, IRON_TOK_EXTENDS)) {
        iron_advance(p);
        if (iron_check(p, IRON_TOK_IDENTIFIER)) {
            Iron_Token *ext_tok = iron_advance(p);
            extends_name = iron_arena_strdup(p->arena, ext_tok->value,
                                              strlen(ext_tok->value));
        }
    }

    /* Optional: implements I1, I2 */
    const char **impl_names = NULL;
    int          impl_count = 0;
    if (iron_check(p, IRON_TOK_IMPLEMENTS)) {
        iron_advance(p);
        while (iron_check(p, IRON_TOK_IDENTIFIER)) {
            Iron_Token *it = iron_advance(p);
            const char *iname = iron_arena_strdup(p->arena, it->value,
                                                   strlen(it->value));
            arrput(impl_names, iname);
            impl_count++;
            if (!iron_match(p, IRON_TOK_COMMA)) break;
        }
    }

    /* Body: { field* } */
    if (!iron_expect(p, IRON_TOK_LBRACE)) return iron_make_error(p);
    iron_skip_newlines(p);

    Iron_Node **fields = NULL;
    int field_count    = 0;

    while (!iron_check(p, IRON_TOK_RBRACE) && !iron_check(p, IRON_TOK_EOF)) {
        iron_skip_newlines(p);
        if (iron_check(p, IRON_TOK_RBRACE)) break;

        bool is_var = false;
        Iron_Token *field_start = iron_current(p);
        if (iron_check(p, IRON_TOK_VAR)) {
            iron_advance(p);
            is_var = true;
        } else if (iron_check(p, IRON_TOK_VAL)) {
            iron_advance(p);
        } else {
            /* Interface method signature inside object — skip with error */
            iron_diag_emit(p->diags, p->arena, IRON_DIAG_ERROR,
                           IRON_ERR_UNEXPECTED_TOKEN,
                           iron_token_span(p, iron_current(p)),
                           "expected field declaration (val or var)", NULL);
            iron_parser_sync_stmt(p);
            continue;
        }

        if (!iron_check(p, IRON_TOK_IDENTIFIER)) {
            iron_diag_emit(p->diags, p->arena, IRON_DIAG_ERROR,
                           IRON_ERR_UNEXPECTED_TOKEN,
                           iron_token_span(p, iron_current(p)),
                           "expected field name", NULL);
            iron_parser_sync_stmt(p);
            continue;
        }
        Iron_Token *fname = iron_advance(p);
        Iron_Node  *ftype = NULL;
        if (iron_match(p, IRON_TOK_COLON)) {
            ftype = iron_parse_type_annotation(p);
        }

        Iron_Field *field = ARENA_ALLOC(p->arena, Iron_Field);
        field->kind       = IRON_NODE_FIELD;
        field->span       = iron_span_merge(iron_token_span(p, field_start),
                                             ftype ? ftype->span
                                                   : iron_token_span(p, fname));
        field->name       = iron_arena_strdup(p->arena, fname->value,
                                               strlen(fname->value));
        field->type_ann   = ftype;
        field->is_var     = is_var;
        arrput(fields, (Iron_Node *)field);
        field_count++;
        iron_skip_newlines(p);
    }

    Iron_Token *end = iron_current(p);
    iron_expect(p, IRON_TOK_RBRACE);

    Iron_ObjectDecl *n         = ARENA_ALLOC(p->arena, Iron_ObjectDecl);
    n->kind                    = IRON_NODE_OBJECT_DECL;
    n->span                    = iron_span_merge(iron_token_span(p, start),
                                                  iron_token_span(p, end));
    n->name                    = iron_arena_strdup(p->arena, name_tok->value,
                                                    strlen(name_tok->value));
    n->fields                  = fields;
    n->field_count             = field_count;
    n->extends_name            = extends_name;
    n->implements_names        = impl_names;
    n->implements_count        = impl_count;
    n->generic_params          = generic_params;
    n->generic_param_count     = generic_count;
    (void)is_private;  /* stored but not used in AST yet */
    return (Iron_Node *)n;
}

static Iron_Node *iron_parse_interface_decl(Iron_Parser *p, bool is_private) {
    Iron_Token *start = iron_current(p);
    iron_advance(p);  /* consume 'interface' */

    if (!iron_check(p, IRON_TOK_IDENTIFIER)) {
        iron_diag_emit(p->diags, p->arena, IRON_DIAG_ERROR,
                       IRON_ERR_UNEXPECTED_TOKEN,
                       iron_token_span(p, iron_current(p)),
                       "expected interface name", NULL);
        return iron_make_error(p);
    }
    Iron_Token *name_tok = iron_advance(p);

    if (!iron_expect(p, IRON_TOK_LBRACE)) return iron_make_error(p);
    iron_skip_newlines(p);

    Iron_Node **method_sigs = NULL;
    int method_count        = 0;

    while (!iron_check(p, IRON_TOK_RBRACE) && !iron_check(p, IRON_TOK_EOF)) {
        iron_skip_newlines(p);
        if (iron_check(p, IRON_TOK_RBRACE)) break;

        /* Method signature: func name(params) [-> Type] — no body */
        if (!iron_check(p, IRON_TOK_FUNC)) {
            iron_diag_emit(p->diags, p->arena, IRON_DIAG_ERROR,
                           IRON_ERR_UNEXPECTED_TOKEN,
                           iron_token_span(p, iron_current(p)),
                           "expected method signature in interface", NULL);
            iron_parser_sync_stmt(p);
            continue;
        }
        Iron_Token *fsig_start = iron_current(p);
        iron_advance(p);  /* consume 'func' */

        /* Method name: can be a regular identifier or the 'draw' keyword used as name */
        if (!iron_check(p, IRON_TOK_IDENTIFIER) && !iron_check(p, IRON_TOK_DRAW)) {
            iron_parser_sync_stmt(p);
            continue;
        }
        Iron_Token *sig_name = iron_advance(p);

        int sig_param_count = 0;
        Iron_Node **sig_params = iron_parse_param_list(p, &sig_param_count);

        Iron_Node *sig_ret = NULL;
        if (iron_match(p, IRON_TOK_ARROW)) {
            sig_ret = iron_parse_type_annotation(p);
        }

        /* Store as a FuncDecl with NULL body to represent a signature */
        Iron_FuncDecl *sig        = ARENA_ALLOC(p->arena, Iron_FuncDecl);
        sig->kind                 = IRON_NODE_FUNC_DECL;
        sig->span                 = iron_span_merge(iron_token_span(p, fsig_start),
                                                     iron_token_span(p, iron_current(p)));
        sig->name                 = iron_arena_strdup(p->arena, sig_name->value,
                                                       strlen(sig_name->value));
        sig->params               = sig_params;
        sig->param_count          = sig_param_count;
        sig->return_type          = sig_ret;
        sig->body                 = NULL;  /* no body — it's a signature */
        sig->is_private           = false;
        sig->is_extern            = false;
        sig->extern_c_name        = NULL;
        sig->generic_params       = NULL;
        sig->generic_param_count  = 0;
        sig->resolved_return_type = NULL;  /* set by type checker */
        arrput(method_sigs, (Iron_Node *)sig);
        method_count++;
        iron_skip_newlines(p);
    }

    Iron_Token *end = iron_current(p);
    iron_expect(p, IRON_TOK_RBRACE);

    Iron_InterfaceDecl *n = ARENA_ALLOC(p->arena, Iron_InterfaceDecl);
    n->kind               = IRON_NODE_INTERFACE_DECL;
    n->span               = iron_span_merge(iron_token_span(p, start),
                                             iron_token_span(p, end));
    n->name               = iron_arena_strdup(p->arena, name_tok->value,
                                               strlen(name_tok->value));
    n->method_sigs        = method_sigs;
    n->method_count       = method_count;
    (void)is_private;
    return (Iron_Node *)n;
}

static Iron_Node *iron_parse_enum_decl(Iron_Parser *p, bool is_private) {
    Iron_Token *start = iron_current(p);
    iron_advance(p);  /* consume 'enum' */

    if (!iron_check(p, IRON_TOK_IDENTIFIER)) {
        iron_diag_emit(p->diags, p->arena, IRON_DIAG_ERROR,
                       IRON_ERR_UNEXPECTED_TOKEN,
                       iron_token_span(p, iron_current(p)),
                       "expected enum name", NULL);
        return iron_make_error(p);
    }
    Iron_Token *name_tok = iron_advance(p);

    if (!iron_expect(p, IRON_TOK_LBRACE)) return iron_make_error(p);
    iron_skip_newlines(p);

    Iron_Node **variants = NULL;
    int variant_count    = 0;

    while (!iron_check(p, IRON_TOK_RBRACE) && !iron_check(p, IRON_TOK_EOF)) {
        iron_skip_newlines(p);
        if (iron_check(p, IRON_TOK_RBRACE)) break;
        if (!iron_check(p, IRON_TOK_IDENTIFIER)) {
            iron_diag_emit(p->diags, p->arena, IRON_DIAG_ERROR,
                           IRON_ERR_UNEXPECTED_TOKEN,
                           iron_token_span(p, iron_current(p)),
                           "expected enum variant name", NULL);
            break;
        }
        Iron_Token *vt  = iron_advance(p);
        Iron_EnumVariant *v = ARENA_ALLOC(p->arena, Iron_EnumVariant);
        v->kind             = IRON_NODE_ENUM_VARIANT;
        v->span             = iron_token_span(p, vt);
        v->name             = iron_arena_strdup(p->arena, vt->value, strlen(vt->value));
        arrput(variants, (Iron_Node *)v);
        variant_count++;
        iron_skip_newlines(p);
        if (!iron_match(p, IRON_TOK_COMMA)) break;
        iron_skip_newlines(p);
    }

    Iron_Token *end = iron_current(p);
    iron_expect(p, IRON_TOK_RBRACE);

    Iron_EnumDecl *n   = ARENA_ALLOC(p->arena, Iron_EnumDecl);
    n->kind            = IRON_NODE_ENUM_DECL;
    n->span            = iron_span_merge(iron_token_span(p, start),
                                          iron_token_span(p, end));
    n->name            = iron_arena_strdup(p->arena, name_tok->value,
                                            strlen(name_tok->value));
    n->variants        = variants;
    n->variant_count   = variant_count;
    (void)is_private;
    return (Iron_Node *)n;
}

static Iron_Node *iron_parse_decl(Iron_Parser *p, bool is_private) {
    switch (iron_peek(p)) {
        case IRON_TOK_EXTERN:    {
            Iron_Node *n = iron_parse_extern_func(p, is_private);
            p->in_error_recovery = false;
            return n;
        }
        case IRON_TOK_FUNC:      {
            Iron_Node *n = iron_parse_func_or_method(p, is_private);
            p->in_error_recovery = false;
            return n;
        }
        case IRON_TOK_OBJECT:    {
            Iron_Node *n = iron_parse_object_decl(p, is_private);
            p->in_error_recovery = false;
            return n;
        }
        case IRON_TOK_INTERFACE: {
            Iron_Node *n = iron_parse_interface_decl(p, is_private);
            p->in_error_recovery = false;
            return n;
        }
        case IRON_TOK_ENUM:      {
            Iron_Node *n = iron_parse_enum_decl(p, is_private);
            p->in_error_recovery = false;
            return n;
        }
        case IRON_TOK_IMPORT:    {
            Iron_Node *n = iron_parse_import_decl(p);
            p->in_error_recovery = false;
            return n;
        }
        case IRON_TOK_VAL:       {
            Iron_Node *n = iron_parse_val_decl(p);
            p->in_error_recovery = false;
            return n;
        }
        case IRON_TOK_VAR:       {
            Iron_Node *n = iron_parse_var_decl(p);
            p->in_error_recovery = false;
            return n;
        }
        default: {
            iron_emit_diag(p, IRON_ERR_UNEXPECTED_TOKEN,
                           iron_token_span(p, iron_current(p)),
                           "unexpected token at top level");
            p->in_error_recovery = true;
            Iron_Node *err = iron_make_error(p);
            iron_advance(p);  /* consume bad token */
            iron_parser_sync_toplevel(p);
            return err;
        }
    }
}

/* ── Entry point ─────────────────────────────────────────────────────────── */

Iron_Node *iron_parse(Iron_Parser *p) {
    Iron_Token *start = iron_current(p);
    iron_skip_newlines(p);

    Iron_Node **decls = NULL;
    int decl_count    = 0;

    while (!iron_check(p, IRON_TOK_EOF)) {
        iron_skip_newlines(p);
        if (iron_check(p, IRON_TOK_EOF)) break;

        /* Skip stray closing braces from incomplete declarations */
        if (iron_check(p, IRON_TOK_RBRACE)) {
            iron_advance(p);
            iron_skip_newlines(p);
            continue;
        }

        bool is_private = false;
        if (iron_check(p, IRON_TOK_PRIVATE)) {
            iron_advance(p);
            is_private = true;
        }

        Iron_Node *d = iron_parse_decl(p, is_private);
        arrput(decls, d);
        decl_count++;
        iron_skip_newlines(p);
    }

    Iron_Program *prog  = ARENA_ALLOC(p->arena, Iron_Program);
    prog->kind          = IRON_NODE_PROGRAM;
    prog->span          = iron_span_merge(iron_token_span(p, start),
                                           iron_token_span(p, iron_current(p)));
    prog->decls         = decls;
    prog->decl_count    = decl_count;
    return (Iron_Node *)prog;
}
