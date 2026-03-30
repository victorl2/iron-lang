/* hir_lower.c — AST-to-HIR lowering pass.
 *
 * Converts a fully-analyzed Iron AST (Iron_Program) into an IronHIR_Module.
 * This is a three-pass implementation:
 *
 *   Pass 1 (lower_module_decls_hir): register func/method signatures in the
 *           HIR module; collect top-level val/var into global_constants_map.
 *   Pass 2 (lower_func_bodies_hir):  lower each function/method body.
 *   Pass 3 (lower_lift_pending_hir): lift lambda/spawn/pfor to top-level HIR
 *           functions.
 *
 * HIR preserves high-level structure (structured control flow, named variables,
 * closures, string interpolation) with only four desugarings:
 *   1. Elif chains → nested if-in-else
 *   2. For-range integer loops → while
 *   3. Compound assignments (+=, -=, *=, /=) → binop + assign
 *   4. String interpolation kept as IRON_HIR_EXPR_INTERP_STRING (no lowering)
 */

#include "hir/hir_lower.h"
#include "lexer/lexer.h"
#include "vendor/stb_ds.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>

/* ── Lift descriptor ─────────────────────────────────────────────────────── */

typedef enum {
    LIFT_LAMBDA,
    LIFT_SPAWN,
    LIFT_PARALLEL_FOR
} LiftKind;

typedef struct {
    LiftKind      kind;
    Iron_Node    *ast_node;        /* the lambda/spawn/pfor AST node */
    const char   *lifted_name;     /* assigned name (__lambda_N, etc.) */
    const char   *enclosing_func;  /* name of the function containing this */
} LiftPending;

/* ── Per-scope frame type ────────────────────────────────────────────────── */
/* Each frame is a stb_ds hash map: name -> VarId */
typedef struct { char *key; IronHIR_VarId value; } ScopeFrame;

/* ── Lowering context ────────────────────────────────────────────────────── */

typedef struct {
    /* Inputs */
    Iron_Program    *program;
    Iron_Scope      *global_scope;
    Iron_DiagList   *diags;

    /* Output being built */
    IronHIR_Module  *module;

    /* Current function being lowered */
    IronHIR_Func    *current_func;

    /* Current block being populated */
    IronHIR_Block   *current_block;

    /* Lexical scope stack: stb_ds array of ScopeFrame* (hash maps) */
    ScopeFrame     **scope_stack;   /* stb_ds array */
    int              scope_depth;

    /* Defer stack: stb_ds array of IronHIR_Block* arrays */
    IronHIR_Block ***defer_stacks;
    int              defer_depth;
    int              function_scope_depth;

    /* Pending lifts (lambdas, spawns, parallel-for) */
    LiftPending     *pending_lifts;  /* stb_ds array */
    int              lift_counter;   /* for generating unique names */

    /* Current enclosing function name (for lifted function naming) */
    const char      *current_func_name;

    /* Global constant lazy lowering */
    struct { char *key; Iron_Node *value; } *global_constants_map;
    struct { char *key; int value; }        *global_mutable_set;

    /* Tracks which globals have been lowered (name -> VarId) */
    struct { char *key; IronHIR_VarId value; } *global_lowered_map;
} IronHIR_LowerCtx;

/* ── Forward declarations ────────────────────────────────────────────────── */

static IronHIR_Stmt *lower_stmt_hir(IronHIR_LowerCtx *ctx, Iron_Node *node);
static IronHIR_Expr *lower_expr_hir(IronHIR_LowerCtx *ctx, Iron_Node *node);
static void lower_block_hir(IronHIR_LowerCtx *ctx, Iron_Block *block,
                             IronHIR_Block *out);

/* ── Common layout prefix for all expression nodes ───────────────────────── */

typedef struct {
    Iron_Span         span;
    Iron_NodeKind     kind;
    struct Iron_Type *resolved_type;
} Iron_ExprNode;

static Iron_Type *expr_type(Iron_Node *node) {
    if (!node) return NULL;
    return ((Iron_ExprNode *)node)->resolved_type;
}

/* ── Scope management ────────────────────────────────────────────────────── */

static void push_scope(IronHIR_LowerCtx *ctx) {
    ScopeFrame *frame = NULL;  /* empty stb_ds hash map */
    arrput(ctx->scope_stack, frame);
    ctx->scope_depth++;
}

static void pop_scope(IronHIR_LowerCtx *ctx) {
    if (ctx->scope_depth <= 0) return;
    ctx->scope_depth--;
    ScopeFrame *frame = ctx->scope_stack[ctx->scope_depth];
    shfree(frame);
    ctx->scope_stack[ctx->scope_depth] = NULL;
    arrsetlen(ctx->scope_stack, ctx->scope_depth);
}

static void declare_var(IronHIR_LowerCtx *ctx, const char *name,
                        IronHIR_VarId id) {
    if (ctx->scope_depth <= 0) return;
    shput(ctx->scope_stack[ctx->scope_depth - 1], name, id);
}

static IronHIR_VarId lookup_var(IronHIR_LowerCtx *ctx, const char *name) {
    for (int d = ctx->scope_depth - 1; d >= 0; d--) {
        ptrdiff_t idx = shgeti(ctx->scope_stack[d], name);
        if (idx >= 0) return ctx->scope_stack[d][idx].value;
    }
    return IRON_HIR_VAR_INVALID;
}

/* ── Defer management ────────────────────────────────────────────────────── */

static void push_defer_scope_hir(IronHIR_LowerCtx *ctx) {
    ctx->defer_depth++;
    while ((int)arrlen(ctx->defer_stacks) < ctx->defer_depth) {
        IronHIR_Block **empty = NULL;
        arrput(ctx->defer_stacks, empty);
    }
    ctx->defer_stacks[ctx->defer_depth - 1] = NULL;
}

static void pop_defer_scope_hir(IronHIR_LowerCtx *ctx) {
    if (ctx->defer_depth <= 0) return;
    ctx->defer_depth--;
    arrfree(ctx->defer_stacks[ctx->defer_depth]);
    ctx->defer_stacks[ctx->defer_depth] = NULL;
}

/* ── Resolve type annotation to Iron_Type* ───────────────────────────────── */

static Iron_Type *resolve_type_ann(IronHIR_LowerCtx *ctx, Iron_Node *ann_node) {
    if (!ann_node) return iron_type_make_primitive(IRON_TYPE_VOID);
    if (ann_node->kind != IRON_NODE_TYPE_ANNOTATION) return NULL;
    Iron_TypeAnnotation *ta = (Iron_TypeAnnotation *)ann_node;

    Iron_Type *base = NULL;
    if (strcmp(ta->name, "Int") == 0)         base = iron_type_make_primitive(IRON_TYPE_INT);
    else if (strcmp(ta->name, "Float") == 0)  base = iron_type_make_primitive(IRON_TYPE_FLOAT);
    else if (strcmp(ta->name, "Bool") == 0)   base = iron_type_make_primitive(IRON_TYPE_BOOL);
    else if (strcmp(ta->name, "String") == 0) base = iron_type_make_primitive(IRON_TYPE_STRING);
    else if (strcmp(ta->name, "Void") == 0)   base = iron_type_make_primitive(IRON_TYPE_VOID);
    else if (strcmp(ta->name, "Int8") == 0)   base = iron_type_make_primitive(IRON_TYPE_INT8);
    else if (strcmp(ta->name, "Int16") == 0)  base = iron_type_make_primitive(IRON_TYPE_INT16);
    else if (strcmp(ta->name, "Int32") == 0)  base = iron_type_make_primitive(IRON_TYPE_INT32);
    else if (strcmp(ta->name, "Int64") == 0)  base = iron_type_make_primitive(IRON_TYPE_INT64);
    else if (strcmp(ta->name, "Float32") == 0) base = iron_type_make_primitive(IRON_TYPE_FLOAT32);
    else if (strcmp(ta->name, "Float64") == 0) base = iron_type_make_primitive(IRON_TYPE_FLOAT64);
    else {
        /* Named type: search program declarations */
        if (ctx->program) {
            for (int i = 0; i < ctx->program->decl_count; i++) {
                Iron_Node *decl = ctx->program->decls[i];
                if (decl->kind == IRON_NODE_OBJECT_DECL) {
                    Iron_ObjectDecl *od = (Iron_ObjectDecl *)decl;
                    if (strcmp(od->name, ta->name) == 0) {
                        base = iron_type_make_object(ctx->module->arena, od);
                        break;
                    }
                } else if (decl->kind == IRON_NODE_ENUM_DECL) {
                    Iron_EnumDecl *ed = (Iron_EnumDecl *)decl;
                    if (strcmp(ed->name, ta->name) == 0) {
                        base = iron_type_make_enum(ctx->module->arena, ed);
                        break;
                    }
                }
            }
        }
    }

    if (ta->is_nullable && base) {
        base = iron_type_make_nullable(ctx->module->arena, base);
    }
    if (ta->is_array && base) {
        int size = -1;
        /* size node not used here for simplicity */
        base = iron_type_make_array(ctx->module->arena, base, size);
    }
    return base;
}

