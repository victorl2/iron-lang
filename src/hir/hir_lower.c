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
    LiftKind           kind;
    Iron_Node         *ast_node;        /* the lambda/spawn/pfor AST node */
    const char        *lifted_name;     /* assigned name (__lambda_N, etc.) */
    const char        *enclosing_func;  /* name of the function containing this */
    /* Capture info for spawn/pfor: populated in Pass 2 while outer scope is live */
    IronHIR_VarId     *capture_var_ids; /* stb_ds array: outer-scope VarIds */
    Iron_CaptureEntry *captures;        /* capture metadata (name, type, is_mutable) */
    int                capture_count;
    /* Phase 22 OQ-04 / READ-08: carries the enclosing readonly context bit
     * forward from queue site to lift Pass 3. Plan 22-03 will define
     * IronHIR_Func.is_readonly and consume this bit at lift time
     * (lower_lift_pending_hir ~line 2027). */
    bool               is_readonly_context;
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

    /* Phase 22 OQ-04: readonly flag of the current enclosing function/method;
     * set by lower_func_body_hir / lower_method_body_hir; consumed by the
     * lambda-queue site to populate LiftPending.is_readonly_context for
     * Plan 22-03's IronHIR_Func.is_readonly assignment. */
    bool             current_func_is_readonly;

    /* Global constant lazy lowering */
    struct { char *key; Iron_Node *value; } *global_constants_map;
    struct { char *key; int value; }        *global_mutable_set;

    /* Tracks which globals have been lowered (name -> VarId) */
    struct { char *key; IronHIR_VarId value; } *global_lowered_map;

    /* Phase 28 ARENA-05 (Plan 28-04): lexical `in arena {}` nesting depth.
     * Incremented on entry to an Iron_InArenaBlock body, decremented on exit.
     * A bare `heap T(...)` lowered while depth > 0 becomes an arena allocation
     * against the TLS-current arena (arena_expr NULL); outside any block it
     * stays a plain IRON_HIR_EXPR_HEAP. */
    int              in_arena_depth;
} IronHIR_LowerCtx;

/* ── Forward declarations ────────────────────────────────────────────────── */

static IronHIR_Stmt *lower_stmt_hir(IronHIR_LowerCtx *ctx, Iron_Node *node);
static IronHIR_Expr *lower_expr_hir(IronHIR_LowerCtx *ctx, Iron_Node *node);
static void lower_block_hir(IronHIR_LowerCtx *ctx, Iron_Block *block,
                             IronHIR_Block *out);

/* Iron_ExprNode lives in src/parser/ast.h now (Phase 66 PROT-01). expr_type()
 * below uses the shared typedef — the layout is compile-time-enforced there. */

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

    /* Phase 59 01d: tuple type annotation — (T0, T1, ...) */
    if (ta->is_tuple) {
        int n = ta->tuple_elem_count;
        Iron_Type **elem_types = (Iron_Type **)iron_arena_alloc(
            ctx->module->arena, sizeof(Iron_Type *) * (size_t)n,
            _Alignof(Iron_Type *));
        if (!elem_types) iron_oom_abort("hir_lower.c:resolve_type_ann tuple_elems");
        for (int i = 0; i < n; i++) {
            elem_types[i] = ta->tuple_elems
                ? resolve_type_ann(ctx, ta->tuple_elems[i])
                : iron_type_make_primitive(IRON_TYPE_ERROR);
            if (!elem_types[i]) {
                elem_types[i] = iron_type_make_primitive(IRON_TYPE_ERROR);
            }
        }
        return iron_type_make_tuple(ctx->module->arena, elem_types, n);
    }

    /* Phase 33: func-type annotation — func(T1, T2) -> R */
    if (ta->is_func) {
        Iron_Type **param_types = NULL;
        int param_count = ta->func_param_count;
        if (param_count > 0) {
            param_types = (Iron_Type **)iron_arena_alloc(
                ctx->module->arena,
                (size_t)param_count * sizeof(Iron_Type *),
                _Alignof(Iron_Type *));
            if (!param_types) iron_oom_abort("hir_lower.c:resolve_type_ann func_params");
            for (int i = 0; i < param_count; i++) {
                param_types[i] = resolve_type_ann(ctx, ta->func_params[i]);
            }
        }
        Iron_Type *ret = ta->func_return
            ? resolve_type_ann(ctx, ta->func_return)
            : iron_type_make_primitive(IRON_TYPE_VOID);
        base = iron_type_make_func(ctx->module->arena, param_types, param_count, ret);
        if (ta->is_nullable && base) {
            base = iron_type_make_nullable(ctx->module->arena, base);
        }
        if (ta->is_array && base) {
            base = iron_type_make_array(ctx->module->arena, base, -1, false);
        }
        return base;
    }

    if (strcmp(ta->name, "Int") == 0)         base = iron_type_make_primitive(IRON_TYPE_INT);
    else if (strcmp(ta->name, "Float") == 0)  base = iron_type_make_primitive(IRON_TYPE_FLOAT);
    else if (strcmp(ta->name, "Bool") == 0)   base = iron_type_make_primitive(IRON_TYPE_BOOL);
    else if (strcmp(ta->name, "String") == 0) base = iron_type_make_primitive(IRON_TYPE_STRING);
    else if (strcmp(ta->name, "Void") == 0)   base = iron_type_make_primitive(IRON_TYPE_VOID);
    else if (strcmp(ta->name, "Int8") == 0)   base = iron_type_make_primitive(IRON_TYPE_INT8);
    else if (strcmp(ta->name, "Int16") == 0)  base = iron_type_make_primitive(IRON_TYPE_INT16);
    else if (strcmp(ta->name, "Int32") == 0)  base = iron_type_make_primitive(IRON_TYPE_INT32);
    else if (strcmp(ta->name, "Int64") == 0)  base = iron_type_make_primitive(IRON_TYPE_INT64);
    else if (strcmp(ta->name, "UInt") == 0)   base = iron_type_make_primitive(IRON_TYPE_UINT);
    else if (strcmp(ta->name, "UInt8") == 0)  base = iron_type_make_primitive(IRON_TYPE_UINT8);
    else if (strcmp(ta->name, "UInt16") == 0) base = iron_type_make_primitive(IRON_TYPE_UINT16);
    else if (strcmp(ta->name, "UInt32") == 0) base = iron_type_make_primitive(IRON_TYPE_UINT32);
    else if (strcmp(ta->name, "UInt64") == 0) base = iron_type_make_primitive(IRON_TYPE_UINT64);
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
                } else if (decl->kind == IRON_NODE_INTERFACE_DECL) {
                    Iron_InterfaceDecl *id = (Iron_InterfaceDecl *)decl;
                    if (strcmp(id->name, ta->name) == 0) {
                        base = iron_type_make_interface(ctx->module->arena, id);
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
        base = iron_type_make_array(ctx->module->arena, base, size, false);
    }
    return base;
}

/* ── Build HIR param array from AST params ───────────────────────────────── */

static IronHIR_Param *build_hir_params_named(IronHIR_LowerCtx *ctx,
                                               Iron_Node **params,
                                               int param_count,
                                               const char *func_name) {
    if (param_count == 0) return NULL;

    /* Try to get pre-resolved param types from the global-scope function symbol.
     * The type checker runs before HIR lowering and resolves generic type annotations
     * (e.g. Option[Int]) into monomorphized Iron_Type objects.  We use these
     * resolved types instead of re-resolving from the raw type annotation, which
     * would lose the generic instantiation information. */
    Iron_Type **resolved_types = NULL;
    if (func_name && ctx->global_scope) {
        Iron_Symbol *fsym = iron_scope_lookup(ctx->global_scope, func_name);
        if (fsym && fsym->type && fsym->type->kind == IRON_TYPE_FUNC &&
            fsym->type->func.param_count == param_count) {
            resolved_types = fsym->type->func.param_types;
        }
    }

    IronHIR_Param *arr = (IronHIR_Param *)iron_arena_alloc(
        ctx->module->arena,
        (size_t)param_count * sizeof(IronHIR_Param),
        _Alignof(IronHIR_Param));
    if (!arr) iron_oom_abort("hir_lower.c:build_hir_params_named");
    for (int p = 0; p < param_count; p++) {
        Iron_Param *ap = (Iron_Param *)params[p];
        Iron_Type  *pt;
        if (resolved_types && resolved_types[p]) {
            pt = resolved_types[p];  /* use type-checker resolved type */
        } else {
            pt = resolve_type_ann(ctx, ap->type_ann);
        }
        arr[p].name   = ap->name;
        arr[p].type   = pt;
        /* var_id assigned later when we push func scope */
        arr[p].var_id = IRON_HIR_VAR_INVALID;
    }
    return arr;
}

/* Backwards-compat wrapper — no function name, no global-scope lookup */
static IronHIR_Param *build_hir_params(IronHIR_LowerCtx *ctx,
                                        Iron_Node **params, int param_count) {
    return build_hir_params_named(ctx, params, param_count, NULL);
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
    switch ((int)op) {
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
        case IRON_TOK_SHL:        return IRON_HIR_BINOP_SHL;
        case IRON_TOK_SHR:        return IRON_HIR_BINOP_SHR;
        case IRON_TOK_AMP:        return IRON_HIR_BINOP_BAND;
        case IRON_TOK_PIPE:       return IRON_HIR_BINOP_BOR;
        case IRON_TOK_CARET:      return IRON_HIR_BINOP_BXOR;
        /* AUDIT-02 #2 fix: previously defaulted silently to ADD, masking
         * upstream parser-recovery bugs. Non-binary tokens reach this arm
         * only when parser error recovery produced a malformed BinaryExpr;
         * the fallback lets HIR lowering proceed so later passes emit a
         * coherent diagnostic instead of crashing. */
        /* -Wswitch-enum opt-out: Iron_TokenKind has ~80 values; only
         * infix-operator tokens are legal as Iron_OpKind here. */
        default:                  return IRON_HIR_BINOP_ADD; /* fallback */
    }
}

