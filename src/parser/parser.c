#include "parser/parser.h"
#include "parser/ast.h"
#include "lexer/lexer.h"
#include "diagnostics/diagnostics.h"
#include "util/arena.h"
#include "stb_ds.h"

#include <errno.h>
#include <limits.h>
#include <string.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <stdbool.h>

/* ── Cancellation helper (HARD-05) ─────────────────────────────────────────── */
/* CONTEXT.md lock: NULL flag means never cancel; relaxed ordering ok.
 * Duplicated static inline across TUs — grep-friendly, no ODR concerns. */
static inline bool iron_cancel_requested(const _Atomic bool *flag) {
    return flag != NULL && atomic_load_explicit(flag, memory_order_relaxed);
}

/* ── PROT-05: recursion-depth guard (HARD-08) ────────────────────────────── */
/* Ceiling chosen per RESEARCH.md §Pitfall 5: worst-case ~3-5 frames per
 * syntactic level, 8 MB default stack with ~200-400 byte frames, safe
 * range 1000-5000. Grep of tests/integration `*.iron` confirms max observed
 * bracket/paren-depth well below 100, giving 10x headroom above any
 * legitimate input.
 *
 * On breach: iron_parser_depth_exceeded emits IRON_ERR_PARSE_DEPTH_EXCEEDED
 * (code 107, reserved by Plan 01), sets p->in_error_recovery = true, and
 * the caller returns an ErrorNode. NO SIGSEGV, NO abort — this is the
 * DoS mitigation T-01-04-02 in the plan's threat model. */
#define IRON_PARSER_MAX_DEPTH 1000

/* Forward decls for helpers used before their definitions. */
static Iron_Span   iron_token_span(Iron_Parser *p, Iron_Token *t);
static Iron_Token *iron_current(Iron_Parser *p);

/* HARD-08: check-and-emit helper. Returns true if the parser has reached
 * IRON_PARSER_MAX_DEPTH — the caller must then return an ErrorNode (or the
 * closest equivalent for its return type) without recursing further. The
 * single-site emit guarantees every depth-exceeded diagnostic carries the
 * same message and code, and suppresses cascade via in_error_recovery. */
static bool iron_parser_depth_exceeded(Iron_Parser *p) {
    if (p->recur_depth < IRON_PARSER_MAX_DEPTH) return false;
    /* Emit exactly once per runaway: after the first breach in_error_recovery
     * is already true, and iron_emit_diag (CLI mode) would suppress the
     * second emission anyway. In LSP mode (cascade-suppression disabled)
     * this may fire multiple times; that's acceptable — LSP clients dedupe
     * by (code, span). */
    iron_diag_emit(p->diags, p->arena, IRON_DIAG_ERROR,
                   IRON_ERR_PARSE_DEPTH_EXCEEDED,
                   iron_token_span(p, iron_current(p)),
                   "expression nesting too deep", NULL);
    p->in_error_recovery = true;
    return true;
}

/* FIX-03 / AUDIT-04 §1: SAFETY — this file contains 17 cross-arena storage
 * sites where stb_ds heap-managed arrays (built via `arrput`) are transferred
 * into arena-allocated AST nodes (assigned to `n->fields`, `n->variants`,
 * `n->params`, `n->args`, `n->stmts`, `n->cases`, `n->parts`, etc.). The
 * concern in the Phase 65 audit was: when the arena is freed, the stb_ds
 * backing buffers leak; if the stb_ds buffers are freed while the AST is
 * still live, the arena-allocated nodes hold dangling pointers.
 *
 * Invariant upheld by the parser lifecycle:
 *
 *   1. Every stb_ds array in this file is either (a) function-scoped and
 *      `arrfree`'d on every exit path (see arrfree sites at 223, 234, 833,
 *      860, 1804, 1813), or (b) ownership-transferred to an arena-allocated
 *      AST node (e.g., `n->fields = fields;`) whose lifetime is coupled to
 *      the compilation unit's parser arena (`p->arena`).
 *
 *   2. Callers NEVER call `arrfree` on the transferred stb_ds array after
 *      ownership transfer — by convention, ownership is irrevocable once
 *      assigned into an arena AST node.
 *
 *   3. The parser arena lives for the entire compilation unit (typecheck,
 *      HIR lower, LIR lower, emit — see src/cli/ironc.c for the teardown
 *      order). When `iron_arena_free(p->arena)` is called, the AST nodes
 *      are reclaimed but the stb_ds backing buffers leak to process exit.
 *      This is a deliberate, batch-compiler tradeoff: stb_ds leak per
 *      compilation unit is bounded by AST size (a few MB per translation
 *      unit worst-case) and process lifetime is short. A full fix would
 *      require migrating every such transferred array into arena storage,
 *      which is out of Phase 67 scope — see REQUIREMENTS.md "out of scope:
 *      rewriting arena allocator to a tracked/ref-counted model".
 *
 * The inline `FIX-03` markers below tag 5 representative transfer sites
 * (function params, function-call args, object fields, enum variants, block
 * statements) for grep discoverability. All 17 sites in this file follow the
 * same pattern; tagging every one would be noise. */

/* ── Precedence levels (Pratt) ────────────────────────────────────────────── */

typedef enum {
    PREC_NONE       = 0,
    PREC_ASSIGN     = 1,
    PREC_IS         = 2,
    PREC_OR         = 3,   /* logical ||                (unchanged)        */
    PREC_AND        = 4,   /* logical &&                (unchanged)        */
    PREC_BIT_OR     = 5,   /* bitwise |                 (Phase 59)         */
    PREC_BIT_XOR    = 6,   /* bitwise ^                 (Phase 59)         */
    PREC_BIT_AND    = 7,   /* bitwise &                 (Phase 59)         */
    PREC_EQUALITY   = 8,   /* == !=                     (renumbered from 5)*/
    PREC_COMPARISON = 9,   /* < > <= >=                 (renumbered from 6)*/
    PREC_SHIFT      = 10,  /* << >>                     (Phase 59)         */
    PREC_TERM       = 11,  /* + -                       (renumbered from 7)*/
    PREC_FACTOR     = 12,  /* * / %                     (renumbered from 8)*/
    PREC_UNARY      = 13,  /* unary - ! ~               (renumbered from 9)*/
    PREC_CALL       = 14   /* . [] ()                   (renumbered from 10)*/
} Precedence;

/* ── Forward declarations ────────────────────────────────────────────────── */

/* HARD-08: public recursive-descent entries are thin wrappers over their
 * `_impl` bodies. The wrapper performs the depth check + inc/dec pair so the
 * impl body can return from any path without plumbing a cleanup label. */
static Iron_Node *iron_parse_expr_prec_impl(Iron_Parser *p, int min_prec);
static Iron_Node *iron_parse_expr_prec(Iron_Parser *p, int min_prec);
static Iron_Node *iron_parse_expr(Iron_Parser *p);
static Iron_Node *iron_parse_stmt_impl(Iron_Parser *p);
static Iron_Node *iron_parse_stmt(Iron_Parser *p);
static Iron_Node *iron_parse_block_impl(Iron_Parser *p);
static Iron_Node *iron_parse_block(Iron_Parser *p);
static Iron_Node *iron_parse_type_annotation_impl(Iron_Parser *p);
static Iron_Node *iron_parse_type_annotation(Iron_Parser *p);
static Iron_Node *iron_parse_decl_impl(Iron_Parser *p, bool is_private, Iron_Node ***extra_decls_out);
static Iron_Node *iron_parse_decl(Iron_Parser *p, bool is_private, Iron_Node ***extra_decls_out);
static Iron_Node *iron_parse_func_or_method(Iron_Parser *p, bool is_private);
static Iron_Node *iron_parse_object_decl(Iron_Parser *p, bool is_private, Iron_Node ***extra_decls_out);
static Iron_Node *iron_parse_patch_decl(Iron_Parser *p, Iron_Node ***extra_decls_out);
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
    p.v3_strict_mode    = true;
    p.mode              = IRON_ANALYSIS_MODE_CLI; /* HARD-02: default preserves legacy behaviour */
    p.cancel_flag       = NULL;                   /* HARD-05: default = never cancel */
    p.recur_depth       = 0;                      /* HARD-08: recursion-depth guard baseline */
    return p;
}

/* HARD-02: LSP mode disables the in_error_recovery effect on diagnostic
 * emission (see iron_emit_diag below), so LSP clients see every error. */
void iron_parser_set_mode(Iron_Parser *p, IronAnalysisMode mode) {
    if (p) p->mode = mode;
}

/* HARD-05: attach caller-owned cancel flag; subsequent poll sites in
 * iron_parse and its helpers observe this flag at relaxed atomic ordering. */