/* ── Build HIR param array from AST params ───────────────────────────────── */

static IronHIR_Param *build_hir_params(IronHIR_LowerCtx *ctx,
                                        Iron_Node **params, int param_count) {
    if (param_count == 0) return NULL;
    IronHIR_Param *arr = (IronHIR_Param *)iron_arena_alloc(
        ctx->module->arena,
        (size_t)param_count * sizeof(IronHIR_Param),
        _Alignof(IronHIR_Param));
    for (int p = 0; p < param_count; p++) {
        Iron_Param *ap = (Iron_Param *)params[p];
        Iron_Type  *pt = resolve_type_ann(ctx, ap->type_ann);
        arr[p].name   = ap->name;
        arr[p].type   = pt;
        /* var_id assigned later when we push func scope */
        arr[p].var_id = IRON_HIR_VAR_INVALID;
    }
    return arr;
}

/* ── Find HIR func by name ───────────────────────────────────────────────── */

static IronHIR_Func *find_hir_func(IronHIR_Module *mod, const char *name) {
    for (int i = 0; i < mod->func_count; i++) {
        if (strcmp(mod->funcs[i]->name, name) == 0) return mod->funcs[i];
    }
    return NULL;
}

/* ─────────────────────────────────────────────────────────────────────────── */
/* ── Statement lowering ──────────────────────────────────────────────────── */
/* ─────────────────────────────────────────────────────────────────────────── */

/* Map AST binary operator token to HIR binary op */
static IronHIR_BinOp ast_op_to_hir_binop(Iron_OpKind op) {
    switch (op) {
        case IRON_TOK_PLUS:       return IRON_HIR_BINOP_ADD;
        case IRON_TOK_MINUS:      return IRON_HIR_BINOP_SUB;
        case IRON_TOK_STAR:       return IRON_HIR_BINOP_MUL;
        case IRON_TOK_SLASH:      return IRON_HIR_BINOP_DIV;
        case IRON_TOK_PERCENT:    return IRON_HIR_BINOP_MOD;
        case IRON_TOK_EQUALS:     return IRON_HIR_BINOP_EQ;
        case IRON_TOK_NOT_EQUALS: return IRON_HIR_BINOP_NEQ;
        case IRON_TOK_LESS:       return IRON_HIR_BINOP_LT;
        case IRON_TOK_LESS_EQ:    return IRON_HIR_BINOP_LTE;
        case IRON_TOK_GREATER:    return IRON_HIR_BINOP_GT;
        case IRON_TOK_GREATER_EQ: return IRON_HIR_BINOP_GTE;
        case IRON_TOK_AND:        return IRON_HIR_BINOP_AND;
        case IRON_TOK_OR:         return IRON_HIR_BINOP_OR;
        default:                  return IRON_HIR_BINOP_ADD; /* fallback */
    }
}

/* Map compound-assign token to base binop token */
static Iron_OpKind compound_assign_base_op(Iron_OpKind op) {
    switch (op) {
        case IRON_TOK_PLUS_ASSIGN:  return IRON_TOK_PLUS;
        case IRON_TOK_MINUS_ASSIGN: return IRON_TOK_MINUS;
        case IRON_TOK_STAR_ASSIGN:  return IRON_TOK_STAR;
        case IRON_TOK_SLASH_ASSIGN: return IRON_TOK_SLASH;
        default:                    return IRON_TOK_PLUS;
    }
}

static bool is_compound_assign(Iron_OpKind op) {
    return op == IRON_TOK_PLUS_ASSIGN  ||
           op == IRON_TOK_MINUS_ASSIGN ||
           op == IRON_TOK_STAR_ASSIGN  ||
           op == IRON_TOK_SLASH_ASSIGN;
}

/* Lower a single statement node; emit it into ctx->current_block.
 * Returns a HIR stmt if the caller needs to add it to a block manually,
 * otherwise NULL (already appended). */