/* Map compound-assign token to base binop token */
static Iron_OpKind compound_assign_base_op(Iron_OpKind op) {
    switch ((int)op) {
        case IRON_TOK_PLUS_ASSIGN:   return IRON_TOK_PLUS;
        case IRON_TOK_MINUS_ASSIGN:  return IRON_TOK_MINUS;
        case IRON_TOK_STAR_ASSIGN:   return IRON_TOK_STAR;
        case IRON_TOK_SLASH_ASSIGN:  return IRON_TOK_SLASH;
        case IRON_TOK_SHL_ASSIGN:    return IRON_TOK_SHL;
        case IRON_TOK_SHR_ASSIGN:    return IRON_TOK_SHR;
        case IRON_TOK_AMP_ASSIGN:    return IRON_TOK_AMP;
        case IRON_TOK_PIPE_ASSIGN:   return IRON_TOK_PIPE;
        case IRON_TOK_CARET_ASSIGN:  return IRON_TOK_CARET;
        /* AUDIT-02 #3 fix: non-compound-assign tokens silently mapped to
         * PLUS. They are not legal callers of this helper, so the fallback
         * is defensive — a wrong mapping here shows up as a type error
         * downstream. */
        /* -Wswitch-enum opt-out: Iron_TokenKind has ~80 values; only the
         * compound-assign tokens are legal inputs. */
        default:                     return IRON_TOK_PLUS;
    }
}

static bool is_compound_assign(Iron_OpKind op) {
    return op == IRON_TOK_PLUS_ASSIGN  ||
           op == IRON_TOK_MINUS_ASSIGN ||
           op == IRON_TOK_STAR_ASSIGN  ||
           op == IRON_TOK_SLASH_ASSIGN ||
           op == IRON_TOK_SHL_ASSIGN   ||
           op == IRON_TOK_SHR_ASSIGN   ||
           op == IRON_TOK_AMP_ASSIGN   ||
           op == IRON_TOK_PIPE_ASSIGN  ||
           op == IRON_TOK_CARET_ASSIGN;
}

/* ── ADT pattern binding injection ────────────────────────────────────────── */

/* Recursively inject IRON_HIR_STMT_LET nodes for pattern bindings into `out`.
 * scrut_expr: the HIR expression for the match scrutinee (or a sub-value for nested).
 * enum_type:  the Iron_Type of the value being matched (may be NULL for top-level,
 *             resolved from pat->enum_name via global_scope).
 * pat:        the Iron_Pattern AST node.
 * field_prefix: dotted field path built up so far (empty string for top-level).
 * out:        the HIR block into which LET stmts are prepended. */
static void inject_pattern_let_stmts(IronHIR_LowerCtx *ctx,
                                      IronHIR_Block     *out,
                                      IronHIR_Expr      *scrut_expr,
                                      Iron_Type         *enum_type,
                                      Iron_Pattern      *pat,
                                      const char        *field_prefix,
                                      Iron_Span          span) {
    IronHIR_Module *mod = ctx->module;

    /* Resolve enum type from pattern name if not provided */
    if (!enum_type && pat->enum_name) {
        Iron_Symbol *esym = iron_scope_lookup(ctx->global_scope, pat->enum_name);
        if (esym && esym->type && esym->type->kind == IRON_TYPE_ENUM) {
            enum_type = esym->type;
        }
    }
    if (!enum_type || enum_type->kind != IRON_TYPE_ENUM) return;

    Iron_EnumDecl *ed = enum_type->enu.decl;
    if (!ed || !enum_type->enu.variant_payload_types) return;

    /* Find variant index */
    int vi = -1;
    for (int i = 0; i < ed->variant_count; i++) {
        Iron_EnumVariant *ev = (Iron_EnumVariant *)ed->variants[i];
        if (strcmp(ev->name, pat->variant_name) == 0) { vi = i; break; }
    }
    if (vi < 0) return;

    Iron_Type **ptypes = enum_type->enu.variant_payload_types[vi];
    Iron_EnumVariant *ev = (Iron_EnumVariant *)ed->variants[vi];

    for (int b = 0; b < pat->binding_count && b < ev->payload_count; b++) {
        const char *bname  = pat->binding_names ? pat->binding_names[b] : NULL;
        Iron_Node  *nested = (pat->nested_patterns && pat->nested_patterns[b])
                             ? pat->nested_patterns[b] : NULL;
        Iron_Type  *ptype  = (ptypes && ptypes[b]) ? ptypes[b] : NULL;

        /* Build the field path for this slot */
        char slot_field[256];
        if (field_prefix && field_prefix[0]) {
            snprintf(slot_field, sizeof(slot_field), "%s.data.%s._%d",
                     field_prefix, pat->variant_name, b);
        } else {
            snprintf(slot_field, sizeof(slot_field), "data.%s._%d",
                     pat->variant_name, b);
        }

        if (bname) {
            /* Simple binding: val bname = scrut.data.VariantName._b */
            char *slot_field_copy = iron_arena_strdup(mod->arena, slot_field,
                                                      strlen(slot_field));
            if (!slot_field_copy) iron_oom_abort("hir_lower.c:inject_pattern_let_stmts slot_field");
            IronHIR_Expr *field_expr = iron_hir_expr_field_access(mod, scrut_expr,
                                         slot_field_copy,
                                         ptype, span);
            IronHIR_VarId vid = iron_hir_alloc_var(mod, bname, ptype, false);
            declare_var(ctx, bname, vid);
            IronHIR_Stmt *let_s = iron_hir_stmt_let(mod, vid, ptype, field_expr,
                                                      false, span);
            iron_hir_block_add_stmt(out, let_s);
        } else if (nested && nested->kind == IRON_NODE_PATTERN) {
            /* Nested pattern: recurse with the sub-enum type */
            Iron_Pattern *npat = (Iron_Pattern *)nested;
            /* Resolve nested enum type */
            Iron_Type *nested_enum_type = ptype;
            if (npat->enum_name) {
                Iron_Symbol *esym2 = iron_scope_lookup(ctx->global_scope, npat->enum_name);
                if (esym2 && esym2->type && esym2->type->kind == IRON_TYPE_ENUM) {
                    nested_enum_type = esym2->type;
                }
            }
            inject_pattern_let_stmts(ctx, out, scrut_expr, nested_enum_type,
                                      npat, slot_field, span);
        }
        /* else: wildcard _ — skip */
    }
}

/* Lower a single statement node; emit it into ctx->current_block.
 * Returns a HIR stmt if the caller needs to add it to a block manually,
 * otherwise NULL (already appended). */