void iron_parser_set_cancel_flag(Iron_Parser *p, const _Atomic bool *flag) {
    if (p) p->cancel_flag = flag;
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

/* True if the current token can appear as a function/method name. */
static bool iron_check_name(Iron_Parser *p) {
    Iron_TokenKind k = iron_peek(p);
    return k == IRON_TOK_IDENTIFIER;
}

/* Phase 85 INIT: `init` is a contextual keyword - reserved inside
 * object-block bodies and interface-block bodies, but MUST remain usable
 * as a regular identifier in parameter names and in classic
 * `func Type.init(...)` method-declaration / `obj.init(...)` call-site
 * positions so the pure-superset guard holds through Phase 87 (v2.2
 * stdlib and raylib bindings already use `init` as a parameter name in
 * reduce and as the method name for Window.init / Audio.init). This
 * helper accepts either IRON_TOK_IDENTIFIER or IRON_TOK_INIT in the
 * places that downstream code treats the token as a bare name; the
 * object-block body loop explicitly branches on IRON_TOK_INIT BEFORE
 * calling this helper so the init keyword still wins inside
 * `object X { ... }`. */
static bool iron_check_name_or_init(Iron_Parser *p) {
    Iron_TokenKind k = iron_peek(p);
    return k == IRON_TOK_IDENTIFIER || k == IRON_TOK_INIT;
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

/* Emit a diagnostic. In CLI mode we suppress cascading errors while in
 * error-recovery so the user sees a clean error list (HARD-11 parity). In
 * LSP mode (HARD-02) suppression is disabled: LSP clients dedupe. */
static void iron_emit_diag(Iron_Parser *p, int code, Iron_Span sp, const char *msg) {
    if (p->in_error_recovery && p->mode != IRON_ANALYSIS_MODE_LSP) {
        return;
    }
    iron_diag_emit(p->diags, p->arena, IRON_DIAG_ERROR, code, sp, msg, NULL);
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

/* HARD-09: shared static ErrorNode sentinel used when arena allocation fails
 * inside iron_make_error itself — we cannot recurse into iron_make_error on
 * its own OOM path, so a process-wide zero-span sentinel is returned instead.
 * Consumers only read kind + span; both are valid on the sentinel.
 * The sentinel is never freed (static storage duration, init via C runtime). */
static Iron_ErrorNode s_parser_oom_sentinel = {
    .span = { .filename = NULL, .line = 0, .col = 0, .end_line = 0, .end_col = 0 },
    .kind = IRON_NODE_ERROR,
};

/* Create an ErrorNode at the current position.
 * HARD-09 REPLACE: on arena OOM, return the static sentinel rather than
 * aborting. The caller sees a valid IRON_NODE_ERROR node; downstream
 * passes already tolerate IRON_NODE_ERROR via the ErrorNode-tolerance
 * added in Plan 02. */
static Iron_Node *iron_make_error(Iron_Parser *p) {
    Iron_ErrorNode *n = ARENA_ALLOC(p->arena, Iron_ErrorNode);
    if (!n) {
        /* HARD-09 REPLACE (audit: parser.c:187 row) — OOM fallback to
         * process-wide sentinel. No recursion, no abort. */
        p->in_error_recovery = true;
        return (Iron_Node *)&s_parser_oom_sentinel;
    }
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

/* Parse: TypeName[?][GenericArgs] or [TypeName; Size] or [TypeName] or func(T)->R
 *        or (T0, T1, ...) — Phase 59 01d tuple type. */
/* HARD-08: wrapper performs the recursion-depth guard (increment on entry,
 * decrement on every return path from _impl). Callers still invoke the
 * unsuffixed name (iron_parse_type_annotation); the _impl body is the
 * pre-HARD-08 body verbatim. */
static Iron_Node *iron_parse_type_annotation(Iron_Parser *p) {
    if (iron_parser_depth_exceeded(p)) {
        return iron_make_error(p);
    }
    p->recur_depth++;
    Iron_Node *r = iron_parse_type_annotation_impl(p);
    p->recur_depth--;
    return r;
}

static Iron_Node *iron_parse_type_annotation_impl(Iron_Parser *p) {
    /* HARD-05: cancel poll at function entry (cheap, 1ns when flag is NULL). */
    if (iron_cancel_requested(p->cancel_flag)) {
        return iron_make_error(p);
    }
    Iron_Token *start = iron_current(p);

    /* Phase 59 01d: Tuple type (T0, T1, ...) — arity >= 2 enforced. */
    if (iron_check(p, IRON_TOK_LPAREN)) {
        Iron_Span start_span = iron_token_span(p, iron_current(p));
        iron_advance(p);  /* consume ( */
        iron_skip_newlines(p);
        Iron_Node **elems = NULL;  /* stb_ds */
        while (!iron_check(p, IRON_TOK_RPAREN) && !iron_check(p, IRON_TOK_EOF)) {
            /* HARD-05: cancel poll at top of tuple-elements loop. */
            if (iron_cancel_requested(p->cancel_flag)) { arrfree(elems); return iron_make_error(p); }
            Iron_Node *elem_ty = iron_parse_type_annotation(p);
            arrput(elems, elem_ty);
            iron_skip_newlines(p);
            if (!iron_check(p, IRON_TOK_COMMA)) break;
            iron_advance(p);  /* consume , */
            iron_skip_newlines(p);
        }
        iron_expect(p, IRON_TOK_RPAREN);
        int count = (int)arrlen(elems);
        if (count < 2) {
            iron_diag_emit(p->diags, p->arena, IRON_DIAG_ERROR,
                           IRON_ERR_UNEXPECTED_TOKEN, start_span,
                           "tuple types must have arity >= 2; "
                           "use a plain type for single values", NULL);
            arrfree(elems);
            return iron_make_error(p);
        }
        /* Transfer the stb_ds element pointer array into the arena so the
         * AST owns it independently of the stb_ds lifetime. Mirrors the
         * func_params ownership transfer pattern below. */
        Iron_Node **arena_elems = (Iron_Node **)iron_arena_alloc(
            p->arena, sizeof(Iron_Node *) * (size_t)count,
            _Alignof(Iron_Node *));
        if (!arena_elems) { /* HARD-09 REPLACE (iron_parse_type_annotation tuple elems) */ p->in_error_recovery = true; return iron_make_error(p); }
        memcpy(arena_elems, elems, sizeof(Iron_Node *) * (size_t)count);
        arrfree(elems);

        Iron_TypeAnnotation *ann = ARENA_ALLOC(p->arena, Iron_TypeAnnotation);
        if (!ann) { /* HARD-09 REPLACE (iron_parse_type_annotation tuple) */ p->in_error_recovery = true; return iron_make_error(p); }
        memset(ann, 0, sizeof(*ann));
        ann->kind             = IRON_NODE_TYPE_ANNOTATION;
        ann->span             = iron_span_merge(start_span,
                                                iron_token_span(p, iron_current(p)));
        ann->is_tuple         = true;
        ann->tuple_elems      = arena_elems;
        ann->tuple_elem_count = count;
        return (Iron_Node *)ann;
    }

    /* Array type: [T] or [T; N] or [func(T)->R] */
    if (iron_match(p, IRON_TOK_LBRACKET)) {
        Iron_TypeAnnotation *ann = ARENA_ALLOC(p->arena, Iron_TypeAnnotation);
        if (!ann) { /* HARD-09 REPLACE (iron_parse_type_annotation array) */ p->in_error_recovery = true; return iron_make_error(p); }
        memset(ann, 0, sizeof(*ann));
        ann->kind              = IRON_NODE_TYPE_ANNOTATION;
        ann->is_array          = true;
        ann->is_nullable       = false;
        ann->generic_args      = NULL;
        ann->generic_arg_count = 0;
        ann->array_size        = NULL;

        /* Initialize func-type fields */
        ann->is_func           = false;
        ann->func_params       = NULL;
        ann->func_param_count  = 0;
        ann->func_return       = NULL;

        /* Phase 48: Initialize layout annotation fields */
        ann->layout_hint       = IRON_LAYOUT_HINT_NONE;
        ann->is_unordered      = false;

        /* element type: either func(...) -> R or a named type */
        if (iron_check(p, IRON_TOK_FUNC)) {
            /* Parse func type as array element: [func(T) -> R] */
            Iron_Node *elem = iron_parse_type_annotation(p);
            if (elem && elem->kind == IRON_NODE_TYPE_ANNOTATION) {
                Iron_TypeAnnotation *elem_ann = (Iron_TypeAnnotation *)elem;
                ann->name             = "func";
                ann->is_func          = elem_ann->is_func;
                ann->func_params      = elem_ann->func_params;
                ann->func_param_count = elem_ann->func_param_count;
                ann->func_return      = elem_ann->func_return;
            } else {
                ann->name = "<error>";
            }
        } else if (!iron_check(p, IRON_TOK_IDENTIFIER)) {
            iron_diag_emit(p->diags, p->arena, IRON_DIAG_ERROR,
                           IRON_ERR_UNEXPECTED_TOKEN,
                           iron_token_span(p, iron_current(p)),
                           "expected type name in array type", NULL);
            /* CRITICAL FIX: skip forward to ] to prevent infinite loop */
            while (!iron_check(p, IRON_TOK_RBRACKET) && !iron_check(p, IRON_TOK_EOF)) {
                iron_advance(p);
            }
            ann->name = "<error>";
        } else {
            Iron_Token *name_tok = iron_advance(p);
            ann->name = iron_arena_strdup(p->arena, name_tok->value,
                                          strlen(name_tok->value));
            if (!ann->name) { /* HARD-09 REPLACE (iron_parse_type_annotation array elem name) */ ann->name = "?"; }
        }

        /* Phase 48: Parse optional layout attributes: [T, layout: soa/aos] [T, unordered] */
        while (iron_match(p, IRON_TOK_COMMA)) {
            Iron_Token *attr = iron_current(p);
            if (iron_check(p, IRON_TOK_IDENTIFIER)) {
                if (strcmp(attr->value, "layout") == 0) {
                    iron_advance(p);  /* consume "layout" */
                    iron_expect(p, IRON_TOK_COLON);
                    Iron_Token *val = iron_current(p);
                    if (iron_check(p, IRON_TOK_IDENTIFIER)) {
                        if (strcmp(val->value, "soa") == 0) {
                            ann->layout_hint = IRON_LAYOUT_HINT_SOA;
                            iron_advance(p);
                        } else if (strcmp(val->value, "aos") == 0) {
                            ann->layout_hint = IRON_LAYOUT_HINT_AOS;
                            iron_advance(p);
                        } else {
                            iron_diag_emit(p->diags, p->arena, IRON_DIAG_ERROR,
                                IRON_ERR_UNEXPECTED_TOKEN,
                                iron_token_span(p, val),
                                "expected 'soa' or 'aos' after 'layout:'", NULL);
                            iron_advance(p);
                        }
                    }
                } else if (strcmp(attr->value, "unordered") == 0) {
                    ann->is_unordered = true;
                    iron_advance(p);
                } else {
                    iron_diag_emit(p->diags, p->arena, IRON_DIAG_ERROR,
                        IRON_ERR_UNEXPECTED_TOKEN,
                        iron_token_span(p, attr),
                        "expected 'layout' or 'unordered' in array type attribute", NULL);
                    iron_advance(p);
                }
            }
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

    /* Standalone func type annotation: func(T, U) -> R */
    if (iron_check(p, IRON_TOK_FUNC)) {
        iron_advance(p);  /* consume 'func' */

        Iron_TypeAnnotation *ann = ARENA_ALLOC(p->arena, Iron_TypeAnnotation);
        if (!ann) { /* HARD-09 REPLACE (iron_parse_type_annotation func) */ p->in_error_recovery = true; return iron_make_error(p); }
        memset(ann, 0, sizeof(*ann));
        ann->kind              = IRON_NODE_TYPE_ANNOTATION;
        ann->is_array          = false;
        ann->array_size        = NULL;
        ann->is_nullable       = false;
        ann->generic_args      = NULL;
        ann->generic_arg_count = 0;
        ann->is_func           = true;
        ann->name              = "func";
        ann->layout_hint       = IRON_LAYOUT_HINT_NONE;
        ann->is_unordered      = false;

        /* Parse parameter types: ( [TypeAnn, ...] ) */
        ann->func_params      = NULL;
        ann->func_param_count = 0;
        if (iron_match(p, IRON_TOK_LPAREN)) {
            Iron_Node **params = NULL;
            int param_count = 0;
            while (!iron_check(p, IRON_TOK_RPAREN) && !iron_check(p, IRON_TOK_EOF)) {
                if (param_count > 0) {
                    iron_expect(p, IRON_TOK_COMMA);
                }
                Iron_Node *param_type = iron_parse_type_annotation(p);
                arrput(params, param_type);
                param_count++;
            }
            iron_expect(p, IRON_TOK_RPAREN);
            /* FIX-03 / AUDIT-04 §1: SAFETY — stb_ds `params` array ownership
             * transfers to the arena-allocated TypeAnnotation node. Parser
             * arena lifetime governs both (file-header comment). */
            ann->func_params      = params;
            ann->func_param_count = param_count;
        }

        /* Parse optional return type: -> R */
        ann->func_return = NULL;
        if (iron_match(p, IRON_TOK_ARROW)) {
            ann->func_return = iron_parse_type_annotation(p);
        }

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
    if (!ann) { /* HARD-09 REPLACE (iron_parse_type_annotation named) */ p->in_error_recovery = true; return iron_make_error(p); }
    memset(ann, 0, sizeof(*ann));
    ann->kind              = IRON_NODE_TYPE_ANNOTATION;
    ann->is_array          = false;
    ann->array_size        = NULL;
    ann->name = iron_arena_strdup(p->arena, name_tok->value,
                                  strlen(name_tok->value));
    if (!ann->name) { /* HARD-09 REPLACE (iron_parse_type_annotation named name) */ ann->name = "?"; }
    /* Phase 87-02 SELF-01/02: mark "Self" as the contextual Self type.
     * "Self" lexes as IRON_TOK_IDENTIFIER (not a keyword) so we detect it
     * by string comparison here. The typechecker resolves is_self_type to
     * the enclosing ObjectDecl type or emits E0259. */
    ann->is_self_type = (strcmp(ann->name, "Self") == 0);

    /* nullable? */
    ann->is_nullable = iron_match(p, IRON_TOK_QUESTION);

    /* generic args: Type[T, U] where T, U are full type annotations (supports nested generics) */
    if (iron_check(p, IRON_TOK_LBRACKET)) {
        iron_advance(p);  /* consume '[' */
        iron_skip_newlines(p);
        ann->generic_args = NULL;
        ann->generic_arg_count = 0;
        while (!iron_check(p, IRON_TOK_RBRACKET) && !iron_check(p, IRON_TOK_EOF)) {
            Iron_Node *arg = iron_parse_type_annotation(p);
            arrput(ann->generic_args, arg);
            ann->generic_arg_count++;
            iron_skip_newlines(p);
            if (!iron_match(p, IRON_TOK_COMMA)) break;
            iron_skip_newlines(p);
        }
        iron_expect(p, IRON_TOK_RBRACKET);
    } else {
        ann->generic_args      = NULL;
        ann->generic_arg_count = 0;
    }

    /* Initialize func-type fields for named type */
    ann->is_func           = false;
    ann->func_params       = NULL;
    ann->func_param_count  = 0;
    ann->func_return       = NULL;

    /* Phase 48: no layout annotations on non-array types */
    ann->layout_hint       = IRON_LAYOUT_HINT_NONE;
    ann->is_unordered      = false;

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
            if (!id) { /* HARD-09 REPLACE (audit: parser.c:509) */
                p->in_error_recovery = true;
                break;
            }
            id->span       = iron_token_span(p, t);
            id->kind       = IRON_NODE_IDENT;
            id->name       = iron_arena_strdup(p->arena, t->value, strlen(t->value));
            if (!id->name) { /* HARD-09 REPLACE (audit: parser.c:513) — strdup fallback to "?" */
                id->name = "?";
            }
            id->constraint_name = NULL;
            if (iron_match(p, IRON_TOK_COLON)) {
                if (iron_check(p, IRON_TOK_IDENTIFIER)) {
                    Iron_Token *ct = iron_advance(p);
                    id->constraint_name = iron_arena_strdup(p->arena, ct->value, strlen(ct->value));
                    if (!id->constraint_name) { /* HARD-09 REPLACE (audit: parser.c:519) — drop constraint */
                        id->constraint_name = NULL;
                    }
                }
            }
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

        /* Phase 88 BREAK-04: reject 'mut' keyword in param position when strict-v3 gate is ON.
         * Consume 'mut' for recovery so the rest of the param list keeps parsing cleanly. */
        if (p->v3_strict_mode && iron_check(p, IRON_TOK_MUT)) {
            iron_diag_emit(p->diags, p->arena, IRON_DIAG_ERROR,
                           IRON_ERR_V3_MUT_KEYWORD,
                           iron_token_span(p, iron_current(p)),
                           "'mut' keyword removed in v3.0; use 'var' for mutable bindings "
                           "or declare a default in-object method to mutate self",
                           "run 'ironc migrate --from v2 --to v3 <file>' to migrate");
            iron_advance(p);  /* consume 'mut' for recovery */
        }

        /* Phase 85 INIT: accept IRON_TOK_INIT as a parameter name so stdlib
         * `reduce(init: U, ...)` lexes under pure-superset. */
        if (!iron_check_name_or_init(p)) {
            iron_emit_diag(p, IRON_ERR_UNEXPECTED_TOKEN,
                           iron_token_span(p, iron_current(p)),
                           "expected parameter name");
            p->in_error_recovery = true;
            iron_parser_sync_stmt(p);
            break;
        }

        Iron_Token *name_tok = iron_advance(p);
        Iron_Param *param    = ARENA_ALLOC(p->arena, Iron_Param);
        if (!param) { /* HARD-09 REPLACE (audit: parser.c:567) */
            p->in_error_recovery = true;
            break;
        }
        param->kind            = IRON_NODE_PARAM;
        param->span            = iron_token_span(p, name_tok);
        param->is_var          = is_var;
        param->is_mut_receiver = false;  /* Phase 79: regular params never get mut */
        param->name = iron_arena_strdup(p->arena, name_tok->value,
                                        strlen(name_tok->value));
        if (!param->name) { /* HARD-09 REPLACE (audit: parser.c:573) — strdup fallback */
            param->name = "?";
        }

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

/* HARD-08: wrapper — see iron_parse_type_annotation for the pattern. */
static Iron_Node *iron_parse_block(Iron_Parser *p) {
    if (iron_parser_depth_exceeded(p)) {
        return iron_make_error(p);
    }
    p->recur_depth++;
    Iron_Node *r = iron_parse_block_impl(p);
    p->recur_depth--;
    return r;
}

static Iron_Node *iron_parse_block_impl(Iron_Parser *p) {
    /* HARD-05: cancel poll at block entry. */
    if (iron_cancel_requested(p->cancel_flag)) {
        return iron_make_error(p);
    }
    Iron_Token *start = iron_current(p);
    if (!iron_expect(p, IRON_TOK_LBRACE)) {
        p->in_error_recovery = true;
        return iron_make_error(p);
    }

    iron_skip_newlines(p);

    Iron_Node **stmts = NULL;
    int stmt_count = 0;

    while (!iron_check(p, IRON_TOK_RBRACE) && !iron_check(p, IRON_TOK_EOF)) {
        /* HARD-05: cancel poll inside the no-progress-guarded block loop
         * (parser.c:605-627 per PATTERNS.md — MANDATORY site). */
        if (iron_cancel_requested(p->cancel_flag)) {
            iron_diag_emit(p->diags, p->arena, IRON_DIAG_NOTE,
                           IRON_ERR_CANCELLED,
                           iron_token_span(p, iron_current(p)),
                           "compilation cancelled", NULL);
            break;
        }
        int pos_before = p->pos;
        iron_skip_newlines(p);
        if (iron_check(p, IRON_TOK_RBRACE)) break;
        Iron_Node *s = iron_parse_stmt(p);
        arrput(stmts, s);
        stmt_count++;
        iron_skip_newlines(p);
        /* No-progress guard: if iron_parse_stmt failed to advance the cursor,
         * emit one generic diagnostic, skip one token, and continue. This
         * prevents the 01c-class hang observed on tuple_return_smoke.iron where
         * a malformed `val (` produced an ErrorNode without consuming the `(`
         * and the outer loop re-entered iron_parse_stmt forever. */
        if (p->pos == pos_before) {
            iron_diag_emit(p->diags, p->arena, IRON_DIAG_ERROR,
                           IRON_ERR_UNEXPECTED_TOKEN,
                           iron_token_span(p, iron_current(p)),
                           "unexpected token in block; skipping", NULL);
            if (iron_peek(p) != IRON_TOK_EOF) iron_advance(p);
        }
    }

    Iron_Token *end = iron_current(p);
    if (!iron_expect(p, IRON_TOK_RBRACE)) {
        /* incomplete block, return what we have */
    }

    Iron_Block *blk  = ARENA_ALLOC(p->arena, Iron_Block);
    if (!blk) { /* HARD-09 REPLACE (iron_parse_block) */ p->in_error_recovery = true; return iron_make_error(p); }
    blk->kind        = IRON_NODE_BLOCK;
    blk->span        = iron_span_merge(iron_token_span(p, start),
                                       iron_token_span(p, end));
    /* FIX-03 / AUDIT-04 §1: SAFETY — stb_ds `stmts` array ownership-
     * transferred to arena-allocated Block; file-header comment. */
    blk->stmts       = stmts;
    blk->stmt_count  = stmt_count;
    return (Iron_Node *)blk;
}

/* ── Call argument list: (expr, ...) ─────────────────────────────────────── */

/* FIX-03 / AUDIT-04 §1: SAFETY — the stb_ds `arr` built here is returned to
 * the caller, which in every case assigns it into an arena-allocated call-
 * expression node (e.g., `call->args = arr;`). Ownership transfers to the
 * arena AST node; stb_ds backing buffer lives for the compilation unit. */
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
    if (!lam) { /* HARD-09 REPLACE (iron_parse_lambda) */ p->in_error_recovery = true; return iron_make_error(p); }
    lam->kind            = IRON_NODE_LAMBDA;
    lam->span            = iron_span_merge(iron_token_span(p, start), body->span);
    /* FIX-03 / AUDIT-04 §1: SAFETY — stb_ds `params` array transferred to
     * arena-allocated LambdaExpr; see file-header comment. */
    lam->params          = params;
    lam->param_count     = param_count;
    lam->return_type     = ret;
    lam->body            = body;
    return (Iron_Node *)lam;
}

/* ── Expression: Pratt parser ────────────────────────────────────────────── */

/* Return the infix precedence of the current token, or PREC_NONE */
static int iron_infix_prec(Iron_TokenKind k) {
    /* -Wswitch-enum opt-out: Iron_TokenKind has ~80 variants; only the
     * infix-operator tokens carry precedence. Cast to int so the switch is
     * not an enum switch and the default arm is accepted by the strictness
     * posture added in Phase 66 Plan 02. */
    switch ((int)k) {
        case IRON_TOK_OR:         return PREC_OR;
        case IRON_TOK_AND:        return PREC_AND;
        case IRON_TOK_PIPE:       return PREC_BIT_OR;
        case IRON_TOK_CARET:      return PREC_BIT_XOR;
        case IRON_TOK_AMP:        return PREC_BIT_AND;
        case IRON_TOK_EQUALS:
        case IRON_TOK_NOT_EQUALS: return PREC_EQUALITY;
        case IRON_TOK_LESS:
        case IRON_TOK_GREATER:
        case IRON_TOK_LESS_EQ:
        case IRON_TOK_GREATER_EQ: return PREC_COMPARISON;
        case IRON_TOK_SHL:
        case IRON_TOK_SHR:        return PREC_SHIFT;
        case IRON_TOK_PLUS:
        case IRON_TOK_MINUS:      return PREC_TERM;
        case IRON_TOK_STAR:
        case IRON_TOK_SLASH:
        case IRON_TOK_PERCENT:    return PREC_FACTOR;
        case IRON_TOK_DOT:        return PREC_CALL;
        case IRON_TOK_LBRACKET:   return PREC_CALL;
        case IRON_TOK_LPAREN:     return PREC_CALL;
        case IRON_TOK_IS:         return PREC_IS;
        /* -Wswitch-enum opt-out: Iron_TokenKind has ~80 variants; only the
         * infix-operator tokens carry precedence. All other tokens (literals,
         * keywords, punctuation) intentionally return PREC_NONE so the Pratt
         * parser stops climbing. */
        default:                  return PREC_NONE;
    }
}

/* Parse a primary (prefix) expression */
static Iron_Node *iron_parse_primary(Iron_Parser *p) {
    iron_skip_newlines(p);
    Iron_Token *t = iron_current(p);

    switch ((int)t->kind) {
        /* Integer literal */
        case IRON_TOK_INTEGER: {
            iron_advance(p);
            Iron_IntLit *n = ARENA_ALLOC(p->arena, Iron_IntLit);
            if (!n) { /* HARD-09 REPLACE (iron_parse_primary IntLit) */ p->in_error_recovery = true; return iron_make_error(p); }
            n->kind  = IRON_NODE_INT_LIT;
            n->span  = iron_token_span(p, t);
            n->value = iron_arena_strdup(p->arena, t->value, strlen(t->value));
            if (!n->value) { /* HARD-09 REPLACE (iron_parse_primary IntLit value) */ n->value = "0"; }
            return (Iron_Node *)n;
        }
        /* Float literal */
        case IRON_TOK_FLOAT: {
            iron_advance(p);
            Iron_FloatLit *n = ARENA_ALLOC(p->arena, Iron_FloatLit);
            if (!n) { /* HARD-09 REPLACE (iron_parse_primary FloatLit) */ p->in_error_recovery = true; return iron_make_error(p); }
            n->kind  = IRON_NODE_FLOAT_LIT;
            n->span  = iron_token_span(p, t);
            n->value = iron_arena_strdup(p->arena, t->value, strlen(t->value));
            if (!n->value) { /* HARD-09 REPLACE (iron_parse_primary FloatLit value) */ n->value = "0.0"; }
            return (Iron_Node *)n;
        }
        /* String literal */
        case IRON_TOK_STRING: {
            iron_advance(p);
            Iron_StringLit *n = ARENA_ALLOC(p->arena, Iron_StringLit);
            if (!n) { /* HARD-09 REPLACE (iron_parse_primary StringLit) */ p->in_error_recovery = true; return iron_make_error(p); }
            n->kind  = IRON_NODE_STRING_LIT;
            n->span  = iron_token_span(p, t);
            n->value = iron_arena_strdup(p->arena, t->value, strlen(t->value));
            if (!n->value) { /* HARD-09 REPLACE (iron_parse_primary StringLit value) */ n->value = ""; }
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
            if (!n) { /* HARD-09 REPLACE (iron_parse_primary BoolLit true) */ p->in_error_recovery = true; return iron_make_error(p); }
            n->kind  = IRON_NODE_BOOL_LIT;
            n->span  = iron_token_span(p, t);
            n->value = true;
            return (Iron_Node *)n;
        }
        /* false */
        case IRON_TOK_FALSE: {
            iron_advance(p);
            Iron_BoolLit *n = ARENA_ALLOC(p->arena, Iron_BoolLit);
            if (!n) { /* HARD-09 REPLACE (iron_parse_primary BoolLit false) */ p->in_error_recovery = true; return iron_make_error(p); }
            n->kind  = IRON_NODE_BOOL_LIT;
            n->span  = iron_token_span(p, t);
            n->value = false;
            return (Iron_Node *)n;
        }
        /* null */
        case IRON_TOK_NULL_KW: {
            iron_advance(p);
            Iron_NullLit *n = ARENA_ALLOC(p->arena, Iron_NullLit);
            if (!n) { /* HARD-09 REPLACE (iron_parse_primary NullLit) */ p->in_error_recovery = true; return iron_make_error(p); }
            n->kind = IRON_NODE_NULL_LIT;
            n->span = iron_token_span(p, t);
            return (Iron_Node *)n;
        }
        /* Unary minus */
        case IRON_TOK_MINUS: {
            iron_advance(p);
            Iron_Node *operand = iron_parse_expr_prec(p, PREC_UNARY);
            Iron_UnaryExpr *n  = ARENA_ALLOC(p->arena, Iron_UnaryExpr);
            if (!n) { /* HARD-09 REPLACE (iron_parse_primary UnaryExpr minus) */ p->in_error_recovery = true; return iron_make_error(p); }
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
            if (!n) { /* HARD-09 REPLACE (iron_parse_primary UnaryExpr not) */ p->in_error_recovery = true; return iron_make_error(p); }
            n->kind            = IRON_NODE_UNARY;
            n->span            = iron_span_merge(iron_token_span(p, t), operand->span);
            n->op              = (Iron_OpKind)IRON_TOK_NOT;
            n->operand         = operand;
            return (Iron_Node *)n;
        }
        /* Unary bitwise NOT (Phase 59) */
        case IRON_TOK_TILDE: {
            iron_advance(p);
            Iron_Node *operand = iron_parse_expr_prec(p, PREC_UNARY);
            Iron_UnaryExpr *n  = ARENA_ALLOC(p->arena, Iron_UnaryExpr);
            if (!n) { /* HARD-09 REPLACE (iron_parse_primary UnaryExpr tilde) */ p->in_error_recovery = true; return iron_make_error(p); }
            n->kind            = IRON_NODE_UNARY;
            n->span            = iron_span_merge(iron_token_span(p, t), operand->span);
            n->op              = (Iron_OpKind)IRON_TOK_TILDE;
            n->operand         = operand;
            return (Iron_Node *)n;
        }
        /* Grouped expression or tuple literal (Phase 59 01d) */
        case IRON_TOK_LPAREN: {
            Iron_Span lparen_span = iron_token_span(p, iron_current(p));
            iron_advance(p);
            iron_skip_newlines(p);
            Iron_Node *first = iron_parse_expr(p);
            iron_skip_newlines(p);
            if (iron_check(p, IRON_TOK_COMMA)) {
                /* Tuple literal: (e0, e1, ...) — arity >= 2 enforced. The
                 * parser lands this as an Iron_ArrayLit with a dedicated
                 * element type annotation flag (no new node kind) — the
                 * typechecker discriminates via resolved_type->kind ==
                 * IRON_TYPE_TUPLE. Research Decision 1 Option B. */
                Iron_Node **elems = NULL;
                arrput(elems, first);
                while (iron_check(p, IRON_TOK_COMMA)) {
                    iron_advance(p);  /* consume , */
                    iron_skip_newlines(p);
                    if (iron_check(p, IRON_TOK_RPAREN)) break;  /* trailing comma */
                    Iron_Node *e = iron_parse_expr(p);
                    arrput(elems, e);
                    iron_skip_newlines(p);
                }
                iron_expect(p, IRON_TOK_RPAREN);
                int count = (int)arrlen(elems);
                if (count < 2) {
                    iron_diag_emit(p->diags, p->arena, IRON_DIAG_ERROR,
                                   IRON_ERR_UNEXPECTED_TOKEN, lparen_span,
                                   "tuple literals must have arity >= 2", NULL);
                    arrfree(elems);
                    return iron_make_error(p);
                }
                /* Reuse Iron_ArrayLit as the storage for the tuple literal —
                 * the type checker detects tuple-ness via context + elem_types.
                 * The alternative would be a dedicated IRON_NODE_TUPLE_LIT kind
                 * but that would require touching every AST walker. Mirror the
                 * existing ArrayLit element-list storage pattern. The tuple flag
                 * is indicated by is_tuple on the attached type annotation (set
                 * by the type checker during expected-type propagation) OR by
                 * resolved_type->kind == IRON_TYPE_TUPLE.
                 *
                 * To disambiguate from a regular array literal during lowering,
                 * we set a sentinel: type_ann points at an Iron_TypeAnnotation
                 * with is_tuple=true. Downstream consumers check this. */
                Iron_ArrayLit *al = ARENA_ALLOC(p->arena, Iron_ArrayLit);
                if (!al) { /* HARD-09 REPLACE (iron_parse_primary tuple ArrayLit) */ p->in_error_recovery = true; return iron_make_error(p); }
                memset(al, 0, sizeof(*al));
                al->kind          = IRON_NODE_ARRAY_LIT;
                al->span          = iron_span_merge(
                    lparen_span, iron_token_span(p, iron_current(p)));
                /* Transfer element ownership into arena. */
                Iron_Node **arena_elems = (Iron_Node **)iron_arena_alloc(
                    p->arena, sizeof(Iron_Node *) * (size_t)count,
                    _Alignof(Iron_Node *));
                if (!arena_elems) { /* HARD-09 REPLACE (iron_parse_primary tuple elems) */ p->in_error_recovery = true; return iron_make_error(p); }
                memcpy(arena_elems, elems, sizeof(Iron_Node *) * (size_t)count);
                arrfree(elems);
                al->elements      = arena_elems;
                al->element_count = count;
                /* Tuple sentinel: attach an Iron_TypeAnnotation with
                 * is_tuple=true. The type checker reads this to know to
                 * treat the array lit as a tuple. */
                Iron_TypeAnnotation *tag = ARENA_ALLOC(p->arena, Iron_TypeAnnotation);
                if (!tag) { /* HARD-09 REPLACE (iron_parse_primary tuple tag) */ p->in_error_recovery = true; return iron_make_error(p); }
                memset(tag, 0, sizeof(*tag));
                tag->kind             = IRON_NODE_TYPE_ANNOTATION;
                tag->span             = al->span;
                tag->is_tuple         = true;
                tag->tuple_elems      = NULL;  /* elements live on al->elements */
                tag->tuple_elem_count = count;
                al->type_ann          = (Iron_Node *)tag;
                return (Iron_Node *)al;
            }
            /* Not a tuple — plain parenthesised expression. */
            iron_expect(p, IRON_TOK_RPAREN);
            return first;
        }
        /* heap expr */
        case IRON_TOK_HEAP: {
            iron_advance(p);
            /* Use PREC_UNARY (9) so PREC_CALL infix operators (., [], ()) are
             * captured as part of the inner expression (e.g. heap Enemy(args)) */
            Iron_Node *inner   = iron_parse_expr_prec(p, PREC_UNARY);
            Iron_HeapExpr *n   = ARENA_ALLOC(p->arena, Iron_HeapExpr);
            if (!n) { /* HARD-09 REPLACE (iron_parse_primary HeapExpr) */ p->in_error_recovery = true; return iron_make_error(p); }
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
            if (!n) { /* HARD-09 REPLACE (iron_parse_primary RcExpr) */ p->in_error_recovery = true; return iron_make_error(p); }
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
            if (!n) { /* HARD-09 REPLACE (iron_parse_primary ComptimeExpr) */ p->in_error_recovery = true; return iron_make_error(p); }
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
            if (!n) { /* HARD-09 REPLACE (iron_parse_primary AwaitExpr) */ p->in_error_recovery = true; return iron_make_error(p); }
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
            if (!arr) { /* HARD-09 REPLACE (iron_parse_primary ArrayLit bracket) */ p->in_error_recovery = true; return iron_make_error(p); }
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
            if (!id) { /* HARD-09 REPLACE (iron_parse_primary Ident) */ p->in_error_recovery = true; return iron_make_error(p); }
            id->kind            = IRON_NODE_IDENT;
            id->span            = iron_token_span(p, t);
            id->name            = iron_arena_strdup(p->arena, t->value, strlen(t->value));
            if (!id->name) { /* HARD-09 REPLACE (iron_parse_primary Ident name) */ id->name = "?"; }
            id->resolved_sym    = NULL;
            id->resolved_type   = NULL;
            id->constraint_name = NULL;
            return (Iron_Node *)id;
        }
        /* self and super are expression keywords that resolve to idents */
        case IRON_TOK_SELF: {
            iron_advance(p);
            Iron_Ident *id = ARENA_ALLOC(p->arena, Iron_Ident);
            if (!id) { /* HARD-09 REPLACE (iron_parse_primary Ident self) */ p->in_error_recovery = true; return iron_make_error(p); }
            id->kind            = IRON_NODE_IDENT;
            id->span            = iron_token_span(p, t);
            id->name            = "self";
            id->resolved_sym    = NULL;
            id->resolved_type   = NULL;
            id->constraint_name = NULL;
            return (Iron_Node *)id;
        }
        case IRON_TOK_SUPER: {
            iron_advance(p);
            Iron_Ident *id = ARENA_ALLOC(p->arena, Iron_Ident);
            if (!id) { /* HARD-09 REPLACE (iron_parse_primary Ident super) */ p->in_error_recovery = true; return iron_make_error(p); }
            id->kind            = IRON_NODE_IDENT;
            id->span            = iron_token_span(p, t);
            id->name            = "super";
            id->resolved_sym    = NULL;
            id->resolved_type   = NULL;
            id->constraint_name = NULL;
            return (Iron_Node *)id;
        }
        /* -Wswitch-enum opt-out: primary-expression switch only handles the
         * token kinds that can begin an expression; every other Iron_TokenKind
         * (punctuation, closers, keywords that are not prefix-position) falls
         * through to the "expected expression" diagnostic below. */
        default:
            break;
    }

    /* Unexpected token in expression position */
    iron_emit_diag(p, IRON_ERR_EXPECTED_EXPR,
                   iron_token_span(p, t),
                   "expected expression");
    /* 2026-04-10 Phase 59 01d: advance past the offending token so callers
     * don't spin. Previous comment "let the caller handle recovery" was wrong —
     * no caller recovers, and the 01c hang on tuple_return_smoke.iron traced
     * back to this exact no-advance path. */
    if (iron_peek(p) != IRON_TOK_EOF) iron_advance(p);
    return iron_make_error(p);
}

/* HARD-08: wrapper — see iron_parse_type_annotation for the pattern. */
static Iron_Node *iron_parse_expr_prec(Iron_Parser *p, int min_prec) {
    if (iron_parser_depth_exceeded(p)) {
        return iron_make_error(p);
    }
    p->recur_depth++;
    Iron_Node *r = iron_parse_expr_prec_impl(p, min_prec);
    p->recur_depth--;
    return r;
}

/* Main Pratt expression parser */
static Iron_Node *iron_parse_expr_prec_impl(Iron_Parser *p, int min_prec) {
    /* HARD-05: cancel poll at expression-parser entry. */
    if (iron_cancel_requested(p->cancel_flag)) {
        return iron_make_error(p);
    }
    Iron_Node *left = iron_parse_primary(p);

    for (;;) {
        /* HARD-05: cancel poll at top of Pratt climb loop. */
        if (iron_cancel_requested(p->cancel_flag)) {
            return left; /* propagate partial result */
        }
        iron_skip_newlines(p);
        Iron_TokenKind cur = iron_peek(p);
        int prec = iron_infix_prec(cur);
        if (prec <= min_prec && cur != IRON_TOK_IS) break;
        if (cur == IRON_TOK_IS && PREC_IS <= min_prec) break;

        /* Field access / method call: expr.name or expr.name(args) */
        if (cur == IRON_TOK_DOT) {
            iron_advance(p);
            /* Phase 85 INIT: accept IRON_TOK_INIT as a method name at call
             * sites so `Window.init(...)` and `Audio.init()` continue to
             * parse under pure-superset. The keyword is only hard inside
             * object-block / interface-block bodies; here it denotes the
             * method named "init" declared via `func Type.init(...)`. */
            if (!iron_check_name_or_init(p)) {
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
            if (!name) { /* HARD-09 REPLACE (iron_parse_expr_prec dot name) */ name = "?"; }

            if (iron_check(p, IRON_TOK_LPAREN)) {
                /* Heuristic: if the LHS is a simple identifier starting with
                 * an uppercase letter, treat as enum construction:
                 *   EnumType.Variant(args) -> IRON_NODE_ENUM_CONSTRUCT
                 * Otherwise treat as method call:
                 *   variable.method(args) -> IRON_NODE_METHOD_CALL
                 * The resolver in Phase 33 will reclassify if needed. */
                if (left->kind == IRON_NODE_IDENT) {
                    Iron_Ident *ident = (Iron_Ident *)left;
                    bool looks_like_type    = (ident->name[0] >= 'A' &&
                                               ident->name[0] <= 'Z');
                    bool looks_like_variant = (name[0] >= 'A' && name[0] <= 'Z');
                    if (looks_like_type && looks_like_variant) {
                        int arg_count = 0;
                        Iron_Node **args = iron_parse_call_args(p, &arg_count);
                        Iron_EnumConstruct *ec = ARENA_ALLOC(p->arena, Iron_EnumConstruct);
                        if (!ec) { /* HARD-09 REPLACE (iron_parse_expr_prec EnumConstruct call) */ p->in_error_recovery = true; return iron_make_error(p); }
                        ec->kind          = IRON_NODE_ENUM_CONSTRUCT;
                        ec->span          = iron_span_merge(left->span,
                                                iron_token_span(p, iron_current(p)));
                        ec->resolved_type = NULL;
                        ec->enum_name     = ident->name;
                        ec->variant_name  = name;
                        ec->args          = args;
                        ec->arg_count     = arg_count;
                        left = (Iron_Node *)ec;
                    } else {
                        int arg_count = 0;
                        Iron_Node **args = iron_parse_call_args(p, &arg_count);
                        Iron_MethodCallExpr *mc = ARENA_ALLOC(p->arena, Iron_MethodCallExpr);
                        if (!mc) { /* HARD-09 REPLACE (iron_parse_expr_prec MethodCall ident) */ p->in_error_recovery = true; return iron_make_error(p); }
                        mc->kind      = IRON_NODE_METHOD_CALL;
                        mc->span      = iron_span_merge(left->span,
                                                        iron_token_span(p, iron_current(p)));
                        mc->resolved_type = NULL;
                        mc->object    = left;
                        mc->method    = name;
                        mc->args      = args;
                        mc->arg_count = arg_count;
                        left = (Iron_Node *)mc;
                    }
                } else {
                    /* Non-ident LHS: always a method call */
                    int arg_count = 0;
                    Iron_Node **args = iron_parse_call_args(p, &arg_count);
                    Iron_MethodCallExpr *mc = ARENA_ALLOC(p->arena, Iron_MethodCallExpr);
                    if (!mc) { /* HARD-09 REPLACE (iron_parse_expr_prec MethodCall nonident) */ p->in_error_recovery = true; return iron_make_error(p); }
                    mc->kind      = IRON_NODE_METHOD_CALL;
                    mc->span      = iron_span_merge(left->span,
                                                    iron_token_span(p, iron_current(p)));
                    mc->resolved_type = NULL;
                    mc->object    = left;
                    mc->method    = name;
                    mc->args      = args;
                    mc->arg_count = arg_count;
                    left = (Iron_Node *)mc;
                }
            } else {
                /* Check for unit enum variant: UppercaseType.UppercaseVariant (no parens) */
                if (left->kind == IRON_NODE_IDENT) {
                    Iron_Ident *ident_node = (Iron_Ident *)left;
                    bool looks_like_type    = (ident_node->name[0] >= 'A' && ident_node->name[0] <= 'Z');
                    bool looks_like_variant = (name[0] >= 'A' && name[0] <= 'Z');
                    if (looks_like_type && looks_like_variant) {
                        Iron_EnumConstruct *ec = ARENA_ALLOC(p->arena, Iron_EnumConstruct);
                        if (!ec) { /* HARD-09 REPLACE (iron_parse_expr_prec EnumConstruct unit) */ p->in_error_recovery = true; return iron_make_error(p); }
                        ec->kind          = IRON_NODE_ENUM_CONSTRUCT;
                        ec->span          = iron_span_merge(left->span,
                                                            iron_token_span(p, iron_current(p)));
                        ec->resolved_type = NULL;
                        ec->enum_name     = ident_node->name;
                        ec->variant_name  = name;
                        ec->args          = NULL;
                        ec->arg_count     = 0;
                        left = (Iron_Node *)ec;
                        continue;
                    }
                }
                /* Field access: obj.field */
                Iron_FieldAccess *fa = ARENA_ALLOC(p->arena, Iron_FieldAccess);
                if (!fa) { /* HARD-09 REPLACE (iron_parse_expr_prec FieldAccess) */ p->in_error_recovery = true; return iron_make_error(p); }
                fa->kind          = IRON_NODE_FIELD_ACCESS;
                fa->span          = iron_span_merge(left->span,
                                             iron_token_span(p, iron_current(p)));
                fa->object        = left;
                fa->field         = name;
                /* Phase 83-02: defensive default; typecheck flips it for
                 * pub-field reads so HIR routes through synthesized getter. */
                fa->is_pub_access = false;
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
                if (!sl) { /* HARD-09 REPLACE (iron_parse_expr_prec SliceExpr) */ p->in_error_recovery = true; return iron_make_error(p); }
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
                if (!ix) { /* HARD-09 REPLACE (iron_parse_expr_prec IndexExpr) */ p->in_error_recovery = true; return iron_make_error(p); }
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
            if (!call) { /* HARD-09 REPLACE (iron_parse_expr_prec CallExpr) */ p->in_error_recovery = true; return iron_make_error(p); }
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
            if (!type_name) { /* HARD-09 REPLACE (iron_parse_expr_prec IsExpr type_name) */ type_name = "?"; }
            Iron_IsExpr *is_n = ARENA_ALLOC(p->arena, Iron_IsExpr);
            if (!is_n) { /* HARD-09 REPLACE (iron_parse_expr_prec IsExpr) */ p->in_error_recovery = true; return iron_make_error(p); }
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
        if (!bin) { /* HARD-09 REPLACE (iron_parse_expr_prec BinaryExpr) */ p->in_error_recovery = true; return iron_make_error(p); }
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
    if (!n) { /* HARD-09 REPLACE (iron_parse_if_stmt) */ p->in_error_recovery = true; return iron_make_error(p); }
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
    if (!n) { /* HARD-09 REPLACE (iron_parse_while_stmt) */ p->in_error_recovery = true; return iron_make_error(p); }
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
    if (!var_name) { /* HARD-09 REPLACE (iron_parse_for_stmt var_name) */ var_name = "?"; }

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
    if (!n) { /* HARD-09 REPLACE (iron_parse_for_stmt) */ p->in_error_recovery = true; return iron_make_error(p); }
    n->kind         = IRON_NODE_FOR;
    n->span         = iron_span_merge(iron_token_span(p, start), body->span);
    n->var_name     = var_name;
    n->iterable     = iterable;
    n->body         = body;
    n->is_parallel  = is_parallel;
    n->pool_expr    = pool_expr;
    return (Iron_Node *)n;
}

/* Parse a variant pattern: EnumName.VariantName(bindings...)
 * Returns IRON_NODE_PATTERN or IRON_NODE_ERROR. */
static Iron_Node *iron_parse_pattern(Iron_Parser *p) {
    Iron_Token *start = iron_current(p);

    /* enum_name */
    if (!iron_check(p, IRON_TOK_IDENTIFIER)) {
        iron_diag_emit(p->diags, p->arena, IRON_DIAG_ERROR,
                       IRON_ERR_UNEXPECTED_TOKEN,
                       iron_token_span(p, iron_current(p)),
                       "expected enum name in pattern", NULL);
        return iron_make_error(p);
    }
    Iron_Token *enum_tok = iron_advance(p);

    if (!iron_expect(p, IRON_TOK_DOT)) return iron_make_error(p);

    /* variant_name */
    if (!iron_check(p, IRON_TOK_IDENTIFIER)) {
        iron_diag_emit(p->diags, p->arena, IRON_DIAG_ERROR,
                       IRON_ERR_UNEXPECTED_TOKEN,
                       iron_token_span(p, iron_current(p)),
                       "expected variant name after '.'", NULL);
        return iron_make_error(p);
    }
    Iron_Token *variant_tok = iron_advance(p);

    const char **binding_names   = NULL;
    Iron_Node  **nested_patterns = NULL;
    int          binding_count   = 0;

    /* Optional binding list: (name, _, name, ...) */
    if (iron_match(p, IRON_TOK_LPAREN)) {
        iron_skip_newlines(p);
        while (!iron_check(p, IRON_TOK_RPAREN) && !iron_check(p, IRON_TOK_EOF)) {
            if (iron_check(p, IRON_TOK_WILDCARD)) {
                /* Wildcard _ */
                iron_advance(p);
                arrput(binding_names,   (const char *)NULL);
                arrput(nested_patterns, (Iron_Node *)NULL);
            } else if (iron_check(p, IRON_TOK_IDENTIFIER)) {
                Iron_Token *name_tok = iron_current(p);
                /* Nested pattern: uppercase identifier followed by '.' is a sub-pattern */
                bool is_nested = (name_tok->value[0] >= 'A' && name_tok->value[0] <= 'Z')
                                  && (p->pos + 1 < p->token_count)
                                  && (p->tokens[p->pos + 1].kind == IRON_TOK_DOT);
                if (is_nested) {
                    /* Recursively parse nested pattern e.g. Inner.Val(n) */
                    Iron_Node *nested_pat = iron_parse_pattern(p);
                    arrput(binding_names,   (const char *)NULL);
                    arrput(nested_patterns, nested_pat);
                } else {
                    /* Simple name binding */
                    iron_advance(p);
                    const char *bname = iron_arena_strdup(p->arena, name_tok->value,
                                                           strlen(name_tok->value));
                    if (!bname) { /* HARD-09 REPLACE (iron_parse_pattern bname) */ bname = "?"; }
                    arrput(binding_names,   bname);
                    arrput(nested_patterns, (Iron_Node *)NULL);
                }
            } else {
                iron_diag_emit(p->diags, p->arena, IRON_DIAG_ERROR,
                               IRON_ERR_UNEXPECTED_TOKEN,
                               iron_token_span(p, iron_current(p)),
                               "expected binding name or '_' in pattern", NULL);
                break;
            }
            binding_count++;
            iron_skip_newlines(p);
            if (!iron_match(p, IRON_TOK_COMMA)) break;
            iron_skip_newlines(p);
        }
        iron_expect(p, IRON_TOK_RPAREN);
    }

    Iron_Pattern *pat   = ARENA_ALLOC(p->arena, Iron_Pattern);
    if (!pat) { /* HARD-09 REPLACE (iron_parse_pattern) */ p->in_error_recovery = true; return iron_make_error(p); }
    pat->kind           = IRON_NODE_PATTERN;
    pat->span           = iron_span_merge(iron_token_span(p, start),
                                           iron_token_span(p, iron_current(p)));
    pat->enum_name      = iron_arena_strdup(p->arena, enum_tok->value,
                                             strlen(enum_tok->value));
    if (!pat->enum_name) { /* HARD-09 REPLACE (iron_parse_pattern enum_name) */ pat->enum_name = "?"; }
    pat->variant_name   = iron_arena_strdup(p->arena, variant_tok->value,
                                             strlen(variant_tok->value));
    if (!pat->variant_name) { /* HARD-09 REPLACE (iron_parse_pattern variant_name) */ pat->variant_name = "?"; }
    pat->binding_names  = binding_names;
    pat->nested_patterns = nested_patterns;
    pat->binding_count  = binding_count;
    return (Iron_Node *)pat;
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

        /* else -> body */
        if (iron_check(p, IRON_TOK_ELSE)) {
            iron_advance(p);
            if (iron_check(p, IRON_TOK_LBRACE)) {
                /* Old syntax: else { body } — emit error but recover */
                iron_diag_emit(p->diags, p->arena, IRON_DIAG_ERROR,
                               IRON_ERR_UNEXPECTED_TOKEN,
                               iron_token_span(p, iron_current(p)),
                               "match arm body must use '->' syntax; '{' arm syntax is no longer supported",
                               NULL);
                else_body = iron_parse_block(p);
            } else {
                if (!iron_expect(p, IRON_TOK_ARROW)) {
                    /* Can't recover here cleanly — skip to closing brace */
                    iron_skip_newlines(p);
                    break;
                }
                if (iron_check(p, IRON_TOK_LBRACE)) {
                    else_body = iron_parse_block(p);
                } else {
                    Iron_Node *single = iron_parse_stmt(p);
                    Iron_Block *blk = ARENA_ALLOC(p->arena, Iron_Block);
                    if (!blk) { /* HARD-09 REPLACE (iron_parse_match_stmt else Block) */ p->in_error_recovery = true; return iron_make_error(p); }
                    blk->kind       = IRON_NODE_BLOCK;
                    blk->span       = single->span;
                    blk->stmts      = NULL;
                    arrput(blk->stmts, single);
                    blk->stmt_count = 1;
                    else_body = (Iron_Node *)blk;
                }
            }
            iron_skip_newlines(p);
            break;
        }

        /* Decide pattern type by lookahead:
         * IDENTIFIER followed by DOT → ADT variant pattern (EnumName.Variant(...))
         * Anything else → expression pattern (integer literal, etc.) */
        Iron_Node *pattern;
        if (iron_check(p, IRON_TOK_IDENTIFIER) &&
            p->pos + 1 < p->token_count &&
            p->tokens[p->pos + 1].kind == IRON_TOK_DOT) {
            pattern = iron_parse_pattern(p);
        } else {
            pattern = iron_parse_expr(p);
        }

        /* Detect old { } arm syntax — emit error but recover */
        if (iron_check(p, IRON_TOK_LBRACE)) {
            iron_diag_emit(p->diags, p->arena, IRON_DIAG_ERROR,
                           IRON_ERR_UNEXPECTED_TOKEN,
                           iron_token_span(p, iron_current(p)),
                           "match arm body must use '->' syntax; '{' arm syntax is no longer supported",
                           NULL);
            /* Error recovery: parse the block anyway to continue */
            Iron_Node *cbody = iron_parse_block(p);
            Iron_MatchCase *mc = ARENA_ALLOC(p->arena, Iron_MatchCase);
            if (!mc) { /* HARD-09 REPLACE (iron_parse_match_stmt MatchCase error recovery) */ p->in_error_recovery = true; return iron_make_error(p); }
            mc->kind    = IRON_NODE_MATCH_CASE;
            mc->span    = iron_span_merge(pattern->span, cbody->span);
            mc->pattern = pattern;
            mc->body    = cbody;
            arrput(cases, (Iron_Node *)mc);
            case_count++;
            iron_skip_newlines(p);
            continue;
        }

        if (!iron_expect(p, IRON_TOK_ARROW)) {
            iron_skip_newlines(p);
            continue;
        }

        /* Body: block { ... } or single statement (wrapped in synthetic block) */
        Iron_Node *cbody;
        if (iron_check(p, IRON_TOK_LBRACE)) {
            cbody = iron_parse_block(p);
        } else {
            Iron_Node *single = iron_parse_stmt(p);
            Iron_Block *blk = ARENA_ALLOC(p->arena, Iron_Block);
            if (!blk) { /* HARD-09 REPLACE (iron_parse_match_stmt case Block) */ p->in_error_recovery = true; return iron_make_error(p); }
            blk->kind       = IRON_NODE_BLOCK;
            blk->span       = single->span;
            blk->stmts      = NULL;
            arrput(blk->stmts, single);
            blk->stmt_count = 1;
            cbody = (Iron_Node *)blk;
        }

        Iron_MatchCase *mc = ARENA_ALLOC(p->arena, Iron_MatchCase);
        if (!mc) { /* HARD-09 REPLACE (iron_parse_match_stmt MatchCase) */ p->in_error_recovery = true; return iron_make_error(p); }
        mc->kind    = IRON_NODE_MATCH_CASE;
        mc->span    = iron_span_merge(pattern->span, cbody->span);
        mc->pattern = pattern;
        mc->body    = cbody;
        arrput(cases, (Iron_Node *)mc);
        case_count++;
        iron_skip_newlines(p);
    }

    Iron_Token *end = iron_current(p);
    iron_expect(p, IRON_TOK_RBRACE);

    Iron_MatchStmt *n = ARENA_ALLOC(p->arena, Iron_MatchStmt);
    if (!n) { /* HARD-09 REPLACE (iron_parse_match_stmt MatchStmt) */ p->in_error_recovery = true; return iron_make_error(p); }
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
        if (!spawn_name) { /* HARD-09 REPLACE (iron_parse_spawn_stmt name) */ spawn_name = "?"; }
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
    if (!n) { /* HARD-09 REPLACE (iron_parse_spawn_stmt SpawnStmt) */ p->in_error_recovery = true; return iron_make_error(p); }
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
    if (!n) { /* HARD-09 REPLACE (iron_parse_interp_string) */ p->in_error_recovery = true; return iron_make_error(p); }
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
                if (!sl) { /* HARD-09 REPLACE (iron_parse_interp_string StringLit segment) */ p->in_error_recovery = true; return iron_make_error(p); }
                sl->kind  = IRON_NODE_STRING_LIT;
                sl->span  = span;
                sl->value = iron_arena_strdup(p->arena, lit_buf, lit_len);
                if (!sl->value) { /* HARD-09 REPLACE (iron_parse_interp_string StringLit segment value) */ sl->value = ""; }
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
        if (!sl) { /* HARD-09 REPLACE (iron_parse_interp_string StringLit tail) */ p->in_error_recovery = true; return iron_make_error(p); }
        sl->kind  = IRON_NODE_STRING_LIT;
        sl->span  = span;
        sl->value = iron_arena_strdup(p->arena, lit_buf, lit_len);
        if (!sl->value) { /* HARD-09 REPLACE (iron_parse_interp_string StringLit tail value) */ sl->value = ""; }
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

    /* Phase 59 01d: tuple destructure form — val (a, b, ...) [: Ty] = expr. */
    if (iron_check(p, IRON_TOK_LPAREN)) {
        iron_advance(p);  /* consume ( */
        iron_skip_newlines(p);
        const char **names = NULL;  /* stb_ds, NULL entries = wildcard */
        while (!iron_check(p, IRON_TOK_RPAREN) && !iron_check(p, IRON_TOK_EOF)) {
            if (iron_check(p, IRON_TOK_IDENTIFIER)) {
                Iron_Token *id = iron_advance(p);
                const char *tuple_name = iron_arena_strdup(p->arena, id->value,
                                                            strlen(id->value));
                if (!tuple_name) { /* HARD-09 REPLACE (iron_parse_val_decl tuple binding name) */ tuple_name = "?"; }
                arrput(names, tuple_name);
            } else if (iron_check(p, IRON_TOK_WILDCARD)) {
                iron_advance(p);
                arrput(names, NULL);  /* sentinel for wildcard */
            } else {
                iron_diag_emit(p->diags, p->arena, IRON_DIAG_ERROR,
                               IRON_ERR_UNEXPECTED_TOKEN,
                               iron_token_span(p, iron_current(p)),
                               "expected identifier or '_' in tuple destructure binding", NULL);
                if (iron_peek(p) != IRON_TOK_EOF) iron_advance(p);
                break;
            }
            iron_skip_newlines(p);
            if (!iron_check(p, IRON_TOK_COMMA)) break;
            iron_advance(p);
            iron_skip_newlines(p);
        }
        iron_expect(p, IRON_TOK_RPAREN);
        int count = (int)arrlen(names);
        if (count < 2) {
            iron_diag_emit(p->diags, p->arena, IRON_DIAG_ERROR,
                           IRON_ERR_UNEXPECTED_TOKEN,
                           iron_token_span(p, start),
                           "tuple destructure requires at least 2 binding names", NULL);
            arrfree(names);
            return iron_make_error(p);
        }
        /* Transfer binding names into arena. */
        const char **arena_names = (const char **)iron_arena_alloc(
            p->arena, sizeof(const char *) * (size_t)count,
            _Alignof(const char *));
        if (!arena_names) { /* HARD-09 REPLACE (iron_parse_val_decl tuple arena_names) */ p->in_error_recovery = true; arrfree(names); return iron_make_error(p); }
        memcpy(arena_names, names, sizeof(const char *) * (size_t)count);
        arrfree(names);

        /* Optional type annotation : Ty (for symmetry, though rarely used). */
        Iron_Node *type_ann = NULL;
        if (iron_match(p, IRON_TOK_COLON)) {
            type_ann = iron_parse_type_annotation(p);
        }
        /* Required initializer. */
        Iron_Node *init = NULL;
        if (iron_match(p, IRON_TOK_ASSIGN)) {
            init = iron_parse_expr(p);
        } else {
            iron_diag_emit(p->diags, p->arena, IRON_DIAG_ERROR,
                           IRON_ERR_UNEXPECTED_TOKEN,
                           iron_token_span(p, iron_current(p)),
                           "destructure binding requires an initializer '= expr'", NULL);
            if (iron_peek(p) != IRON_TOK_EOF) iron_advance(p);
            return iron_make_error(p);
        }

        Iron_ValDecl *n = ARENA_ALLOC(p->arena, Iron_ValDecl);
        if (!n) { /* HARD-09 REPLACE (iron_parse_val_decl tuple ValDecl) */ p->in_error_recovery = true; return iron_make_error(p); }
        memset(n, 0, sizeof(*n));
        n->kind          = IRON_NODE_VAL_DECL;
        n->span          = iron_span_merge(iron_token_span(p, start),
                                            init ? init->span
                                                 : iron_token_span(p, iron_current(p)));
        n->name          = NULL;
        n->type_ann      = type_ann;
        n->init          = init;
        n->declared_type = NULL;
        n->binding_names = arena_names;
        n->binding_count = count;
        return (Iron_Node *)n;
    }

    if (!iron_check(p, IRON_TOK_IDENTIFIER) && !iron_check(p, IRON_TOK_WILDCARD)) {
        iron_diag_emit(p->diags, p->arena, IRON_DIAG_ERROR,
                       IRON_ERR_UNEXPECTED_TOKEN,
                       iron_token_span(p, iron_current(p)),
                       "expected variable name or tuple destructure pattern after 'val'", NULL);
        /* 2026-04-10 Phase 59 01d: advance past the offending token. Paired
         * with the no-progress guard in iron_parse_block this is belt-and-
         * braces, but it also guarantees forward progress even if this path
         * is reached from an outer caller that doesn't run the guard. */
        if (iron_peek(p) != IRON_TOK_EOF) iron_advance(p);
        return iron_make_error(p);
    }
    Iron_Token *name_tok = iron_advance(p);

    Iron_Node *type_ann = NULL;
    if (iron_match(p, IRON_TOK_COLON)) {
        type_ann = iron_parse_type_annotation(p);
    }

    Iron_Node *init = NULL;
    if (iron_match(p, IRON_TOK_ASSIGN)) {
        if (iron_check(p, IRON_TOK_SPAWN)) {
            /* val h = spawn("name") { body } -- spawn as expression */
            Iron_Node *spawn_node = iron_parse_spawn_stmt(p);
            if (spawn_node && spawn_node->kind == IRON_NODE_SPAWN) {
                Iron_SpawnStmt *ss = (Iron_SpawnStmt *)spawn_node;
                ss->handle_name = iron_arena_strdup(p->arena, name_tok->value,
                                                     strlen(name_tok->value));
                if (!ss->handle_name) { /* HARD-09 REPLACE (iron_parse_val_decl spawn handle_name) */ ss->handle_name = "?"; }
            }
            init = spawn_node;
        } else {
            init = iron_parse_expr(p);
        }
    }

    Iron_ValDecl *n = ARENA_ALLOC(p->arena, Iron_ValDecl);
    if (!n) { /* HARD-09 REPLACE (iron_parse_val_decl ValDecl) */ p->in_error_recovery = true; return iron_make_error(p); }
    n->kind          = IRON_NODE_VAL_DECL;
    n->span          = iron_span_merge(iron_token_span(p, start),
                                       init ? init->span : iron_token_span(p, name_tok));
    n->name          = iron_arena_strdup(p->arena, name_tok->value,
                                         strlen(name_tok->value));
    if (!n->name) { /* HARD-09 REPLACE (iron_parse_val_decl ValDecl name) */ n->name = "?"; }
    n->type_ann      = type_ann;
    n->init          = init;
    n->declared_type = NULL;  /* set by type checker */
    /* Phase 59 01d: destructure bindings default off; populated below if we
     * add a LPAREN branch in Task 2. */
    n->binding_names = NULL;
    n->binding_count = 0;
    return (Iron_Node *)n;
}

static Iron_Node *iron_parse_var_decl(Iron_Parser *p) {
    Iron_Token *start = iron_current(p);
    iron_advance(p);  /* consume 'var' */

    if (!iron_check(p, IRON_TOK_IDENTIFIER) && !iron_check(p, IRON_TOK_WILDCARD)) {
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
        if (iron_check(p, IRON_TOK_SPAWN)) {
            /* var h = spawn("name") { body } -- spawn as expression */
            Iron_Node *spawn_node = iron_parse_spawn_stmt(p);
            if (spawn_node && spawn_node->kind == IRON_NODE_SPAWN) {
                Iron_SpawnStmt *ss = (Iron_SpawnStmt *)spawn_node;
                ss->handle_name = iron_arena_strdup(p->arena, name_tok->value,
                                                     strlen(name_tok->value));
                if (!ss->handle_name) { /* HARD-09 REPLACE (iron_parse_var_decl spawn handle_name) */ ss->handle_name = "?"; }
            }
            init = spawn_node;
        } else {
            init = iron_parse_expr(p);
        }
    }

    Iron_VarDecl *n = ARENA_ALLOC(p->arena, Iron_VarDecl);
    if (!n) { /* HARD-09 REPLACE (iron_parse_var_decl VarDecl) */ p->in_error_recovery = true; return iron_make_error(p); }
    n->kind          = IRON_NODE_VAR_DECL;
    n->span          = iron_span_merge(iron_token_span(p, start),
                                       init ? init->span : iron_token_span(p, name_tok));
    n->name          = iron_arena_strdup(p->arena, name_tok->value,
                                         strlen(name_tok->value));
    if (!n->name) { /* HARD-09 REPLACE (iron_parse_var_decl VarDecl name) */ n->name = "?"; }
    n->type_ann      = type_ann;
    n->init          = init;
    n->declared_type = NULL;  /* set by type checker */
    return (Iron_Node *)n;
}

/* HARD-08: wrapper — see iron_parse_type_annotation for the pattern. */
static Iron_Node *iron_parse_stmt(Iron_Parser *p) {
    if (iron_parser_depth_exceeded(p)) {
        return iron_make_error(p);
    }
    p->recur_depth++;
    Iron_Node *r = iron_parse_stmt_impl(p);
    p->recur_depth--;
    return r;
}

/* Parse a single statement */
static Iron_Node *iron_parse_stmt_impl(Iron_Parser *p) {
    /* HARD-05: cancel poll at statement parser entry. */
    if (iron_cancel_requested(p->cancel_flag)) {
        return iron_make_error(p);
    }
    iron_skip_newlines(p);
    Iron_Token *t = iron_current(p);

    switch ((int)t->kind) {
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
            if (!n) { /* HARD-09 REPLACE (iron_parse_stmt ReturnStmt) */ p->in_error_recovery = true; return iron_make_error(p); }
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
            if (!n) { /* HARD-09 REPLACE (iron_parse_stmt DeferStmt) */ p->in_error_recovery = true; return iron_make_error(p); }
            n->kind           = IRON_NODE_DEFER;
            n->span           = iron_span_merge(iron_token_span(p, t), expr->span);
            n->expr           = expr;
            return (Iron_Node *)n;
        }
        case IRON_TOK_FREE: {
            iron_advance(p);
            Iron_Node *expr = iron_parse_expr(p);
            Iron_FreeStmt *n = ARENA_ALLOC(p->arena, Iron_FreeStmt);
            if (!n) { /* HARD-09 REPLACE (iron_parse_stmt FreeStmt) */ p->in_error_recovery = true; return iron_make_error(p); }
            n->kind          = IRON_NODE_FREE;
            n->span          = iron_span_merge(iron_token_span(p, t), expr->span);
            n->expr          = expr;
            return (Iron_Node *)n;
        }
        case IRON_TOK_LEAK: {
            iron_advance(p);
            Iron_Node *expr = iron_parse_expr(p);
            Iron_LeakStmt *n = ARENA_ALLOC(p->arena, Iron_LeakStmt);
            if (!n) { /* HARD-09 REPLACE (iron_parse_stmt LeakStmt) */ p->in_error_recovery = true; return iron_make_error(p); }
            n->kind          = IRON_NODE_LEAK;
            n->span          = iron_span_merge(iron_token_span(p, t), expr->span);
            n->expr          = expr;
            return (Iron_Node *)n;
        }
        case IRON_TOK_SPAWN:
            return iron_parse_spawn_stmt(p);
        case IRON_TOK_LBRACE:
            return iron_parse_block(p);
        case IRON_TOK_MUT:
            /* Phase 88 BREAK-04: 'mut' as a statement-level prefix is removed in v3.0.
             * When strict-v3 is ON emit E0263; consume 'mut' and parse the remainder
             * as an expression statement for recovery.
             * Gate OFF: fall through to expression-statement / default path so
             * existing gate-off behavior is completely unchanged. */
            if (p->v3_strict_mode) {
                iron_diag_emit(p->diags, p->arena, IRON_DIAG_ERROR,
                               IRON_ERR_V3_MUT_KEYWORD,
                               iron_token_span(p, t),
                               "'mut' keyword removed in v3.0; use 'var' for mutable bindings "
                               "or declare a default in-object method to mutate self",
                               "run 'ironc migrate --from v2 --to v3 <file>' to migrate");
                iron_advance(p);  /* consume 'mut' for recovery */
                return iron_parse_expr(p);
            }
            __attribute__((fallthrough));
        /* -Wswitch-enum opt-out: statement switch only handles the token kinds
         * that begin a statement keyword; every other Iron_TokenKind falls
         * through to the expression-statement / assignment default below. */
        default: {
            /* Expression statement, or assignment */
            Iron_Node *expr = iron_parse_expr(p);

            iron_skip_newlines(p);

            /* Check for assignment operators */
            Iron_TokenKind op = iron_peek(p);
            if (op == IRON_TOK_ASSIGN       ||
                op == IRON_TOK_PLUS_ASSIGN  ||
                op == IRON_TOK_MINUS_ASSIGN ||
                op == IRON_TOK_STAR_ASSIGN  ||
                op == IRON_TOK_SLASH_ASSIGN ||
                op == IRON_TOK_SHL_ASSIGN   ||
                op == IRON_TOK_SHR_ASSIGN   ||
                op == IRON_TOK_AMP_ASSIGN   ||
                op == IRON_TOK_PIPE_ASSIGN  ||
                op == IRON_TOK_CARET_ASSIGN) {
                iron_advance(p);
                iron_skip_newlines(p);
                Iron_Node *val = iron_parse_expr(p);
                Iron_AssignStmt *a = ARENA_ALLOC(p->arena, Iron_AssignStmt);
                if (!a) { /* HARD-09 REPLACE (iron_parse_stmt AssignStmt) */ p->in_error_recovery = true; return iron_make_error(p); }
                a->kind          = IRON_NODE_ASSIGN;
                a->span          = iron_span_merge(expr->span, val->span);
                a->target        = expr;
                a->value         = val;
                a->op            = (Iron_OpKind)op;
                /* Phase 83-02: defensive default; typecheck flips it for
                 * writes to pub var fields so HIR emits set_<field>(value). */
                a->is_pub_setter = false;
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
    if (!path) { /* HARD-09 REPLACE (iron_parse_import_decl path) */ path = "?"; }

    /* optional: as alias */
    const char *alias = NULL;
    if (iron_check(p, IRON_TOK_IDENTIFIER) &&
        strcmp(iron_current(p)->value, "as") == 0) {
        iron_advance(p);  /* consume 'as' */
        if (iron_check(p, IRON_TOK_IDENTIFIER)) {
            Iron_Token *at = iron_advance(p);
            alias = iron_arena_strdup(p->arena, at->value, strlen(at->value));
            if (!alias) { /* HARD-09 REPLACE (iron_parse_import_decl alias) */ alias = "?"; }
        }
    }

    Iron_ImportDecl *n = ARENA_ALLOC(p->arena, Iron_ImportDecl);
    if (!n) { /* HARD-09 REPLACE (iron_parse_import_decl ImportDecl) */ p->in_error_recovery = true; return iron_make_error(p); }
    n->kind            = IRON_NODE_IMPORT_DECL;
    n->span            = iron_span_merge(iron_token_span(p, start),
                                          iron_token_span(p, iron_current(p)));
    n->path            = path;
    n->alias           = alias;
    return (Iron_Node *)n;
}

/* ── Helper: convert Iron snake_case name to C CamelCase ─────────────────── */

/* e.g. "init_window" -> "InitWindow", "draw_text" -> "DrawText"
 * Names with no underscores are returned unchanged so that lowercase C
 * library functions like "puts" or "printf" are not accidentally capitalized. */
static const char *iron_snake_to_camel(Iron_Arena *arena, const char *name) {
    if (!name) return name;
    /* Only apply conversion when the name contains underscores */
    if (strchr(name, '_') == NULL) return name;

    size_t len = strlen(name);
    /* Output can be at most len bytes (we remove underscores, add nothing) */
    char *buf = (char *)iron_arena_alloc(arena, len + 1, 1);
    if (!buf) { /* HARD-09 REPLACE (iron_snake_to_camel) */ buf = "?"; }

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
    if (!iron_name) { /* HARD-09 REPLACE (iron_parse_extern_func iron_name) */ iron_name = "?"; }
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
    if (!f) { /* HARD-09 REPLACE (iron_parse_extern_func FuncDecl) */ p->in_error_recovery = true; return iron_make_error(p); }
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
    f->is_readonly          = false;  /* Phase 87: readonly/pure only valid on iface sigs */
    f->is_pure              = false;
    return (Iron_Node *)f;
}

static Iron_Node *iron_parse_func_or_method(Iron_Parser *p, bool is_private) {
    Iron_Token *start = iron_current(p);
    iron_advance(p);  /* consume 'func' */

    /* v2.1 receiver-method form: `func (name: Type) method(...) {...}`.
     * Desugars to the existing `func Type.method(name: Type, ...)`
     * MethodDecl — the receiver becomes the first positional param, and
     * the body references it by the declared name the same way that
     * Duration.to_ms (stdlib/time.iron) already does. Zero HIR/LIR
     * changes required; the resolver and lowerer see an ordinary method
     * declaration. See `.planning/REQUIREMENTS.md` GRAMMAR-01..05. */
    if (iron_check(p, IRON_TOK_LPAREN)) {
        iron_advance(p);  /* consume '(' */

        /* Receiver-binding prefix: `var` (mutable binding, v2.1), `val`
         * (immutable binding, v2.1 default, explicit form), or `mut`
         * (Phase 79 MUT-01: mutable receiver capability). These three
         * are mutually exclusive — `mut var` and `var mut` fail at the
         * name check below because the second keyword is not a valid
         * identifier.
         *
         * Semantic interpretation of the flags is Phase 80's job:
         *   - is_var           = true  → binding is mutable (v2.1 behavior unchanged)
         *   - is_mut_receiver  = true  → receiver has mutable capability (Phase 80
         *                                resolver grants this, typechecker enforces)
         *   - both false               → v2.1 val-receiver (default, immutable) */
        bool recv_is_var = false;
        bool recv_is_mut = false;
        if (iron_check(p, IRON_TOK_VAR)) {
            iron_advance(p);
            recv_is_var = true;
        } else if (iron_check(p, IRON_TOK_VAL)) {
            iron_advance(p);
        } else if (iron_check(p, IRON_TOK_MUT)) {
            iron_advance(p);
            recv_is_mut = true;
        }

        /* Phase 88 BREAK-01/02: reject receiver-method form when strict-v3 gate is ON.
         * E0260 for plain receiver, E0261 for mut-receiver. Both forms removed in v3.0. */
        if (p->v3_strict_mode) {
            int code = recv_is_mut ? IRON_ERR_V3_MUT_RECEIVER : IRON_ERR_V3_RECEIVER_SYNTAX;
            const char *msg = recv_is_mut
                ? "mut-receiver syntax 'func (mut recv: T) name()' removed in v3.0; "
                  "declare as a default method inside 'object T { func name() { ... } }'"
                : "receiver-method syntax 'func (recv: T) name()' removed in v3.0; "
                  "declare as a method inside 'object T { func name() { ... } }'";
            iron_diag_emit(p->diags, p->arena, IRON_DIAG_ERROR, code,
                           iron_token_span(p, start),
                           msg,
                           "run 'ironc migrate --from v2 --to v3 <file>' to migrate");
            p->in_error_recovery = true;
            iron_parser_sync_toplevel(p);
            return iron_make_error(p);
        }

        if (!iron_check_name(p)) {
            iron_emit_diag(p, IRON_ERR_UNEXPECTED_TOKEN,
                           iron_token_span(p, iron_current(p)),
                           "expected receiver parameter name after 'func ('");
            p->in_error_recovery = true;
            iron_parser_sync_toplevel(p);
            return iron_make_error(p);
        }
        Iron_Token *recv_name_tok = iron_advance(p);

        if (!iron_match(p, IRON_TOK_COLON)) {
            iron_emit_diag(p, IRON_ERR_UNEXPECTED_TOKEN,
                           iron_token_span(p, iron_current(p)),
                           "expected ':' after receiver name — "
                           "receiver syntax is `func (name: Type) method(...)`");
            p->in_error_recovery = true;
            iron_parser_sync_toplevel(p);
            return iron_make_error(p);
        }

        Iron_Node *recv_type_ann = iron_parse_type_annotation(p);
        if (!recv_type_ann ||
            recv_type_ann->kind != IRON_NODE_TYPE_ANNOTATION) {
            iron_emit_diag(p, IRON_ERR_UNEXPECTED_TOKEN,
                           iron_token_span(p, iron_current(p)),
                           "expected receiver type after ':'");
            p->in_error_recovery = true;
            iron_parser_sync_toplevel(p);
            return iron_make_error(p);
        }

        /* Receiver type must be a simple named type — no tuples, arrays,
         * func types, or generics. Rejecting these early gives clear
         * diagnostics (GRAMMAR-05) instead of a cryptic downstream error. */
        Iron_TypeAnnotation *recv_ann = (Iron_TypeAnnotation *)recv_type_ann;
        if (recv_ann->is_tuple || recv_ann->is_array || recv_ann->is_func ||
            !recv_ann->name || recv_ann->generic_arg_count > 0) {
            iron_emit_diag(p, IRON_ERR_UNEXPECTED_TOKEN, recv_ann->span,
                           "receiver type must be a named object or enum type "
                           "(tuples, arrays, function types, and generics are not receivers)");
            p->in_error_recovery = true;
            iron_parser_sync_toplevel(p);
            return iron_make_error(p);
        }

        if (!iron_match(p, IRON_TOK_RPAREN)) {
            iron_emit_diag(p, IRON_ERR_UNEXPECTED_TOKEN,
                           iron_token_span(p, iron_current(p)),
                           "expected ')' after receiver type");
            p->in_error_recovery = true;
            iron_parser_sync_toplevel(p);
            return iron_make_error(p);
        }

        if (!iron_check_name(p)) {
            iron_emit_diag(p, IRON_ERR_UNEXPECTED_TOKEN,
                           iron_token_span(p, iron_current(p)),
                           "expected method name after '(receiver: Type)'");
            p->in_error_recovery = true;
            iron_parser_sync_toplevel(p);
            return iron_make_error(p);
        }
        Iron_Token *recv_method_tok = iron_advance(p);

        /* Optional method-level generics (e.g. `func (l: List) map[U](...)`) */
        int         recv_generic_count  = 0;
        Iron_Node **recv_generic_params = NULL;
        if (iron_check(p, IRON_TOK_LBRACKET)) {
            recv_generic_params = iron_parse_generic_params(p, &recv_generic_count, p->arena);
        }

        int recv_explicit_count = 0;
        Iron_Node **recv_explicit_params = iron_parse_param_list(p, &recv_explicit_count);

        Iron_Node *recv_ret = NULL;
        if (iron_match(p, IRON_TOK_ARROW)) {
            recv_ret = iron_parse_type_annotation(p);
        }

        Iron_Node *recv_body = iron_parse_block(p);

        /* Build the synthetic receiver Param node and prepend it to the
         * params list. The result is a MethodDecl that looks identical
         * to writing `func <Type>.<method>(<receiver_name>: <Type>, ...)`. */
        Iron_Param *synth_recv = ARENA_ALLOC(p->arena, Iron_Param);
        if (!synth_recv) iron_oom_abort("parser.c:iron_parse_func_or_method receiver param");
        synth_recv->kind            = IRON_NODE_PARAM;
        synth_recv->span            = iron_token_span(p, recv_name_tok);
        synth_recv->is_var          = recv_is_var;
        synth_recv->is_mut_receiver = recv_is_mut;  /* Phase 79 MUT-01: mut-receiver capability flag */
        synth_recv->name            = iron_arena_strdup(p->arena, recv_name_tok->value,
                                                  strlen(recv_name_tok->value));
        if (!synth_recv->name) iron_oom_abort("parser.c:iron_parse_func_or_method receiver name");
        synth_recv->type_ann = recv_type_ann;

        int recv_total = recv_explicit_count + 1;
        Iron_Node **recv_all_params = (Iron_Node **)iron_arena_alloc(
            p->arena, sizeof(Iron_Node *) * (size_t)recv_total,
            _Alignof(Iron_Node *));
        if (!recv_all_params) iron_oom_abort("parser.c:iron_parse_func_or_method receiver params");
        recv_all_params[0] = (Iron_Node *)synth_recv;
        for (int i = 0; i < recv_explicit_count; i++) {
            recv_all_params[i + 1] = recv_explicit_params[i];
        }

        Iron_MethodDecl *rm = ARENA_ALLOC(p->arena, Iron_MethodDecl);
        if (!rm) iron_oom_abort("parser.c:iron_parse_func_or_method receiver MethodDecl");
        rm->kind                 = IRON_NODE_METHOD_DECL;
        rm->span                 = iron_span_merge(iron_token_span(p, start), recv_body->span);
        rm->type_name            = iron_arena_strdup(p->arena, recv_ann->name,
                                                      strlen(recv_ann->name));
        if (!rm->type_name) iron_oom_abort("parser.c:iron_parse_func_or_method receiver type_name");
        rm->method_name          = iron_arena_strdup(p->arena, recv_method_tok->value,
                                                      strlen(recv_method_tok->value));
        if (!rm->method_name) iron_oom_abort("parser.c:iron_parse_func_or_method receiver method_name");
        rm->params               = recv_all_params;
        rm->param_count          = recv_total;
        rm->return_type          = recv_ret;
        rm->body                 = recv_body;
        rm->is_private           = is_private;
        rm->generic_params       = recv_generic_params;
        rm->generic_param_count  = recv_generic_count;
        rm->resolved_return_type = NULL;
        rm->owner_sym            = NULL;
        rm->is_array_extension   = false;
        rm->elem_type_name       = NULL;
        rm->is_fusible           = false;
        rm->is_receiver_form     = true;
        rm->is_synth_accessor    = false;  /* Phase 83-01: default; 83-02 writes */
        rm->is_readonly          = false;  /* Phase 84: receiver form has no tier modifier */
        rm->is_pure              = false;
        rm->is_init              = false;  /* Phase 85: receiver form is never init */
        rm->init_name            = NULL;
        return (Iron_Node *)rm;
    }

    /* Check for array extension method: func [T].method_name(...) */
    if (iron_check(p, IRON_TOK_LBRACKET)) {
        iron_advance(p);  /* consume '[' */

        /* Parse element type parameter name (e.g., T) */
        if (!iron_check_name(p)) {
            iron_emit_diag(p, IRON_ERR_UNEXPECTED_TOKEN,
                           iron_token_span(p, iron_current(p)),
                           "expected element type parameter in array extension method");
            p->in_error_recovery = true;
            iron_parser_sync_toplevel(p);
            return iron_make_error(p);
        }
        Iron_Token *elem_type_tok = iron_advance(p);

        if (!iron_match(p, IRON_TOK_RBRACKET)) {
            iron_emit_diag(p, IRON_ERR_UNEXPECTED_TOKEN,
                           iron_token_span(p, iron_current(p)),
                           "expected ']' after element type in array extension method");
            p->in_error_recovery = true;
            iron_parser_sync_toplevel(p);
            return iron_make_error(p);
        }

        if (!iron_match(p, IRON_TOK_DOT)) {
            iron_emit_diag(p, IRON_ERR_UNEXPECTED_TOKEN,
                           iron_token_span(p, iron_current(p)),
                           "expected '.' after array type in extension method");
            p->in_error_recovery = true;
            iron_parser_sync_toplevel(p);
            return iron_make_error(p);
        }

        /* Phase 85 INIT: accept IRON_TOK_INIT for array extension method
         * name to keep stdlib list.iron / set.iron room to use `init` if
         * needed in future extensions. Currently no in-tree file exercises
         * this branch but the symmetry with classic Type.method form is
         * worth preserving. */
        if (!iron_check_name_or_init(p)) {
            iron_emit_diag(p, IRON_ERR_UNEXPECTED_TOKEN,
                           iron_token_span(p, iron_current(p)),
                           "expected method name after '[T].'");
            p->in_error_recovery = true;
            iron_parser_sync_toplevel(p);
            return iron_make_error(p);
        }
        Iron_Token *method_tok = iron_advance(p);

        /* Method-level generic params (e.g., [U] in func [T].map[U](...)) */
        int         generic_count  = 0;
        Iron_Node **generic_params = NULL;
        if (iron_check(p, IRON_TOK_LBRACKET)) {
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
        if (!m) { /* HARD-09 REPLACE (iron_parse_func_or_method array MethodDecl) */ p->in_error_recovery = true; return iron_make_error(p); }
        m->kind                 = IRON_NODE_METHOD_DECL;
        m->span                 = iron_span_merge(iron_token_span(p, start), body->span);
        m->type_name            = "__Array";  /* sentinel: marks this as array extension */
        m->method_name          = iron_arena_strdup(p->arena, method_tok->value,
                                                     strlen(method_tok->value));
        if (!m->method_name) { /* HARD-09 REPLACE (iron_parse_func_or_method array method_name) */ m->method_name = "?"; }
        m->params               = params;
        m->param_count          = param_count;
        m->return_type          = ret;
        m->body                 = body;
        m->is_private           = is_private;
        m->generic_params       = generic_params;
        m->generic_param_count  = generic_count;
        m->resolved_return_type = NULL;
        m->owner_sym            = NULL;
        m->is_array_extension   = true;
        m->elem_type_name       = iron_arena_strdup(p->arena, elem_type_tok->value,
                                                     strlen(elem_type_tok->value));
        if (!m->elem_type_name) { /* HARD-09 REPLACE (iron_parse_func_or_method array elem_type_name) */ m->elem_type_name = "?"; }
        m->is_receiver_form     = false;
        m->is_synth_accessor    = false;  /* Phase 83-01: default; 83-02 writes */
        m->is_readonly          = false;  /* Phase 84: array extension has no tier modifier */
        m->is_pure              = false;
        m->is_init              = false;  /* Phase 85: array extension is never init */
        m->init_name            = NULL;
        return (Iron_Node *)m;
    }

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
        /* Phase 85 INIT: accept IRON_TOK_INIT as the method name in classic
         * `func Type.init(...)` form so stdlib `func Window.init(...)` and
         * `func Audio.init()` continue to parse under pure-superset. */
        if (!iron_check_name_or_init(p)) {
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
        if (!m) { /* HARD-09 REPLACE (iron_parse_func_or_method MethodDecl) */ p->in_error_recovery = true; return iron_make_error(p); }
        m->kind                 = IRON_NODE_METHOD_DECL;
        m->span                 = iron_span_merge(iron_token_span(p, start), body->span);
        m->type_name            = iron_arena_strdup(p->arena, name_tok->value,
                                                     strlen(name_tok->value));
        if (!m->type_name) { /* HARD-09 REPLACE (iron_parse_func_or_method type_name) */ m->type_name = "?"; }
        m->method_name          = iron_arena_strdup(p->arena, method_tok->value,
                                                     strlen(method_tok->value));
        if (!m->method_name) { /* HARD-09 REPLACE (iron_parse_func_or_method method_name) */ m->method_name = "?"; }
        m->params               = params;
        m->param_count          = param_count;
        m->return_type          = ret;
        m->body                 = body;
        m->is_private           = is_private;
        m->generic_params       = generic_params;
        m->generic_param_count  = generic_count;
        m->resolved_return_type = NULL;  /* set by type checker */
        m->owner_sym            = NULL;  /* set by resolver */
        m->is_array_extension   = false;
        m->elem_type_name       = NULL;
        m->is_receiver_form     = false;
        m->is_synth_accessor    = false;  /* Phase 83-01: default; 83-02 writes */
        m->is_readonly          = false;  /* Phase 84: classic Type.method has no tier modifier */
        m->is_pure              = false;
        m->is_init              = false;  /* Phase 85: classic Type.method is never init */
        m->init_name            = NULL;
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
    if (!f) { /* HARD-09 REPLACE (iron_parse_func_or_method FuncDecl) */ p->in_error_recovery = true; return iron_make_error(p); }
    f->kind                 = IRON_NODE_FUNC_DECL;
    f->span                 = iron_span_merge(iron_token_span(p, start), body->span);
    f->name                 = iron_arena_strdup(p->arena, name_tok->value,
                                               strlen(name_tok->value));
    if (!f->name) { /* HARD-09 REPLACE (iron_parse_func_or_method FuncDecl name) */ f->name = "?"; }
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
    f->is_readonly          = false;  /* Phase 87: readonly/pure only valid on iface sigs */
    f->is_pure              = false;
    return (Iron_Node *)f;
}

static Iron_Node *iron_parse_object_decl(Iron_Parser *p, bool is_private,
                                         Iron_Node ***extra_decls_out) {
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
            if (!extends_name) { /* HARD-09 REPLACE (iron_parse_object_decl extends_name) */ extends_name = "?"; }
        }
    }

    /* Optional: impl I1, I2 */
    const char **impl_names = NULL;
    int          impl_count = 0;
    if (iron_check(p, IRON_TOK_IMPL)) {
        iron_advance(p);
        while (iron_check(p, IRON_TOK_IDENTIFIER)) {
            Iron_Token *it = iron_advance(p);
            const char *iname = iron_arena_strdup(p->arena, it->value,
                                                   strlen(it->value));
            if (!iname) { /* HARD-09 REPLACE (iron_parse_object_decl impl iname) */ iname = "?"; }
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
    int var_field_count = 0;  /* mutable (var) fields only — used for E0264 */

    while (!iron_check(p, IRON_TOK_RBRACE) && !iron_check(p, IRON_TOK_EOF)) {
        iron_skip_newlines(p);
        if (iron_check(p, IRON_TOK_RBRACE)) break;

        /* Phase 83 ACCESS-02: optional `pub` modifier on fields and methods.
         * At method level in Plan 83-01 the bit is silently accepted but has
         * no AST effect — methods default public in v2.2; Phase 88 flips the
         * default. At field level the bit lands on Iron_Field.is_pub and
         * drives accessor synthesis in Plan 83-02. */
        bool member_is_pub = false;
        if (iron_check(p, IRON_TOK_PUB)) {
            iron_advance(p);
            member_is_pub = true;
        }

        /* Phase 84 MUTTIER-01/02/03: optional tier modifier after pub, before
         * func. `readonly` XOR `pure` — never both. Valid only on object-block
         * methods; top-level placement is rejected by the top-level decl loop
         * via IRON_ERR_TIER_MODIFIER_PLACEMENT. */
        bool member_is_readonly = false;
        bool member_is_pure     = false;
        if (iron_check(p, IRON_TOK_READONLY)) {
            iron_advance(p);
            member_is_readonly = true;
            if (iron_check(p, IRON_TOK_PURE)) {
                iron_diag_emit(p->diags, p->arena, IRON_DIAG_ERROR,
                               IRON_ERR_TIER_MODIFIER_PLACEMENT,
                               iron_token_span(p, iron_current(p)),
                               "method cannot be both 'readonly' and 'pure' "
                               "- pick one tier", NULL);
                iron_advance(p);  /* consume 'pure' to recover */
            }
        } else if (iron_check(p, IRON_TOK_PURE)) {
            iron_advance(p);
            member_is_pure = true;
            if (iron_check(p, IRON_TOK_READONLY)) {
                iron_diag_emit(p->diags, p->arena, IRON_DIAG_ERROR,
                               IRON_ERR_TIER_MODIFIER_PLACEMENT,
                               iron_token_span(p, iron_current(p)),
                               "method cannot be both 'pure' and 'readonly' "
                               "- pick one tier", NULL);
                iron_advance(p);  /* consume 'readonly' to recover */
            }
        }

        /* Phase 85 INIT-03/07/08/11: init declaration inside object body.
         *
         * Two forms are accepted:
         *   Anonymous: `init(params) { body }`        -> init_name = NULL
         *   Named:     `init <ident>(params) { body }` -> init_name = <ident>
         *
         * Flag interactions with previously-consumed modifiers:
         *   `readonly init` / `pure init`: Phase 84 E0245 already rejected
         *     these at the modifier-consumption block above (the modifier
         *     keyword was consumed but `init` is not `func`, so we fall
         *     through and the readonly/pure flags would be silently dropped).
         *     To honor MUTTIER-07 we emit E0245 here when a tier flag is
         *     set - the modifier is a placement violation on an init.
         *   `pub init`: init visibility is tied to the object; reject with
         *     IRON_ERR_UNEXPECTED_TOKEN and a locked message. Recovery
         *     continues parsing the init so one diagnostic = one signal.
         *
         * Return-type rejection (INIT-11 parser side): `init() -> T` is a
         * parse error; the `->` token is consumed and the type annotation
         * discarded so the rest of the object body keeps parsing cleanly.
         *
         * The resulting Iron_MethodDecl mirrors the Phase 82 in-block shape
         * (is_receiver_form = true, synthesized self param) so Phase 79's
         * resolver path handles it without modification. Plan 85-02 reads
         * is_init to gate definite-assignment and delegation-rejection. */
        if (iron_check(p, IRON_TOK_INIT)) {
            Iron_Token *istart = iron_current(p);
            if (member_is_pub) {
                iron_diag_emit(p->diags, p->arena, IRON_DIAG_ERROR,
                               IRON_ERR_UNEXPECTED_TOKEN,
                               iron_token_span(p, istart),
                               "init visibility is tied to its object; "
                               "cannot be marked pub", NULL);
                /* Recover: continue parsing the init so a single diagnostic
                 * represents the pub-init violation. */
            }
            if (member_is_readonly || member_is_pure) {
                iron_diag_emit(p->diags, p->arena, IRON_DIAG_ERROR,
                               IRON_ERR_TIER_MODIFIER_PLACEMENT,
                               iron_token_span(p, istart),
                               "init cannot be 'readonly' or 'pure' - init "
                               "always writes self to initialize fields",
                               NULL);
                /* Drop the tier flags; init is always default-mutating. */
                member_is_readonly = false;
                member_is_pure     = false;
            }
            iron_advance(p);  /* consume 'init' */

            /* Named vs anonymous: named form has IDENTIFIER before `(`. */
            const char *init_name = NULL;
            const char *method_name = NULL;
            if (iron_check(p, IRON_TOK_IDENTIFIER)) {
                Iron_Token *nt = iron_advance(p);
                init_name = iron_arena_strdup(p->arena, nt->value,
                                               strlen(nt->value));
                if (!init_name) iron_oom_abort("parser.c:iron_parse_object_decl init name");
                /* Named init uses its name for call-site dispatch. */
                method_name = init_name;
            } else {
                /* Anonymous init: method_name is the literal "init" so the
                 * resolver finds it under Type.init and Plan 85-02 can match
                 * on method_name == "init" when is_init is true. */
                method_name = iron_arena_strdup(p->arena, "init", 4);
                if (!method_name) iron_oom_abort("parser.c:iron_parse_object_decl anon init name");
            }

            /* Explicit params (no receiver in source; synth self below). */
            int explicit_count = 0;
            Iron_Node **explicit_params = iron_parse_param_list(p, &explicit_count);

            /* INIT-11 parser branch: explicit return type is forbidden on
             * init. Emit a diagnostic and consume the `-> T` so later tokens
             * still parse as an init body. */
            if (iron_check(p, IRON_TOK_ARROW)) {
                iron_diag_emit(p->diags, p->arena, IRON_DIAG_ERROR,
                               IRON_ERR_UNEXPECTED_TOKEN,
                               iron_token_span(p, iron_current(p)),
                               "init cannot declare a return type - init "
                               "always returns Self", NULL);
                iron_advance(p);  /* consume '->' */
                (void)iron_parse_type_annotation(p);  /* discard the type */
            }

            Iron_Node *ibody = iron_parse_block(p);

            /* Synthesize self param; mirrors Phase 82 in-block synthesis. */
            Iron_Param *synth_self = ARENA_ALLOC(p->arena, Iron_Param);
            if (!synth_self) iron_oom_abort("parser.c:iron_parse_object_decl init synth self");
            synth_self->kind            = IRON_NODE_PARAM;
            synth_self->span            = iron_token_span(p, istart);
            synth_self->is_var          = false;
            /* Init ALWAYS writes self.field to establish initial values;
             * the receiver is mutating regardless of any tier modifier that
             * might have been rejected above. Plan 85-02 relies on this. */
            synth_self->is_mut_receiver = true;
            synth_self->name            = iron_arena_strdup(p->arena, "self", 4);
            if (!synth_self->name) iron_oom_abort("parser.c:iron_parse_object_decl init self name");

            Iron_TypeAnnotation *self_type = ARENA_ALLOC(p->arena, Iron_TypeAnnotation);
            if (!self_type) iron_oom_abort("parser.c:iron_parse_object_decl init self type");
            memset(self_type, 0, sizeof(*self_type));
            self_type->kind = IRON_NODE_TYPE_ANNOTATION;
            self_type->span = iron_token_span(p, istart);
            self_type->name = iron_arena_strdup(p->arena, name_tok->value,
                                                 strlen(name_tok->value));
            if (!self_type->name) iron_oom_abort("parser.c:iron_parse_object_decl init self type name");
            synth_self->type_ann = (Iron_Node *)self_type;

            int total = explicit_count + 1;
            Iron_Node **all_params = (Iron_Node **)iron_arena_alloc(
                p->arena, sizeof(Iron_Node *) * (size_t)total,
                _Alignof(Iron_Node *));
            if (!all_params) iron_oom_abort("parser.c:iron_parse_object_decl init params array");
            all_params[0] = (Iron_Node *)synth_self;
            for (int i = 0; i < explicit_count; i++) {
                all_params[i + 1] = explicit_params[i];
            }

            Iron_MethodDecl *m = ARENA_ALLOC(p->arena, Iron_MethodDecl);
            if (!m) iron_oom_abort("parser.c:iron_parse_object_decl init MethodDecl");
            m->kind                 = IRON_NODE_METHOD_DECL;
            m->span                 = iron_span_merge(iron_token_span(p, istart),
                                                       ibody ? ibody->span
                                                             : iron_token_span(p, istart));
            m->type_name            = iron_arena_strdup(p->arena, name_tok->value,
                                                         strlen(name_tok->value));
            if (!m->type_name) iron_oom_abort("parser.c:iron_parse_object_decl init type_name");
            m->method_name          = method_name;
            m->params               = all_params;
            m->param_count          = total;
            m->return_type          = NULL;   /* INIT-11: always NULL */
            m->body                 = ibody;
            m->is_private           = false;
            m->generic_params       = NULL;
            m->generic_param_count  = 0;
            m->resolved_return_type = NULL;
            m->owner_sym            = NULL;
            m->is_array_extension   = false;
            m->elem_type_name       = NULL;
            m->is_fusible           = false;
            m->is_receiver_form     = true;   /* triggers Phase 79 resolver */
            m->is_synth_accessor    = false;
            m->is_readonly          = false;
            m->is_pure              = false;
            m->is_init              = true;
            m->init_name            = init_name;  /* NULL for anonymous */

            if (extra_decls_out) {
                arrput(*extra_decls_out, (Iron_Node *)m);
            }
            iron_skip_newlines(p);
            continue;
        }

        /* Phase 82 GRAMMAR-01..04: in-block method declaration.
         * `func name(params) [-> ret] { body }` inside an object body
         * desugars to a top-level Iron_MethodDecl with is_receiver_form=true,
         * identical in shape to `func (r: T) name()` from Phase 79. The
         * synthesized `self` param is prepended to the explicit param list.
         * The resulting MethodDecl is pushed into *extra_decls_out; the
         * top-level iron_parse loop flushes it into program->decls
         * immediately after this ObjectDecl. */
        if (iron_check(p, IRON_TOK_FUNC)) {
            Iron_Token *fstart = iron_current(p);
            iron_advance(p);  /* consume 'func' */

            if (!iron_check(p, IRON_TOK_IDENTIFIER)) {
                iron_emit_diag(p, IRON_ERR_UNEXPECTED_TOKEN,
                               iron_token_span(p, iron_current(p)),
                               "expected method name in object body");
                iron_parser_sync_stmt(p);
                iron_skip_newlines(p);
                continue;
            }
            Iron_Token *mname_tok = iron_advance(p);

            /* Explicit params (no receiver in source; we synthesize self). */
            int explicit_count = 0;
            Iron_Node **explicit_params = iron_parse_param_list(p, &explicit_count);

            /* Optional return type */
            Iron_Node *mret = NULL;
            if (iron_match(p, IRON_TOK_ARROW)) {
                mret = iron_parse_type_annotation(p);
            }

            /* Body */
            Iron_Node *mbody = iron_parse_block(p);

            /* Synthesize the implicit `self` receiver param. Mirrors the
             * receiver-param synthesis pattern at parser.c:2392-2411. */
            Iron_Param *synth_self = ARENA_ALLOC(p->arena, Iron_Param);
            if (!synth_self) iron_oom_abort("parser.c:iron_parse_object_decl in-block synth self");
            synth_self->kind            = IRON_NODE_PARAM;
            synth_self->span            = iron_token_span(p, fstart);
            synth_self->is_var          = false;
            /* Phase 82 in-block method receivers are default-mutating per
             * CONTEXT.md: "Default-mutating receiver ABI uses pointer-receiver
             * from Phase 82 onward — matches v2.2 `mut` path." Phase 84 MUTTIER
             * flips is_mut_receiver off for readonly/pure methods: those tiers
             * cannot write self, so the receiver binding is logically non-mut
             * and MUT-04 (E0235) must NOT fire when a val-bound receiver calls
             * a readonly/pure method (MUTTIER-05 lock). Mutable-default methods
             * keep is_mut_receiver=true; readonly/pure methods get false. */
            synth_self->is_mut_receiver = !(member_is_readonly || member_is_pure);
            synth_self->name            = iron_arena_strdup(p->arena, "self", 4);
            if (!synth_self->name) iron_oom_abort("parser.c:iron_parse_object_decl in-block self name");

            /* Type annotation for self: the enclosing object type. */
            Iron_TypeAnnotation *self_type = ARENA_ALLOC(p->arena, Iron_TypeAnnotation);
            if (!self_type) iron_oom_abort("parser.c:iron_parse_object_decl in-block self type");
            memset(self_type, 0, sizeof(*self_type));
            self_type->kind = IRON_NODE_TYPE_ANNOTATION;
            self_type->span = iron_token_span(p, fstart);
            self_type->name = iron_arena_strdup(p->arena, name_tok->value,
                                                 strlen(name_tok->value));
            if (!self_type->name) iron_oom_abort("parser.c:iron_parse_object_decl in-block self type name");
            synth_self->type_ann = (Iron_Node *)self_type;

            /* Prepend synth_self to explicit params. */
            int total = explicit_count + 1;
            Iron_Node **all_params = (Iron_Node **)iron_arena_alloc(
                p->arena, sizeof(Iron_Node *) * (size_t)total,
                _Alignof(Iron_Node *));
            if (!all_params) iron_oom_abort("parser.c:iron_parse_object_decl in-block params array");
            all_params[0] = (Iron_Node *)synth_self;
            for (int i = 0; i < explicit_count; i++) {
                all_params[i + 1] = explicit_params[i];
            }

            /* Build the Iron_MethodDecl; mirror parser.c:2413-2436. */
            Iron_MethodDecl *m = ARENA_ALLOC(p->arena, Iron_MethodDecl);
            if (!m) iron_oom_abort("parser.c:iron_parse_object_decl in-block MethodDecl");
            m->kind                 = IRON_NODE_METHOD_DECL;
            m->span                 = iron_span_merge(iron_token_span(p, fstart),
                                                       mbody ? mbody->span
                                                             : iron_token_span(p, fstart));
            m->type_name            = iron_arena_strdup(p->arena, name_tok->value,
                                                         strlen(name_tok->value));
            if (!m->type_name) iron_oom_abort("parser.c:iron_parse_object_decl in-block type_name");
            m->method_name          = iron_arena_strdup(p->arena, mname_tok->value,
                                                         strlen(mname_tok->value));
            if (!m->method_name) iron_oom_abort("parser.c:iron_parse_object_decl in-block method_name");
            m->params               = all_params;
            m->param_count          = total;
            m->return_type          = mret;
            m->body                 = mbody;
            m->is_private           = false;  /* Phase 82: default public; Phase 83 adds pub opt-in */
            m->generic_params       = NULL;
            m->generic_param_count  = 0;
            m->resolved_return_type = NULL;
            m->owner_sym            = NULL;
            m->is_array_extension   = false;
            m->elem_type_name       = NULL;
            m->is_fusible           = false;
            m->is_receiver_form     = true;   /* CRITICAL: triggers Phase 79 resolver path */
            /* Phase 83-01: `pub` on methods is silently accepted — methods
             * default public in v2.2 so `pub func foo()` and `func foo()`
             * are semantically identical today. Phase 88 may reinterpret the
             * bit when the default flips to private. */
            (void)member_is_pub;
            /* Phase 83-01: default; Plan 83-02 flips on for synthesized
             * accessor methods; Phase 84 MUTTIER reads the bit. */
            m->is_synth_accessor    = false;
            /* Phase 84 MUTTIER-01/02/03: carry the tier modifier consumed by
             * the object body loop onto the AST. Plan 84-02 reads these bits
             * to fire tier-violation diagnostics (E0238..E0244). */
            m->is_readonly          = member_is_readonly;
            m->is_pure              = member_is_pure;
            /* Phase 85 INIT: Phase 82 in-block `func` is never init; the init
             * branch lives separately above. Defaults here for defensive
             * clarity so grep for is_init finds intent at every site. */
            m->is_init              = false;
            m->init_name            = NULL;

            if (extra_decls_out) {
                arrput(*extra_decls_out, (Iron_Node *)m);
            }
            iron_skip_newlines(p);
            continue;
        }

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

        /* Phase 88 BREAK-03: reject inline field defaults 'var x: T = expr' when
         * strict-v3 gate is ON. Consume and discard the initializer for recovery. */
        if (p->v3_strict_mode && iron_check(p, IRON_TOK_ASSIGN)) {
            iron_diag_emit(p->diags, p->arena, IRON_DIAG_ERROR,
                           IRON_ERR_V3_INLINE_DEFAULT,
                           iron_token_span(p, iron_current(p)),
                           "inline field defaults 'var x: T = expr' removed in v3.0; "
                           "assign fields in an init instead",
                           "run 'ironc migrate --from v2 --to v3 <file>' to migrate");
            iron_advance(p);  /* consume '=' */
            (void)iron_parse_expr(p);  /* discard initializer expression for recovery */
        }

        Iron_Field *field = ARENA_ALLOC(p->arena, Iron_Field);
        if (!field) { /* HARD-09 REPLACE (iron_parse_object_decl Field) */ p->in_error_recovery = true; return iron_make_error(p); }
        field->kind       = IRON_NODE_FIELD;
        field->span       = iron_span_merge(iron_token_span(p, field_start),
                                             ftype ? ftype->span
                                                   : iron_token_span(p, fname));
        field->name       = iron_arena_strdup(p->arena, fname->value,
                                               strlen(fname->value));
        if (!field->name) { /* HARD-09 REPLACE (iron_parse_object_decl Field name) */ field->name = "?"; }
        field->type_ann   = ftype;
        field->is_var     = is_var;
        /* Phase 83 ACCESS-02: is_pub carries the optional `pub` modifier
         * consumed earlier in this iteration. Plan 83-02 reads it to drive
         * accessor synthesis. */
        field->is_pub     = member_is_pub;
        arrput(fields, (Iron_Node *)field);
        field_count++;
        if (is_var) var_field_count++;
        iron_skip_newlines(p);
    }

    /* Phase 83-02 ACCESS-03/04/06: synthesize accessor methods for every
     * `pub val` and `pub var` field, then scan for user-declared methods that
     * collide with a synthesized name.
     *
     * Accessors are standard receiver-form Iron_MethodDecl nodes pushed into
     * *extra_decls_out — identical in shape to Phase 82 in-block methods —
     * so they ride the existing resolver / typecheck / HIR / LIR / emit
     * pipeline unchanged. Phase 84 MUTTIER reads is_synth_accessor to
     * retrofit `readonly` on getters.
     *
     * Getter  (pub val or pub var): `return self.<field>`  (1 param: self; return_type = field type)
     * Setter  (pub var only):       `self.<field> = _v`    (2 params: self + _v; return_type NULL)
     *
     * Collision scan runs AFTER synthesis so user methods flushed earlier in
     * this iteration are still reachable via *extra_decls_out. We compare
     * type_name + method_name and only emit on user-declared (non-synth)
     * methods whose type_name matches the enclosing object. */
    if (extra_decls_out) {
        /* Record the extra_decls_out range covered by this object so the
         * collision scan below does not pick up methods from earlier objects
         * in the program. The synthesis loop appends to the array; the scan
         * walks the full range and filters by type_name == enclosing. */
        const char *enclosing = name_tok->value;
        size_t enclosing_len  = strlen(enclosing);

        for (int fi = 0; fi < field_count; fi++) {
            Iron_Field *field = (Iron_Field *)fields[fi];
            if (!field->is_pub) continue;

            Iron_Span field_span = field->span;

            /* ── Getter ─────────────────────────────────────────────────── */
            Iron_Param *g_self = ARENA_ALLOC(p->arena, Iron_Param);
            if (!g_self) iron_oom_abort("parser.c:iron_parse_object_decl synth getter self");
            g_self->kind            = IRON_NODE_PARAM;
            g_self->span            = field_span;
            g_self->is_var          = false;
            /* Getter: read-only receiver. Phase 84 will read this bit via
             * is_synth_accessor + is_mut_receiver to confirm readonly. */
            g_self->is_mut_receiver = false;
            g_self->name            = iron_arena_strdup(p->arena, "self", 4);
            if (!g_self->name) iron_oom_abort("parser.c:iron_parse_object_decl synth getter self name");

            Iron_TypeAnnotation *g_self_ty = ARENA_ALLOC(p->arena, Iron_TypeAnnotation);
            if (!g_self_ty) iron_oom_abort("parser.c:iron_parse_object_decl synth getter self type");
            memset(g_self_ty, 0, sizeof(*g_self_ty));
            g_self_ty->kind = IRON_NODE_TYPE_ANNOTATION;
            g_self_ty->span = field_span;
            g_self_ty->name = iron_arena_strdup(p->arena, enclosing, enclosing_len);
            if (!g_self_ty->name) iron_oom_abort("parser.c:iron_parse_object_decl synth getter self type name");
            g_self->type_ann = (Iron_Node *)g_self_ty;

            /* Getter body: `return self.<field>` */
            Iron_Ident *g_self_ref = ARENA_ALLOC(p->arena, Iron_Ident);
            if (!g_self_ref) iron_oom_abort("parser.c:iron_parse_object_decl synth getter self ref");
            g_self_ref->kind             = IRON_NODE_IDENT;
            g_self_ref->span             = field_span;
            g_self_ref->resolved_type    = NULL;
            g_self_ref->name             = iron_arena_strdup(p->arena, "self", 4);
            if (!g_self_ref->name) iron_oom_abort("parser.c:iron_parse_object_decl synth getter self ref name");
            g_self_ref->resolved_sym     = NULL;
            g_self_ref->constraint_name  = NULL;

            Iron_FieldAccess *g_fa = ARENA_ALLOC(p->arena, Iron_FieldAccess);
            if (!g_fa) iron_oom_abort("parser.c:iron_parse_object_decl synth getter field access");
            g_fa->kind          = IRON_NODE_FIELD_ACCESS;
            g_fa->span          = field_span;
            g_fa->resolved_type = NULL;
            g_fa->object        = (Iron_Node *)g_self_ref;
            g_fa->field         = field->name;
            /* Synth getter body: direct field load, is_pub_access stays
             * false so HIR does NOT recurse through this same getter. */
            g_fa->is_pub_access = false;

            Iron_ReturnStmt *g_ret = ARENA_ALLOC(p->arena, Iron_ReturnStmt);
            if (!g_ret) iron_oom_abort("parser.c:iron_parse_object_decl synth getter return");
            g_ret->kind  = IRON_NODE_RETURN;
            g_ret->span  = field_span;
            g_ret->value = (Iron_Node *)g_fa;

            Iron_Block *g_body = ARENA_ALLOC(p->arena, Iron_Block);
            if (!g_body) iron_oom_abort("parser.c:iron_parse_object_decl synth getter body");
            g_body->kind       = IRON_NODE_BLOCK;
            g_body->span       = field_span;
            g_body->stmts      = NULL;
            g_body->stmt_count = 0;
            arrput(g_body->stmts, (Iron_Node *)g_ret);
            g_body->stmt_count = 1;

            /* Getter params array: [self] */
            Iron_Node **g_params = (Iron_Node **)iron_arena_alloc(
                p->arena, sizeof(Iron_Node *),
                _Alignof(Iron_Node *));
            if (!g_params) iron_oom_abort("parser.c:iron_parse_object_decl synth getter params");
            g_params[0] = (Iron_Node *)g_self;

            Iron_MethodDecl *g_m = ARENA_ALLOC(p->arena, Iron_MethodDecl);
            if (!g_m) iron_oom_abort("parser.c:iron_parse_object_decl synth getter MethodDecl");
            g_m->kind                 = IRON_NODE_METHOD_DECL;
            g_m->span                 = field_span;
            g_m->type_name            = iron_arena_strdup(p->arena, enclosing, enclosing_len);
            if (!g_m->type_name) iron_oom_abort("parser.c:iron_parse_object_decl synth getter type_name");
            g_m->method_name          = field->name;  /* arena-owned, safe to share */
            g_m->params               = g_params;
            g_m->param_count          = 1;
            /* Sharing field->type_ann is safe: types are read-only post-parse
             * so the AST printer and downstream passes treat this as a fresh
             * read of the same annotation. Matches Phase 82's shared-type
             * pattern for in-block method receivers. */
            g_m->return_type          = field->type_ann;
            g_m->body                 = (Iron_Node *)g_body;
            g_m->is_private           = false;
            g_m->generic_params       = NULL;
            g_m->generic_param_count  = 0;
            g_m->resolved_return_type = NULL;
            g_m->owner_sym            = NULL;
            g_m->is_array_extension   = false;
            g_m->elem_type_name       = NULL;
            g_m->is_fusible           = false;
            g_m->is_receiver_form     = true;
            g_m->is_synth_accessor    = true;  /* Phase 83-02 marker */
            /* Phase 84 MUTTIER retrofit: a synthesized getter is a read-only
             * field load. Mark both tiers true so readonly AND pure methods
             * can call `obj.field` through this getter without tripping
             * E0239 (readonly calls mutating) or E0242 (pure calls non-pure)
             * in Plan 84-02. */
            g_m->is_readonly          = true;
            g_m->is_pure              = true;
            /* Phase 85 INIT: synth getter is never init - it's an accessor
             * over a pre-existing field, not a constructor. */
            g_m->is_init              = false;
            g_m->init_name            = NULL;
            arrput(*extra_decls_out, (Iron_Node *)g_m);

            if (!field->is_var) continue;

            /* ── Setter (pub var only) ──────────────────────────────────── */
            Iron_Param *s_self = ARENA_ALLOC(p->arena, Iron_Param);
            if (!s_self) iron_oom_abort("parser.c:iron_parse_object_decl synth setter self");
            s_self->kind            = IRON_NODE_PARAM;
            s_self->span            = field_span;
            s_self->is_var          = false;
            s_self->is_mut_receiver = true;   /* setter mutates self.field */
            s_self->name            = iron_arena_strdup(p->arena, "self", 4);
            if (!s_self->name) iron_oom_abort("parser.c:iron_parse_object_decl synth setter self name");

            Iron_TypeAnnotation *s_self_ty = ARENA_ALLOC(p->arena, Iron_TypeAnnotation);
            if (!s_self_ty) iron_oom_abort("parser.c:iron_parse_object_decl synth setter self type");
            memset(s_self_ty, 0, sizeof(*s_self_ty));
            s_self_ty->kind = IRON_NODE_TYPE_ANNOTATION;
            s_self_ty->span = field_span;
            s_self_ty->name = iron_arena_strdup(p->arena, enclosing, enclosing_len);
            if (!s_self_ty->name) iron_oom_abort("parser.c:iron_parse_object_decl synth setter self type name");
            s_self->type_ann = (Iron_Node *)s_self_ty;

            Iron_Param *s_v = ARENA_ALLOC(p->arena, Iron_Param);
            if (!s_v) iron_oom_abort("parser.c:iron_parse_object_decl synth setter v");
            s_v->kind            = IRON_NODE_PARAM;
            s_v->span            = field_span;
            s_v->is_var          = false;
            s_v->is_mut_receiver = false;
            s_v->name            = iron_arena_strdup(p->arena, "_v", 2);
            if (!s_v->name) iron_oom_abort("parser.c:iron_parse_object_decl synth setter v name");
            /* Reuse the field's type_ann for `_v` — read-only shared. */
            s_v->type_ann = field->type_ann;

            /* Setter body: `self.<field> = _v` */
            Iron_Ident *s_self_ref = ARENA_ALLOC(p->arena, Iron_Ident);
            if (!s_self_ref) iron_oom_abort("parser.c:iron_parse_object_decl synth setter self ref");
            s_self_ref->kind            = IRON_NODE_IDENT;
            s_self_ref->span            = field_span;
            s_self_ref->resolved_type   = NULL;
            s_self_ref->name            = iron_arena_strdup(p->arena, "self", 4);
            if (!s_self_ref->name) iron_oom_abort("parser.c:iron_parse_object_decl synth setter self ref name");
            s_self_ref->resolved_sym    = NULL;
            s_self_ref->constraint_name = NULL;

            Iron_FieldAccess *s_fa = ARENA_ALLOC(p->arena, Iron_FieldAccess);
            if (!s_fa) iron_oom_abort("parser.c:iron_parse_object_decl synth setter field access");
            s_fa->kind          = IRON_NODE_FIELD_ACCESS;
            s_fa->span          = field_span;
            s_fa->resolved_type = NULL;
            s_fa->object        = (Iron_Node *)s_self_ref;
            s_fa->field         = field->name;
            /* Synth setter body: direct field store target; the typechecker
             * path inside a synth accessor suppresses the rewrite so this
             * stays false and HIR emits a plain store, not a re-dispatch. */
            s_fa->is_pub_access = false;

            Iron_Ident *s_v_ref = ARENA_ALLOC(p->arena, Iron_Ident);
            if (!s_v_ref) iron_oom_abort("parser.c:iron_parse_object_decl synth setter v ref");
            s_v_ref->kind            = IRON_NODE_IDENT;
            s_v_ref->span            = field_span;
            s_v_ref->resolved_type   = NULL;
            s_v_ref->name            = iron_arena_strdup(p->arena, "_v", 2);
            if (!s_v_ref->name) iron_oom_abort("parser.c:iron_parse_object_decl synth setter v ref name");
            s_v_ref->resolved_sym    = NULL;
            s_v_ref->constraint_name = NULL;

            Iron_AssignStmt *s_as = ARENA_ALLOC(p->arena, Iron_AssignStmt);
            if (!s_as) iron_oom_abort("parser.c:iron_parse_object_decl synth setter assign");
            s_as->kind          = IRON_NODE_ASSIGN;
            s_as->span          = field_span;
            s_as->target        = (Iron_Node *)s_fa;
            s_as->value         = (Iron_Node *)s_v_ref;
            s_as->op            = IRON_TOK_ASSIGN;
            /* Synth setter body's own assign: direct store, not a recursive
             * dispatch. The typechecker's synth-accessor guard leaves this
             * false so HIR emits `set_field %self.field, _v`. */
            s_as->is_pub_setter = false;

            Iron_Block *s_body = ARENA_ALLOC(p->arena, Iron_Block);
            if (!s_body) iron_oom_abort("parser.c:iron_parse_object_decl synth setter body");
            s_body->kind       = IRON_NODE_BLOCK;
            s_body->span       = field_span;
            s_body->stmts      = NULL;
            s_body->stmt_count = 0;
            arrput(s_body->stmts, (Iron_Node *)s_as);
            s_body->stmt_count = 1;

            Iron_Node **s_params = (Iron_Node **)iron_arena_alloc(
                p->arena, sizeof(Iron_Node *) * 2,
                _Alignof(Iron_Node *));
            if (!s_params) iron_oom_abort("parser.c:iron_parse_object_decl synth setter params array");
            s_params[0] = (Iron_Node *)s_self;
            s_params[1] = (Iron_Node *)s_v;

            /* method_name = "set_<field>" */
            size_t fname_len = strlen(field->name);
            char  *setter_name = (char *)iron_arena_alloc(
                p->arena, fname_len + 5 /* "set_" + NUL */,
                _Alignof(char));
            if (!setter_name) iron_oom_abort("parser.c:iron_parse_object_decl synth setter name");
            snprintf(setter_name, fname_len + 5, "set_%s", field->name);

            Iron_MethodDecl *s_m = ARENA_ALLOC(p->arena, Iron_MethodDecl);
            if (!s_m) iron_oom_abort("parser.c:iron_parse_object_decl synth setter MethodDecl");
            s_m->kind                 = IRON_NODE_METHOD_DECL;
            s_m->span                 = field_span;
            s_m->type_name            = iron_arena_strdup(p->arena, enclosing, enclosing_len);
            if (!s_m->type_name) iron_oom_abort("parser.c:iron_parse_object_decl synth setter type_name");
            s_m->method_name          = setter_name;
            s_m->params               = s_params;
            s_m->param_count          = 2;
            s_m->return_type          = NULL;  /* setter returns void */
            s_m->body                 = (Iron_Node *)s_body;
            s_m->is_private           = false;
            s_m->generic_params       = NULL;
            s_m->generic_param_count  = 0;
            s_m->resolved_return_type = NULL;
            s_m->owner_sym            = NULL;
            s_m->is_array_extension   = false;
            s_m->elem_type_name       = NULL;
            s_m->is_fusible           = false;
            s_m->is_receiver_form     = true;
            s_m->is_synth_accessor    = true;
            /* Phase 84 MUTTIER: synth setter writes self.field — default
             * mutating. Spell the false-false explicitly for audit; arena
             * zero-init already gives this, but future readers should see
             * the intent on the line, not inferred. */
            s_m->is_readonly          = false;
            s_m->is_pure              = false;
            /* Phase 85 INIT: synth setter is never init - it writes a single
             * field rather than constructing the object. */
            s_m->is_init              = false;
            s_m->init_name            = NULL;
            arrput(*extra_decls_out, (Iron_Node *)s_m);
        }

        /* ── Collision scan (ACCESS-06) ─────────────────────────────────────
         * Walk the *extra_decls_out array. For every user-declared
         * (is_synth_accessor == false) Iron_MethodDecl whose type_name
         * matches the enclosing object, check whether its method_name
         * collides with any synthesized accessor's method_name in the same
         * object. On collision, emit IRON_ERR_ACCESSOR_NAME_RESERVED (237)
         * with the locked message text.
         *
         * Both the synthesized node and the user node remain in the AST;
         * the diagnostic blocks compilation downstream. A duplicate-method
         * detection in the resolver is subsumed by this cleaner message. */
        int extra_count = (int)arrlen(*extra_decls_out);
        for (int ui = 0; ui < extra_count; ui++) {
            Iron_Node *un = (*extra_decls_out)[ui];
            if (!un || un->kind != IRON_NODE_METHOD_DECL) continue;
            Iron_MethodDecl *um = (Iron_MethodDecl *)un;
            if (um->is_synth_accessor) continue;
            if (!um->type_name || strcmp(um->type_name, enclosing) != 0) continue;

            for (int si = 0; si < extra_count; si++) {
                if (si == ui) continue;
                Iron_Node *sn = (*extra_decls_out)[si];
                if (!sn || sn->kind != IRON_NODE_METHOD_DECL) continue;
                Iron_MethodDecl *sm = (Iron_MethodDecl *)sn;
                if (!sm->is_synth_accessor) continue;
                if (!sm->type_name || strcmp(sm->type_name, enclosing) != 0) continue;
                if (!sm->method_name || !um->method_name) continue;
                if (strcmp(sm->method_name, um->method_name) != 0) continue;

                char msg[256];
                snprintf(msg, sizeof(msg),
                         "name '%s' reserved by synthesized accessor from pub field",
                         um->method_name);
                iron_diag_emit(p->diags, p->arena, IRON_DIAG_ERROR,
                               IRON_ERR_ACCESSOR_NAME_RESERVED,
                               um->span, msg, NULL);
                break;  /* one diagnostic per colliding user method */
            }
        }

        /* Phase 85 INIT-07: duplicate-init detection scan.
         *
         * After the accessor collision scan (which already walked
         * extra_decls_out), we run a second O(N^2) pass restricted to
         * is_init=true MethodDecls whose type_name matches the enclosing
         * object. Two classes of duplicate:
         *   (1) two anonymous inits (init_name NULL on both) -> E0201 with
         *       "duplicate anonymous init; only one allowed per object"
         *   (2) two named inits with the same init_name -> E0201 with
         *       "duplicate named init '<name>'"
         *
         * A diagnostic fires on the LATER of each duplicate pair (ui > si)
         * so the first-declared init is the "canonical" one referenced by
         * any Plan 85-02 downstream analysis that walks the array. */
        int init_scan_count = (int)arrlen(*extra_decls_out);
        for (int ui = 0; ui < init_scan_count; ui++) {
            Iron_Node *un = (*extra_decls_out)[ui];
            if (!un || un->kind != IRON_NODE_METHOD_DECL) continue;
            Iron_MethodDecl *um = (Iron_MethodDecl *)un;
            if (!um->is_init) continue;
            if (!um->type_name || strcmp(um->type_name, enclosing) != 0) continue;

            for (int si = 0; si < ui; si++) {
                Iron_Node *sn = (*extra_decls_out)[si];
                if (!sn || sn->kind != IRON_NODE_METHOD_DECL) continue;
                Iron_MethodDecl *sm = (Iron_MethodDecl *)sn;
                if (!sm->is_init) continue;
                if (!sm->type_name || strcmp(sm->type_name, enclosing) != 0) continue;

                /* Both anonymous (init_name NULL on both)? */
                if (sm->init_name == NULL && um->init_name == NULL) {
                    iron_diag_emit(p->diags, p->arena, IRON_DIAG_ERROR,
                                   IRON_ERR_DUPLICATE_DECL,
                                   um->span,
                                   "duplicate anonymous init; only one "
                                   "allowed per object", NULL);
                    break;
                }
                /* Both named with same init_name? */
                if (sm->init_name && um->init_name &&
                    strcmp(sm->init_name, um->init_name) == 0) {
                    char msg[256];
                    snprintf(msg, sizeof(msg),
                             "duplicate named init '%s'",
                             um->init_name);
                    iron_diag_emit(p->diags, p->arena, IRON_DIAG_ERROR,
                                   IRON_ERR_DUPLICATE_DECL,
                                   um->span, msg, NULL);
                    break;
                }
            }
        }

        /* Phase 85 INIT-13: field-less auto-synth.
         *
         * An object with zero fields AND zero user-declared inits receives
         * a synthesized empty `init() {}` so every object has exactly one
         * callable init even before Phase 88 flips the mandatory-init gate.
         * This preserves pure-superset during Phase 85-87: v2.2 field-less
         * objects still "just work" via the positional-constructor path;
         * adding the synth here makes is_init visible on the AST so later
         * phases can rely on it.
         *
         * The synth shape mirrors user-declared anonymous init:
         *   is_init = true, init_name = NULL, method_name = "init",
         *   param_count = 1 (synth self), body = empty Iron_Block,
         *   is_receiver_form = true, is_mut_receiver on self. */
        if (field_count == 0) {
            bool has_user_init = false;
            for (int i = 0; i < (int)arrlen(*extra_decls_out); i++) {
                Iron_Node *d = (*extra_decls_out)[i];
                if (!d || d->kind != IRON_NODE_METHOD_DECL) continue;
                Iron_MethodDecl *mm = (Iron_MethodDecl *)d;
                if (mm->is_init && mm->type_name &&
                    strcmp(mm->type_name, enclosing) == 0) {
                    has_user_init = true;
                    break;
                }
            }
            if (!has_user_init) {
                Iron_Span ospan = iron_token_span(p, name_tok);

                Iron_Block *empty_body = ARENA_ALLOC(p->arena, Iron_Block);
                if (!empty_body) iron_oom_abort("parser.c:iron_parse_object_decl fieldless init body");
                empty_body->kind       = IRON_NODE_BLOCK;
                empty_body->span       = ospan;
                empty_body->stmts      = NULL;
                empty_body->stmt_count = 0;

                Iron_Param *ss = ARENA_ALLOC(p->arena, Iron_Param);
                if (!ss) iron_oom_abort("parser.c:iron_parse_object_decl fieldless self");
                ss->kind            = IRON_NODE_PARAM;
                ss->span            = ospan;
                ss->is_var          = false;
                ss->is_mut_receiver = true;
                ss->name            = iron_arena_strdup(p->arena, "self", 4);
                if (!ss->name) iron_oom_abort("parser.c:iron_parse_object_decl fieldless self name");

                Iron_TypeAnnotation *sty = ARENA_ALLOC(p->arena, Iron_TypeAnnotation);
                if (!sty) iron_oom_abort("parser.c:iron_parse_object_decl fieldless self ty");
                memset(sty, 0, sizeof(*sty));
                sty->kind = IRON_NODE_TYPE_ANNOTATION;
                sty->span = ospan;
                sty->name = iron_arena_strdup(p->arena, enclosing, enclosing_len);
                if (!sty->name) iron_oom_abort("parser.c:iron_parse_object_decl fieldless self ty name");
                ss->type_ann = (Iron_Node *)sty;

                Iron_Node **sparams = (Iron_Node **)iron_arena_alloc(
                    p->arena, sizeof(Iron_Node *), _Alignof(Iron_Node *));
                if (!sparams) iron_oom_abort("parser.c:iron_parse_object_decl fieldless params");
                sparams[0] = (Iron_Node *)ss;

                Iron_MethodDecl *synth = ARENA_ALLOC(p->arena, Iron_MethodDecl);
                if (!synth) iron_oom_abort("parser.c:iron_parse_object_decl fieldless synth init");
                synth->kind                 = IRON_NODE_METHOD_DECL;
                synth->span                 = ospan;
                synth->type_name            = iron_arena_strdup(p->arena, enclosing, enclosing_len);
                if (!synth->type_name) iron_oom_abort("parser.c:iron_parse_object_decl fieldless synth type_name");
                synth->method_name          = iron_arena_strdup(p->arena, "init", 4);
                if (!synth->method_name) iron_oom_abort("parser.c:iron_parse_object_decl fieldless synth method_name");
                synth->params               = sparams;
                synth->param_count          = 1;
                synth->return_type          = NULL;
                synth->body                 = (Iron_Node *)empty_body;
                synth->is_private           = false;
                synth->generic_params       = NULL;
                synth->generic_param_count  = 0;
                synth->resolved_return_type = NULL;
                synth->owner_sym            = NULL;
                synth->is_array_extension   = false;
                synth->elem_type_name       = NULL;
                synth->is_fusible           = false;
                synth->is_receiver_form     = true;
                synth->is_synth_accessor    = false;
                synth->is_readonly          = false;
                synth->is_pure              = false;
                synth->is_init              = true;
                synth->init_name            = NULL;  /* anonymous */
                arrput(*extra_decls_out, (Iron_Node *)synth);
            }
        }

        /* Phase 88 INIT-02: when strict-v3 gate is ON, an object with mutable (var)
         * fields but no user-declared init is an error. val-only objects are treated as
         * C-layout types whose lifetime is managed by C (e.g. raylib structs); they do
         * not need an Iron init constructor because they are never constructed from Iron
         * code directly. Only var fields require assignment in an init body. */
        if (p->v3_strict_mode && var_field_count > 0) {
            bool has_user_init = false;
            for (int i = 0; i < (int)arrlen(*extra_decls_out); i++) {
                Iron_Node *d = (*extra_decls_out)[i];
                if (!d || d->kind != IRON_NODE_METHOD_DECL) continue;
                Iron_MethodDecl *mm = (Iron_MethodDecl *)d;
                if (mm->is_init && !mm->is_synth_accessor && mm->type_name &&
                    strcmp(mm->type_name, enclosing) == 0) {
                    has_user_init = true;
                    break;
                }
            }
            if (!has_user_init) {
                char msg[256];
                snprintf(msg, sizeof(msg),
                         "object '%s' has %d mutable field(s) but no init; "
                         "v3.0 requires an explicit init to construct objects with mutable fields",
                         enclosing, var_field_count);
                iron_diag_emit(p->diags, p->arena, IRON_DIAG_ERROR,
                               IRON_ERR_V3_NO_INIT,
                               iron_token_span(p, name_tok),
                               msg,
                               "run 'ironc migrate --from v2 --to v3 <file>' to migrate");
            }
        }
    }

    Iron_Token *end = iron_current(p);
    iron_expect(p, IRON_TOK_RBRACE);

    Iron_ObjectDecl *n         = ARENA_ALLOC(p->arena, Iron_ObjectDecl);
    if (!n) { /* HARD-09 REPLACE (iron_parse_object_decl ObjectDecl) */ p->in_error_recovery = true; return iron_make_error(p); }
    n->kind                    = IRON_NODE_OBJECT_DECL;
    n->span                    = iron_span_merge(iron_token_span(p, start),
                                                  iron_token_span(p, end));
    n->name                    = iron_arena_strdup(p->arena, name_tok->value,
                                                    strlen(name_tok->value));
    if (!n->name) { /* HARD-09 REPLACE (iron_parse_object_decl ObjectDecl name) */ n->name = "?"; }
    /* FIX-03 / AUDIT-04 §1: SAFETY — stb_ds `fields` and `impl_names` arrays
     * ownership-transferred to arena-allocated ObjectDecl; file-header. */
    n->fields                  = fields;
    n->field_count             = field_count;
    n->extends_name            = extends_name;
    n->implements_names        = impl_names;
    n->implements_count        = impl_count;
    n->generic_params          = generic_params;
    n->generic_param_count     = generic_count;
    /* Phase 86 PATCH-01: classic `object T {}` is never a patch. Defensive
     * explicit defaults so Plan 86-02 resolver walking decls for
     * is_patch=true has no uninitialized reads. ARENA_ALLOC zeroes the
     * struct, but spelling these out documents intent at the only classic
     * alloc site. */
    n->is_patch                = false;
    n->target_type_name        = NULL;
    (void)is_private;  /* stored but not used in AST yet */
    return (Iron_Node *)n;
}

/* ── Phase 86 PATCH-01/02/05: patch object T { ... } ─────────────────────── */
/*
 * `patch object T { methods+inits }` contributes methods (and optionally
 * inits) to an existing type T without modifying T's original decl. The
 * resulting AST node is an Iron_ObjectDecl with is_patch=true,
 * target_type_name=T, field_count=0 — Plan 86-02 walks program decls
 * keying on is_patch to wire the type-patch registry and extend dispatch.
 *
 * Body grammar mirrors the object-body method/init branches (Phase 82 in-
 * block func, Phase 83 pub, Phase 84 readonly/pure, Phase 85 init). The
 * difference: field declarations (val/var) inside a patch body emit E0253
 * IRON_ERR_PATCH_ADDS_FIELD and are consumed-and-dropped for recovery;
 * the resulting ObjectDecl always has field_count=0.
 *
 * Generic patch targets (`patch object List[T] { ... }`) are deferred to
 * v3.1+ per 86-CONTEXT.md Deferred Ideas and rejected with a locked
 * IRON_ERR_UNEXPECTED_TOKEN message.
 *
 * Body-consumption is an inline replay of the object-body loop rather
 * than a shared helper. The plan's <interfaces> block permits either
 * "extracted helper or inline replay"; replay keeps iron_parse_object_decl
 * 100% untouched (strongest regression guarantee for the 70+ existing
 * object/method parser tests) at the cost of ~duplicating the body-
 * consumption arm. Phase 87+ may refactor to a shared helper if another
 * consumer appears (`trait` extension would be the candidate).
 */
static Iron_Node *iron_parse_patch_decl(Iron_Parser *p,
                                        Iron_Node ***extra_decls_out) {
    Iron_Token *start = iron_current(p);
    iron_advance(p);  /* consume 'patch' */

    /* Expect `object` next. Locked diagnostic on mismatch. */
    if (!iron_check(p, IRON_TOK_OBJECT)) {
        iron_diag_emit(p->diags, p->arena, IRON_DIAG_ERROR,
                       IRON_ERR_UNEXPECTED_TOKEN,
                       iron_token_span(p, iron_current(p)),
                       "expected 'object' after 'patch'", NULL);
        p->in_error_recovery = true;
        iron_parser_sync_toplevel(p);
        return iron_make_error(p);
    }
    iron_advance(p);  /* consume 'object' */

    /* Patch target name. */
    if (!iron_check(p, IRON_TOK_IDENTIFIER)) {
        iron_diag_emit(p->diags, p->arena, IRON_DIAG_ERROR,
                       IRON_ERR_UNEXPECTED_TOKEN,
                       iron_token_span(p, iron_current(p)),
                       "expected patch target name", NULL);
        p->in_error_recovery = true;
        iron_parser_sync_toplevel(p);
        return iron_make_error(p);
    }
    Iron_Token *name_tok = iron_advance(p);
    const char *target_name = iron_arena_strdup(p->arena, name_tok->value,
                                                 strlen(name_tok->value));
    if (!target_name) iron_oom_abort("parser.c:iron_parse_patch_decl target_name");

    /* Generic patch targets (`patch object List[T] { ... }`) are deferred
     * to v3.1+ per 86-CONTEXT.md. Emit locked rejection and consume the
     * [T] tokens so the body still parses cleanly. */
    if (iron_check(p, IRON_TOK_LBRACKET)) {
        iron_diag_emit(p->diags, p->arena, IRON_DIAG_ERROR,
                       IRON_ERR_UNEXPECTED_TOKEN,
                       iron_token_span(p, iron_current(p)),
                       "generic patch targets not supported in v3.0", NULL);
        int generic_count = 0;
        (void)iron_parse_generic_params(p, &generic_count, p->arena);
    }

    /* Phase 87-02 PATCH-08: optional `implements I, J, K` clause.
     * "implements" is a contextual keyword lexed as IRON_TOK_IDENTIFIER;
     * we detect it by string comparison, matching the Phase 85 init-as-
     * contextual-keyword precedent. Patches do NOT accept `extends`. */
    const char **impl_names = NULL;
    int          impl_count = 0;
    if (iron_check(p, IRON_TOK_IDENTIFIER) &&
        strcmp(iron_current(p)->value, "implements") == 0) {
        iron_advance(p);  /* consume 'implements' */
        iron_skip_newlines(p);
        while (iron_check(p, IRON_TOK_IDENTIFIER)) {
            Iron_Token *it = iron_advance(p);
            const char *iname = iron_arena_strdup(p->arena, it->value,
                                                   strlen(it->value));
            if (!iname) iron_oom_abort("parser.c:iron_parse_patch_decl impl iname");
            arrput(impl_names, iname);
            impl_count++;
            iron_skip_newlines(p);
            if (!iron_match(p, IRON_TOK_COMMA)) break;
            iron_skip_newlines(p);
        }
    }

    /* Body. Patches do NOT accept `extends`. */
    if (!iron_expect(p, IRON_TOK_LBRACE)) return iron_make_error(p);
    iron_skip_newlines(p);

    while (!iron_check(p, IRON_TOK_RBRACE) && !iron_check(p, IRON_TOK_EOF)) {
        iron_skip_newlines(p);
        if (iron_check(p, IRON_TOK_RBRACE)) break;

        /* Optional pub modifier (Phase 83). */
        bool member_is_pub = false;
        if (iron_check(p, IRON_TOK_PUB)) {
            iron_advance(p);
            member_is_pub = true;
        }

        /* Optional readonly/pure modifier (Phase 84). */
        bool member_is_readonly = false;
        bool member_is_pure     = false;
        if (iron_check(p, IRON_TOK_READONLY)) {
            iron_advance(p);
            member_is_readonly = true;
            if (iron_check(p, IRON_TOK_PURE)) {
                iron_diag_emit(p->diags, p->arena, IRON_DIAG_ERROR,
                               IRON_ERR_TIER_MODIFIER_PLACEMENT,
                               iron_token_span(p, iron_current(p)),
                               "method cannot be both 'readonly' and 'pure' "
                               "- pick one tier", NULL);
                iron_advance(p);
            }
        } else if (iron_check(p, IRON_TOK_PURE)) {
            iron_advance(p);
            member_is_pure = true;
            if (iron_check(p, IRON_TOK_READONLY)) {
                iron_diag_emit(p->diags, p->arena, IRON_DIAG_ERROR,
                               IRON_ERR_TIER_MODIFIER_PLACEMENT,
                               iron_token_span(p, iron_current(p)),
                               "method cannot be both 'pure' and 'readonly' "
                               "- pick one tier", NULL);
                iron_advance(p);
            }
        }

        /* Phase 85 INIT inside a patch body: same grammar as classic
         * object body. `readonly init` / `pure init` / `pub init` all
         * rejected via the same precedent paths. */
        if (iron_check(p, IRON_TOK_INIT)) {
            Iron_Token *istart = iron_current(p);
            if (member_is_pub) {
                iron_diag_emit(p->diags, p->arena, IRON_DIAG_ERROR,
                               IRON_ERR_UNEXPECTED_TOKEN,
                               iron_token_span(p, istart),
                               "init visibility is tied to its object; "
                               "cannot be marked pub", NULL);
            }
            if (member_is_readonly || member_is_pure) {
                iron_diag_emit(p->diags, p->arena, IRON_DIAG_ERROR,
                               IRON_ERR_TIER_MODIFIER_PLACEMENT,
                               iron_token_span(p, istart),
                               "init cannot be 'readonly' or 'pure' - init "
                               "always writes self to initialize fields",
                               NULL);
                member_is_readonly = false;
                member_is_pure     = false;
            }
            iron_advance(p);  /* consume 'init' */

            const char *init_name   = NULL;
            const char *method_name = NULL;
            if (iron_check(p, IRON_TOK_IDENTIFIER)) {
                Iron_Token *nt = iron_advance(p);
                init_name = iron_arena_strdup(p->arena, nt->value,
                                               strlen(nt->value));
                if (!init_name) iron_oom_abort("parser.c:iron_parse_patch_decl init name");
                method_name = init_name;
            } else {
                method_name = iron_arena_strdup(p->arena, "init", 4);
                if (!method_name) iron_oom_abort("parser.c:iron_parse_patch_decl anon init name");
            }

            int explicit_count = 0;
            Iron_Node **explicit_params = iron_parse_param_list(p, &explicit_count);

            if (iron_check(p, IRON_TOK_ARROW)) {
                iron_diag_emit(p->diags, p->arena, IRON_DIAG_ERROR,
                               IRON_ERR_UNEXPECTED_TOKEN,
                               iron_token_span(p, iron_current(p)),
                               "init cannot declare a return type - init "
                               "always returns Self", NULL);
                iron_advance(p);
                (void)iron_parse_type_annotation(p);
            }

            Iron_Node *ibody = iron_parse_block(p);

            Iron_Param *synth_self = ARENA_ALLOC(p->arena, Iron_Param);
            if (!synth_self) iron_oom_abort("parser.c:iron_parse_patch_decl init synth self");
            synth_self->kind            = IRON_NODE_PARAM;
            synth_self->span            = iron_token_span(p, istart);
            synth_self->is_var          = false;
            synth_self->is_mut_receiver = true;
            synth_self->name            = iron_arena_strdup(p->arena, "self", 4);
            if (!synth_self->name) iron_oom_abort("parser.c:iron_parse_patch_decl init self name");

            Iron_TypeAnnotation *self_type = ARENA_ALLOC(p->arena, Iron_TypeAnnotation);
            if (!self_type) iron_oom_abort("parser.c:iron_parse_patch_decl init self type");
            memset(self_type, 0, sizeof(*self_type));
            self_type->kind = IRON_NODE_TYPE_ANNOTATION;
            self_type->span = iron_token_span(p, istart);
            self_type->name = iron_arena_strdup(p->arena, target_name,
                                                 strlen(target_name));
            if (!self_type->name) iron_oom_abort("parser.c:iron_parse_patch_decl init self type name");
            synth_self->type_ann = (Iron_Node *)self_type;

            int total = explicit_count + 1;
            Iron_Node **all_params = (Iron_Node **)iron_arena_alloc(
                p->arena, sizeof(Iron_Node *) * (size_t)total,
                _Alignof(Iron_Node *));
            if (!all_params) iron_oom_abort("parser.c:iron_parse_patch_decl init params array");
            all_params[0] = (Iron_Node *)synth_self;
            for (int i = 0; i < explicit_count; i++) {
                all_params[i + 1] = explicit_params[i];
            }

            Iron_MethodDecl *m = ARENA_ALLOC(p->arena, Iron_MethodDecl);
            if (!m) iron_oom_abort("parser.c:iron_parse_patch_decl init MethodDecl");
            m->kind                 = IRON_NODE_METHOD_DECL;
            m->span                 = iron_span_merge(iron_token_span(p, istart),
                                                       ibody ? ibody->span
                                                             : iron_token_span(p, istart));
            m->type_name            = iron_arena_strdup(p->arena, target_name,
                                                         strlen(target_name));
            if (!m->type_name) iron_oom_abort("parser.c:iron_parse_patch_decl init type_name");
            m->method_name          = method_name;
            m->params               = all_params;
            m->param_count          = total;
            m->return_type          = NULL;
            m->body                 = ibody;
            m->is_private           = false;
            m->generic_params       = NULL;
            m->generic_param_count  = 0;
            m->resolved_return_type = NULL;
            m->owner_sym            = NULL;
            m->is_array_extension   = false;
            m->elem_type_name       = NULL;
            m->is_fusible           = false;
            m->is_receiver_form     = true;
            m->is_synth_accessor    = false;
            m->is_readonly          = false;
            m->is_pure              = false;
            m->is_init              = true;
            m->init_name            = init_name;

            if (extra_decls_out) {
                arrput(*extra_decls_out, (Iron_Node *)m);
            }
            iron_skip_newlines(p);
            continue;
        }

        /* Phase 82 in-block func inside patch body. */
        if (iron_check(p, IRON_TOK_FUNC)) {
            Iron_Token *fstart = iron_current(p);
            iron_advance(p);  /* consume 'func' */

            if (!iron_check(p, IRON_TOK_IDENTIFIER)) {
                iron_diag_emit(p->diags, p->arena, IRON_DIAG_ERROR,
                               IRON_ERR_UNEXPECTED_TOKEN,
                               iron_token_span(p, iron_current(p)),
                               "expected method name in patch body", NULL);
                iron_parser_sync_stmt(p);
                iron_skip_newlines(p);
                continue;
            }
            Iron_Token *mname_tok = iron_advance(p);

            int explicit_count = 0;
            Iron_Node **explicit_params = iron_parse_param_list(p, &explicit_count);

            Iron_Node *mret = NULL;
            if (iron_match(p, IRON_TOK_ARROW)) {
                mret = iron_parse_type_annotation(p);
            }

            Iron_Node *mbody = iron_parse_block(p);

            Iron_Param *synth_self = ARENA_ALLOC(p->arena, Iron_Param);
            if (!synth_self) iron_oom_abort("parser.c:iron_parse_patch_decl method synth self");
            synth_self->kind            = IRON_NODE_PARAM;
            synth_self->span            = iron_token_span(p, fstart);
            synth_self->is_var          = false;
            /* Mirror Phase 82 in-block receiver mutability: default-mutating
             * unless readonly/pure, matching iron_parse_object_decl. */
            synth_self->is_mut_receiver = !(member_is_readonly || member_is_pure);
            synth_self->name            = iron_arena_strdup(p->arena, "self", 4);
            if (!synth_self->name) iron_oom_abort("parser.c:iron_parse_patch_decl method self name");

            Iron_TypeAnnotation *self_type = ARENA_ALLOC(p->arena, Iron_TypeAnnotation);
            if (!self_type) iron_oom_abort("parser.c:iron_parse_patch_decl method self type");
            memset(self_type, 0, sizeof(*self_type));
            self_type->kind = IRON_NODE_TYPE_ANNOTATION;
            self_type->span = iron_token_span(p, fstart);
            self_type->name = iron_arena_strdup(p->arena, target_name,
                                                 strlen(target_name));
            if (!self_type->name) iron_oom_abort("parser.c:iron_parse_patch_decl method self type name");
            synth_self->type_ann = (Iron_Node *)self_type;

            int total = explicit_count + 1;
            Iron_Node **all_params = (Iron_Node **)iron_arena_alloc(
                p->arena, sizeof(Iron_Node *) * (size_t)total,
                _Alignof(Iron_Node *));
            if (!all_params) iron_oom_abort("parser.c:iron_parse_patch_decl method params array");
            all_params[0] = (Iron_Node *)synth_self;
            for (int i = 0; i < explicit_count; i++) {
                all_params[i + 1] = explicit_params[i];
            }

            Iron_MethodDecl *m = ARENA_ALLOC(p->arena, Iron_MethodDecl);
            if (!m) iron_oom_abort("parser.c:iron_parse_patch_decl method MethodDecl");
            m->kind                 = IRON_NODE_METHOD_DECL;
            m->span                 = iron_span_merge(iron_token_span(p, fstart),
                                                       mbody ? mbody->span
                                                             : iron_token_span(p, fstart));
            m->type_name            = iron_arena_strdup(p->arena, target_name,
                                                         strlen(target_name));
            if (!m->type_name) iron_oom_abort("parser.c:iron_parse_patch_decl method type_name");
            m->method_name          = iron_arena_strdup(p->arena, mname_tok->value,
                                                         strlen(mname_tok->value));
            if (!m->method_name) iron_oom_abort("parser.c:iron_parse_patch_decl method method_name");
            m->params               = all_params;
            m->param_count          = total;
            m->return_type          = mret;
            m->body                 = mbody;
            m->is_private           = false;
            m->generic_params       = NULL;
            m->generic_param_count  = 0;
            m->resolved_return_type = NULL;
            m->owner_sym            = NULL;
            m->is_array_extension   = false;
            m->elem_type_name       = NULL;
            m->is_fusible           = false;
            m->is_receiver_form     = true;
            (void)member_is_pub;  /* pub on methods: silent accept (Phase 83) */
            m->is_synth_accessor    = false;
            m->is_readonly          = member_is_readonly;
            m->is_pure              = member_is_pure;
            m->is_init              = false;
            m->init_name            = NULL;

            if (extra_decls_out) {
                arrput(*extra_decls_out, (Iron_Node *)m);
            }
            iron_skip_newlines(p);
            continue;
        }

        /* PATCH-05: var/val at patch body position. Emit E0253, consume
         * the declaration, drop it. field_count stays 0. */
        if (iron_check(p, IRON_TOK_VAR) || iron_check(p, IRON_TOK_VAL)) {
            Iron_Token *ftok = iron_current(p);
            iron_diag_emit(p->diags, p->arena, IRON_DIAG_ERROR,
                           IRON_ERR_PATCH_ADDS_FIELD,
                           iron_token_span(p, ftok),
                           "patches may only add methods and inits; "
                           "fields must be declared on the original object",
                           NULL);
            iron_advance(p);  /* consume var/val */
            /* Consume the rest of the field declaration: name [: type]
             * [= expr]. Stop at newline/RBRACE/EOF. */
            iron_parser_sync_stmt(p);
            iron_skip_newlines(p);
            continue;
        }

        /* Unknown token at patch body position. */
        iron_diag_emit(p->diags, p->arena, IRON_DIAG_ERROR,
                       IRON_ERR_UNEXPECTED_TOKEN,
                       iron_token_span(p, iron_current(p)),
                       "expected method or init declaration in patch body",
                       NULL);
        iron_parser_sync_stmt(p);
        iron_skip_newlines(p);
    }

    Iron_Token *end = iron_current(p);
    iron_expect(p, IRON_TOK_RBRACE);

    Iron_ObjectDecl *n     = ARENA_ALLOC(p->arena, Iron_ObjectDecl);
    if (!n) iron_oom_abort("parser.c:iron_parse_patch_decl ObjectDecl");
    n->kind                = IRON_NODE_OBJECT_DECL;
    n->span                = iron_span_merge(iron_token_span(p, start),
                                              iron_token_span(p, end));
    n->name                = target_name;
    n->fields              = NULL;
    n->field_count         = 0;
    n->extends_name        = NULL;
    /* Phase 87-02 PATCH-08: populate implements clause if present. */
    n->implements_names    = impl_names;
    n->implements_count    = impl_count;
    n->generic_params      = NULL;
    n->generic_param_count = 0;
    /* PATCH-01/02: the is_patch bit is the resolver's dispatch key; both
     * target_type_name and name point to the same arena-strdup so Plan
     * 86-02 can key the registry via either field. */
    n->is_patch            = true;
    n->target_type_name    = target_name;
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

        /* Phase 85 INIT-15 / Phase 87 IFACE-04: interfaces describe behavior,
         * not construction. Reject `init` in interface bodies with a clear
         * message pointing at the Self-returning factory alternative.
         * Phase 87 upgrades the error code from the generic
         * IRON_ERR_UNEXPECTED_TOKEN to the dedicated
         * IRON_ERR_IFACE_CANNOT_DECLARE_INIT (E0256). */
        if (iron_check(p, IRON_TOK_INIT)) {
            iron_diag_emit(p->diags, p->arena, IRON_DIAG_ERROR,
                           IRON_ERR_IFACE_CANNOT_DECLARE_INIT,
                           iron_token_span(p, iron_current(p)),
                           "interfaces may not declare init; use "
                           "Self-returning methods for factory patterns",
                           NULL);
            iron_advance(p);  /* consume 'init' */
            iron_parser_sync_stmt(p);
            continue;
        }

        /* Phase 87 IFACE-01: consume optional readonly/pure tier modifier before
         * func in interface body. Mirrors the pattern in iron_parse_object_decl
         * body loop (parser.c:2756) and iron_parse_patch_decl (parser.c:3688).
         * First-wins XOR recovery: emit E0245 and consume the second modifier
         * if both appear together. */
        bool member_is_readonly = false;
        bool member_is_pure     = false;
        if (iron_check(p, IRON_TOK_READONLY)) {
            iron_advance(p);
            member_is_readonly = true;
            if (iron_check(p, IRON_TOK_PURE)) {
                iron_diag_emit(p->diags, p->arena, IRON_DIAG_ERROR,
                               IRON_ERR_TIER_MODIFIER_PLACEMENT,
                               iron_token_span(p, iron_current(p)),
                               "method cannot be both 'readonly' and 'pure' "
                               "- pick one tier", NULL);
                iron_advance(p);  /* consume 'pure' to recover */
            }
        } else if (iron_check(p, IRON_TOK_PURE)) {
            iron_advance(p);
            member_is_pure = true;
            if (iron_check(p, IRON_TOK_READONLY)) {
                iron_diag_emit(p->diags, p->arena, IRON_DIAG_ERROR,
                               IRON_ERR_TIER_MODIFIER_PLACEMENT,
                               iron_token_span(p, iron_current(p)),
                               "method cannot be both 'pure' and 'readonly' "
                               "- pick one tier", NULL);
                iron_advance(p);  /* consume 'readonly' to recover */
            }
        }

        /* Method signature: [readonly|pure] func name(params) [-> Type] [{ body }] */
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

        /* Method name: must be a regular identifier */
        if (!iron_check(p, IRON_TOK_IDENTIFIER)) {
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

        /* Phase 87 IFACE-03: accept an optional default body after the return
         * type annotation. body != NULL is the has_default_body signal per
         * the ast.h Iron_FuncDecl invariant documented above. */
        Iron_Node *sig_body = NULL;
        if (iron_check(p, IRON_TOK_LBRACE)) {
            sig_body = iron_parse_block(p);
        }

        /* Store as a FuncDecl. body == NULL means signature-only; body != NULL
         * means has_default_body (Phase 87 IFACE-03 invariant). */
        Iron_FuncDecl *sig        = ARENA_ALLOC(p->arena, Iron_FuncDecl);
        if (!sig) { /* HARD-09 REPLACE (iron_parse_interface_decl sig FuncDecl) */ p->in_error_recovery = true; return iron_make_error(p); }
        sig->kind                 = IRON_NODE_FUNC_DECL;
        sig->span                 = iron_span_merge(iron_token_span(p, fsig_start),
                                                     iron_token_span(p, iron_current(p)));
        sig->name                 = iron_arena_strdup(p->arena, sig_name->value,
                                                       strlen(sig_name->value));
        if (!sig->name) { /* HARD-09 REPLACE (iron_parse_interface_decl sig name) */ sig->name = "?"; }
        sig->params               = sig_params;
        sig->param_count          = sig_param_count;
        sig->return_type          = sig_ret;
        sig->body                 = sig_body;  /* NULL = sig-only; non-NULL = default body */
        sig->is_private           = false;
        sig->is_extern            = false;
        sig->extern_c_name        = NULL;
        sig->generic_params       = NULL;
        sig->generic_param_count  = 0;
        sig->resolved_return_type = NULL;  /* set by type checker */
        sig->resolved_param_types = NULL;
        sig->is_fusible           = false;
        /* Phase 87 IFACE-01: tier modifier bits from the pre-func consumption above. */
        sig->is_readonly          = member_is_readonly;
        sig->is_pure              = member_is_pure;
        arrput(method_sigs, (Iron_Node *)sig);
        method_count++;
        iron_skip_newlines(p);
    }

    Iron_Token *end = iron_current(p);
    iron_expect(p, IRON_TOK_RBRACE);

    Iron_InterfaceDecl *n = ARENA_ALLOC(p->arena, Iron_InterfaceDecl);
    if (!n) { /* HARD-09 REPLACE (iron_parse_interface_decl InterfaceDecl) */ p->in_error_recovery = true; return iron_make_error(p); }
    n->kind               = IRON_NODE_INTERFACE_DECL;
    n->span               = iron_span_merge(iron_token_span(p, start),
                                             iron_token_span(p, end));
    n->name               = iron_arena_strdup(p->arena, name_tok->value,
                                               strlen(name_tok->value));
    if (!n->name) { /* HARD-09 REPLACE (iron_parse_interface_decl InterfaceDecl name) */ n->name = "?"; }
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

    /* Optional generic params: [T, E, ...] */
    Iron_Node **generic_params = NULL;
    int generic_count = 0;
    if (iron_check(p, IRON_TOK_LBRACKET)) {
        generic_params = iron_parse_generic_params(p, &generic_count, p->arena);
    }

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
        if (!v) { /* HARD-09 REPLACE (iron_parse_enum_decl EnumVariant) */ p->in_error_recovery = true; return iron_make_error(p); }
        v->kind               = IRON_NODE_ENUM_VARIANT;
        v->span               = iron_token_span(p, vt);
        v->name               = iron_arena_strdup(p->arena, vt->value, strlen(vt->value));
        if (!v->name) { /* HARD-09 REPLACE (iron_parse_enum_decl EnumVariant name) */ v->name = "?"; }
        v->has_explicit_value = false;
        v->explicit_value     = 0;
        v->payload_type_anns  = NULL;
        v->payload_count      = 0;

        /* Check for payload types: VariantName(Type, Type, ...) */
        if (iron_match(p, IRON_TOK_LPAREN)) {
            iron_skip_newlines(p);
            while (!iron_check(p, IRON_TOK_RPAREN) && !iron_check(p, IRON_TOK_EOF)) {
                Iron_Node *type_ann = iron_parse_type_annotation(p);
                arrput(v->payload_type_anns, type_ann);
                v->payload_count++;
                iron_skip_newlines(p);
                if (!iron_match(p, IRON_TOK_COMMA)) break;
                iron_skip_newlines(p);
            }
            iron_expect(p, IRON_TOK_RPAREN);
            v->span = iron_span_merge(iron_token_span(p, vt),
                                      iron_token_span(p, iron_current(p)));
        } else if (iron_check(p, IRON_TOK_ASSIGN)) {
            /* Optional explicit ordinal: VARIANT = INTEGER */
            iron_advance(p);  /* consume '=' */
            if (iron_check(p, IRON_TOK_INTEGER)) {
                Iron_Token *num = iron_advance(p);
                v->has_explicit_value = true;
                /* FIX-01 rank 19: atoi has UB on overflow and no range
                 * validation (C11 7.22.1.1p2). Use strtol + errno/ERANGE
                 * + INT_MIN/INT_MAX bounds so out-of-range enum variant
                 * values become a parse error instead of silently
                 * producing wrong tags on optimizer upgrade. */
                errno = 0;
                char *end = NULL;
                long parsed = strtol(num->value, &end, 10);
                bool consumed_all = (end != NULL && *end == '\0' && end != num->value);
                if (errno == ERANGE || !consumed_all ||
                    parsed > (long)INT_MAX || parsed < (long)INT_MIN) {
                    char msg[160];
                    snprintf(msg, sizeof(msg),
                             "enum variant value '%s' out of range (expected int32 in [INT_MIN, INT_MAX])",
                             num->value);
                    iron_emit_diag(p, IRON_ERR_UNEXPECTED_TOKEN,
                                   iron_token_span(p, num), msg);
                    v->explicit_value = 0;  /* defensive fallback keeps parse tree well-formed */
                } else {
                    v->explicit_value = (int)parsed;
                }
            }
        }
        arrput(variants, (Iron_Node *)v);
        variant_count++;
        iron_skip_newlines(p);
        if (!iron_match(p, IRON_TOK_COMMA)) break;
        iron_skip_newlines(p);
    }

    /* Compute has_payloads: true if any variant has associated data */
    bool has_payloads = false;
    for (int i = 0; i < variant_count; i++) {
        Iron_EnumVariant *v = (Iron_EnumVariant *)variants[i];
        if (v->payload_count > 0) { has_payloads = true; break; }
    }

    Iron_Token *end = iron_current(p);
    iron_expect(p, IRON_TOK_RBRACE);

    Iron_EnumDecl *n   = ARENA_ALLOC(p->arena, Iron_EnumDecl);
    if (!n) { /* HARD-09 REPLACE (iron_parse_enum_decl EnumDecl) */ p->in_error_recovery = true; return iron_make_error(p); }
    n->kind            = IRON_NODE_ENUM_DECL;
    n->span            = iron_span_merge(iron_token_span(p, start),
                                          iron_token_span(p, end));
    n->name            = iron_arena_strdup(p->arena, name_tok->value,
                                            strlen(name_tok->value));
    if (!n->name) { /* HARD-09 REPLACE (iron_parse_enum_decl EnumDecl name) */ n->name = "?"; }
    /* FIX-03 / AUDIT-04 §1: SAFETY — stb_ds `variants` array ownership-
     * transferred to arena-allocated EnumDecl; file-header comment. */
    n->variants             = variants;
    n->variant_count        = variant_count;
    n->has_payloads         = has_payloads;
    n->generic_params       = generic_params;
    n->generic_param_count  = generic_count;
    (void)is_private;
    return (Iron_Node *)n;
}

/* HARD-08: wrapper — see iron_parse_type_annotation for the pattern. */
static Iron_Node *iron_parse_decl(Iron_Parser *p, bool is_private,
                                  Iron_Node ***extra_decls_out) {
    if (iron_parser_depth_exceeded(p)) {
        return iron_make_error(p);
    }
    p->recur_depth++;
    Iron_Node *r = iron_parse_decl_impl(p, is_private, extra_decls_out);
    p->recur_depth--;
    return r;
}

static Iron_Node *iron_parse_decl_impl(Iron_Parser *p, bool is_private,
                                       Iron_Node ***extra_decls_out) {
    /* HARD-05: cancel poll at declaration parser entry. */
    if (iron_cancel_requested(p->cancel_flag)) {
        return iron_make_error(p);
    }
    switch ((int)iron_peek(p)) {
        case IRON_TOK_AT: {
            iron_advance(p);  /* consume '@' */
            Iron_Token *ann_tok = iron_current(p);
            bool is_fusible_ann = false;
            if (iron_check_name(p) && strcmp(ann_tok->value, "fusible") == 0) {
                is_fusible_ann = true;
                iron_advance(p);  /* consume 'fusible' */
                iron_skip_newlines(p);
            } else {
                iron_emit_diag(p, IRON_ERR_UNEXPECTED_TOKEN,
                               iron_token_span(p, ann_tok),
                               "expected 'fusible' after '@'");
                iron_advance(p);
            }
            if (iron_check(p, IRON_TOK_FUNC)) {
                Iron_Node *n = iron_parse_func_or_method(p, is_private);
                if (is_fusible_ann && n) {
                    if (n->kind == IRON_NODE_FUNC_DECL) {
                        ((Iron_FuncDecl *)n)->is_fusible = true;
                    } else if (n->kind == IRON_NODE_METHOD_DECL) {
                        ((Iron_MethodDecl *)n)->is_fusible = true;
                    }
                }
                p->in_error_recovery = false;
                return n;
            }
            iron_emit_diag(p, IRON_ERR_UNEXPECTED_TOKEN,
                           iron_token_span(p, iron_current(p)),
                           "expected 'func' after '@fusible'");
            return iron_make_error(p);
        }
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
            Iron_Node *n = iron_parse_object_decl(p, is_private, extra_decls_out);
            p->in_error_recovery = false;
            return n;
        }
        case IRON_TOK_PATCH:     {
            /* Phase 86 PATCH-01: `patch object T { methods+inits }`. */
            Iron_Node *n = iron_parse_patch_decl(p, extra_decls_out);
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
        /* -Wswitch-enum opt-out: top-level declaration switch handles only
         * the token kinds that introduce a declaration; every other
         * Iron_TokenKind is a parse error routed through the default arm. */
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

    /* Phase 82 GRAMMAR: in-block methods synthesized by iron_parse_object_decl
     * are delivered here via this extra-decls channel. Each call to
     * iron_parse_decl may append zero or more Iron_MethodDecl nodes to
     * *extra_decls; we flush them into `decls` after every iteration so they
     * appear at program->decls ordered after their enclosing ObjectDecl. The
     * storage is reused across iterations (arrsetlen(..., 0)). */
    Iron_Node **extra_decls = NULL;

    while (!iron_check(p, IRON_TOK_EOF)) {
        /* HARD-05: cancel poll inside the no-progress-guarded top-level loop
         * (parser.c:2895-2937 per PATTERNS.md — MANDATORY site). */
        if (iron_cancel_requested(p->cancel_flag)) {
            iron_diag_emit(p->diags, p->arena, IRON_DIAG_NOTE,
                           IRON_ERR_CANCELLED,
                           iron_token_span(p, iron_current(p)),
                           "compilation cancelled", NULL);
            break;
        }
        int pos_before = p->pos;
        iron_skip_newlines(p);
        if (iron_check(p, IRON_TOK_EOF)) break;

        /* Skip stray closing braces from incomplete declarations */
        if (iron_check(p, IRON_TOK_RBRACE)) {
            iron_advance(p);
            iron_skip_newlines(p);
            continue;
        }

        /* Phase 83 ACCESS-02: `pub` outside an object body is illegal in
         * v2.2/v3.0. Top-level decls default public; there is no opt-in
         * knob. Phase 88 may reinterpret top-level `pub` when the default
         * flips — for now fail fast with a clear message. */
        if (iron_check(p, IRON_TOK_PUB)) {
            iron_emit_diag(p, IRON_ERR_UNEXPECTED_TOKEN,
                           iron_token_span(p, iron_current(p)),
                           "'pub' modifier only valid on object-block declarations");
            iron_advance(p);  /* consume pub */
            iron_parser_sync_toplevel(p);
            continue;
        }

        /* Phase 84 MUTTIER-01/02/03: `readonly` and `pure` are only valid on
         * object-block methods. At top level fail fast with E0245
         * (IRON_ERR_TIER_MODIFIER_PLACEMENT) so the user gets a clear
         * pointer to the actual usage site. */
        if (iron_check(p, IRON_TOK_READONLY)) {
            iron_diag_emit(p->diags, p->arena, IRON_DIAG_ERROR,
                           IRON_ERR_TIER_MODIFIER_PLACEMENT,
                           iron_token_span(p, iron_current(p)),
                           "'readonly' modifier only valid on object-block methods",
                           NULL);
            iron_advance(p);
            iron_parser_sync_toplevel(p);
            continue;
        }
        if (iron_check(p, IRON_TOK_PURE)) {
            iron_diag_emit(p->diags, p->arena, IRON_DIAG_ERROR,
                           IRON_ERR_TIER_MODIFIER_PLACEMENT,
                           iron_token_span(p, iron_current(p)),
                           "'pure' modifier only valid on object-block methods",
                           NULL);
            iron_advance(p);
            iron_parser_sync_toplevel(p);
            continue;
        }

        bool is_private = false;
        if (iron_check(p, IRON_TOK_PRIVATE)) {
            iron_advance(p);
            is_private = true;
        }

        Iron_Node *d = iron_parse_decl(p, is_private, &extra_decls);
        arrput(decls, d);
        decl_count++;
        /* Phase 82: flush any in-block methods synthesized during this decl
         * so they live at top level right after their enclosing ObjectDecl. */
        for (int i = 0; i < (int)arrlen(extra_decls); i++) {
            arrput(decls, extra_decls[i]);
            decl_count++;
        }
        if (arrlen(extra_decls) > 0) arrsetlen(extra_decls, 0);
        iron_skip_newlines(p);
        /* No-progress guard at the top level: if iron_parse_decl failed to
         * consume any tokens, emit one generic diagnostic and skip one token.
         * This keeps the parser bounded even when a declaration parser leaves
         * the cursor stuck. Defense-in-depth for the 01c-class hang. */
        if (p->pos == pos_before) {
            iron_diag_emit(p->diags, p->arena, IRON_DIAG_ERROR,
                           IRON_ERR_UNEXPECTED_TOKEN,
                           iron_token_span(p, iron_current(p)),
                           "unexpected token at top level; skipping", NULL);
            if (iron_peek(p) != IRON_TOK_EOF) iron_advance(p);
        }
    }
    arrfree(extra_decls);

    /* Phase 85 INIT-13 post-parse cleanup: drop any field-less auto-synth
     * init that collides with a user-declared classic-form method
     * `func Type.init(...)`. This keeps pure-superset alive for stdlib
     * raylib bindings that define `func Window.init(...)` AND declare
     * `object Window {}` in the same module. The auto-synth fires at
     * object-body finalization time (before the user classic init is
     * visible to the loop) so this post-parse scan is the cleanest place
     * to reconcile. Identification markers for the synth:
     *   - is_init == true
     *   - body is an empty Iron_Block (stmt_count == 0) with span equal
     *     to the ObjectDecl's name-token span (matches what INIT-13
     *     writes; user-written empty `init() {}` is distinguishable
     *     because its span covers the `init` keyword, not the object
     *     name). A classic-form user init has is_init=false because it
     *     comes through iron_parse_func_or_method, not the object-body
     *     init branch.
     *
     * Collision rule: same type_name + method_name=="init" + one side
     * has is_init=true (the synth) and the other has is_init=false (the
     * classic user decl). The synth yields. */
    for (int i = 0; i < decl_count; i++) {
        Iron_Node *n = decls[i];
        if (!n || n->kind != IRON_NODE_METHOD_DECL) continue;
        Iron_MethodDecl *synth = (Iron_MethodDecl *)n;
        if (!synth->is_init) continue;
        if (synth->init_name != NULL) continue;  /* only anonymous synth */
        /* A user-written anonymous init has the SAME shape as the synth;
         * we must only drop the synth when a CLASSIC user init
         * (is_init=false) collides. Check the synth's body is empty:
         * the INIT-13 auto-synth always emits an empty block. A user
         * init with non-empty body cannot have fired INIT-13 (has_user_init
         * would have been true). A user init with empty body `init() {}`
         * is indistinguishable at the AST level - but in that case the
         * duplicate-scan in iron_parse_object_decl already rejected any
         * true duplicate, so we rely on the classic-form collision being
         * the only residual case to clean up here. */
        if (!synth->body || synth->body->kind != IRON_NODE_BLOCK) continue;
        Iron_Block *b = (Iron_Block *)synth->body;
        if (b->stmt_count != 0) continue;

        /* Look for a classic-form user init on the same type. */
        bool classic_exists = false;
        for (int j = 0; j < decl_count; j++) {
            if (j == i) continue;
            Iron_Node *m = decls[j];
            if (!m || m->kind != IRON_NODE_METHOD_DECL) continue;
            Iron_MethodDecl *mm = (Iron_MethodDecl *)m;
            if (mm->is_init) continue;  /* another init-keyword decl is not "classic" */
            if (!mm->type_name || !synth->type_name) continue;
            if (strcmp(mm->type_name, synth->type_name) != 0) continue;
            if (!mm->method_name) continue;
            if (strcmp(mm->method_name, "init") != 0) continue;
            classic_exists = true;
            break;
        }
        if (classic_exists) {
            /* Drop the synth: shift tail left. */
            for (int k = i; k < decl_count - 1; k++) {
                decls[k] = decls[k + 1];
            }
            decl_count--;
            i--;  /* recheck this index after shift */
        }
    }

    Iron_Program *prog  = ARENA_ALLOC(p->arena, Iron_Program);
    if (!prog) { /* HARD-09 REPLACE (iron_parse Program) */ p->in_error_recovery = true; return iron_make_error(p); }
    prog->kind          = IRON_NODE_PROGRAM;
    prog->span          = iron_span_merge(iron_token_span(p, start),
                                           iron_token_span(p, iron_current(p)));
    prog->decls         = decls;
    prog->decl_count    = decl_count;
    return (Iron_Node *)prog;
}