static IronHIR_Stmt *lower_stmt_hir(IronHIR_LowerCtx *ctx, Iron_Node *node) {
    if (!node) return NULL;
    IronHIR_Module *mod  = ctx->module;
    IronHIR_Block  *blk  = ctx->current_block;
    Iron_Span       span = node->span;

    switch (node->kind) {

    /* ── Val declaration ───────────────────────────────────────────────────── */
    case IRON_NODE_VAL_DECL: {
        Iron_ValDecl *vd = (Iron_ValDecl *)node;
        Iron_Type    *ty = vd->declared_type;
        if (!ty) ty = resolve_type_ann(ctx, vd->type_ann);
        IronHIR_Expr *init = vd->init ? lower_expr_hir(ctx, vd->init) : NULL;
        IronHIR_VarId id   = iron_hir_alloc_var(mod, vd->name, ty, false);
        declare_var(ctx, vd->name, id);
        IronHIR_Stmt *s = iron_hir_stmt_let(mod, id, ty, init, false, span);
        iron_hir_block_add_stmt(blk, s);
        return NULL;
    }

    /* ── Var declaration ───────────────────────────────────────────────────── */
    case IRON_NODE_VAR_DECL: {
        Iron_VarDecl *vd = (Iron_VarDecl *)node;
        Iron_Type    *ty = vd->declared_type;
        if (!ty) ty = resolve_type_ann(ctx, vd->type_ann);
        IronHIR_Expr *init = vd->init ? lower_expr_hir(ctx, vd->init) : NULL;
        IronHIR_VarId id   = iron_hir_alloc_var(mod, vd->name, ty, true);
        declare_var(ctx, vd->name, id);
        IronHIR_Stmt *s = iron_hir_stmt_let(mod, id, ty, init, true, span);
        iron_hir_block_add_stmt(blk, s);
        return NULL;
    }

    /* ── Assignment (including compound assignment desugaring) ────────────── */
    case IRON_NODE_ASSIGN: {
        Iron_AssignStmt *as = (Iron_AssignStmt *)node;
        IronHIR_Expr *target = lower_expr_hir(ctx, as->target);
        IronHIR_Expr *value  = lower_expr_hir(ctx, as->value);

        if (is_compound_assign(as->op)) {
            /* Desugar: target op= value  →  target = target binop value */
            Iron_OpKind   base = compound_assign_base_op(as->op);
            IronHIR_BinOp hop  = ast_op_to_hir_binop(base);
            /* Re-lower target for the RHS read (fresh expr node, same source) */
            IronHIR_Expr *target2 = lower_expr_hir(ctx, as->target);
            Iron_Type    *ty      = expr_type(as->target);
            IronHIR_Expr *binop   = iron_hir_expr_binop(mod, hop, target2, value,
                                                         ty, span);
            IronHIR_Stmt *s = iron_hir_stmt_assign(mod, target, binop, span);
            iron_hir_block_add_stmt(blk, s);
        } else {
            IronHIR_Stmt *s = iron_hir_stmt_assign(mod, target, value, span);
            iron_hir_block_add_stmt(blk, s);
        }
        return NULL;
    }

    /* ── If / elif / else (desugar elif chain to nested if-in-else) ────────── */
    case IRON_NODE_IF: {
        Iron_IfStmt *is = (Iron_IfStmt *)node;

        /* Helper: lower body (Iron_Block*) into a new HIR block */
        IronHIR_Block *then_blk = iron_hir_block_create(mod);
        lower_block_hir(ctx, (Iron_Block *)is->body, then_blk);

        IronHIR_Block *else_blk = NULL;

        if (is->elif_count > 0) {
            /* Build elif chain from the inside out */
            /* Start with the final else (if any) */
            IronHIR_Block *inner_else = NULL;
            if (is->else_body) {
                inner_else = iron_hir_block_create(mod);
                lower_block_hir(ctx, (Iron_Block *)is->else_body, inner_else);
            }

            /* Walk elifs from last to first, wrapping inner_else each time */
            for (int ei = is->elif_count - 1; ei >= 0; ei--) {
                IronHIR_Expr  *elif_cond = lower_expr_hir(ctx, is->elif_conds[ei]);
                IronHIR_Block *elif_then = iron_hir_block_create(mod);
                lower_block_hir(ctx, (Iron_Block *)is->elif_bodies[ei], elif_then);

                /* Create wrapping if stmt */
                IronHIR_Stmt *elif_if = iron_hir_stmt_if(mod, elif_cond,
                                                          elif_then, inner_else,
                                                          is->elif_conds[ei]->span);
                /* The "else" of the outer if is a block containing this elif if */
                IronHIR_Block *wrapper = iron_hir_block_create(mod);
                iron_hir_block_add_stmt(wrapper, elif_if);
                inner_else = wrapper;
            }
            else_blk = inner_else;
        } else if (is->else_body) {
            else_blk = iron_hir_block_create(mod);
            lower_block_hir(ctx, (Iron_Block *)is->else_body, else_blk);
        }

        IronHIR_Expr *cond = lower_expr_hir(ctx, is->condition);
        IronHIR_Stmt *s = iron_hir_stmt_if(mod, cond, then_blk, else_blk, span);
        iron_hir_block_add_stmt(blk, s);
        return NULL;
    }

    /* ── While loop ────────────────────────────────────────────────────────── */
    case IRON_NODE_WHILE: {
        Iron_WhileStmt *ws = (Iron_WhileStmt *)node;
        IronHIR_Expr  *cond     = lower_expr_hir(ctx, ws->condition);
        IronHIR_Block *body_blk = iron_hir_block_create(mod);
        lower_block_hir(ctx, (Iron_Block *)ws->body, body_blk);
        IronHIR_Stmt *s = iron_hir_stmt_while(mod, cond, body_blk, span);
        iron_hir_block_add_stmt(blk, s);
        return NULL;
    }

    /* ── For loop ──────────────────────────────────────────────────────────── */
    case IRON_NODE_FOR: {
        Iron_ForStmt *fs = (Iron_ForStmt *)node;

        /* Parallel for */
        if (fs->is_parallel) {
            /* Allocate a VarId for the loop variable */
            IronHIR_VarId loop_var = iron_hir_alloc_var(mod, fs->var_name,
                                                         iron_type_make_primitive(IRON_TYPE_INT),
                                                         true);
            IronHIR_Expr *range_expr = lower_expr_hir(ctx, fs->iterable);

            push_scope(ctx);
            declare_var(ctx, fs->var_name, loop_var);
            IronHIR_Block *pfor_body = iron_hir_block_create(mod);
            lower_block_hir(ctx, (Iron_Block *)fs->body, pfor_body);
            pop_scope(ctx);

            /* Assign lifted name first so the HIR expr can store it */
            char lifted_name[64];
            snprintf(lifted_name, sizeof(lifted_name), "__pfor_%d",
                     ctx->lift_counter++);
            char *name_copy = (char *)iron_arena_alloc(
                ctx->module->arena,
                strlen(lifted_name) + 1,
                _Alignof(char));
            memcpy(name_copy, lifted_name, strlen(lifted_name) + 1);

            Iron_Type *void_ty = iron_type_make_primitive(IRON_TYPE_VOID);
            IronHIR_Expr *pfor_expr = iron_hir_expr_parallel_for(mod, loop_var,
                                                                   range_expr,
                                                                   pfor_body,
                                                                   void_ty, name_copy, span);
            IronHIR_Stmt *s = iron_hir_stmt_expr(mod, pfor_expr, span);
            iron_hir_block_add_stmt(blk, s);

            /* Queue for lifting */
            LiftPending lp;
            lp.kind          = LIFT_PARALLEL_FOR;
            lp.ast_node      = node;
            lp.lifted_name   = name_copy;
            lp.enclosing_func = ctx->current_func_name;
            arrput(ctx->pending_lifts, lp);
            return NULL;
        }

        /* Determine if this is a range for (iterable is a binary .. expr or
         * a range-typed expression) vs an array/collection for. */
        bool is_range = false;
        if (fs->iterable && fs->iterable->kind == IRON_NODE_BINARY) {
            Iron_BinaryExpr *bin = (Iron_BinaryExpr *)fs->iterable;
            if (bin->op == IRON_TOK_DOTDOT) {
                is_range = true;
            }
        }
        /* Also treat integer-typed iterables as range */
        if (!is_range && fs->iterable) {
            Iron_Type *it_ty = expr_type(fs->iterable);
            if (it_ty && iron_type_is_integer(it_ty)) {
                is_range = true;
            }
        }

        if (is_range) {
            /* Desugar: for i in start..end → while loop with counter */
            IronHIR_VarId loop_var = iron_hir_alloc_var(mod, fs->var_name,
                                                         iron_type_make_primitive(IRON_TYPE_INT),
                                                         true);
            Iron_Type *int_ty = iron_type_make_primitive(IRON_TYPE_INT);
            Iron_Span  s0 = span;

            /* Extract start and end from the .. expression */
            IronHIR_Expr *start_expr = NULL;
            IronHIR_Expr *end_expr   = NULL;
            if (fs->iterable->kind == IRON_NODE_BINARY) {
                Iron_BinaryExpr *rng = (Iron_BinaryExpr *)fs->iterable;
                start_expr = lower_expr_hir(ctx, rng->left);
                end_expr   = lower_expr_hir(ctx, rng->right);
            } else {
                /* Single integer bound: iterate 0..n */
                start_expr = iron_hir_expr_int_lit(mod, 0, int_ty, s0);
                end_expr   = lower_expr_hir(ctx, fs->iterable);
            }

            /* STMT_LET i = start, mutable */
            IronHIR_Stmt *init_stmt = iron_hir_stmt_let(mod, loop_var, int_ty,
                                                          start_expr, true, s0);
            iron_hir_block_add_stmt(blk, init_stmt);

            /* Condition: i < end */
            IronHIR_Expr *i_ref    = iron_hir_expr_ident(mod, loop_var,
                                                           fs->var_name, int_ty, s0);
            Iron_Type     *bool_ty  = iron_type_make_primitive(IRON_TYPE_BOOL);
            IronHIR_Expr  *cond     = iron_hir_expr_binop(mod, IRON_HIR_BINOP_LT,
                                                           i_ref, end_expr,
                                                           bool_ty, s0);

            /* Declare loop var in scope for body lowering */
            push_scope(ctx);
            declare_var(ctx, fs->var_name, loop_var);
            IronHIR_Block *body_blk = iron_hir_block_create(mod);
            lower_block_hir(ctx, (Iron_Block *)fs->body, body_blk);
            pop_scope(ctx);

            /* Append increment: i = i + 1 */
            IronHIR_Expr *i_ref2  = iron_hir_expr_ident(mod, loop_var,
                                                          fs->var_name, int_ty, s0);
            IronHIR_Expr *one     = iron_hir_expr_int_lit(mod, 1, int_ty, s0);
            IronHIR_Expr *inc     = iron_hir_expr_binop(mod, IRON_HIR_BINOP_ADD,
                                                         i_ref2, one, int_ty, s0);
            IronHIR_Expr *i_tgt   = iron_hir_expr_ident(mod, loop_var,
                                                          fs->var_name, int_ty, s0);
            IronHIR_Stmt *inc_stmt = iron_hir_stmt_assign(mod, i_tgt, inc, s0);
            iron_hir_block_add_stmt(body_blk, inc_stmt);

            IronHIR_Stmt *ws = iron_hir_stmt_while(mod, cond, body_blk, s0);
            iron_hir_block_add_stmt(blk, ws);
        } else {
            /* Array/collection for: STMT_FOR */
            IronHIR_VarId loop_var = iron_hir_alloc_var(mod, fs->var_name,
                                                         expr_type(fs->iterable),
                                                         false);
            IronHIR_Expr *iterable = lower_expr_hir(ctx, fs->iterable);

            push_scope(ctx);
            declare_var(ctx, fs->var_name, loop_var);
            IronHIR_Block *body_blk = iron_hir_block_create(mod);
            lower_block_hir(ctx, (Iron_Block *)fs->body, body_blk);
            pop_scope(ctx);

            IronHIR_Stmt *s = iron_hir_stmt_for(mod, loop_var, iterable,
                                                  body_blk, span);
            iron_hir_block_add_stmt(blk, s);
        }
        return NULL;
    }

    /* ── Match statement ───────────────────────────────────────────────────── */
    case IRON_NODE_MATCH: {
        Iron_MatchStmt *ms    = (Iron_MatchStmt *)node;
        IronHIR_Expr   *scrut = lower_expr_hir(ctx, ms->subject);
        IronHIR_MatchArm *arms = NULL;

        for (int i = 0; i < ms->case_count; i++) {
            Iron_MatchCase *mc = (Iron_MatchCase *)ms->cases[i];
            IronHIR_Expr  *pat = lower_expr_hir(ctx, mc->pattern);
            IronHIR_Block *mbody = iron_hir_block_create(mod);
            lower_block_hir(ctx, (Iron_Block *)mc->body, mbody);
            IronHIR_MatchArm arm;
            arm.pattern = pat;
            arm.guard   = NULL;
            arm.body    = mbody;
            arrput(arms, arm);
        }

        /* Also handle the else body if present */
        if (ms->else_body) {
            IronHIR_Expr  *null_pat = iron_hir_expr_null_lit(mod, NULL, span);
            IronHIR_Block *else_blk = iron_hir_block_create(mod);
            lower_block_hir(ctx, (Iron_Block *)ms->else_body, else_blk);
            IronHIR_MatchArm arm;
            arm.pattern = null_pat;
            arm.guard   = NULL;
            arm.body    = else_blk;
            arrput(arms, arm);
        }

        int arm_count = (int)arrlen(arms);
        /* NOTE: arms stb_ds array ownership transfers to the HIR stmt — do NOT arrfree */
        IronHIR_Stmt *s = iron_hir_stmt_match(mod, scrut, arms, arm_count, span);
        iron_hir_block_add_stmt(blk, s);
        return NULL;
    }

    /* ── Return ────────────────────────────────────────────────────────────── */
    case IRON_NODE_RETURN: {
        Iron_ReturnStmt *rs = (Iron_ReturnStmt *)node;
        IronHIR_Expr    *val = rs->value ? lower_expr_hir(ctx, rs->value) : NULL;
        IronHIR_Stmt    *s   = iron_hir_stmt_return(mod, val, span);
        iron_hir_block_add_stmt(blk, s);
        return NULL;
    }

    /* ── Defer ─────────────────────────────────────────────────────────────── */
    case IRON_NODE_DEFER: {
        Iron_DeferStmt *ds = (Iron_DeferStmt *)node;
        IronHIR_Block  *defer_body = iron_hir_block_create(mod);
        /* Lower the deferred expression as a single expression statement */
        IronHIR_Expr *dexpr = lower_expr_hir(ctx, ds->expr);
        IronHIR_Stmt *dstmt = iron_hir_stmt_expr(mod, dexpr, span);
        iron_hir_block_add_stmt(defer_body, dstmt);
        IronHIR_Stmt *s = iron_hir_stmt_defer(mod, defer_body, span);
        iron_hir_block_add_stmt(blk, s);
        /* Push deferred block onto the current scope's defer stack */
        if (ctx->defer_depth > 0) {
            arrput(ctx->defer_stacks[ctx->defer_depth - 1], defer_body);
        }
        return NULL;
    }

    /* ── Free ──────────────────────────────────────────────────────────────── */
    case IRON_NODE_FREE: {
        Iron_FreeStmt *fs = (Iron_FreeStmt *)node;
        IronHIR_Expr  *val = lower_expr_hir(ctx, fs->expr);
        IronHIR_Stmt  *s   = iron_hir_stmt_free(mod, val, span);
        iron_hir_block_add_stmt(blk, s);
        return NULL;
    }

    /* ── Leak ──────────────────────────────────────────────────────────────── */
    case IRON_NODE_LEAK: {
        Iron_LeakStmt *ls = (Iron_LeakStmt *)node;
        IronHIR_Expr  *val = lower_expr_hir(ctx, ls->expr);
        IronHIR_Stmt  *s   = iron_hir_stmt_leak(mod, val, span);
        iron_hir_block_add_stmt(blk, s);
        return NULL;
    }

    /* ── Spawn ─────────────────────────────────────────────────────────────── */
    case IRON_NODE_SPAWN: {
        Iron_SpawnStmt *ss = (Iron_SpawnStmt *)node;
        const char     *hname = ss->handle_name ? ss->handle_name : "__spawn_handle";

        /* Assign lifted name first so the HIR stmt can store it */
        char lifted_name[64];
        snprintf(lifted_name, sizeof(lifted_name), "__spawn_%d",
                 ctx->lift_counter++);
        char *name_copy = (char *)iron_arena_alloc(
            ctx->module->arena,
            strlen(lifted_name) + 1,
            _Alignof(char));
        memcpy(name_copy, lifted_name, strlen(lifted_name) + 1);

        IronHIR_Block *spawn_body = iron_hir_block_create(mod);
        lower_block_hir(ctx, (Iron_Block *)ss->body, spawn_body);

        IronHIR_Stmt *s = iron_hir_stmt_spawn(mod, hname, spawn_body, name_copy, span);
        iron_hir_block_add_stmt(blk, s);

        /* Queue for lifting */
        LiftPending lp;
        lp.kind           = LIFT_SPAWN;
        lp.ast_node       = node;
        lp.lifted_name    = name_copy;
        lp.enclosing_func = ctx->current_func_name;
        arrput(ctx->pending_lifts, lp);
        return NULL;
    }

    /* ── Block statement ───────────────────────────────────────────────────── */
    case IRON_NODE_BLOCK: {
        Iron_Block    *inner  = (Iron_Block *)node;
        IronHIR_Block *hblk   = iron_hir_block_create(mod);
        push_scope(ctx);
        lower_block_hir(ctx, inner, hblk);
        pop_scope(ctx);
        IronHIR_Stmt *s = iron_hir_stmt_block(mod, hblk, span);
        iron_hir_block_add_stmt(blk, s);
        return NULL;
    }

    /* ── Expression statement ──────────────────────────────────────────────── */
    default: {
        /* All expressions used as statements */
        IronHIR_Expr *e = lower_expr_hir(ctx, node);
        if (e) {
            IronHIR_Stmt *s = iron_hir_stmt_expr(mod, e, span);
            iron_hir_block_add_stmt(blk, s);
        }
        return NULL;
    }

    } /* end switch */

    return NULL;
}