static IronHIR_Stmt *lower_stmt_hir(IronHIR_LowerCtx *ctx, Iron_Node *node) {
    if (!node) return NULL;
    IronHIR_Module *mod  = ctx->module;
    IronHIR_Block  *blk  = ctx->current_block;
    Iron_Span       span = node->span;

    switch ((int)(node->kind)) {

    /* ── Val declaration ───────────────────────────────────────────────────── */
    case IRON_NODE_VAL_DECL: {
        Iron_ValDecl *vd = (Iron_ValDecl *)node;
        Iron_Type    *ty = vd->declared_type;
        if (!ty) ty = resolve_type_ann(ctx, vd->type_ann);

        /* Phase 59 01d: destructure binding — desugar into
         *   let __tuple_tmp_N = init
         *   let <name_i>      = __tuple_tmp_N.v<i>  (per binding)
         * Wildcard bindings are skipped. Zero new HIR node kinds. */
        if (vd->binding_count > 0) {
            IronHIR_Expr *init_expr = vd->init
                ? lower_expr_hir(ctx, vd->init)
                : NULL;
            /* Resolved tuple type: prefer the declared_type set by the
             * typechecker (authoritative); fall back to the lowered
             * expression's type. */
            Iron_Type *tuple_ty = vd->declared_type
                ? vd->declared_type
                : (init_expr ? init_expr->type : NULL);
            if (!tuple_ty || tuple_ty->kind != IRON_TYPE_TUPLE) {
                /* Typechecker should have caught this; emit a stub let so
                 * downstream passes have something to chew on and return. */
                iron_hir_block_add_stmt(blk,
                    iron_hir_stmt_let(mod, iron_hir_alloc_var(mod, "__bad_tuple",
                                                                iron_type_make_primitive(IRON_TYPE_ERROR),
                                                                false),
                                      iron_type_make_primitive(IRON_TYPE_ERROR),
                                      init_expr, false, span));
                return NULL;
            }

            /* Create tmp var for the tuple init. */
            char tmp_name[64];
            snprintf(tmp_name, sizeof(tmp_name), "__tuple_tmp_%d",
                     ctx->lift_counter++);
            IronHIR_VarId tmp_id = iron_hir_alloc_var(mod, tmp_name,
                                                       tuple_ty, false);
            iron_hir_block_add_stmt(blk,
                iron_hir_stmt_let(mod, tmp_id, tuple_ty, init_expr,
                                  false, span));

            /* Emit one let per binding (skip wildcards). */
            for (int i = 0; i < vd->binding_count; i++) {
                const char *binding = vd->binding_names[i];
                if (!binding) continue;  /* wildcard */
                Iron_Type *elem_ty = (i < tuple_ty->tuple.elem_count)
                    ? tuple_ty->tuple.elem_types[i]
                    : iron_type_make_primitive(IRON_TYPE_ERROR);
                IronHIR_VarId bind_id = iron_hir_alloc_var(mod, binding,
                                                            elem_ty, false);
                declare_var(ctx, binding, bind_id);

                char field_name[12];
                snprintf(field_name, sizeof(field_name), "v%d", i);
                const char *fname_arena = (const char *)iron_arena_alloc(
                    ctx->module->arena, strlen(field_name) + 1, 1);
                if (!fname_arena) iron_oom_abort("hir_lower.c:lower_stmt_hir tuple_destructure fname");
                memcpy((char *)fname_arena, field_name, strlen(field_name) + 1);

                IronHIR_Expr *tmp_ref = iron_hir_expr_ident(mod, tmp_id,
                                                             tmp_name,
                                                             tuple_ty, span);
                IronHIR_Expr *elem_expr = iron_hir_expr_field_access(mod,
                                                                      tmp_ref,
                                                                      fname_arena,
                                                                      elem_ty,
                                                                      span);
                iron_hir_block_add_stmt(blk,
                    iron_hir_stmt_let(mod, bind_id, elem_ty, elem_expr,
                                      false, span));
            }
            return NULL;
        }

        if (vd->init && vd->init->kind == IRON_NODE_SPAWN) {
            /* val h = spawn("name") { body } -- spawn handle binding.
             * Allocate the HIR var for h, then emit a spawn stmt that binds
             * its result LIR value to this var (via handle_var). */
            Iron_Type *obj_ty = iron_type_make_primitive(IRON_TYPE_OBJECT);
            IronHIR_VarId id  = iron_hir_alloc_var(mod, vd->name, obj_ty, false);
            declare_var(ctx, vd->name, id);

            /* Lower the spawn as a statement (sets handle_name) */
            Iron_SpawnStmt *ss = (Iron_SpawnStmt *)vd->init;
            const char     *hname = ss->handle_name ? ss->handle_name : vd->name;

            char lifted_name[64];
            snprintf(lifted_name, sizeof(lifted_name), "__spawn_%d",
                     ctx->lift_counter++);
            char *name_copy = (char *)iron_arena_alloc(
                ctx->module->arena,
                strlen(lifted_name) + 1,
                _Alignof(char));
            if (!name_copy) iron_oom_abort("hir_lower.c:lower_stmt_hir val_decl spawn lifted_name");
            memcpy(name_copy, lifted_name, strlen(lifted_name) + 1);

            IronHIR_Block *spawn_body = iron_hir_block_create(mod);
            lower_block_hir(ctx, (Iron_Block *)ss->body, spawn_body);

            IronHIR_Stmt *spawn_s = iron_hir_stmt_spawn(mod, hname, spawn_body, name_copy, span);
            spawn_s->spawn.handle_var = id;  /* bind spawn result to h */

            /* Resolve capture VarIds from current (outer) scope while it is live */
            if (ss->capture_count > 0 && ss->captures) {
                IronHIR_VarId *cap_var_ids = NULL;
                for (int c = 0; c < ss->capture_count; c++) {
                    IronHIR_VarId vid = lookup_var(ctx, ss->captures[c].name);
                    arrput(cap_var_ids, vid);
                }
                spawn_s->spawn.capture_var_ids = cap_var_ids;
                spawn_s->spawn.captures        = ss->captures;
                spawn_s->spawn.capture_count   = ss->capture_count;
            }

            iron_hir_block_add_stmt(blk, spawn_s);

            /* Queue for lifting — store capture info while outer scope is still live */
            LiftPending lp;
            memset(&lp, 0, sizeof(lp));
            lp.kind             = LIFT_SPAWN;
            lp.ast_node         = vd->init;
            lp.lifted_name      = name_copy;
            lp.enclosing_func   = ctx->current_func_name;
            lp.captures         = ss->captures;
            lp.capture_count    = ss->capture_count;
            lp.capture_var_ids  = spawn_s->spawn.capture_var_ids;
            arrput(ctx->pending_lifts, lp);

            /* Create HIR LET with null init -- the spawn result will be bound
             * in hir_to_lir.c via spawn_s->spawn.handle_var */
            IronHIR_Stmt *let_s = iron_hir_stmt_let(mod, id, obj_ty, NULL, false, span);
            iron_hir_block_add_stmt(blk, let_s);
            return NULL;
        }

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

        /* Phase 83-02 ACCESS-05: pub-setter dispatch.
         * When the typechecker flagged this assign as a pub-var-field write,
         * lower it as a call to the synthesized set_<field> method rather
         * than a direct field store. The synth setter body runs the actual
         * self.<field> = _v store with its own direct-access FieldAccess
         * (is_pub_access=false), so there is no infinite loop.
         *
         * The target FieldAccess is NEVER lowered in this branch — its
         * is_pub_access flag (set during LHS typecheck) is intentionally
         * unread here. The assign becomes a statement-level method call
         * expression. */
        if (as->is_pub_setter && as->target &&
            as->target->kind == IRON_NODE_FIELD_ACCESS) {
            Iron_FieldAccess *tfa = (Iron_FieldAccess *)as->target;
            IronHIR_Expr *obj   = lower_expr_hir(ctx, tfa->object);
            IronHIR_Expr *value = lower_expr_hir(ctx, as->value);
            /* Build the setter name: set_<field>. Arena-alloc so the HIR
             * expression can reference the string stably. */
            size_t flen = strlen(tfa->field);
            char *setter_name = (char *)iron_arena_alloc(
                mod->arena, flen + 5 /* "set_" + NUL */,
                _Alignof(char));
            if (!setter_name) iron_oom_abort("hir_lower.c:ASSIGN pub_setter name");
            snprintf(setter_name, flen + 5, "set_%s", tfa->field);
            /* One-arg method call: set_<field>(value). */
            IronHIR_Expr **args = NULL;
            arrput(args, value);
            IronHIR_Expr *mc = iron_hir_expr_method_call(
                mod, obj, setter_name, args, 1, NULL, span);
            IronHIR_Stmt *s = iron_hir_stmt_expr(mod, mc, span);
            iron_hir_block_add_stmt(blk, s);
            return NULL;
        }

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
            if (!name_copy) iron_oom_abort("hir_lower.c:lower_stmt_hir parallel_for lifted_name");
            memcpy(name_copy, lifted_name, strlen(lifted_name) + 1);

            Iron_Type *void_ty = iron_type_make_primitive(IRON_TYPE_VOID);
            IronHIR_Expr *pfor_expr = iron_hir_expr_parallel_for(mod, loop_var,
                                                                   range_expr,
                                                                   pfor_body,
                                                                   void_ty, name_copy, span);

            /* Resolve pfor capture VarIds from current (outer) scope while it is live */
            if (fs->pfor_capture_count > 0 && fs->pfor_captures) {
                IronHIR_VarId *cap_var_ids = NULL;
                for (int c = 0; c < fs->pfor_capture_count; c++) {
                    IronHIR_VarId vid = lookup_var(ctx, fs->pfor_captures[c].name);
                    arrput(cap_var_ids, vid);
                }
                pfor_expr->parallel_for.capture_var_ids = cap_var_ids;
                pfor_expr->parallel_for.captures        = fs->pfor_captures;
                pfor_expr->parallel_for.capture_count   = fs->pfor_capture_count;
            }

            IronHIR_Stmt *s = iron_hir_stmt_expr(mod, pfor_expr, span);
            iron_hir_block_add_stmt(blk, s);

            /* Queue for lifting — store capture info while outer scope is still live */
            LiftPending lp;
            memset(&lp, 0, sizeof(lp));
            lp.kind             = LIFT_PARALLEL_FOR;
            lp.ast_node         = node;
            lp.lifted_name      = name_copy;
            lp.enclosing_func   = ctx->current_func_name;
            lp.captures         = fs->pfor_captures;
            lp.capture_count    = fs->pfor_capture_count;
            lp.capture_var_ids  = pfor_expr->parallel_for.capture_var_ids;
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
            /* Lower user body into an inner block so defer fires before increment */
            IronHIR_Block *inner_blk = iron_hir_block_create(mod);
            lower_block_hir(ctx, (Iron_Block *)fs->body, inner_blk);
            pop_scope(ctx);

            /* Wrap user body in BLOCK stmt — gives it its own defer scope so any
             * deferred stmts inside the loop body fire at the end of the iteration
             * scope, before the loop increment below. */
            IronHIR_Stmt *block_stmt = iron_hir_stmt_block(mod, inner_blk, s0);

            /* Build the outer body: [block_stmt, inc_stmt] */
            IronHIR_Block *body_blk = iron_hir_block_create(mod);
            iron_hir_block_add_stmt(body_blk, block_stmt);

            /* Append increment: i = i + 1 (runs after defer scope exits) */
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
        Iron_Type      *scrut_ty = expr_type(ms->subject);
        IronHIR_MatchArm *arms = NULL;

        for (int i = 0; i < ms->case_count; i++) {
            Iron_MatchCase *mc = (Iron_MatchCase *)ms->cases[i];
            IronHIR_Expr  *pat = lower_expr_hir(ctx, mc->pattern);
            IronHIR_Block *mbody = iron_hir_block_create(mod);

            /* For ADT patterns: push scope, inject binding LET stmts, lower body */
            if (mc->pattern && mc->pattern->kind == IRON_NODE_PATTERN) {
                Iron_Pattern *ast_pat = (Iron_Pattern *)mc->pattern;
                push_scope(ctx);
                inject_pattern_let_stmts(ctx, mbody, scrut, scrut_ty,
                                          ast_pat, "", span);
                lower_block_hir(ctx, (Iron_Block *)mc->body, mbody);
                pop_scope(ctx);
            } else {
                lower_block_hir(ctx, (Iron_Block *)mc->body, mbody);
            }

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

        /* Phase 32 DEFER-01: `defer` accepts any statement. Build a defer
         * body (an IronHIR_Block) by lowering ds->expr; the downstream
         * emit_scope_defers / emit_defer_cleanup inline-lower whatever
         * statements defer_body holds at every normal exit edge, in LIFO order
         * (DEFER-03) ahead of local drops (DEFER-04). No machinery change.
         *
         * Pitfall 4: produce NO HIR node before lowering on a genuinely
         * un-lowerable body (NULL / error). */
        if (!ds->expr || ds->expr->kind == IRON_NODE_ERROR) {
            return NULL;
        }

        IronHIR_Block  *defer_body = iron_hir_block_create(mod);
        if (ds->expr->kind == IRON_NODE_FREE) {
            /* `defer free <ident>` fast-path — kept byte-identical to Phase 21
             * so existing `defer free` codegen is unchanged. ds->expr is the
             * IRON_NODE_FREE statement node; lower_expr_hir cannot lower FREE,
             * so we lower its operand and build an IRON_HIR_STMT_FREE. */
            Iron_FreeStmt *fs = (Iron_FreeStmt *)ds->expr;
            IronHIR_Expr *free_val = lower_expr_hir(ctx, fs->expr);
            IronHIR_Stmt *dstmt = iron_hir_stmt_free(mod, free_val, span);
            iron_hir_block_add_stmt(defer_body, dstmt);
        } else if (ds->expr->kind == IRON_NODE_BLOCK) {
            /* `defer { ... }` — lower each inner statement into defer_body
             * (lower_block_hir swaps ctx->current_block + brackets a defer
             * scope around the inner statements). */
            lower_block_hir(ctx, (Iron_Block *)ds->expr, defer_body);
        } else {
            /* General single statement — lower it directly into defer_body by
             * temporarily retargeting ctx->current_block (mirrors the
             * self-append discipline lower_block_hir relies on). */
            IronHIR_Block *saved_block = ctx->current_block;
            ctx->current_block = defer_body;
            lower_stmt_hir(ctx, ds->expr);
            ctx->current_block = saved_block;
        }
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
        if (!name_copy) iron_oom_abort("hir_lower.c:lower_stmt_hir spawn lifted_name");
        memcpy(name_copy, lifted_name, strlen(lifted_name) + 1);

        IronHIR_Block *spawn_body = iron_hir_block_create(mod);
        lower_block_hir(ctx, (Iron_Block *)ss->body, spawn_body);

        IronHIR_Stmt *s = iron_hir_stmt_spawn(mod, hname, spawn_body, name_copy, span);

        /* Resolve capture VarIds from the current (enclosing) scope.
         * capture analysis (pass 3b of semantic pipeline) has already populated
         * ss->captures[]. We resolve each capture name to its outer-scope VarId
         * while the outer scope is still live (Pass 2). */
        if (ss->capture_count > 0 && ss->captures) {
            IronHIR_VarId *cap_var_ids = NULL;
            for (int c = 0; c < ss->capture_count; c++) {
                IronHIR_VarId vid = lookup_var(ctx, ss->captures[c].name);
                arrput(cap_var_ids, vid);
            }
            s->spawn.capture_var_ids = cap_var_ids;
            s->spawn.captures        = ss->captures;
            s->spawn.capture_count   = ss->capture_count;
        }

        iron_hir_block_add_stmt(blk, s);

        /* Queue for lifting — store capture info while outer scope is still live */
        LiftPending lp;
        memset(&lp, 0, sizeof(lp));
        lp.kind             = LIFT_SPAWN;
        lp.ast_node         = node;
        lp.lifted_name      = name_copy;
        lp.enclosing_func   = ctx->current_func_name;
        lp.captures         = ss->captures;
        lp.capture_count    = ss->capture_count;
        lp.capture_var_ids  = s->spawn.capture_var_ids; /* resolved in outer scope above */
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

    /* Phase 28 ARENA-04/05 (Plan 28-04): `in <arena> { ... }` default-arena
     * block. Lower the arena expression, then the body with in_arena_depth
     * bumped so bare `heap T(...)` inside resolves to the TLS-current arena.
     * hir_to_lir emits ARENA_PUSH on entry + ARENA_POP on every exit edge. */
    case IRON_NODE_IN_ARENA: {
        Iron_InArenaBlock *ia = (Iron_InArenaBlock *)node;
        IronHIR_Expr *arena = ia->arena_expr
            ? lower_expr_hir(ctx, ia->arena_expr) : NULL;
        IronHIR_Block *hblk = iron_hir_block_create(mod);
        ctx->in_arena_depth++;
        push_scope(ctx);
        if (ia->body && ia->body->kind == IRON_NODE_BLOCK) {
            lower_block_hir(ctx, (Iron_Block *)ia->body, hblk);
        }
        pop_scope(ctx);
        ctx->in_arena_depth--;
        IronHIR_Stmt *s = iron_hir_stmt_in_arena(mod, arena, hblk, span);
        iron_hir_block_add_stmt(blk, s);
        return NULL;
    }

    /* ── Expression statement ──────────────────────────────────────────────── */
    /* -Wswitch-enum opt-out: statement lowering handles every real statement
     * kind explicitly above; every other Iron_NodeKind is an expression used
     * as a statement and is routed through lower_expr_hir. */
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

    switch ((int)(node->kind)) {

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
            /* FIX-03 / AUDIT-04 §7: SAFETY — lower_expr_hir may return NULL
             * (e.g., unrecognized inner node kind); storing NULL here is
             * safe — consumers tolerate NULL entries (see emit_c.c
             * emit_interp_string walker). The loop CANNOT fail partway in
             * a way that leaves `parts` half-built and then aborts: no
             * call in lower_expr_hir ever aborts or longjmps, and the
             * iron_hir_expr_interp_string constructor below uses
             * iron_oom_abort on its own arena_alloc failure (noreturn),
             * so the only exit from this block is `return` with `parts`
             * already ownership-transferred to the HIR expr. */
            arrput(parts, p);
        }
        int part_count = (int)arrlen(parts);
        /* FIX-03 / AUDIT-04 §7: SAFETY — parts stb_ds array ownership
         * transfers to the HIR expr — do NOT arrfree. The stb_ds backing
         * buffer is NEVER explicitly freed; when the HIR module is
         * destroyed (iron_hir_module_destroy in hir.c), only the
         * name_table stb_ds array is arrfreed. Every other stb_ds array
         * stored on HIR nodes (including this `parts` buffer) leaks when
         * the HIR arena is freed. This is the same bounded, batch-compile
         * tradeoff documented in the parser.c file-header comment (FIX-03
         * §1) — out-of-scope full fix in Phase 67. */
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

        /* Phase 20 PTR-07 (Plan 20-02b): is_auto_address_target flag (set by
         * Plan 20-02a typecheck.c when this ident is a call-arg matched
         * against a *T / *var T parameter) marks the call-site auto-address
         * insertion point. The HIR lowering for IRON_NODE_CALL is responsible
         * for wrapping the materialized arg in IRON_HIR_EXPR_ADDR_OF when
         * the AST flag is set; this IDENT path only reads the flag for
         * documentation (Pitfall 4 honoured: AST stays unchanged; auto-
         * address is HIR-only synthesis). */
        (void)id->is_auto_address_target;

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
        IronHIR_Expr *lhs = lower_expr_hir(ctx, bin->left);
        IronHIR_Expr *rhs = lower_expr_hir(ctx, bin->right);

        /* Phase 96 STR-01: lower String + String as a runtime call to
         * iron_string_concat. The bit is set by typecheck.c when op ==
         * IRON_TOK_PLUS and both operands are IRON_TYPE_STRING; otherwise
         * fall through to the standard binop lowering. The func_ref + call
         * pattern matches the precedent for Iron_int_to_string (lowercase
         * iron_string_concat is a known runtime symbol; the C-name resolver
         * in src/lir/emit_helpers.c keeps lowercase iron_* names verbatim
         * to bypass the Iron_-prefix mangler, and src/lir/emit_c.c special-
         * cases the call site to wrap both args with `&` since the runtime
         * helper takes `const Iron_String *`). */
        if (bin->is_string_concat) {
            IronHIR_Expr *callee = iron_hir_expr_func_ref(
                mod, "iron_string_concat", bin->resolved_type, span);
            IronHIR_Expr **args = NULL;
            arrput(args, lhs);
            arrput(args, rhs);
            return iron_hir_expr_call(mod, callee, args, 2,
                                      bin->resolved_type, span);
        }

        IronHIR_BinOp hop = ast_op_to_hir_binop(bin->op);
        return iron_hir_expr_binop(mod, hop, lhs, rhs,
                                   bin->resolved_type, span);
    }

    /* ── Unary expression ────────────────────────────────────────────────── */
    case IRON_NODE_UNARY: {
        Iron_UnaryExpr *un = (Iron_UnaryExpr *)node;

        /* Phase 20 PTR-04 (Plan 20-02b): &lvalue lowers to IRON_HIR_EXPR_ADDR_OF
         * carrying a gen_source tag. As of Phase 20, all local bindings
         * (val/var) are stack-allocated — Phase 21's heap T(...) syntax is
         * the next step that introduces heap-source &-targets; until then,
         * gen_source is unconditionally STACK. The OQ-C field-pointer
         * "outermost-allocation gen" walk is therefore degenerate this
         * phase; it becomes load-bearing once Phase 21 lands.
         *
         * The analyzer side (Plan 20-02a IRON_NODE_UNARY-AMP path) has
         * already populated un->resolved_type with IRON_TYPE_PTR
         * (with .ptr.is_var derived from the source binding's mutability),
         * so HIR carries the typed result-of-& through unchanged. */
        if ((int)un->op == IRON_TOK_AMP) {
            IronHIR_Expr *target = lower_expr_hir(ctx, un->operand);
            /* Phase 21 Plan 02: detect heap-allocated bindings so ADDR_OF
             * carries IRON_HIR_GEN_HEAP when &binding targets heap T(...)
             * storage. The deref-side runtime check then calls
             * iron_check_pointer_gen (header-based) not iron_check_stack_pointer_gen
             * (TLS-based) — correct for use-after-free detection (SAFE-01). */
            IronHIR_GenSource gen_src = IRON_HIR_GEN_STACK;
            if (un->operand && un->operand->kind == IRON_NODE_IDENT) {
                Iron_Ident *id = (Iron_Ident *)un->operand;
                if (id->resolved_sym && id->resolved_sym->decl_node) {
                    Iron_Node *decl = id->resolved_sym->decl_node;
                    Iron_Node *init_node = NULL;
                    if (decl->kind == IRON_NODE_VAL_DECL) {
                        init_node = ((Iron_ValDecl *)decl)->init;
                    } else if (decl->kind == IRON_NODE_VAR_DECL) {
                        init_node = ((Iron_VarDecl *)decl)->init;
                    }
                    if (init_node && init_node->kind == IRON_NODE_HEAP) {
                        gen_src = IRON_HIR_GEN_HEAP;
                    }
                }
            }
            return iron_hir_expr_addr_of(mod, target, gen_src,
                                         un->resolved_type, span);
        }

        IronHIR_UnOp hop;
        switch ((int)un->op) {
            case IRON_TOK_MINUS: hop = IRON_HIR_UNOP_NEG;  break;
            case IRON_TOK_NOT:   hop = IRON_HIR_UNOP_NOT;  break;
            case IRON_TOK_TILDE: hop = IRON_HIR_UNOP_BNOT; break;
            /* AUDIT-02 #4 fix: non-unary tokens silently mapped to NEG. This
             * arm only fires on malformed AST from parser error recovery. */
            /* -Wswitch-enum opt-out: Iron_TokenKind has ~80 values; only the
             * prefix-operator tokens are legal as unary ops. */
            default:             hop = IRON_HIR_UNOP_NEG;  break;
        }
        IronHIR_Expr *operand = lower_expr_hir(ctx, un->operand);
        return iron_hir_expr_unop(mod, hop, operand, un->resolved_type, span);
    }

    /* ── Call expression ─────────────────────────────────────────────────── */
    case IRON_NODE_CALL: {
        Iron_CallExpr *ce = (Iron_CallExpr *)node;

        /* Phase 20 OQ-D + Phase 33 STDLIB-10: Ptr.cast[T](p) compiler builtin.
         * Lowers to a no-op HIR CAST node — the C output is a pointer
         * reinterpretation (typecheck.c verified pointee size equality for the
         * checked regime and skipped the check for the unchecked regime). The
         * source is `*S` (or `*unchecked S`); the target carried on
         * ce->resolved_type is `*T` (or `*unchecked T`). At the C level both
         * are pointer values, so a single iron_hir_expr_cast does the job.
         * Sidesteps the broken generic-CALL fallthrough where the callee
         * INDEX(FIELD_ACCESS) would produce an undeclared `_v` reference. */
        if (ce->callee && ce->callee->kind == IRON_NODE_INDEX) {
            Iron_IndexExpr *idx = (Iron_IndexExpr *)ce->callee;
            if (idx->object && idx->object->kind == IRON_NODE_FIELD_ACCESS) {
                Iron_FieldAccess *fa = (Iron_FieldAccess *)idx->object;
                if (fa->object && fa->object->kind == IRON_NODE_IDENT &&
                    fa->field && strcmp(fa->field, "cast") == 0 &&
                    ((Iron_Ident *)fa->object)->name &&
                    strcmp(((Iron_Ident *)fa->object)->name, "Ptr") == 0 &&
                    ce->arg_count == 1) {
                    IronHIR_Expr *src = lower_expr_hir(ctx, ce->args[0]);
                    return iron_hir_expr_cast(mod, src, ce->resolved_type, span);
                }
            }
        }

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

        /* Phase 27 POL-08 / POL-09 (Plan 27-02): intercept .downgrade() and
         * .upgrade() built-in method calls before generic method-call
         * lowering. Typecheck has already validated the receiver kind and
         * argument count (E0300 / E0299 fired upstream when the receiver
         * was wrong); here we just dispatch to the dedicated HIR kind.
         * All expression AST nodes share the Iron_ExprNode prefix
         * (PROT-01 — see ast.h IRON_ASSERT_EXPR_PREFIX), so we read
         * resolved_type generically without dispatching on node kind. */
        if (mc->method && mc->object) {
            Iron_Type *recv_t = ((Iron_ExprNode *)mc->object)->resolved_type;
            if (recv_t && recv_t->kind == IRON_TYPE_RC &&
                strcmp(mc->method, "downgrade") == 0) {
                IronHIR_Expr *strong = lower_expr_hir(ctx, mc->object);
                return iron_hir_expr_weak_rc_downgrade(
                    mod, strong, mc->resolved_type, span);
            }
            if (recv_t && recv_t->kind == IRON_TYPE_WEAK_RC &&
                strcmp(mc->method, "upgrade") == 0) {
                IronHIR_Expr *weak = lower_expr_hir(ctx, mc->object);
                return iron_hir_expr_weak_rc_upgrade(
                    mod, weak, mc->resolved_type, span);
            }
        }

        IronHIR_Expr **args = NULL;
        for (int i = 0; i < mc->arg_count; i++) {
            IronHIR_Expr *a = lower_expr_hir(ctx, mc->args[i]);
            arrput(args, a);
        }
        int arg_count = (int)arrlen(args);
        IronHIR_Expr *obj  = lower_expr_hir(ctx, mc->object);
        /* Phase 20 PTR-06 (Plan 20-02b): when the analyzer flagged
         * mc->is_auto_deref=true (set by Plan 20-02a typecheck.c at
         * IRON_NODE_METHOD_CALL when the receiver is *T), the receiver
         * passed downstream is the pointee value reached through
         * iron_check_pointer_gen. Today's lowering preserves that flag
         * implicitly via mc->resolved_type pointing at the pointee — the
         * full DEREF-before-dispatch wiring is deferred to the receiver-
         * lvalue chain rework in Phase 20-03 (closure capture lifts the
         * full DEREF semantics by-value). The flag-read here documents
         * that the HIR layer is aware of the analyzer's tag. */
        (void)mc->is_auto_deref;  /* read flag — explicit handling tracked in 20-03 */
        /* NOTE: args stb_ds array ownership transfers to the HIR expr — do NOT arrfree */
        return iron_hir_expr_method_call(mod, obj, mc->method,
                                          args, arg_count,
                                          mc->resolved_type, span);
    }

    /* ── Field access ────────────────────────────────────────────────────── */
    case IRON_NODE_FIELD_ACCESS: {
        Iron_FieldAccess *fa = (Iron_FieldAccess *)node;
        IronHIR_Expr *obj = lower_expr_hir(ctx, fa->object);
        /* Phase 83-02 ACCESS-05: pub-getter dispatch.
         * When the typechecker flagged this read as a pub-field access, lower
         * it as a zero-arg method call against the synthesized getter named
         * after the field. The synthesized getter body uses direct field load
         * (its inner FieldAccess has is_pub_access=false), so no recursion.
         *
         * Non-pub reads fall through to the normal field-load path,
         * preserving the pure-superset guard. */
        if (fa->is_pub_access) {
            IronHIR_Expr **args = NULL;  /* zero-arg getter */
            return iron_hir_expr_method_call(mod, obj, fa->field,
                                              args, 0,
                                              fa->resolved_type, span);
        }
        /* Phase 20 PTR-06 (Plan 20-02b): is_auto_deref flag (set by Plan
         * 20-02a typecheck.c when receiver resolves to IRON_TYPE_PTR)
         * marks the receiver chain as needing iron_check_pointer_gen
         * before the field load. The receiver's resolved_type already
         * carries the pointee at this layer (analyzer unwraps for the
         * field lookup), so the field-load path is shape-equivalent to
         * the non-pointer path. End-to-end DEREF + LIR PTR_LOAD wiring
         * for read-side field access lives in the lvalue-chain rework
         * (Plan 20-03 closure work) — this read of fa->is_auto_deref
         * documents the flag is consumed at HIR. */
        (void)fa->is_auto_deref;  /* read flag — explicit handling tracked in 20-03 */
        (void)fa->is_auto_address_target;  /* documented at CALL-arg lowering */
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
        if (!name_copy) iron_oom_abort("hir_lower.c:lower_expr_hir lambda lifted_name");
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
                                                       le->resolved_type, name_copy,
                                                       le->captures, le->capture_count,
                                                       span);

        /* Resolve capture VarIds from the current (enclosing) scope */
        if (le->capture_count > 0) {
            IronHIR_VarId *cap_var_ids = NULL;
            for (int c = 0; c < le->capture_count; c++) {
                IronHIR_VarId vid = lookup_var(ctx, le->captures[c].name);
                arrput(cap_var_ids, vid);
            }
            result->closure.capture_var_ids = cap_var_ids;
        }

        /* Queue for lifting */
        LiftPending lp;
        memset(&lp, 0, sizeof(lp));
        lp.kind                 = LIFT_LAMBDA;
        lp.ast_node             = node;
        lp.lifted_name          = name_copy;
        lp.enclosing_func       = ctx->current_func_name;
        lp.is_readonly_context  = ctx->current_func_is_readonly;
        arrput(ctx->pending_lifts, lp);

        return result;
    }

    /* ── Heap allocation ─────────────────────────────────────────────────── */
    case IRON_NODE_HEAP: {
        Iron_HeapExpr *he = (Iron_HeapExpr *)node;
        IronHIR_Expr  *inner = lower_expr_hir(ctx, he->inner);
        /* Phase 28 ARENA-03/05 (Plan 28-04): route to arena allocation when
         * the explicit `heap(in: arena)` form is used, OR when a bare
         * `heap T(...)` appears lexically inside an `in arena {}` block (the
         * arena is then the TLS-current arena, arena_expr NULL → emit_c emits
         * iron_arena_rt_current()). Outside any block, bare heap stays a plain
         * IRON_HIR_EXPR_HEAP. */
        if (he->arena_expr || ctx->in_arena_depth > 0) {
            IronHIR_Expr *arena = he->arena_expr
                ? lower_expr_hir(ctx, he->arena_expr) : NULL;
            return iron_hir_expr_arena_alloc(mod, inner, arena,
                                             he->allow_drop_skip,
                                             he->resolved_type, span);
        }
        return iron_hir_expr_heap(mod, inner, he->auto_free, he->escapes,
                                   he->resolved_type, span);
    }

    /* ── RC allocation ───────────────────────────────────────────────────── */
    case IRON_NODE_RC: {
        Iron_RcExpr  *rc    = (Iron_RcExpr *)node;
        IronHIR_Expr *inner = lower_expr_hir(ctx, rc->inner);
        return iron_hir_expr_rc(mod, inner, rc->resolved_type, span);
    }

    /* Phase 27 POL-08 (Plan 27-02): `weak rc null` constructor lowers to a
     * dedicated HIR expression kind that emit_c renders as a literal NULL
     * pointer in the weak slot. */
    case IRON_NODE_WEAK_RC_NULL: {
        Iron_WeakRcNullExpr *wn = (Iron_WeakRcNullExpr *)node;
        return iron_hir_expr_weak_rc_null(mod, wn->resolved_type, span);
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

        /* Phase 59 01d: tuple literal — lower to IRON_HIR_EXPR_CONSTRUCT with
         * field names "v0", "v1", ... so downstream hir_to_lir emits an
         * IRON_LIR_CONSTRUCT which emit_c turns into a C99 designated
         * initializer. Zero new HIR kinds required. */
        if (al->resolved_type && al->resolved_type->kind == IRON_TYPE_TUPLE) {
            int n = al->element_count;
            IronHIR_Expr **tup_vals = NULL;
            const char **tup_names  = NULL;
            for (int i = 0; i < n; i++) {
                IronHIR_Expr *v = lower_expr_hir(ctx, al->elements[i]);
                arrput(tup_vals, v);
                /* Field name "v<i>" — max 12 chars for i up to 9 digits. */
                char *fname = (char *)iron_arena_alloc(ctx->module->arena, 12, 1);
                if (!fname) iron_oom_abort("hir_lower.c:lower_expr_hir tuple_array_lit fname");
                snprintf(fname, 12, "v%d", i);
                arrput(tup_names, fname);
            }
            return iron_hir_expr_construct(mod, al->resolved_type,
                                           tup_names, tup_vals, n, span);
        }

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

    /* ── ADT enum variant construction ─────────────────────────────────── */
    case IRON_NODE_ENUM_CONSTRUCT: {
        Iron_EnumConstruct *ec = (Iron_EnumConstruct *)node;
        Iron_Type *ty = ec->resolved_type;
        /* Find variant index */
        int variant_idx = -1;
        if (ty && ty->kind == IRON_TYPE_ENUM && ty->enu.decl) {
            Iron_EnumDecl *ed = ty->enu.decl;
            for (int i = 0; i < ed->variant_count; i++) {
                Iron_EnumVariant *ev = (Iron_EnumVariant *)ed->variants[i];
                if (strcmp(ev->name, ec->variant_name) == 0) {
                    variant_idx = i;
                    break;
                }
            }
        }
        /* Lower args */
        IronHIR_Expr **args = NULL;
        for (int i = 0; i < ec->arg_count; i++) {
            IronHIR_Expr *a = lower_expr_hir(ctx, ec->args[i]);
            arrput(args, a);
        }
        int ac = (int)arrlen(args);
        /* NOTE: args stb_ds array ownership transfers to the HIR expr — do NOT arrfree */
        return iron_hir_expr_enum_construct(mod, ty, ec->enum_name, ec->variant_name,
                                             variant_idx, args, ac, span);
    }

    /* ── ADT pattern ─────────────────────────────────────────────────────── */
    case IRON_NODE_PATTERN: {
        Iron_Pattern *pat = (Iron_Pattern *)node;
        /* Variant index is -1 here; resolved from match scrutinee type in hir_to_lir.c */
        int variant_idx = -1;
        /* Lower nested patterns recursively */
        IronHIR_Expr **nested = NULL;
        for (int i = 0; i < pat->binding_count; i++) {
            if (pat->nested_patterns && pat->nested_patterns[i]) {
                IronHIR_Expr *np = lower_expr_hir(ctx, pat->nested_patterns[i]);
                arrput(nested, np);
            } else {
                arrput(nested, NULL);
            }
        }
        /* Copy binding names */
        const char **names = NULL;
        for (int i = 0; i < pat->binding_count; i++) {
            arrput(names, pat->binding_names ? pat->binding_names[i] : NULL);
        }
        /* NOTE: nested and names stb_ds array ownership transfers to HIR expr — do NOT arrfree */
        return iron_hir_expr_pattern(mod, pat->enum_name, pat->variant_name,
                                      variant_idx, names, nested, pat->binding_count, span);
    }

    /* ── Error or unsupported node ───────────────────────────────────────── */
    /* -Wswitch-enum opt-out: lower_expr_hir handles every valid expression
     * AST kind; statement / declaration kinds that reach here are poisoned
     * to a null literal so later verification stages can emit a coherent
     * diagnostic. */
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

        switch ((int)(decl->kind)) {

        case IRON_NODE_FUNC_DECL: {
            Iron_FuncDecl *fd   = (Iron_FuncDecl *)decl;
            IronHIR_Param *params = build_hir_params_named(ctx, fd->params,
                                                            fd->param_count,
                                                            fd->name);
            Iron_Type *ret_ty = fd->resolved_return_type;
            if (!ret_ty) {
                ret_ty = resolve_type_ann(ctx, fd->return_type);
            }
            IronHIR_Func *f = iron_hir_func_create(mod, fd->name,
                                                     params, fd->param_count,
                                                     ret_ty);
            f->is_extern    = fd->is_extern;
            f->extern_c_name = fd->extern_c_name;
            /* Phase 20 PTR-10 (Plan 20-02b): propagate takes_local_addr from
             * AST decl (set by Plan 20-02a's mark_takes_local_addr_pass). */
            f->takes_local_addr = fd->takes_local_addr;
            /* Phase 22 READ-08: propagate is_readonly from AST FuncDecl.
             * Top-level free functions use is_readonly directly (no is_pure
             * on free functions per Iron design). */
            f->is_readonly = fd->is_readonly;
            iron_hir_module_add_func(mod, f);
            break;
        }

        case IRON_NODE_METHOD_DECL: {
            Iron_MethodDecl *md = (Iron_MethodDecl *)decl;

            /* Phase 33 OQ-02 unblock: the box.iron `func Box.new[T]() / unwrap[T]()
             * / is_null() / free()` declarations are BY-NAME compiler builtins
             * (the Ptr.cast precedent), NOT real foreign-stub functions. Their
             * empty bodies + method-level generic return types would otherwise
             * lower to broken foreign C prototypes (e.g. `Iron_box_new(void value)`
             * because `value: T` collapses to void, plus a missing C symbol),
             * which fails clang for EVERY compilation since box.iron is always
             * prepended. Skip lowering them entirely; real Box dispatch + the
             * emit_ensure_box codegen is the dedicated OQ-02 follow-up plan
             * (deferred-items 33-01). This keeps all non-Box compilation — and
             * the OQ-06 interface-collection corpus — clean. */
            if (md->type_name && strcmp(md->type_name, "Box") == 0) {
                break;
            }

            /* Phase 33 STDLIB-07/08/09 (Plan 33-05): the nocopy resource-type
             * surfaces (mutex/rwlock/channel/filehandle.iron) are by-name
             * compiler builtins exactly like Box above — their empty-body
             * `func Mutex.new[T] / lock[T] / Channel.send[T] / ...` stubs would
             * otherwise lower to broken foreign C prototypes (`void value`
             * params from method-level `T`) and collide with the pre-existing
             * runtime Iron_Mutex / Iron_Channel typedefs + Iron_channel_send/recv
             * symbols, breaking EVERY compilation (these surfaces are always
             * prepended). Skip lowering them; real dispatch + emit_ensure_*
             * codegen is the positive-path follow-up. */
            if (md->type_name &&
                (strcmp(md->type_name, "Mutex") == 0 ||
                 strcmp(md->type_name, "MutexGuard") == 0 ||
                 strcmp(md->type_name, "RWLock") == 0 ||
                 strcmp(md->type_name, "RWReadGuard") == 0 ||
                 strcmp(md->type_name, "RWWriteGuard") == 0 ||
                 strcmp(md->type_name, "Channel") == 0 ||
                 strcmp(md->type_name, "FileHandle") == 0)) {
                break;
            }

            /* Phase 33 STDLIB-10 (Plan 33-06): rawptr.iron's `func RawPtr.of[T]`
             * is a by-name compiler builtin (Box / Mutex precedent). The empty
             * body + method-level T return type would otherwise lower to a
             * broken foreign C prototype. The real dispatch + value production
             * happens in typecheck.c + hir_to_lir.c. Skip lowering. */
            if (md->type_name && strcmp(md->type_name, "RawPtr") == 0) {
                break;
            }

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
                /* Stub method: only explicit params (no self).
                 *
                 * Phase 98 PATCH-01: when migrating stdlib from standalone
                 * form `func TYPE.method(...)` to patch-body form
                 * `patch object TYPE { func method(...) }`, the patch parser
                 * synthesizes a `self: TYPE` first parameter
                 * (parser.c:4102-4128). For empty-body stubs whose C
                 * runtime ABI is namespace-only (no self at the C
                 * boundary), we must strip the synth_self so the HIR
                 * func signature matches the runtime header.
                 *
                 * Two distinct C ABI conventions exist among stub stdlib
                 * methods:
                 *
                 *   (A) Header-declared receiver-style. iron_runtime.h
                 *       declares Iron_string_upper(Iron_String self),
                 *       Iron_list_len(Iron_List_T self), etc. These match
                 *       the synth_self shape; keep self in HIR.
                 *
                 *   (B) Namespace-only. iron_math.h declares
                 *       Iron_math_sin(double x) (no self). iron_raylib.c
                 *       defines Iron_window_init(...), Iron_audio_init(),
                 *       Iron_random_seed(int64_t) etc. with no self at
                 *       the C boundary. These do NOT match the synth_self
                 *       shape; strip self from HIR so the call-site arity
                 *       matches the runtime header.
                 *
                 * Discriminator: prefix of the mangled name. The list is
                 * locked here (rather than discovered dynamically) so the
                 * codemod's allowlist of namespace-only stdlib types
                 * (Math + raylib namespaces Window/Audio/Files/Random/
                 * Text/Draw/Keyboard/Mouse/Gamepad/Touch/Gestures/RMath)
                 * matches what the HIR layer recognises. Trailing
                 * underscore prevents prefix collisions (math_ != matrix_). */
                int skip_self = 0;
                if (md->is_receiver_form && md->param_count > 0) {
                    Iron_Param *p0 = (Iron_Param *)md->params[0];
                    if (p0 && p0->name && strcmp(p0->name, "self") == 0) {
                        static const char *k_no_self_prefixes[] = {
                            "math_",  "io_",   "time_", "log_",
                            "hint_",  "int_",  "int32_", "float_",
                            "float32_",
                            /* raylib namespace types - no self at C boundary */
                            "window_", "audio_",   "files_",
                            "random_", "text_",    "draw_",
                            "keyboard_", "mouse_", "gamepad_",
                            "touch_",   "gestures_", "rmath_",
                            NULL
                        };
                        for (int pi = 0; k_no_self_prefixes[pi]; pi++) {
                            size_t plen = strlen(k_no_self_prefixes[pi]);
                            if (strncmp(mangled, k_no_self_prefixes[pi], plen) == 0) {
                                skip_self = 1;
                                break;
                            }
                        }
                        /* Iron_string_from_byte specifically: the
                         * runtime declares it as `(int64_t b)` with no
                         * self (an inconsistency among the receiver-style
                         * Iron_string_* prefixes). Hard-coded here rather
                         * than via a generic factory heuristic because
                         * raylib factories like Iron_image_from_rectangle
                         * DO take self at the C boundary - the
                         * convention is per-symbol, not per-prefix. */
                        if (!skip_self && md->method_name &&
                            strncmp(mangled, "string_from_byte", 16) == 0) {
                            skip_self = 1;
                        }
                    }
                }
                total_params = md->param_count - skip_self;
                params = NULL;
                if (total_params > 0) {
                    params = (IronHIR_Param *)iron_arena_alloc(
                        mod->arena,
                        (size_t)total_params * sizeof(IronHIR_Param),
                        _Alignof(IronHIR_Param));
                    if (!params) iron_oom_abort("hir_lower.c:lower_module_decls_hir stub_params");
                    for (int p = 0; p < total_params; p++) {
                        Iron_Param *ap = (Iron_Param *)md->params[p + skip_self];
                        params[p].name   = ap->name;
                        params[p].type   = resolve_type_ann(ctx, ap->type_ann);
                        params[p].var_id = IRON_HIR_VAR_INVALID;
                    }
                }
            } else if (md->is_receiver_form) {
                /* v2.1 receiver-method form: `func (r: Type) method(...)`.
                 * The parser desugars into a MethodDecl whose first param
                 * IS the receiver (declared name kept). Don't auto-prepend
                 * `self` — the receiver is visible in the body under the
                 * declared name, matching the existing stdlib pattern
                 * `func Duration.to_ms(d: Duration) { return d.ms }`. */
                total_params = md->param_count;
                params = (IronHIR_Param *)iron_arena_alloc(
                    mod->arena,
                    (size_t)total_params * sizeof(IronHIR_Param),
                    _Alignof(IronHIR_Param));
                if (!params) iron_oom_abort("hir_lower.c:lower_module_decls_hir receiver_method_params");
                for (int p = 0; p < md->param_count; p++) {
                    Iron_Param *ap = (Iron_Param *)md->params[p];
                    params[p].name   = ap->name;
                    params[p].type   = resolve_type_ann(ctx, ap->type_ann);
                    params[p].var_id = IRON_HIR_VAR_INVALID;
                }
            } else {
                /* Classic `func Type.method(...)` form: prepend self + explicit params */
                total_params = md->param_count + 1;
                params = (IronHIR_Param *)iron_arena_alloc(
                    mod->arena,
                    (size_t)total_params * sizeof(IronHIR_Param),
                    _Alignof(IronHIR_Param));
                if (!params) iron_oom_abort("hir_lower.c:lower_module_decls_hir method_params");

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
                        } else if (d->kind == IRON_NODE_ENUM_DECL) {
                            Iron_EnumDecl *ed = (Iron_EnumDecl *)d;
                            if (strcmp(ed->name, md->type_name) == 0) {
                                self_type = iron_type_make_enum(mod->arena, ed);
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
            if (!mname) iron_oom_abort("hir_lower.c:lower_module_decls_hir method_mangled_name");
            memcpy(mname, mangled, mlen);

            IronHIR_Func *f = iron_hir_func_create(mod, mname, params,
                                                     total_params, ret_ty);
            /* Phase 80 MUT-07: propagate receiver mut-ness to the HIR func so
             * HIR→LIR can fire self_by_addr at call sites. Gated on
             * is_receiver_form + params[0]->is_mut_receiver so non-receiver-form
             * methods, stub bodies, and free functions stay false.
             *
             * Phase 98 PATCH-01: stubs (FFI-bound to C runtime) MUST stay
             * false even when patch-body parser synthesized a mutating self.
             * The C runtime takes the receiver by value (or by no receiver
             * at all for static-style calls); firing self_by_addr would
             * pass `&value` to a function expecting the value. */
            if (md->is_receiver_form && md->param_count > 0 && !is_stub) {
                Iron_Param *recv = (Iron_Param *)md->params[0];
                if (recv && recv->is_mut_receiver) {
                    f->is_mut_receiver_method = true;
                }
            }
            /* Phase 20 PTR-10 (Plan 20-02b): propagate takes_local_addr from
             * AST method decl (set by Plan 20-02a's mark_takes_local_addr_pass). */
            f->takes_local_addr = md->takes_local_addr;
            /* Phase 22 READ-08: propagate is_readonly from AST MethodDecl.
             * Methods inherit readonly when EITHER is_readonly OR is_pure
             * (pure >= readonly one-way subsumption per OQ-05). */
            f->is_readonly = md->is_readonly || md->is_pure;
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
        /* -Wswitch-enum opt-out: pass 1 collects globals; type-level decls
         * and everything non-declarative (expressions, statements reaching
         * pass 1 by mistake) are legitimate no-ops. */
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

    ctx->current_func             = fn;
    ctx->current_func_name        = fd->name;
    /* Phase 22 OQ-04: propagate readonly bit so lambda-queue site can
     * populate LiftPending.is_readonly_context. */
    ctx->current_func_is_readonly = fd->is_readonly;

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

    ctx->current_func             = fn;
    ctx->current_func_name        = fn->name;
    /* Phase 22 OQ-04: propagate readonly bit; pure methods are also readonly. */
    ctx->current_func_is_readonly = (md->is_readonly || md->is_pure);

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

    /* Phase 85 INIT-11: init always returns Self implicitly. The parser
     * forbids explicit `return <expr>` inside init (INIT-11 + E0252), so the
     * only control path into C-land is natural-exit. Append an implicit
     * `return self` at the tail of the lowered body so the init function
     * (whose resolved_return_type is now the enclosing object type) produces
     * a properly-typed C return. The definite-assignment pass (Plan 85-02)
     * already guaranteed every field was written on every exit path before
     * this point, so reading `self` here is safe. */
    if (md->is_init && fn->param_count > 0 &&
        fn->params[0].name && strcmp(fn->params[0].name, "self") == 0) {
        IronHIR_Expr *self_expr = iron_hir_expr_ident(
            ctx->module, fn->params[0].var_id,
            fn->params[0].name, fn->params[0].type, md->span);
        IronHIR_Stmt *ret_s = iron_hir_stmt_return(ctx->module, self_expr, md->span);
        iron_hir_block_add_stmt(fn->body, ret_s);
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

            /* Always prepend a void *_env parameter as the first param.
             * All lifted lambda functions use a uniform calling convention:
             * fn(void *_env, arg0, arg1, ...). For non-capturing closures,
             * _env is NULL at call sites and ignored inside the function.
             * This ensures indirect calls through Iron_Closure always work
             * regardless of whether the closure captures variables. */
            IronHIR_Param *final_params = NULL;
            int total_params = le->param_count + 1;
            final_params = (IronHIR_Param *)iron_arena_alloc(
                mod->arena,
                (size_t)total_params * sizeof(IronHIR_Param),
                _Alignof(IronHIR_Param));
            if (!final_params) iron_oom_abort("hir_lower.c:lower_lift_pending_hir LAMBDA final_params");
            /* _env placeholder — VarId set below after alloc */
            final_params[0].name   = "_env";
            final_params[0].type   = NULL; /* void* — emit_c handles NULL-typed params */
            final_params[0].var_id = IRON_HIR_VAR_INVALID;
            for (int p = 0; p < le->param_count; p++) {
                final_params[p + 1] = params ? params[p] : (IronHIR_Param){0};
            }

            IronHIR_Func *lifted = iron_hir_func_create(mod, lp->lifted_name,
                                                          final_params, total_params,
                                                          ret_ty);
            lifted->body = iron_hir_block_create(mod);

            /* Store capture metadata on the lifted function */
            lifted->captures       = le->captures;
            lifted->capture_count  = le->capture_count;

            /* Push scope and declare params */
            push_scope(ctx);
            ctx->current_func = lifted;
            ctx->current_func_name = lp->lifted_name;

            /* Declare _env param in scope (always present — uniform calling convention) */
            {
                IronHIR_VarId env_vid = iron_hir_alloc_var(mod, "_env", NULL, false);
                final_params[0].var_id = env_vid;
                declare_var(ctx, "_env", env_vid);
            }

            for (int p = 0; p < le->param_count; p++) {
                Iron_Param *ap = (Iron_Param *)le->params[p];
                int pi = p + 1; /* offset 1 for _env at position 0 */
                Iron_Type  *pt = params ? params[p].type : NULL;
                IronHIR_VarId pid = iron_hir_alloc_var(mod, ap->name, pt, false);
                if (final_params) final_params[pi].var_id = pid;
                declare_var(ctx, ap->name, pid);
            }

            /* Declare captured variables in scope and emit LET statements into
             * the lifted function body. This ensures hir_to_lir creates an ALLOCA
             * for each captured variable, which emit_c.c then redirects to env field
             * accesses via the capture_alias_map. */
            IronHIR_Block *saved = ctx->current_block;
            ctx->current_block = lifted->body;
            push_defer_scope_hir(ctx);

            if (le->capture_count > 0) {
                for (int c = 0; c < le->capture_count; c++) {
                    IronHIR_VarId cvid = iron_hir_alloc_var(mod,
                                                              le->captures[c].name,
                                                              le->captures[c].type,
                                                              le->captures[c].is_mutable);
                    declare_var(ctx, le->captures[c].name, cvid);
                    /* Emit a mutable var LET (no initializer) so hir_to_lir creates an
                     * ALLOCA with the captured variable's name_hint. The ALLOCA will be
                     * redirected to env field accesses by emit_c.c. */
                    Iron_Span zero_span = {0};
                    IronHIR_Stmt *cap_let = iron_hir_stmt_let(mod, cvid,
                                                                le->captures[c].type,
                                                                NULL, /* no init */
                                                                true, /* mutable: alloca always created */
                                                                zero_span);
                    iron_hir_block_add_stmt(lifted->body, cap_let);
                }
            }

            lower_block_hir(ctx, (Iron_Block *)le->body, lifted->body);
            pop_defer_scope_hir(ctx);
            ctx->current_block = saved;

            pop_scope(ctx);
            ctx->current_func = NULL;
            /* Phase 22 READ-08: consume LiftPending bit set by Plan 22-02 at
             * queue site (~line 1561). The lifted lambda inherits readonly
             * context from the enclosing function. */
            lifted->is_readonly = lp->is_readonly_context;
            iron_hir_module_add_func(mod, lifted);
            break;
        }

        case LIFT_SPAWN: {
            Iron_SpawnStmt *ss = (Iron_SpawnStmt *)lp->ast_node;
            /* Infer return type from the spawn body's return statement */
            Iron_Type *spawn_ret_ty = NULL;
            if (ss->body) {
                Iron_Block *sblk = (Iron_Block *)ss->body;
                for (int ri = 0; ri < sblk->stmt_count; ri++) {
                    if (sblk->stmts[ri]->kind == IRON_NODE_RETURN) {
                        Iron_ReturnStmt *rs = (Iron_ReturnStmt *)sblk->stmts[ri];
                        if (rs->value) {
                            /* All expr nodes share layout: { span, kind, resolved_type } */
                            Iron_IntLit *rexpr = (Iron_IntLit *)rs->value;
                            spawn_ret_ty = rexpr->resolved_type;
                        }
                        break;
                    }
                }
            }

            /* Spawn body is a block; create a function with the inferred return type.
             * If there are captures, prepend a void *_env parameter (like lifted lambdas)
             * so emit_c can pass the env struct to the spawned function. */
            int cap_count = lp->capture_count;
            Iron_CaptureEntry *cap_meta = lp->captures;

            IronHIR_Param *final_params = NULL;
            int total_params = 0;
            if (cap_count > 0) {
                total_params = 1; /* only _env */
                final_params = (IronHIR_Param *)iron_arena_alloc(
                    mod->arena,
                    (size_t)total_params * sizeof(IronHIR_Param),
                    _Alignof(IronHIR_Param));
                if (!final_params) iron_oom_abort("hir_lower.c:lower_lift_pending_hir SPAWN final_params");
                final_params[0].name   = "_env";
                final_params[0].type   = NULL; /* void* */
                final_params[0].var_id = IRON_HIR_VAR_INVALID;
            }

            IronHIR_Func *lifted = iron_hir_func_create(mod, lp->lifted_name,
                                                          final_params, total_params,
                                                          spawn_ret_ty);
            lifted->body = iron_hir_block_create(mod);

            /* Store capture metadata on the lifted function (same as for lambdas) */
            lifted->captures      = cap_meta;
            lifted->capture_count = cap_count;

            push_scope(ctx);
            ctx->current_func = lifted;
            ctx->current_func_name = lp->lifted_name;

            /* Declare _env param in scope if there are captures */
            if (cap_count > 0 && final_params) {
                IronHIR_VarId env_vid = iron_hir_alloc_var(mod, "_env", NULL, false);
                final_params[0].var_id = env_vid;
                declare_var(ctx, "_env", env_vid);
            }

            IronHIR_Block *saved = ctx->current_block;
            ctx->current_block = lifted->body;
            push_defer_scope_hir(ctx);

            /* Declare captured variables in scope and emit LET statements, just
             * like LIFT_LAMBDA. emit_c redirects accesses to env field reads. */
            if (cap_count > 0 && cap_meta) {
                for (int c = 0; c < cap_count; c++) {
                    IronHIR_VarId cvid = iron_hir_alloc_var(mod,
                                                              cap_meta[c].name,
                                                              cap_meta[c].type,
                                                              cap_meta[c].is_mutable);
                    declare_var(ctx, cap_meta[c].name, cvid);
                    Iron_Span zero_span = {0};
                    IronHIR_Stmt *cap_let = iron_hir_stmt_let(mod, cvid,
                                                                cap_meta[c].type,
                                                                NULL, /* no init */
                                                                true, /* mutable: alloca always created */
                                                                zero_span);
                    iron_hir_block_add_stmt(lifted->body, cap_let);
                }
            }

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
            /* pfor chunk function: takes the loop variable as first Int param.
             * If there are captures, prepend a void *_env parameter. */
            int pfor_cap_count = lp->capture_count;
            Iron_CaptureEntry *pfor_cap_meta = lp->captures;

            Iron_Type *int_ty = iron_type_make_primitive(IRON_TYPE_INT);
            /* Total params: optionally _env + the loop variable */
            int total_pfor_params = (pfor_cap_count > 0) ? 2 : 1;
            IronHIR_Param *params = (IronHIR_Param *)iron_arena_alloc(
                mod->arena,
                (size_t)total_pfor_params * sizeof(IronHIR_Param),
                _Alignof(IronHIR_Param));
            if (!params) iron_oom_abort("hir_lower.c:lower_lift_pending_hir PARALLEL_FOR params");
            int loop_var_idx = 0;
            if (pfor_cap_count > 0) {
                /* _env as first param */
                params[0].name   = "_env";
                params[0].type   = NULL; /* void* */
                params[0].var_id = IRON_HIR_VAR_INVALID;
                loop_var_idx = 1;
            }
            params[loop_var_idx].name   = fs->var_name;
            params[loop_var_idx].type   = int_ty;
            params[loop_var_idx].var_id = IRON_HIR_VAR_INVALID;

            IronHIR_Func *lifted = iron_hir_func_create(mod, lp->lifted_name,
                                                          params, total_pfor_params, NULL);
            lifted->body = iron_hir_block_create(mod);

            /* Store capture metadata on the lifted function */
            lifted->captures      = pfor_cap_meta;
            lifted->capture_count = pfor_cap_count;

            push_scope(ctx);
            ctx->current_func = lifted;
            ctx->current_func_name = lp->lifted_name;

            /* Declare _env param in scope if there are captures */
            if (pfor_cap_count > 0) {
                IronHIR_VarId env_vid = iron_hir_alloc_var(mod, "_env", NULL, false);
                params[0].var_id = env_vid;
                declare_var(ctx, "_env", env_vid);
            }

            IronHIR_VarId pid = iron_hir_alloc_var(mod, fs->var_name, int_ty, false);
            params[loop_var_idx].var_id = pid;
            declare_var(ctx, fs->var_name, pid);

            IronHIR_Block *saved = ctx->current_block;
            ctx->current_block = lifted->body;
            push_defer_scope_hir(ctx);

            /* Declare captured variables in scope and emit LET stmts */
            if (pfor_cap_count > 0 && pfor_cap_meta) {
                for (int c = 0; c < pfor_cap_count; c++) {
                    IronHIR_VarId cvid = iron_hir_alloc_var(mod,
                                                              pfor_cap_meta[c].name,
                                                              pfor_cap_meta[c].type,
                                                              pfor_cap_meta[c].is_mutable);
                    declare_var(ctx, pfor_cap_meta[c].name, cvid);
                    Iron_Span zero_span = {0};
                    IronHIR_Stmt *cap_let = iron_hir_stmt_let(mod, cvid,
                                                                pfor_cap_meta[c].type,
                                                                NULL,
                                                                true,
                                                                zero_span);
                    iron_hir_block_add_stmt(lifted->body, cap_let);
                }
            }

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