/* ─────────────────────────────────────────────────────────────────────────── */
/* ── Expression lowering ─────────────────────────────────────────────────── */
/* ─────────────────────────────────────────────────────────────────────────── */

static IronHIR_Expr *lower_expr_hir(IronHIR_LowerCtx *ctx, Iron_Node *node) {
    if (!node) return NULL;
    IronHIR_Module *mod  = ctx->module;
    Iron_Span       span = node->span;

    switch (node->kind) {

    /* ── Integer literal ─────────────────────────────────────────────────── */
    case IRON_NODE_INT_LIT: {
        Iron_IntLit *lit = (Iron_IntLit *)node;
        int64_t val = (int64_t)strtoll(lit->value, NULL, 0);
        return iron_hir_expr_int_lit(mod, val, lit->resolved_type, span);
    }

    /* ── Float literal ───────────────────────────────────────────────────── */
    case IRON_NODE_FLOAT_LIT: {
        Iron_FloatLit *lit = (Iron_FloatLit *)node;
        double val = strtod(lit->value, NULL);
        return iron_hir_expr_float_lit(mod, val, lit->resolved_type, span);
    }

    /* ── String literal ──────────────────────────────────────────────────── */
    case IRON_NODE_STRING_LIT: {
        Iron_StringLit *lit = (Iron_StringLit *)node;
        return iron_hir_expr_string_lit(mod, lit->value, lit->resolved_type, span);
    }

    /* ── Interpolated string ─────────────────────────────────────────────── */
    case IRON_NODE_INTERP_STRING: {
        Iron_InterpString *is = (Iron_InterpString *)node;
        IronHIR_Expr **parts = NULL;
        for (int i = 0; i < is->part_count; i++) {
            IronHIR_Expr *p = lower_expr_hir(ctx, is->parts[i]);
            arrput(parts, p);
        }
        int part_count = (int)arrlen(parts);
        /* NOTE: parts stb_ds array ownership transfers to the HIR expr — do NOT arrfree */
        return iron_hir_expr_interp_string(mod, parts, part_count,
                                           is->resolved_type, span);
    }

    /* ── Bool literal ────────────────────────────────────────────────────── */
    case IRON_NODE_BOOL_LIT: {
        Iron_BoolLit *lit = (Iron_BoolLit *)node;
        return iron_hir_expr_bool_lit(mod, lit->value, lit->resolved_type, span);
    }

    /* ── Null literal ────────────────────────────────────────────────────── */
    case IRON_NODE_NULL_LIT: {
        Iron_NullLit *lit = (Iron_NullLit *)node;
        return iron_hir_expr_null_lit(mod, lit->resolved_type, span);
    }

    /* ── Identifier ──────────────────────────────────────────────────────── */
    case IRON_NODE_IDENT: {
        Iron_Ident *id = (Iron_Ident *)node;

        /* 1. Look in lexical scope stack (locals and params) */
        IronHIR_VarId var_id = lookup_var(ctx, id->name);
        if (var_id != IRON_HIR_VAR_INVALID) {
            return iron_hir_expr_ident(mod, var_id, id->name,
                                       id->resolved_type, span);
        }

        /* 2. Check if it's already a lowered global */
        {
            ptrdiff_t gidx = shgeti(ctx->global_lowered_map, id->name);
            if (gidx >= 0) {
                IronHIR_VarId gid = ctx->global_lowered_map[gidx].value;
                return iron_hir_expr_ident(mod, gid, id->name,
                                           id->resolved_type, span);
            }
        }

        /* 3. Lazy lowering: if name is a global constant, inject a STMT_LET */
        {
            ptrdiff_t cidx = shgeti(ctx->global_constants_map, id->name);
            if (cidx >= 0 && ctx->current_block) {
                Iron_Node *init_node = ctx->global_constants_map[cidx].value;
                Iron_Type *ty        = id->resolved_type;
                bool is_mutable = shgeti(ctx->global_mutable_set, id->name) >= 0;
                IronHIR_Expr *init_expr = init_node
                                          ? lower_expr_hir(ctx, init_node)
                                          : NULL;
                IronHIR_VarId gid = iron_hir_alloc_var(mod, id->name, ty,
                                                        is_mutable);
                shput(ctx->global_lowered_map, id->name, gid);
                /* Inject the let at the current position in the current block */
                IronHIR_Stmt *let = iron_hir_stmt_let(mod, gid, ty, init_expr,
                                                       is_mutable, span);
                iron_hir_block_add_stmt(ctx->current_block, let);
                /* Also register in scope so subsequent refs don't re-inject */
                declare_var(ctx, id->name, gid);
                return iron_hir_expr_ident(mod, gid, id->name,
                                           id->resolved_type, span);
            }
        }

        /* 4. Function reference: check module funcs */
        for (int i = 0; i < mod->func_count; i++) {
            if (strcmp(mod->funcs[i]->name, id->name) == 0) {
                return iron_hir_expr_func_ref(mod, id->name,
                                              id->resolved_type, span);
            }
        }

        /* 5. Unresolved — emit func_ref as fallback for extern/builtin names */
        return iron_hir_expr_func_ref(mod, id->name, id->resolved_type, span);
    }

    /* ── Binary expression ───────────────────────────────────────────────── */
    case IRON_NODE_BINARY: {
        Iron_BinaryExpr *bin = (Iron_BinaryExpr *)node;
        IronHIR_BinOp hop = ast_op_to_hir_binop(bin->op);
        IronHIR_Expr *lhs = lower_expr_hir(ctx, bin->left);
        IronHIR_Expr *rhs = lower_expr_hir(ctx, bin->right);
        return iron_hir_expr_binop(mod, hop, lhs, rhs,
                                   bin->resolved_type, span);
    }

    /* ── Unary expression ────────────────────────────────────────────────── */
    case IRON_NODE_UNARY: {
        Iron_UnaryExpr *un = (Iron_UnaryExpr *)node;
        IronHIR_UnOp hop;
        switch (un->op) {
            case IRON_TOK_MINUS: hop = IRON_HIR_UNOP_NEG; break;
            case IRON_TOK_NOT:   hop = IRON_HIR_UNOP_NOT; break;
            default:             hop = IRON_HIR_UNOP_NEG; break;
        }
        IronHIR_Expr *operand = lower_expr_hir(ctx, un->operand);
        return iron_hir_expr_unop(mod, hop, operand, un->resolved_type, span);
    }

    /* ── Call expression ─────────────────────────────────────────────────── */
    case IRON_NODE_CALL: {
        Iron_CallExpr *ce = (Iron_CallExpr *)node;

        /* Primitive cast: Float(x), Int(x), etc. */
        if (ce->is_primitive_cast && ce->arg_count == 1) {
            /* Determine target type from callee name */
            Iron_Type *target_ty = ce->resolved_type;
            if (ce->callee->kind == IRON_NODE_IDENT) {
                Iron_Ident *callee_id = (Iron_Ident *)ce->callee;
                if (strcmp(callee_id->name, "Float") == 0)
                    target_ty = iron_type_make_primitive(IRON_TYPE_FLOAT);
                else if (strcmp(callee_id->name, "Int") == 0)
                    target_ty = iron_type_make_primitive(IRON_TYPE_INT);
                else if (strcmp(callee_id->name, "String") == 0)
                    target_ty = iron_type_make_primitive(IRON_TYPE_STRING);
            }
            IronHIR_Expr *val = lower_expr_hir(ctx, ce->args[0]);
            return iron_hir_expr_cast(mod, val, target_ty, span);
        }

        /* Build arg list */
        IronHIR_Expr **args = NULL;
        for (int i = 0; i < ce->arg_count; i++) {
            IronHIR_Expr *a = lower_expr_hir(ctx, ce->args[i]);
            arrput(args, a);
        }
        int arg_count = (int)arrlen(args);
        IronHIR_Expr *callee = lower_expr_hir(ctx, ce->callee);
        /* NOTE: args stb_ds array ownership transfers to the HIR expr — do NOT arrfree */
        return iron_hir_expr_call(mod, callee, args, arg_count,
                                   ce->resolved_type, span);
    }

    /* ── Method call ─────────────────────────────────────────────────────── */
    case IRON_NODE_METHOD_CALL: {
        Iron_MethodCallExpr *mc = (Iron_MethodCallExpr *)node;
        IronHIR_Expr **args = NULL;
        for (int i = 0; i < mc->arg_count; i++) {
            IronHIR_Expr *a = lower_expr_hir(ctx, mc->args[i]);
            arrput(args, a);
        }
        int arg_count = (int)arrlen(args);
        IronHIR_Expr *obj  = lower_expr_hir(ctx, mc->object);
        /* NOTE: args stb_ds array ownership transfers to the HIR expr — do NOT arrfree */
        return iron_hir_expr_method_call(mod, obj, mc->method,
                                          args, arg_count,
                                          mc->resolved_type, span);
    }

    /* ── Field access ────────────────────────────────────────────────────── */
    case IRON_NODE_FIELD_ACCESS: {
        Iron_FieldAccess *fa = (Iron_FieldAccess *)node;
        IronHIR_Expr *obj = lower_expr_hir(ctx, fa->object);
        return iron_hir_expr_field_access(mod, obj, fa->field,
                                           fa->resolved_type, span);
    }

    /* ── Index access ────────────────────────────────────────────────────── */
    case IRON_NODE_INDEX: {
        Iron_IndexExpr *ix = (Iron_IndexExpr *)node;
        IronHIR_Expr *arr = lower_expr_hir(ctx, ix->object);
        IronHIR_Expr *idx = lower_expr_hir(ctx, ix->index);
        return iron_hir_expr_index(mod, arr, idx, ix->resolved_type, span);
    }

    /* ── Slice ───────────────────────────────────────────────────────────── */
    case IRON_NODE_SLICE: {
        Iron_SliceExpr *sl = (Iron_SliceExpr *)node;
        IronHIR_Expr *arr   = lower_expr_hir(ctx, sl->object);
        IronHIR_Expr *start = sl->start ? lower_expr_hir(ctx, sl->start) : NULL;
        IronHIR_Expr *end   = sl->end   ? lower_expr_hir(ctx, sl->end)   : NULL;
        return iron_hir_expr_slice(mod, arr, start, end, sl->resolved_type, span);
    }

    /* ── Lambda ──────────────────────────────────────────────────────────── */
    case IRON_NODE_LAMBDA: {
        Iron_LambdaExpr *le = (Iron_LambdaExpr *)node;

        /* Build HIR params for the lambda */
        IronHIR_Param *hir_params = build_hir_params(ctx, le->params,
                                                       le->param_count);

        /* Push scope and declare params */
        push_scope(ctx);
        for (int p = 0; p < le->param_count; p++) {
            Iron_Param *ap = (Iron_Param *)le->params[p];
            IronHIR_VarId pid = iron_hir_alloc_var(mod, ap->name,
                                                     hir_params
                                                     ? hir_params[p].type
                                                     : NULL,
                                                     false);
            if (hir_params) hir_params[p].var_id = pid;
            declare_var(ctx, ap->name, pid);
        }
        IronHIR_Block *lambda_body = iron_hir_block_create(mod);
        lower_block_hir(ctx, (Iron_Block *)le->body, lambda_body);
        pop_scope(ctx);

        /* Assign lifted name now so the closure expr can store it */
        char lifted_name[64];
        snprintf(lifted_name, sizeof(lifted_name), "__lambda_%d",
                 ctx->lift_counter++);
        char *name_copy = (char *)iron_arena_alloc(
            ctx->module->arena,
            strlen(lifted_name) + 1,
            _Alignof(char));
        memcpy(name_copy, lifted_name, strlen(lifted_name) + 1);

        /* Extract the actual return type from the lambda's function type.
         * le->resolved_type is the whole func type (e.g. func(Int)->Int);
         * closure.return_type should be just the return portion (Int). */
        Iron_Type *ret_ty = NULL;
        if (le->resolved_type && le->resolved_type->kind == IRON_TYPE_FUNC) {
            ret_ty = le->resolved_type->func.return_type;
        } else {
            ret_ty = le->resolved_type;
        }
        IronHIR_Expr *result = iron_hir_expr_closure(mod,
                                                       hir_params ? hir_params : NULL,
                                                       le->param_count,
                                                       ret_ty, lambda_body,
                                                       le->resolved_type, name_copy, span);

        /* Queue for lifting */
        LiftPending lp;
        lp.kind           = LIFT_LAMBDA;
        lp.ast_node       = node;
        lp.lifted_name    = name_copy;
        lp.enclosing_func = ctx->current_func_name;
        arrput(ctx->pending_lifts, lp);

        return result;
    }

    /* ── Heap allocation ─────────────────────────────────────────────────── */
    case IRON_NODE_HEAP: {
        Iron_HeapExpr *he = (Iron_HeapExpr *)node;
        IronHIR_Expr  *inner = lower_expr_hir(ctx, he->inner);
        return iron_hir_expr_heap(mod, inner, he->auto_free, he->escapes,
                                   he->resolved_type, span);
    }

    /* ── RC allocation ───────────────────────────────────────────────────── */
    case IRON_NODE_RC: {
        Iron_RcExpr  *rc    = (Iron_RcExpr *)node;
        IronHIR_Expr *inner = lower_expr_hir(ctx, rc->inner);
        return iron_hir_expr_rc(mod, inner, rc->resolved_type, span);
    }

    /* ── Object construction ─────────────────────────────────────────────── */
    case IRON_NODE_CONSTRUCT: {
        Iron_ConstructExpr *ce    = (Iron_ConstructExpr *)node;
        Iron_Type          *ty    = ce->resolved_type;
        const char        **names = NULL;
        IronHIR_Expr      **vals  = NULL;

        /* Construct args map to positional fields — use NULL names */
        for (int i = 0; i < ce->arg_count; i++) {
            const char *fname = NULL;
            if (ty && ty->kind == IRON_TYPE_OBJECT && ty->object.decl) {
                Iron_ObjectDecl *od = ty->object.decl;
                if (i < od->field_count) {
                    fname = ((Iron_Field *)od->fields[i])->name;
                }
            }
            arrput(names, fname);
            IronHIR_Expr *v = lower_expr_hir(ctx, ce->args[i]);
            arrput(vals, v);
        }
        int fc = (int)arrlen(names);
        /* NOTE: names and vals stb_ds arrays ownership transfers to the HIR expr — do NOT arrfree */
        return iron_hir_expr_construct(mod, ty, names, vals, fc, span);
    }

    /* ── Array literal ───────────────────────────────────────────────────── */
    case IRON_NODE_ARRAY_LIT: {
        Iron_ArrayLit *al = (Iron_ArrayLit *)node;
        IronHIR_Expr **elems = NULL;
        for (int i = 0; i < al->element_count; i++) {
            IronHIR_Expr *e = lower_expr_hir(ctx, al->elements[i]);
            arrput(elems, e);
        }
        int ec = (int)arrlen(elems);
        Iron_Type *elem_ty = NULL;
        if (al->resolved_type && al->resolved_type->kind == IRON_TYPE_ARRAY) {
            elem_ty = al->resolved_type->array.elem;
        }
        /* NOTE: elems stb_ds array ownership transfers to the HIR expr — do NOT arrfree */
        return iron_hir_expr_array_lit(mod, elem_ty, elems, ec,
                                       al->resolved_type, span);
    }

    /* ── Await ───────────────────────────────────────────────────────────── */
    case IRON_NODE_AWAIT: {
        Iron_AwaitExpr *ae = (Iron_AwaitExpr *)node;
        IronHIR_Expr   *handle = lower_expr_hir(ctx, ae->handle);
        return iron_hir_expr_await(mod, handle, ae->resolved_type, span);
    }

    /* ── Comptime (already evaluated by analyzer; lower inner directly) ─── */
    case IRON_NODE_COMPTIME: {
        Iron_ComptimeExpr *ce = (Iron_ComptimeExpr *)node;
        return lower_expr_hir(ctx, ce->inner);
    }

    /* ── Is expression ───────────────────────────────────────────────────── */
    case IRON_NODE_IS: {
        Iron_IsExpr  *ie = (Iron_IsExpr *)node;
        IronHIR_Expr *val = lower_expr_hir(ctx, ie->expr);
        if (ie->type_name && strcmp(ie->type_name, "Null") == 0) {
            return iron_hir_expr_is_null(mod, val, span);
        }
        /* General type test */
        Iron_Type *check_ty = ie->resolved_type;
        return iron_hir_expr_is(mod, val, check_ty, span);
    }

    /* ── Error or unsupported node ───────────────────────────────────────── */
    case IRON_NODE_ERROR:
    default:
        /* Return null literal as poison for unsupported nodes */
        return iron_hir_expr_null_lit(mod, expr_type(node), span);

    } /* end switch */
}

/* ─────────────────────────────────────────────────────────────────────────── */
/* ── Block lowering helper ───────────────────────────────────────────────── */
/* ─────────────────────────────────────────────────────────────────────────── */

static void lower_block_hir(IronHIR_LowerCtx *ctx, Iron_Block *block,
                             IronHIR_Block *out) {
    if (!block || !out) return;

    IronHIR_Block *saved_block = ctx->current_block;
    ctx->current_block = out;

    push_defer_scope_hir(ctx);
    for (int i = 0; i < block->stmt_count; i++) {
        lower_stmt_hir(ctx, block->stmts[i]);
    }
    pop_defer_scope_hir(ctx);

    ctx->current_block = saved_block;
}

/* ─────────────────────────────────────────────────────────────────────────── */
/* ── Pass 1: Register module-level declarations ──────────────────────────── */
/* ─────────────────────────────────────────────────────────────────────────── */

static void lower_module_decls_hir(IronHIR_LowerCtx *ctx) {
    IronHIR_Module *mod = ctx->module;

    for (int i = 0; i < ctx->program->decl_count; i++) {
        Iron_Node *decl = ctx->program->decls[i];

        switch (decl->kind) {

        case IRON_NODE_FUNC_DECL: {
            Iron_FuncDecl *fd   = (Iron_FuncDecl *)decl;
            IronHIR_Param *params = build_hir_params(ctx, fd->params,
                                                      fd->param_count);
            Iron_Type *ret_ty = fd->resolved_return_type;
            if (!ret_ty) {
                ret_ty = resolve_type_ann(ctx, fd->return_type);
            }
            IronHIR_Func *f = iron_hir_func_create(mod, fd->name,
                                                     params, fd->param_count,
                                                     ret_ty);
            f->is_extern    = fd->is_extern;
            f->extern_c_name = fd->extern_c_name;
            iron_hir_module_add_func(mod, f);
            break;
        }

        case IRON_NODE_METHOD_DECL: {
            Iron_MethodDecl *md = (Iron_MethodDecl *)decl;

            /* Build mangled name: typeName_methodName (lowercase type name
             * to match Iron's C convention: Iron_io_read_file, not Iron_IO_read_file) */
            char mangled[256];
            snprintf(mangled, sizeof(mangled), "%s_%s",
                     md->type_name, md->method_name);
            for (int ci = 0; mangled[ci] && mangled[ci] != '_'; ci++) {
                if (mangled[ci] >= 'A' && mangled[ci] <= 'Z')
                    mangled[ci] = (char)(mangled[ci] + ('a' - 'A'));
            }

            /* For empty-body stubs (C-implemented methods), skip self param
             * to match the C runtime signature. Instance methods with bodies
             * get self as first param. */
            /* Stub: no body, OR body is an empty block (e.g., func Time.sleep(ms: Int) {}) */
            bool is_stub = (!md->body);
            if (!is_stub && md->body && md->body->kind == IRON_NODE_BLOCK) {
                Iron_Block *blk = (Iron_Block *)md->body;
                if (blk->stmt_count == 0) is_stub = true;
            }
            int total_params;
            IronHIR_Param *params;

            if (is_stub) {
                /* Stub method: only explicit params (no self) */
                total_params = md->param_count;
                params = NULL;
                if (total_params > 0) {
                    params = (IronHIR_Param *)iron_arena_alloc(
                        mod->arena,
                        (size_t)total_params * sizeof(IronHIR_Param),
                        _Alignof(IronHIR_Param));
                    for (int p = 0; p < md->param_count; p++) {
                        Iron_Param *ap = (Iron_Param *)md->params[p];
                        params[p].name   = ap->name;
                        params[p].type   = resolve_type_ann(ctx, ap->type_ann);
                        params[p].var_id = IRON_HIR_VAR_INVALID;
                    }
                }
            } else {
                /* Instance method: self + explicit params */
                total_params = md->param_count + 1;
                params = (IronHIR_Param *)iron_arena_alloc(
                    mod->arena,
                    (size_t)total_params * sizeof(IronHIR_Param),
                    _Alignof(IronHIR_Param));

                /* Self param: resolve to the object type by name */
                Iron_Type *self_type = NULL;
                if (ctx->program && md->type_name) {
                    for (int di = 0; di < ctx->program->decl_count; di++) {
                        Iron_Node *d = ctx->program->decls[di];
                        if (d->kind == IRON_NODE_OBJECT_DECL) {
                            Iron_ObjectDecl *od = (Iron_ObjectDecl *)d;
                            if (strcmp(od->name, md->type_name) == 0) {
                                self_type = iron_type_make_object(mod->arena, od);
                                break;
                            }
                        }
                    }
                }
                params[0].name   = "self";
                params[0].type   = self_type;
                params[0].var_id = IRON_HIR_VAR_INVALID;

                /* Explicit params */
                for (int p = 0; p < md->param_count; p++) {
                    Iron_Param *ap = (Iron_Param *)md->params[p];
                    params[p + 1].name   = ap->name;
                    params[p + 1].type   = resolve_type_ann(ctx, ap->type_ann);
                    params[p + 1].var_id = IRON_HIR_VAR_INVALID;
                }
            }

            Iron_Type *ret_ty = md->resolved_return_type;
            if (!ret_ty) ret_ty = resolve_type_ann(ctx, md->return_type);

            /* Copy mangled name to arena */
            size_t mlen = strlen(mangled) + 1;
            char *mname = (char *)iron_arena_alloc(mod->arena, mlen, _Alignof(char));
            memcpy(mname, mangled, mlen);

            IronHIR_Func *f = iron_hir_func_create(mod, mname, params,
                                                     total_params, ret_ty);
            iron_hir_module_add_func(mod, f);
            break;
        }

        case IRON_NODE_VAL_DECL: {
            Iron_ValDecl *vd = (Iron_ValDecl *)decl;
            if (vd->init) {
                shput(ctx->global_constants_map, vd->name, vd->init);
            }
            break;
        }

        case IRON_NODE_VAR_DECL: {
            Iron_VarDecl *vd = (Iron_VarDecl *)decl;
            if (vd->init) {
                shput(ctx->global_constants_map, vd->name, vd->init);
                shput(ctx->global_mutable_set, vd->name, 1);
            }
            break;
        }

        case IRON_NODE_OBJECT_DECL:
        case IRON_NODE_INTERFACE_DECL:
        case IRON_NODE_ENUM_DECL:
        case IRON_NODE_IMPORT_DECL:
        default:
            /* Type-level declarations: HIR module has no type_decls section.
             * Object/interface/enum info is preserved via the AST program reference
             * and accessed during HIR-to-LIR lowering. */
            break;
        }
    }
}

/* ─────────────────────────────────────────────────────────────────────────── */
/* ── Pass 2: Lower function bodies ──────────────────────────────────────── */
/* ─────────────────────────────────────────────────────────────────────────── */

static void lower_func_body_hir(IronHIR_LowerCtx *ctx,
                                 Iron_FuncDecl *fd) {
    if (!fd->body) return;  /* extern func — no body */

    IronHIR_Func *fn = find_hir_func(ctx->module, fd->name);
    if (!fn) return;

    ctx->current_func      = fn;
    ctx->current_func_name = fd->name;

    /* Create the function body block */
    fn->body = iron_hir_block_create(ctx->module);

    push_scope(ctx);

    /* Register params as VarIds in scope */
    for (int p = 0; p < fd->param_count; p++) {
        Iron_Param    *ap = (Iron_Param *)fd->params[p];
        Iron_Type     *pt = (p < fn->param_count) ? fn->params[p].type : NULL;
        IronHIR_VarId  pid = iron_hir_alloc_var(ctx->module, ap->name, pt, false);
        fn->params[p].var_id = pid;
        declare_var(ctx, ap->name, pid);
    }

    /* Lower body */
    IronHIR_Block *saved = ctx->current_block;
    ctx->current_block = fn->body;
    push_defer_scope_hir(ctx);

    Iron_Block *body = (Iron_Block *)fd->body;
    for (int i = 0; i < body->stmt_count; i++) {
        lower_stmt_hir(ctx, body->stmts[i]);
    }

    pop_defer_scope_hir(ctx);
    ctx->current_block = saved;

    pop_scope(ctx);
    ctx->current_func = NULL;
}

static void lower_method_body_hir(IronHIR_LowerCtx *ctx, Iron_MethodDecl *md) {
    if (!md->body) return;

    char mangled[256];
    snprintf(mangled, sizeof(mangled), "%s_%s", md->type_name, md->method_name);
    for (int ci = 0; mangled[ci] && mangled[ci] != '_'; ci++) {
        if (mangled[ci] >= 'A' && mangled[ci] <= 'Z')
            mangled[ci] = (char)(mangled[ci] + ('a' - 'A'));
    }

    IronHIR_Func *fn = find_hir_func(ctx->module, mangled);
    if (!fn) return;

    ctx->current_func      = fn;
    ctx->current_func_name = fn->name;

    fn->body = iron_hir_block_create(ctx->module);

    push_scope(ctx);

    /* Register params (fn->params[0] = self, fn->params[1..] = explicit) */
    for (int p = 0; p < fn->param_count; p++) {
        const char    *pname = fn->params[p].name;
        Iron_Type     *pt    = fn->params[p].type;
        IronHIR_VarId  pid   = iron_hir_alloc_var(ctx->module, pname, pt, false);
        fn->params[p].var_id = pid;
        declare_var(ctx, pname, pid);
    }

    IronHIR_Block *saved = ctx->current_block;
    ctx->current_block = fn->body;
    push_defer_scope_hir(ctx);

    Iron_Block *body = (Iron_Block *)md->body;
    for (int i = 0; i < body->stmt_count; i++) {
        lower_stmt_hir(ctx, body->stmts[i]);
    }

    pop_defer_scope_hir(ctx);
    ctx->current_block = saved;

    pop_scope(ctx);
    ctx->current_func = NULL;
}

static void lower_func_bodies_hir(IronHIR_LowerCtx *ctx) {
    for (int i = 0; i < ctx->program->decl_count; i++) {
        Iron_Node *decl = ctx->program->decls[i];
        if (decl->kind == IRON_NODE_FUNC_DECL) {
            lower_func_body_hir(ctx, (Iron_FuncDecl *)decl);
        } else if (decl->kind == IRON_NODE_METHOD_DECL) {
            lower_method_body_hir(ctx, (Iron_MethodDecl *)decl);
        }
    }
}

/* ─────────────────────────────────────────────────────────────────────────── */
/* ── Pass 3: Lift pending lambdas/spawn/pfor to top-level HIR functions ─── */
/* ─────────────────────────────────────────────────────────────────────────── */

static void lower_lift_pending_hir(IronHIR_LowerCtx *ctx) {
    int n = (int)arrlen(ctx->pending_lifts);
    for (int i = 0; i < n; i++) {
        LiftPending *lp = &ctx->pending_lifts[i];
        IronHIR_Module *mod = ctx->module;

        switch (lp->kind) {

        case LIFT_LAMBDA: {
            Iron_LambdaExpr *le = (Iron_LambdaExpr *)lp->ast_node;
            IronHIR_Param *params = build_hir_params(ctx, le->params,
                                                       le->param_count);
            /* Extract actual return type from the function type */
            Iron_Type *ret_ty = le->resolved_type;
            if (ret_ty && ret_ty->kind == IRON_TYPE_FUNC) {
                ret_ty = ret_ty->func.return_type;
            }
            IronHIR_Func *lifted = iron_hir_func_create(mod, lp->lifted_name,
                                                          params, le->param_count,
                                                          ret_ty);
            lifted->body = iron_hir_block_create(mod);

            /* Push scope and declare params */
            push_scope(ctx);
            ctx->current_func = lifted;
            ctx->current_func_name = lp->lifted_name;
            for (int p = 0; p < le->param_count; p++) {
                Iron_Param *ap = (Iron_Param *)le->params[p];
                Iron_Type  *pt = params ? params[p].type : NULL;
                IronHIR_VarId pid = iron_hir_alloc_var(mod, ap->name, pt, false);
                if (params) params[p].var_id = pid;
                declare_var(ctx, ap->name, pid);
            }

            IronHIR_Block *saved = ctx->current_block;
            ctx->current_block = lifted->body;
            push_defer_scope_hir(ctx);
            lower_block_hir(ctx, (Iron_Block *)le->body, lifted->body);
            pop_defer_scope_hir(ctx);
            ctx->current_block = saved;

            pop_scope(ctx);
            ctx->current_func = NULL;
            iron_hir_module_add_func(mod, lifted);
            break;
        }

        case LIFT_SPAWN: {
            Iron_SpawnStmt *ss = (Iron_SpawnStmt *)lp->ast_node;
            /* Spawn body is a block; create a no-arg void function */
            IronHIR_Func *lifted = iron_hir_func_create(mod, lp->lifted_name,
                                                          NULL, 0, NULL);
            lifted->body = iron_hir_block_create(mod);

            push_scope(ctx);
            ctx->current_func = lifted;
            ctx->current_func_name = lp->lifted_name;

            IronHIR_Block *saved = ctx->current_block;
            ctx->current_block = lifted->body;
            push_defer_scope_hir(ctx);
            lower_block_hir(ctx, (Iron_Block *)ss->body, lifted->body);
            pop_defer_scope_hir(ctx);
            ctx->current_block = saved;

            pop_scope(ctx);
            ctx->current_func = NULL;
            iron_hir_module_add_func(mod, lifted);
            break;
        }

        case LIFT_PARALLEL_FOR: {
            Iron_ForStmt *fs = (Iron_ForStmt *)lp->ast_node;
            /* pfor chunk function: takes the loop variable as an Int parameter */
            Iron_Type *int_ty = iron_type_make_primitive(IRON_TYPE_INT);
            IronHIR_Param *params = (IronHIR_Param *)iron_arena_alloc(
                mod->arena,
                sizeof(IronHIR_Param),
                _Alignof(IronHIR_Param));
            params[0].name   = fs->var_name;
            params[0].type   = int_ty;
            params[0].var_id = IRON_HIR_VAR_INVALID;

            IronHIR_Func *lifted = iron_hir_func_create(mod, lp->lifted_name,
                                                          params, 1, NULL);
            lifted->body = iron_hir_block_create(mod);

            push_scope(ctx);
            ctx->current_func = lifted;
            ctx->current_func_name = lp->lifted_name;

            IronHIR_VarId pid = iron_hir_alloc_var(mod, fs->var_name, int_ty, false);
            params[0].var_id = pid;
            declare_var(ctx, fs->var_name, pid);

            IronHIR_Block *saved = ctx->current_block;
            ctx->current_block = lifted->body;
            push_defer_scope_hir(ctx);
            lower_block_hir(ctx, (Iron_Block *)fs->body, lifted->body);
            pop_defer_scope_hir(ctx);
            ctx->current_block = saved;

            pop_scope(ctx);
            ctx->current_func = NULL;
            iron_hir_module_add_func(mod, lifted);
            break;
        }

        } /* end switch */
    }
}

/* ─────────────────────────────────────────────────────────────────────────── */
/* ── Public API ──────────────────────────────────────────────────────────── */
/* ─────────────────────────────────────────────────────────────────────────── */

IronHIR_Module *iron_hir_lower(Iron_Program *program, Iron_Scope *global_scope,
                               Iron_Arena *hir_arena, Iron_DiagList *diags) {
    (void)hir_arena; /* HIR module creates its own arena */
    if (!program || !diags) return NULL;

    /* Initialize primitive type singletons (idempotent) */
    iron_types_init(NULL);

    /* Create the module */
    IronHIR_Module *module = iron_hir_module_create("module");
    if (!module) return NULL;

    /* Initialize lowering context */
    IronHIR_LowerCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.program      = program;
    ctx.global_scope = global_scope;
    ctx.diags        = diags;
    ctx.module       = module;

    /* Pass 1: register declarations + collect global constants */
    lower_module_decls_hir(&ctx);

    /* Pass 2: lower function bodies */
    lower_func_bodies_hir(&ctx);

    /* Pass 3: lift pending lambdas/spawn/pfor */
    lower_lift_pending_hir(&ctx);

    /* Clean up context-owned resources */
    arrfree(ctx.pending_lifts);
    if (ctx.defer_stacks) {
        for (int d = 0; d < (int)arrlen(ctx.defer_stacks); d++) {
            arrfree(ctx.defer_stacks[d]);
        }
        arrfree(ctx.defer_stacks);
    }
    for (int d = 0; d < ctx.scope_depth; d++) {
        shfree(ctx.scope_stack[d]);
    }
    arrfree(ctx.scope_stack);
    shfree(ctx.global_constants_map);
    shfree(ctx.global_mutable_set);
    shfree(ctx.global_lowered_map);

    /* Verify the output module */
    Iron_DiagList verify_diags;
    memset(&verify_diags, 0, sizeof(verify_diags));
    Iron_Arena varena = iron_arena_create(64 * 1024);
    bool ok = iron_hir_verify(module, &verify_diags, &varena);
    if (!ok) {
        /* Verification failed — print errors now (while varena is still live),
         * then bump caller's error count so NULL return is handled correctly. */
        iron_diag_print_all(&verify_diags, NULL);
        diags->error_count += verify_diags.error_count > 0 ? verify_diags.error_count : 1;
        iron_diaglist_free(&verify_diags);
        iron_arena_free(&varena);
        iron_hir_module_destroy(module);
        return NULL;
    }
    iron_diaglist_free(&verify_diags);
    iron_arena_free(&varena);

    if (diags->error_count > 0) {
        iron_hir_module_destroy(module);
        return NULL;
    }

    return module;
}
