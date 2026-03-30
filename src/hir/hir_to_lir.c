/* hir_to_lir.c — Three-pass HIR-to-LIR lowering with SSA construction.
 *
 * Pass 0: lower_type_decls_from_ast()
 *   Walk Iron_Program->decls[] to register interfaces, objects, enums, externs.
 *
 * Pass 1: flatten_func()
 *   For each HIR function, walk HIR body recursively, emitting LIR basic blocks.
 *   Named variables become alloca/store/load. Control flow is flattened to
 *   CFG blocks with BRANCH/JUMP/SWITCH terminators.
 *
 * Pass 2: ssa_construct_func()
 *   Rebuild CFG edges, build dominator tree, compute dominance frontiers,
 *   insert phi nodes at DF+ sites, rename variables to SSA form.
 */

#include "hir/hir_to_lir.h"
#include "lir/verify.h"
#include "diagnostics/diagnostics.h"
#include "analyzer/types.h"
#include "vendor/stb_ds.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <assert.h>

/* ── Domtree entry type (mirrors lir_optimize.c) ──────────────────────────── */
typedef struct { IronLIR_BlockId key; IronLIR_BlockId value; } HirLir_DomEntry;

/* ── Per-variable alloca tracking ────────────────────────────────────────── */
typedef struct { IronHIR_VarId key; IronLIR_ValueId value; } VarAllocaEntry;
typedef struct { IronHIR_VarId key; IronLIR_ValueId value; } VarValEntry;
typedef struct { IronHIR_VarId key; IronLIR_ValueId value; } ParamEntry;

/* ── Lowering context ────────────────────────────────────────────────────── */
typedef struct {
    IronHIR_Module  *hir;
    Iron_Program    *program;
    Iron_Scope      *global_scope;
    Iron_Arena      *lir_arena;
    Iron_DiagList   *diags;
    IronLIR_Module  *lir_module;

    /* Current function state */
    IronLIR_Func    *current_func;
    IronLIR_Block   *current_block;
    IronLIR_Block   *entry_block;   /* fn->blocks[0] — for alloca insertion */

    /* Variable maps (per-function, reset each function) */
    VarAllocaEntry  *var_alloca_map;  /* HIR VarId -> LIR alloca ValueId */
    VarValEntry     *val_binding_map; /* HIR VarId -> LIR ValueId (immutable, no alloca) */
    ParamEntry      *param_map;       /* HIR VarId -> LIR param ValueId */

    /* Defer tracking (per-function) */
    IronHIR_Block ***defer_stacks;   /* stb_ds array of stb_ds arrays */
    int              defer_depth;
    int              function_scope_depth;

    /* Cleanup blocks for defer */
    IronLIR_Block  **cleanup_blocks; /* stb_ds array, one per scope depth */

    /* Counter for generating unique block labels */
    int label_counter;
} HIR_to_LIR_Ctx;

/* ── Forward declarations ────────────────────────────────────────────────── */
static IronLIR_ValueId lower_expr(HIR_to_LIR_Ctx *ctx, IronHIR_Expr *expr);
static void lower_block_stmts(HIR_to_LIR_Ctx *ctx, IronHIR_Block *block);
static void lower_stmt(HIR_to_LIR_Ctx *ctx, IronHIR_Stmt *stmt);
static void ssa_construct_func(IronLIR_Func *fn);

/* ── Helpers ─────────────────────────────────────────────────────────────── */

static char *make_label(HIR_to_LIR_Ctx *ctx, const char *prefix) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%s_%d", prefix, ctx->label_counter++);
    size_t len = strlen(buf) + 1;
    char *label = (char *)iron_arena_alloc(ctx->lir_arena, len, 1);
    memcpy(label, buf, len);
    return label;
}

static IronLIR_Block *new_block(HIR_to_LIR_Ctx *ctx, const char *label) {
    return iron_lir_block_create(ctx->current_func, label);
}

static void switch_block(HIR_to_LIR_Ctx *ctx, IronLIR_Block *block) {
    ctx->current_block = block;
}

/* Emit alloca into entry block (not current block) */
static IronLIR_ValueId emit_alloca_in_entry(HIR_to_LIR_Ctx *ctx,
                                             Iron_Type *type,
                                             const char *name_hint,
                                             Iron_Span span) {
    IronLIR_Instr *instr = iron_lir_alloca(ctx->current_func, ctx->entry_block,
                                            type, name_hint, span);
    return instr->id;
}

/* Check if current block already has a terminator */
static bool block_is_terminated(IronLIR_Block *block) {
    if (!block || block->instr_count == 0) return false;
    IronLIR_Instr *last = block->instrs[block->instr_count - 1];
    return iron_lir_is_terminator(last->kind);
}

/* Find a LIR function in the module by name */
static IronLIR_Func *find_lir_func(IronLIR_Module *module, const char *name) {
    if (!name) return NULL;
    for (int i = 0; i < module->func_count; i++) {
        if (strcmp(module->funcs[i]->name, name) == 0)
            return module->funcs[i];
    }
    return NULL;
}

/* ── CFG helpers (duplicated from lir_optimize.c — static there) ─────────── */

static IronLIR_Block *hirlir_find_block(IronLIR_Func *fn, IronLIR_BlockId id) {
    for (int i = 0; i < fn->block_count; i++) {
        if (fn->blocks[i]->id == id) return fn->blocks[i];
    }
    return NULL;
}

/* Rebuild preds/succs arrays by scanning terminators */
static void rebuild_cfg_edges(IronLIR_Func *fn) {
    for (int bi = 0; bi < fn->block_count; bi++) {
        IronLIR_Block *blk = fn->blocks[bi];
        arrfree(blk->succs);
        arrfree(blk->preds);
        blk->succs = NULL;
        blk->preds = NULL;
    }
    for (int bi = 0; bi < fn->block_count; bi++) {
        IronLIR_Block *blk = fn->blocks[bi];
        for (int ii = 0; ii < blk->instr_count; ii++) {
            IronLIR_Instr *instr = blk->instrs[ii];
            switch (instr->kind) {
            case IRON_LIR_JUMP: {
                IronLIR_BlockId t = instr->jump.target;
                arrput(blk->succs, t);
                IronLIR_Block *tb = hirlir_find_block(fn, t);
                if (tb) arrput(tb->preds, blk->id);
                break;
            }
            case IRON_LIR_BRANCH: {
                IronLIR_BlockId th = instr->branch.then_block;
                IronLIR_BlockId el = instr->branch.else_block;
                arrput(blk->succs, th);
                arrput(blk->succs, el);
                IronLIR_Block *tb = hirlir_find_block(fn, th);
                IronLIR_Block *eb = hirlir_find_block(fn, el);
                if (tb) arrput(tb->preds, blk->id);
                if (eb) arrput(eb->preds, blk->id);
                break;
            }
            case IRON_LIR_SWITCH: {
                IronLIR_BlockId def = instr->sw.default_block;
                arrput(blk->succs, def);
                IronLIR_Block *db = hirlir_find_block(fn, def);
                if (db) arrput(db->preds, blk->id);
                for (int ci = 0; ci < instr->sw.case_count; ci++) {
                    IronLIR_BlockId cid = instr->sw.case_blocks[ci];
                    arrput(blk->succs, cid);
                    IronLIR_Block *cb = hirlir_find_block(fn, cid);
                    if (cb) arrput(cb->preds, blk->id);
                }
                break;
            }
            default: break;
            }
        }
    }
}

/* Build reverse post-order from entry */
static IronLIR_BlockId *build_rpo(IronLIR_Func *fn) {
    if (fn->block_count == 0) return NULL;

    struct { IronLIR_BlockId key; bool value; } *visited = NULL;
    IronLIR_BlockId *postorder = NULL;

    typedef struct { IronLIR_BlockId bid; int succ_idx; } StackFrame;
    StackFrame *stack = NULL;

    IronLIR_BlockId entry_id = fn->blocks[0]->id;
    StackFrame sf;
    sf.bid = entry_id;
    sf.succ_idx = 0;
    arrput(stack, sf);
    hmput(visited, entry_id, true);

    while (arrlen(stack) > 0) {
        StackFrame *top = &stack[arrlen(stack) - 1];
        IronLIR_Block *blk = hirlir_find_block(fn, top->bid);
        if (!blk || top->succ_idx >= (int)arrlen(blk->succs)) {
            arrput(postorder, top->bid);
            (void)arrpop(stack);
        } else {
            IronLIR_BlockId succ_id = blk->succs[top->succ_idx++];
            if (hmgeti(visited, succ_id) < 0) {
                hmput(visited, succ_id, true);
                StackFrame nsf;
                nsf.bid = succ_id;
                nsf.succ_idx = 0;
                arrput(stack, nsf);
            }
        }
    }

    IronLIR_BlockId *rpo = NULL;
    for (int i = (int)arrlen(postorder) - 1; i >= 0; i--) {
        arrput(rpo, postorder[i]);
    }

    hmfree(visited);
    arrfree(postorder);
    arrfree(stack);
    return rpo;
}

/* Build dominator tree: returns stb_ds hashmap BlockId -> idom BlockId */
static HirLir_DomEntry *build_domtree(IronLIR_Func *fn) {
    if (fn->block_count == 0) return NULL;

    HirLir_DomEntry *idom = NULL;
    IronLIR_BlockId entry_id = fn->blocks[0]->id;

    IronLIR_BlockId *rpo = build_rpo(fn);
    int rpo_len = (int)arrlen(rpo);
    if (rpo_len == 0) { arrfree(rpo); return NULL; }

    struct { IronLIR_BlockId key; int value; } *rpo_idx = NULL;
    for (int i = 0; i < rpo_len; i++) {
        hmput(rpo_idx, rpo[i], i);
    }

    hmput(idom, entry_id, entry_id);

    bool changed = true;
    while (changed) {
        changed = false;
        for (int ri = 1; ri < rpo_len; ri++) {
            IronLIR_BlockId bid = rpo[ri];
            IronLIR_Block *blk  = hirlir_find_block(fn, bid);
            if (!blk) continue;

            IronLIR_BlockId new_idom = 0;
            bool found_first = false;

            for (int pi = 0; pi < (int)arrlen(blk->preds); pi++) {
                IronLIR_BlockId pred = blk->preds[pi];
                if (hmgeti(idom, pred) >= 0) {
                    if (!found_first) {
                        new_idom = pred;
                        found_first = true;
                    } else {
                        IronLIR_BlockId a = new_idom;
                        IronLIR_BlockId b = pred;
                        while (a != b) {
                            ptrdiff_t ai = hmgeti(rpo_idx, a);
                            ptrdiff_t bi2 = hmgeti(rpo_idx, b);
                            int a_pos = (ai >= 0) ? rpo_idx[ai].value : rpo_len;
                            int b_pos = (bi2 >= 0) ? rpo_idx[bi2].value : rpo_len;
                            while (a_pos > b_pos) {
                                ptrdiff_t idom_idx = hmgeti(idom, a);
                                if (idom_idx < 0) break;
                                a = idom[idom_idx].value;
                                ptrdiff_t ai2 = hmgeti(rpo_idx, a);
                                a_pos = (ai2 >= 0) ? rpo_idx[ai2].value : rpo_len;
                            }
                            while (b_pos > a_pos) {
                                ptrdiff_t idom_idx = hmgeti(idom, b);
                                if (idom_idx < 0) break;
                                b = idom[idom_idx].value;
                                ptrdiff_t bi3 = hmgeti(rpo_idx, b);
                                b_pos = (bi3 >= 0) ? rpo_idx[bi3].value : rpo_len;
                            }
                        }
                        new_idom = a;
                    }
                }
            }

            if (found_first) {
                ptrdiff_t cur_idx = hmgeti(idom, bid);
                if (cur_idx < 0 || idom[cur_idx].value != new_idom) {
                    hmput(idom, bid, new_idom);
                    changed = true;
                }
            }
        }
    }

    hmfree(rpo_idx);
    arrfree(rpo);
    return idom;
}

/* dominates: true if block a dominates block b
 * Used in DF computation and available for future loop analysis. */
static bool hirlir_dominates(HirLir_DomEntry *idom, IronLIR_BlockId a, IronLIR_BlockId b)
    __attribute__((unused));
static bool hirlir_dominates(HirLir_DomEntry *idom, IronLIR_BlockId a, IronLIR_BlockId b) {
    IronLIR_BlockId cur = b;
    int limit = 10000;
    while (limit-- > 0) {
        if (cur == a) return true;
        ptrdiff_t idx = hmgeti(idom, cur);
        if (idx < 0) return false;
        IronLIR_BlockId parent = idom[idx].value;
        if (parent == cur) return (cur == a);
        cur = parent;
    }
    return false;
}

/* ── Compute dominance frontiers ─────────────────────────────────────────── */
/* Returns stb_ds array of arrays: df[block_index] = stb_ds array of BlockId */
static IronLIR_BlockId **compute_dominance_frontiers(IronLIR_Func *fn,
                                                       HirLir_DomEntry *idom) {
    int n = fn->block_count;
    IronLIR_BlockId **df = (IronLIR_BlockId **)calloc((size_t)n, sizeof(IronLIR_BlockId *));
    if (!df) return NULL;

    /* DF[n] = { y : exists pred p of y such that n dom p, n does not sdom y } */
    for (int bi = 0; bi < n; bi++) {
        IronLIR_Block *blk = fn->blocks[bi];
        /* For each predecessor p of blk */
        for (int pi = 0; pi < (int)arrlen(blk->preds); pi++) {
            IronLIR_BlockId pred_id = blk->preds[pi];
            /* Walk up from pred_id to idom[blk], adding blk to DF(runner) */
            IronLIR_BlockId runner = pred_id;
            ptrdiff_t idom_idx = hmgeti(idom, blk->id);
            IronLIR_BlockId blk_idom = (idom_idx >= 0) ? idom[idom_idx].value : blk->id;

            int limit = 10000;
            while (limit-- > 0 && runner != blk_idom) {
                /* Find index of runner in fn->blocks[] */
                int runner_idx = -1;
                for (int ri = 0; ri < fn->block_count; ri++) {
                    if (fn->blocks[ri]->id == runner) { runner_idx = ri; break; }
                }
                if (runner_idx < 0) break;
                /* Add blk->id to df[runner_idx] if not already there */
                bool already = false;
                for (int di = 0; di < (int)arrlen(df[runner_idx]); di++) {
                    if (df[runner_idx][di] == blk->id) { already = true; break; }
                }
                if (!already) arrput(df[runner_idx], blk->id);

                /* Move runner to its idom */
                ptrdiff_t r_idom_idx = hmgeti(idom, runner);
                if (r_idom_idx < 0) break;
                IronLIR_BlockId r_idom = idom[r_idom_idx].value;
                if (r_idom == runner) break; /* reached entry */
                runner = r_idom;
            }
        }
    }
    return df;
}

/* ── Pass 0: Register type declarations from AST program ────────────────── */

static void lower_type_decls_from_ast(HIR_to_LIR_Ctx *ctx) {
    if (!ctx->program) return;

    /* 0a: Interfaces first (vtable dependency) */
    for (int i = 0; i < ctx->program->decl_count; i++) {
        Iron_Node *decl = ctx->program->decls[i];
        if (decl->kind != IRON_NODE_INTERFACE_DECL) continue;
        Iron_InterfaceDecl *iface = (Iron_InterfaceDecl *)decl;
        iron_lir_module_add_type_decl(ctx->lir_module, IRON_LIR_TYPE_INTERFACE,
                                      iface->name, NULL);
    }

    /* 0b: Objects */
    for (int i = 0; i < ctx->program->decl_count; i++) {
        Iron_Node *decl = ctx->program->decls[i];
        if (decl->kind != IRON_NODE_OBJECT_DECL) continue;
        Iron_ObjectDecl *obj = (Iron_ObjectDecl *)decl;
        Iron_Type *obj_type = iron_type_make_object(ctx->lir_arena, obj);
        iron_lir_module_add_type_decl(ctx->lir_module, IRON_LIR_TYPE_OBJECT,
                                      obj->name, obj_type);
    }

    /* 0c: Enums */
    for (int i = 0; i < ctx->program->decl_count; i++) {
        Iron_Node *decl = ctx->program->decls[i];
        if (decl->kind != IRON_NODE_ENUM_DECL) continue;
        Iron_EnumDecl *en = (Iron_EnumDecl *)decl;
        iron_lir_module_add_type_decl(ctx->lir_module, IRON_LIR_TYPE_ENUM,
                                      en->name, NULL);
    }

    /* 0d: Extern functions */
    for (int i = 0; i < ctx->program->decl_count; i++) {
        Iron_Node *decl = ctx->program->decls[i];
        if (decl->kind != IRON_NODE_FUNC_DECL) continue;
        Iron_FuncDecl *fd = (Iron_FuncDecl *)decl;
        if (!fd->is_extern) continue;

        Iron_Type *ret_type = fd->resolved_return_type;
        if (ret_type && ret_type->kind == IRON_TYPE_VOID) ret_type = NULL;

        /* Build param types array */
        Iron_Type **param_types = NULL;
        IronLIR_Param *lir_params = NULL;
        if (fd->param_count > 0) {
            param_types = (Iron_Type **)iron_arena_alloc(
                ctx->lir_arena,
                (size_t)fd->param_count * sizeof(Iron_Type *),
                _Alignof(Iron_Type *));
            lir_params = (IronLIR_Param *)iron_arena_alloc(
                ctx->lir_arena,
                (size_t)fd->param_count * sizeof(IronLIR_Param),
                _Alignof(IronLIR_Param));
            for (int p = 0; p < fd->param_count; p++) {
                Iron_Param *ap = (Iron_Param *)fd->params[p];
                Iron_Type *pt = iron_type_make_primitive(IRON_TYPE_VOID);
                /* Use type_ann if available, but we don't resolve it here */
                (void)ap->type_ann;
                param_types[p] = pt;
                lir_params[p].name = ap->name;
                lir_params[p].type = pt;
            }
        }

        IronLIR_Func *fn = iron_lir_func_create(ctx->lir_module, fd->name,
                                                  lir_params, fd->param_count,
                                                  ret_type);
        fn->is_extern     = true;
        fn->extern_c_name = fd->extern_c_name;
        iron_lir_module_add_extern(ctx->lir_module, fd->name, fd->extern_c_name,
                                   param_types, fd->param_count, ret_type);
    }
}

/* ── HIR BinOp -> LIR InstrKind conversion ───────────────────────────────── */
static IronLIR_InstrKind hir_binop_to_lir(IronHIR_BinOp op) {
    switch (op) {
    case IRON_HIR_BINOP_ADD: return IRON_LIR_ADD;
    case IRON_HIR_BINOP_SUB: return IRON_LIR_SUB;
    case IRON_HIR_BINOP_MUL: return IRON_LIR_MUL;
    case IRON_HIR_BINOP_DIV: return IRON_LIR_DIV;
    case IRON_HIR_BINOP_MOD: return IRON_LIR_MOD;
    case IRON_HIR_BINOP_EQ:  return IRON_LIR_EQ;
    case IRON_HIR_BINOP_NEQ: return IRON_LIR_NEQ;
    case IRON_HIR_BINOP_LT:  return IRON_LIR_LT;
    case IRON_HIR_BINOP_LTE: return IRON_LIR_LTE;
    case IRON_HIR_BINOP_GT:  return IRON_LIR_GT;
    case IRON_HIR_BINOP_GTE: return IRON_LIR_GTE;
    case IRON_HIR_BINOP_AND: return IRON_LIR_AND;
    case IRON_HIR_BINOP_OR:  return IRON_LIR_OR;
    default: return IRON_LIR_POISON;
    }
}

/* ── Short-circuit lowering helpers ─────────────────────────────────────── */

/* Lower `left && right` to multi-block branch+phi pattern */
static IronLIR_ValueId lower_short_circuit_and(HIR_to_LIR_Ctx *ctx,
                                                 IronHIR_Expr *expr) {
    Iron_Span span = expr->span;
    Iron_Type *bool_type = iron_type_make_primitive(IRON_TYPE_BOOL);

    /* Evaluate left operand in current block */
    IronLIR_ValueId left_val = lower_expr(ctx, expr->binop.left);
    IronLIR_Block   *left_block = ctx->current_block;

    /* Create rhs_block and merge_block */
    IronLIR_Block *rhs_block   = new_block(ctx, make_label(ctx, "and_rhs"));
    IronLIR_Block *merge_block = new_block(ctx, make_label(ctx, "and_merge"));

    /* Branch: if left then rhs_block else merge_block (short-circuit to false) */
    if (!block_is_terminated(ctx->current_block)) {
        iron_lir_branch(ctx->current_func, ctx->current_block, left_val,
                        rhs_block->id, merge_block->id, span);
    }

    /* Evaluate right in rhs_block */
    switch_block(ctx, rhs_block);
    IronLIR_ValueId right_val  = lower_expr(ctx, expr->binop.right);
    IronLIR_Block   *rhs_end_block = ctx->current_block;
    if (!block_is_terminated(rhs_end_block)) {
        iron_lir_jump(ctx->current_func, rhs_end_block, merge_block->id, span);
    }

    /* Merge block: phi(false from left_block, right from rhs_end_block) */
    switch_block(ctx, merge_block);
    IronLIR_Instr *phi = iron_lir_phi(ctx->current_func, merge_block, bool_type, span);
    IronLIR_Instr *false_const = iron_lir_const_bool(ctx->current_func, ctx->entry_block,
                                                       false, bool_type, span);
    iron_lir_phi_add_incoming(phi, false_const->id, left_block->id);
    iron_lir_phi_add_incoming(phi, right_val, rhs_end_block->id);
    return phi->id;
}

/* Lower `left || right` to multi-block branch+phi pattern */
static IronLIR_ValueId lower_short_circuit_or(HIR_to_LIR_Ctx *ctx,
                                                IronHIR_Expr *expr) {
    Iron_Span span = expr->span;
    Iron_Type *bool_type = iron_type_make_primitive(IRON_TYPE_BOOL);

    IronLIR_ValueId left_val  = lower_expr(ctx, expr->binop.left);
    IronLIR_Block   *left_block = ctx->current_block;

    IronLIR_Block *rhs_block   = new_block(ctx, make_label(ctx, "or_rhs"));
    IronLIR_Block *merge_block = new_block(ctx, make_label(ctx, "or_merge"));

    /* Branch: if left then merge_block (short-circuit to true) else rhs_block */
    if (!block_is_terminated(ctx->current_block)) {
        iron_lir_branch(ctx->current_func, ctx->current_block, left_val,
                        merge_block->id, rhs_block->id, span);
    }

    /* Evaluate right in rhs_block */
    switch_block(ctx, rhs_block);
    IronLIR_ValueId right_val  = lower_expr(ctx, expr->binop.right);
    IronLIR_Block   *rhs_end_block = ctx->current_block;
    if (!block_is_terminated(rhs_end_block)) {
        iron_lir_jump(ctx->current_func, rhs_end_block, merge_block->id, span);
    }

    /* Merge block: phi(true from left_block, right from rhs_end_block) */
    switch_block(ctx, merge_block);
    IronLIR_Instr *phi = iron_lir_phi(ctx->current_func, merge_block, bool_type, span);
    IronLIR_Instr *true_const = iron_lir_const_bool(ctx->current_func, ctx->entry_block,
                                                      true, bool_type, span);
    iron_lir_phi_add_incoming(phi, true_const->id, left_block->id);
    iron_lir_phi_add_incoming(phi, right_val, rhs_end_block->id);
    return phi->id;
}

/* ── Pass 1: Expression lowering ─────────────────────────────────────────── */

static IronLIR_ValueId lower_expr(HIR_to_LIR_Ctx *ctx, IronHIR_Expr *expr) {
    if (!expr) return IRON_LIR_VALUE_INVALID;
    if (!ctx->current_block) return IRON_LIR_VALUE_INVALID; /* dead code after return */

    Iron_Span span  = expr->span;
    Iron_Type *type = expr->type;

    switch (expr->kind) {
    case IRON_HIR_EXPR_INT_LIT:
        return iron_lir_const_int(ctx->current_func, ctx->current_block,
                                   expr->int_lit.value, type, span)->id;

    case IRON_HIR_EXPR_FLOAT_LIT:
        return iron_lir_const_float(ctx->current_func, ctx->current_block,
                                     expr->float_lit.value, type, span)->id;

    case IRON_HIR_EXPR_STRING_LIT:
        return iron_lir_const_string(ctx->current_func, ctx->current_block,
                                      expr->string_lit.value, type, span)->id;

    case IRON_HIR_EXPR_BOOL_LIT:
        return iron_lir_const_bool(ctx->current_func, ctx->current_block,
                                    expr->bool_lit.value, type, span)->id;

    case IRON_HIR_EXPR_NULL_LIT:
        return iron_lir_const_null(ctx->current_func, ctx->current_block,
                                    type, span)->id;

    case IRON_HIR_EXPR_INTERP_STRING: {
        IronLIR_ValueId *parts = NULL;
        for (int i = 0; i < expr->interp_string.part_count; i++) {
            IronLIR_ValueId pv = lower_expr(ctx, expr->interp_string.parts[i]);
            arrput(parts, pv);
        }
        IronLIR_Instr *instr = iron_lir_interp_string(ctx->current_func, ctx->current_block,
                                                        parts, expr->interp_string.part_count,
                                                        type, span);
        arrfree(parts);
        return instr->id;
    }

    case IRON_HIR_EXPR_IDENT: {
        IronHIR_VarId vid = expr->ident.var_id;
        /* Check val binding first (immutable, no alloca) */
        ptrdiff_t vi = hmgeti(ctx->val_binding_map, vid);
        if (vi >= 0) return ctx->val_binding_map[vi].value;
        /* Check param map */
        ptrdiff_t pi2 = hmgeti(ctx->param_map, vid);
        if (pi2 >= 0) return ctx->param_map[pi2].value;
        /* Check var alloca (mutable) — emit LOAD */
        ptrdiff_t ai = hmgeti(ctx->var_alloca_map, vid);
        if (ai >= 0) {
            IronLIR_ValueId alloca_id = ctx->var_alloca_map[ai].value;
            IronLIR_Instr *load = iron_lir_load(ctx->current_func, ctx->current_block,
                                                  alloca_id, type, span);
            return load->id;
        }
        /* Unknown variable — emit poison */
        return iron_lir_poison(ctx->current_func, ctx->current_block, type, span)->id;
    }

    case IRON_HIR_EXPR_BINOP: {
        IronHIR_BinOp op = expr->binop.op;
        /* Short-circuit operators */
        if (op == IRON_HIR_BINOP_AND) return lower_short_circuit_and(ctx, expr);
        if (op == IRON_HIR_BINOP_OR)  return lower_short_circuit_or(ctx, expr);

        IronLIR_ValueId left  = lower_expr(ctx, expr->binop.left);
        IronLIR_ValueId right = lower_expr(ctx, expr->binop.right);
        if (!ctx->current_block) return IRON_LIR_VALUE_INVALID;
        IronLIR_InstrKind kind = hir_binop_to_lir(op);
        return iron_lir_binop(ctx->current_func, ctx->current_block,
                               kind, left, right, type, span)->id;
    }

    case IRON_HIR_EXPR_UNOP: {
        IronLIR_ValueId operand = lower_expr(ctx, expr->unop.operand);
        if (!ctx->current_block) return IRON_LIR_VALUE_INVALID;
        IronLIR_InstrKind kind = (expr->unop.op == IRON_HIR_UNOP_NEG)
            ? IRON_LIR_NEG : IRON_LIR_NOT;
        return iron_lir_unop(ctx->current_func, ctx->current_block,
                              kind, operand, type, span)->id;
    }

    case IRON_HIR_EXPR_CALL: {
        /* Lower callee expression to get callee info */
        IronLIR_ValueId *args = NULL;
        for (int i = 0; i < expr->call.arg_count; i++) {
            IronLIR_ValueId av = lower_expr(ctx, expr->call.args[i]);
            arrput(args, av);
        }
        /* For direct calls: callee is an IDENT or FUNC_REF → look up func_decl */
        Iron_FuncDecl *func_decl = NULL;
        IronLIR_ValueId func_ptr = IRON_LIR_VALUE_INVALID;

        if (expr->call.callee && expr->call.callee->kind == IRON_HIR_EXPR_FUNC_REF) {
            const char *fname = expr->call.callee->func_ref.func_name;
            /* Build a minimal func_decl pointer lookup via LIR module */
            /* We emit FUNC_REF instruction and use indirect call */
            IronLIR_Instr *fref = iron_lir_func_ref(ctx->current_func, ctx->current_block,
                                                      fname, expr->call.callee->type, span);
            func_ptr = fref->id;
        } else if (expr->call.callee && expr->call.callee->kind == IRON_HIR_EXPR_IDENT) {
            /* Could be a function reference via identifier */
            func_ptr = lower_expr(ctx, expr->call.callee);
        } else {
            func_ptr = lower_expr(ctx, expr->call.callee);
        }

        IronLIR_Instr *call = iron_lir_call(ctx->current_func, ctx->current_block,
                                              func_decl, func_ptr,
                                              args, expr->call.arg_count,
                                              type, span);
        arrfree(args);
        return call->id;
    }

    case IRON_HIR_EXPR_METHOD_CALL: {
        /* Mangle: TypeName_methodName, emit with self as first arg */
        IronLIR_ValueId self_val = lower_expr(ctx, expr->method_call.object);
        IronLIR_ValueId *args = NULL;
        arrput(args, self_val);
        for (int i = 0; i < expr->method_call.arg_count; i++) {
            IronLIR_ValueId av = lower_expr(ctx, expr->method_call.args[i]);
            arrput(args, av);
        }
        int arg_count = 1 + expr->method_call.arg_count;

        /* Determine receiver type name for mangling */
        const char *type_name = "Unknown";
        if (expr->method_call.object && expr->method_call.object->type) {
            Iron_Type *obj_type = expr->method_call.object->type;
            if (obj_type->kind == IRON_TYPE_OBJECT && obj_type->object.decl) {
                type_name = obj_type->object.decl->name;
            }
        }
        /* Build mangled name */
        size_t mlen = strlen(type_name) + 1 + strlen(expr->method_call.method) + 1;
        char *mangled = (char *)iron_arena_alloc(ctx->lir_arena, mlen, 1);
        snprintf(mangled, mlen, "%s_%s", type_name, expr->method_call.method);

        IronLIR_Instr *fref = iron_lir_func_ref(ctx->current_func, ctx->current_block,
                                                  mangled, NULL, span);
        IronLIR_Instr *call = iron_lir_call(ctx->current_func, ctx->current_block,
                                              NULL, fref->id,
                                              args, arg_count, type, span);
        arrfree(args);
        return call->id;
    }

    case IRON_HIR_EXPR_FIELD_ACCESS: {
        IronLIR_ValueId obj = lower_expr(ctx, expr->field_access.object);
        return iron_lir_get_field(ctx->current_func, ctx->current_block,
                                   obj, expr->field_access.field, type, span)->id;
    }

    case IRON_HIR_EXPR_INDEX: {
        IronLIR_ValueId arr = lower_expr(ctx, expr->index.array);
        IronLIR_ValueId idx = lower_expr(ctx, expr->index.index);
        return iron_lir_get_index(ctx->current_func, ctx->current_block,
                                   arr, idx, type, span)->id;
    }

    case IRON_HIR_EXPR_SLICE: {
        IronLIR_ValueId arr   = lower_expr(ctx, expr->slice.array);
        IronLIR_ValueId start = lower_expr(ctx, expr->slice.start);
        IronLIR_ValueId end   = lower_expr(ctx, expr->slice.end);
        return iron_lir_slice(ctx->current_func, ctx->current_block,
                               arr, start, end, type, span)->id;
    }

    case IRON_HIR_EXPR_CLOSURE: {
        /* Lift closure body as a new top-level function */
        char lifted_name[64];
        snprintf(lifted_name, sizeof(lifted_name), "__lambda_%d", ctx->label_counter++);

        /* Build LIR params for the lifted function */
        int param_count = expr->closure.param_count;
        IronLIR_Param *lir_params = NULL;
        if (param_count > 0) {
            lir_params = (IronLIR_Param *)iron_arena_alloc(
                ctx->lir_arena, (size_t)param_count * sizeof(IronLIR_Param),
                _Alignof(IronLIR_Param));
            for (int p = 0; p < param_count; p++) {
                lir_params[p].name = expr->closure.params[p].name;
                lir_params[p].type = expr->closure.params[p].type;
            }
        }
        char *lifted_name_arena = (char *)iron_arena_alloc(ctx->lir_arena,
                                                             strlen(lifted_name) + 1, 1);
        memcpy(lifted_name_arena, lifted_name, strlen(lifted_name) + 1);

        IronLIR_Func *lifted = iron_lir_func_create(ctx->lir_module, lifted_name_arena,
                                                      lir_params, param_count,
                                                      expr->closure.return_type);

        /* Save and set up context for lifted function */
        IronLIR_Func  *save_func  = ctx->current_func;
        IronLIR_Block *save_block = ctx->current_block;
        IronLIR_Block *save_entry = ctx->entry_block;
        VarAllocaEntry *save_var_alloca = ctx->var_alloca_map;
        VarValEntry    *save_val_bind   = ctx->val_binding_map;
        ParamEntry     *save_param      = ctx->param_map;

        ctx->current_func  = lifted;
        IronLIR_Block *lifted_entry = iron_lir_block_create(lifted, "entry");
        ctx->current_block = lifted_entry;
        ctx->entry_block   = lifted_entry;
        ctx->var_alloca_map  = NULL;
        ctx->val_binding_map = NULL;
        ctx->param_map       = NULL;

        /* Register params */
        for (int p = 0; p < param_count; p++) {
            IronLIR_ValueId param_val = (IronLIR_ValueId)(p * 2 + 1);
            hmput(ctx->param_map, expr->closure.params[p].var_id, param_val);
        }

        /* Lower closure body */
        lower_block_stmts(ctx, expr->closure.body);
        if (ctx->current_block && !block_is_terminated(ctx->current_block)) {
            iron_lir_return(ctx->current_func, ctx->current_block,
                            IRON_LIR_VALUE_INVALID, true, NULL,
                            expr->span);
        }

        /* Apply SSA to lifted function */
        ssa_construct_func(lifted);

        /* Restore context */
        hmfree(ctx->var_alloca_map);
        hmfree(ctx->val_binding_map);
        hmfree(ctx->param_map);
        ctx->current_func    = save_func;
        ctx->current_block   = save_block;
        ctx->entry_block     = save_entry;
        ctx->var_alloca_map  = save_var_alloca;
        ctx->val_binding_map = save_val_bind;
        ctx->param_map       = save_param;

        /* Emit MAKE_CLOSURE in parent function */
        return iron_lir_make_closure(ctx->current_func, ctx->current_block,
                                      lifted_name_arena, NULL, 0, type, span)->id;
    }

    case IRON_HIR_EXPR_PARALLEL_FOR: {
        /* Lift parallel-for body as a chunk function */
        char chunk_name[64];
        snprintf(chunk_name, sizeof(chunk_name), "__pfor_%d", ctx->label_counter++);
        char *chunk_arena = (char *)iron_arena_alloc(ctx->lir_arena,
                                                       strlen(chunk_name) + 1, 1);
        memcpy(chunk_arena, chunk_name, strlen(chunk_name) + 1);

        /* Evaluate range */
        IronLIR_ValueId range_val = lower_expr(ctx, expr->parallel_for.range);

        /* Get var name for loop variable */
        const char *var_name = iron_hir_var_name(ctx->hir, expr->parallel_for.var_id);

        return iron_lir_parallel_for(ctx->current_func, ctx->current_block,
                                      var_name ? var_name : "i",
                                      range_val, chunk_arena,
                                      IRON_LIR_VALUE_INVALID,
                                      NULL, 0, span)->id;
    }

    case IRON_HIR_EXPR_COMPTIME:
        /* Comptime: lower inner expression directly */
        return lower_expr(ctx, expr->comptime.inner);

    case IRON_HIR_EXPR_HEAP: {
        IronLIR_ValueId inner = lower_expr(ctx, expr->heap.inner);
        return iron_lir_heap_alloc(ctx->current_func, ctx->current_block,
                                    inner, expr->heap.auto_free, expr->heap.escapes,
                                    type, span)->id;
    }

    case IRON_HIR_EXPR_RC: {
        IronLIR_ValueId inner = lower_expr(ctx, expr->rc.inner);
        return iron_lir_rc_alloc(ctx->current_func, ctx->current_block,
                                  inner, type, span)->id;
    }

    case IRON_HIR_EXPR_CONSTRUCT: {
        /* Lower field values, build field_vals array in declaration order */
        IronLIR_ValueId *field_vals = NULL;
        for (int i = 0; i < expr->construct.field_count; i++) {
            IronLIR_ValueId fv = lower_expr(ctx, expr->construct.field_values[i]);
            arrput(field_vals, fv);
        }
        IronLIR_Instr *instr = iron_lir_construct(ctx->current_func, ctx->current_block,
                                                    expr->construct.type,
                                                    field_vals, expr->construct.field_count,
                                                    span);
        arrfree(field_vals);
        return instr->id;
    }

    case IRON_HIR_EXPR_ARRAY_LIT: {
        IronLIR_ValueId *elements = NULL;
        for (int i = 0; i < expr->array_lit.element_count; i++) {
            IronLIR_ValueId ev = lower_expr(ctx, expr->array_lit.elements[i]);
            arrput(elements, ev);
        }
        IronLIR_Instr *instr = iron_lir_array_lit(ctx->current_func, ctx->current_block,
                                                    expr->array_lit.elem_type,
                                                    elements, expr->array_lit.element_count,
                                                    type, span);
        arrfree(elements);
        return instr->id;
    }

    case IRON_HIR_EXPR_AWAIT: {
        IronLIR_ValueId handle = lower_expr(ctx, expr->await_expr.handle);
        return iron_lir_await(ctx->current_func, ctx->current_block,
                               handle, type, span)->id;
    }

    case IRON_HIR_EXPR_CAST: {
        IronLIR_ValueId val = lower_expr(ctx, expr->cast.value);
        return iron_lir_cast(ctx->current_func, ctx->current_block,
                              val, expr->cast.target_type, span)->id;
    }

    case IRON_HIR_EXPR_IS_NULL: {
        IronLIR_ValueId val = lower_expr(ctx, expr->null_check.value);
        return iron_lir_is_null(ctx->current_func, ctx->current_block, val, span)->id;
    }

    case IRON_HIR_EXPR_IS_NOT_NULL: {
        IronLIR_ValueId val = lower_expr(ctx, expr->null_check.value);
        return iron_lir_is_not_null(ctx->current_func, ctx->current_block, val, span)->id;
    }

    case IRON_HIR_EXPR_FUNC_REF: {
        return iron_lir_func_ref(ctx->current_func, ctx->current_block,
                                  expr->func_ref.func_name, type, span)->id;
    }

    case IRON_HIR_EXPR_IS: {
        /* Type test — emit as IS_NULL check or poison for now */
        IronLIR_ValueId val = lower_expr(ctx, expr->is_check.value);
        (void)val;
        /* IS check: emit a poison placeholder — type tests need runtime support */
        return iron_lir_poison(ctx->current_func, ctx->current_block, type, span)->id;
    }

    default:
        return iron_lir_poison(ctx->current_func, ctx->current_block, type, span)->id;
    }
}

/* ── Emit defer cleanup blocks ───────────────────────────────────────────── */
static void emit_defer_cleanup(HIR_to_LIR_Ctx *ctx, IronLIR_Block *after_block,
                                Iron_Span span) {
    /* Emit deferred bodies in LIFO order into cleanup blocks.
     * Each cleanup block handles one defer body and jumps to the next. */
    if (!ctx->defer_stacks || ctx->defer_depth == 0) {
        /* No defers — jump directly to after_block */
        if (!block_is_terminated(ctx->current_block)) {
            iron_lir_jump(ctx->current_func, ctx->current_block, after_block->id, span);
        }
        return;
    }

    /* Walk the defer stack in LIFO order (innermost scope first) */
    IronLIR_Block *cleanup_entry = NULL;
    IronLIR_Block *prev_cleanup  = NULL;

    for (int d = ctx->defer_depth - 1; d >= 0; d--) {
        IronHIR_Block **defer_list = ctx->defer_stacks[d];
        int n = (int)arrlen(defer_list);
        for (int i = n - 1; i >= 0; i--) {
            IronHIR_Block *defer_body = defer_list[i];
            IronLIR_Block *cleanup_blk = new_block(ctx, make_label(ctx, "defer_cleanup"));
            if (!cleanup_entry) cleanup_entry = cleanup_blk;

            if (prev_cleanup && !block_is_terminated(prev_cleanup)) {
                iron_lir_jump(ctx->current_func, prev_cleanup, cleanup_blk->id, span);
            }

            switch_block(ctx, cleanup_blk);
            lower_block_stmts(ctx, defer_body);
            prev_cleanup = ctx->current_block;
        }
    }

    /* Final cleanup block jumps to after_block */
    if (prev_cleanup && !block_is_terminated(prev_cleanup)) {
        iron_lir_jump(ctx->current_func, prev_cleanup, after_block->id, span);
    }

    /* Jump from current block to cleanup chain entry */
    if (cleanup_entry) {
        /* The call site will connect to cleanup_entry */
        (void)cleanup_entry;
    }
}

/* ── Pass 1: Statement lowering ──────────────────────────────────────────── */

static void lower_stmt(HIR_to_LIR_Ctx *ctx, IronHIR_Stmt *stmt) {
    if (!stmt) return;
    if (!ctx->current_block) return; /* dead code after return */

    Iron_Span span = stmt->span;

    switch (stmt->kind) {
    case IRON_HIR_STMT_LET: {
        IronHIR_VarId vid  = stmt->let.var_id;
        Iron_Type    *type = stmt->let.type;
        bool is_mut        = stmt->let.is_mutable;

        if (!type && stmt->let.init) type = stmt->let.init->type;

        if (is_mut) {
            /* Mutable: ALLOCA in entry block + STORE initial value */
            const char *name = iron_hir_var_name(ctx->hir, vid);
            IronLIR_ValueId alloca_id = emit_alloca_in_entry(ctx, type, name, span);
            hmput(ctx->var_alloca_map, vid, alloca_id);

            if (stmt->let.init) {
                IronLIR_ValueId init_val = lower_expr(ctx, stmt->let.init);
                if (ctx->current_block && !block_is_terminated(ctx->current_block)) {
                    iron_lir_store(ctx->current_func, ctx->current_block,
                                   alloca_id, init_val, span);
                }
            }
        } else {
            /* Immutable val: use alloca for consistency (enables SSA rename pass) */
            const char *name = iron_hir_var_name(ctx->hir, vid);
            IronLIR_ValueId alloca_id = emit_alloca_in_entry(ctx, type, name, span);
            hmput(ctx->var_alloca_map, vid, alloca_id);

            if (stmt->let.init) {
                IronLIR_ValueId init_val = lower_expr(ctx, stmt->let.init);
                if (ctx->current_block && !block_is_terminated(ctx->current_block)) {
                    iron_lir_store(ctx->current_func, ctx->current_block,
                                   alloca_id, init_val, span);
                }
            }
        }
        break;
    }

    case IRON_HIR_STMT_ASSIGN: {
        /* Evaluate RHS value */
        IronLIR_ValueId val = lower_expr(ctx, stmt->assign.value);
        if (!ctx->current_block || block_is_terminated(ctx->current_block)) break;

        /* Determine target: IDENT -> store to alloca, FIELD_ACCESS -> SET_FIELD, INDEX -> SET_INDEX */
        IronHIR_Expr *target = stmt->assign.target;
        if (target->kind == IRON_HIR_EXPR_IDENT) {
            IronHIR_VarId vid = target->ident.var_id;
            ptrdiff_t ai = hmgeti(ctx->var_alloca_map, vid);
            if (ai >= 0) {
                iron_lir_store(ctx->current_func, ctx->current_block,
                               ctx->var_alloca_map[ai].value, val, span);
            }
        } else if (target->kind == IRON_HIR_EXPR_FIELD_ACCESS) {
            IronLIR_ValueId obj = lower_expr(ctx, target->field_access.object);
            iron_lir_set_field(ctx->current_func, ctx->current_block,
                               obj, target->field_access.field, val, span);
        } else if (target->kind == IRON_HIR_EXPR_INDEX) {
            IronLIR_ValueId arr = lower_expr(ctx, target->index.array);
            IronLIR_ValueId idx = lower_expr(ctx, target->index.index);
            iron_lir_set_index(ctx->current_func, ctx->current_block,
                               arr, idx, val, span);
        }
        break;
    }

    case IRON_HIR_STMT_IF: {
        /* Evaluate condition */
        IronLIR_ValueId cond = lower_expr(ctx, stmt->if_else.condition);
        if (!ctx->current_block || block_is_terminated(ctx->current_block)) break;

        IronLIR_Block *then_block  = new_block(ctx, make_label(ctx, "if_then"));
        IronLIR_Block *merge_block = new_block(ctx, make_label(ctx, "if_merge"));
        IronLIR_Block *else_block  = stmt->if_else.else_body
            ? new_block(ctx, make_label(ctx, "if_else"))
            : merge_block;

        /* Emit BRANCH terminator */
        iron_lir_branch(ctx->current_func, ctx->current_block, cond,
                        then_block->id, else_block->id, span);

        /* Lower then body */
        switch_block(ctx, then_block);
        lower_block_stmts(ctx, stmt->if_else.then_body);
        if (ctx->current_block && !block_is_terminated(ctx->current_block)) {
            iron_lir_jump(ctx->current_func, ctx->current_block, merge_block->id, span);
        }

        /* Lower else body (if present) */
        if (stmt->if_else.else_body) {
            switch_block(ctx, else_block);
            lower_block_stmts(ctx, stmt->if_else.else_body);
            if (ctx->current_block && !block_is_terminated(ctx->current_block)) {
                iron_lir_jump(ctx->current_func, ctx->current_block, merge_block->id, span);
            }
        }

        switch_block(ctx, merge_block);
        break;
    }

    case IRON_HIR_STMT_WHILE: {
        /* Create blocks: header, body, exit */
        IronLIR_Block *header_block = new_block(ctx, make_label(ctx, "while_header"));
        IronLIR_Block *body_block   = new_block(ctx, make_label(ctx, "while_body"));
        IronLIR_Block *exit_block   = new_block(ctx, make_label(ctx, "while_exit"));

        /* Jump from current block to header */
        if (!block_is_terminated(ctx->current_block)) {
            iron_lir_jump(ctx->current_func, ctx->current_block, header_block->id, span);
        }

        /* Header: evaluate condition, branch to body or exit */
        switch_block(ctx, header_block);
        IronLIR_ValueId cond = lower_expr(ctx, stmt->while_loop.condition);
        if (!block_is_terminated(ctx->current_block)) {
            iron_lir_branch(ctx->current_func, ctx->current_block, cond,
                            body_block->id, exit_block->id, span);
        }

        /* Body */
        switch_block(ctx, body_block);
        lower_block_stmts(ctx, stmt->while_loop.body);
        if (ctx->current_block && !block_is_terminated(ctx->current_block)) {
            iron_lir_jump(ctx->current_func, ctx->current_block, header_block->id, span); /* back-edge */
        }

        switch_block(ctx, exit_block);
        break;
    }

    case IRON_HIR_STMT_FOR: {
        /* for var in iterable { body }
         * Create: pre_header (init iter), header (check done), body, increment, exit */
        IronLIR_Block *pre_header = new_block(ctx, make_label(ctx, "for_pre"));
        IronLIR_Block *header     = new_block(ctx, make_label(ctx, "for_header"));
        IronLIR_Block *body_blk   = new_block(ctx, make_label(ctx, "for_body"));
        IronLIR_Block *inc_blk    = new_block(ctx, make_label(ctx, "for_inc"));
        IronLIR_Block *exit_blk   = new_block(ctx, make_label(ctx, "for_exit"));

        if (!block_is_terminated(ctx->current_block)) {
            iron_lir_jump(ctx->current_func, ctx->current_block, pre_header->id, span);
        }

        /* Pre-header: evaluate iterable, create iter alloca, init index to 0 */
        switch_block(ctx, pre_header);
        IronLIR_ValueId iterable_val = lower_expr(ctx, stmt->for_loop.iterable);
        Iron_Type *int_type  = iron_type_make_primitive(IRON_TYPE_INT);

        /* Alloca for loop index */
        IronLIR_ValueId idx_alloca = emit_alloca_in_entry(ctx, int_type, "for_idx", span);
        IronLIR_ValueId zero_val   = iron_lir_const_int(ctx->current_func, pre_header, 0, int_type, span)->id;
        iron_lir_store(ctx->current_func, pre_header, idx_alloca, zero_val, span);

        /* Alloca for loop variable */
        Iron_Type *elem_type = NULL;
        if (stmt->for_loop.iterable && stmt->for_loop.iterable->type &&
            stmt->for_loop.iterable->type->kind == IRON_TYPE_ARRAY) {
            elem_type = stmt->for_loop.iterable->type->array.elem;
        }
        if (!elem_type) elem_type = int_type;

        IronLIR_ValueId var_alloca = emit_alloca_in_entry(ctx, elem_type, "for_var", span);
        hmput(ctx->var_alloca_map, stmt->for_loop.var_id, var_alloca);

        if (!block_is_terminated(pre_header)) {
            iron_lir_jump(ctx->current_func, pre_header, header->id, span);
        }

        /* Header: check if index < iterable.count */
        switch_block(ctx, header);
        IronLIR_ValueId idx_val = iron_lir_load(ctx->current_func, header, idx_alloca, int_type, span)->id;
        IronLIR_ValueId count_val = iron_lir_get_field(ctx->current_func, header,
                                                         iterable_val, "count", int_type, span)->id;
        IronLIR_ValueId done_cond = iron_lir_binop(ctx->current_func, header,
                                                     IRON_LIR_LT, idx_val, count_val, int_type, span)->id;
        if (!block_is_terminated(header)) {
            iron_lir_branch(ctx->current_func, header, done_cond,
                            body_blk->id, exit_blk->id, span);
        }

        /* Body: load element, store to var, execute body */
        switch_block(ctx, body_blk);
        IronLIR_ValueId idx_body = iron_lir_load(ctx->current_func, body_blk, idx_alloca, int_type, span)->id;
        IronLIR_ValueId elem_val = iron_lir_get_index(ctx->current_func, body_blk,
                                                       iterable_val, idx_body, elem_type, span)->id;
        iron_lir_store(ctx->current_func, body_blk, var_alloca, elem_val, span);
        lower_block_stmts(ctx, stmt->for_loop.body);
        if (ctx->current_block && !block_is_terminated(ctx->current_block)) {
            iron_lir_jump(ctx->current_func, ctx->current_block, inc_blk->id, span);
        }

        /* Increment: idx++ */
        switch_block(ctx, inc_blk);
        IronLIR_ValueId idx_inc = iron_lir_load(ctx->current_func, inc_blk, idx_alloca, int_type, span)->id;
        IronLIR_Instr  *one_c   = iron_lir_const_int(ctx->current_func, inc_blk, 1, int_type, span);
        IronLIR_ValueId new_idx = iron_lir_binop(ctx->current_func, inc_blk,
                                                   IRON_LIR_ADD, idx_inc, one_c->id, int_type, span)->id;
        iron_lir_store(ctx->current_func, inc_blk, idx_alloca, new_idx, span);
        if (!block_is_terminated(inc_blk)) {
            iron_lir_jump(ctx->current_func, inc_blk, header->id, span); /* back-edge */
        }

        switch_block(ctx, exit_blk);
        break;
    }

    case IRON_HIR_STMT_MATCH: {
        /* Evaluate scrutinee */
        IronLIR_ValueId subj = lower_expr(ctx, stmt->match_stmt.scrutinee);
        if (!ctx->current_block || block_is_terminated(ctx->current_block)) break;

        IronLIR_Block *join_block = new_block(ctx, make_label(ctx, "match_join"));

        int arm_count = stmt->match_stmt.arm_count;
        IronLIR_Block *default_block = join_block; /* default falls through to join */

        /* Build case_values and case_blocks arrays for SWITCH */
        int *case_values    = NULL;
        IronLIR_BlockId *case_blocks = NULL;

        /* Create arm blocks */
        IronLIR_Block **arm_blocks = NULL;
        for (int i = 0; i < arm_count; i++) {
            IronLIR_Block *arm_blk = new_block(ctx, make_label(ctx, "match_arm"));
            arrput(arm_blocks, arm_blk);
            /* Only add integer pattern arms to switch case table */
            IronHIR_MatchArm *arm = &stmt->match_stmt.arms[i];
            if (arm->pattern && arm->pattern->kind == IRON_HIR_EXPR_INT_LIT) {
                arrput(case_values, (int)arm->pattern->int_lit.value);
                arrput(case_blocks, arm_blk->id);
            } else {
                /* Non-integer pattern: use as default arm */
                default_block = arm_blk;
            }
        }

        int cc = (int)arrlen(case_blocks);
        iron_lir_switch(ctx->current_func, ctx->current_block,
                        subj, default_block->id,
                        case_values, case_blocks, cc, span);

        /* Lower each arm body */
        for (int i = 0; i < arm_count; i++) {
            IronHIR_MatchArm *arm = &stmt->match_stmt.arms[i];
            switch_block(ctx, arm_blocks[i]);
            lower_block_stmts(ctx, arm->body);
            if (ctx->current_block && !block_is_terminated(ctx->current_block)) {
                iron_lir_jump(ctx->current_func, ctx->current_block, join_block->id, span);
            }
        }

        arrfree(arm_blocks);
        arrfree(case_values);
        arrfree(case_blocks);

        switch_block(ctx, join_block);
        break;
    }

    case IRON_HIR_STMT_RETURN: {
        /* Emit defer cleanups before return */
        /* Create an exit block to hold the actual RETURN */
        IronLIR_Block *exit_block = new_block(ctx, make_label(ctx, "return_exit"));

        /* Emit deferred cleanups inline (simplified: emit in current block chain) */
        if (ctx->defer_stacks && ctx->defer_depth > 0) {
            emit_defer_cleanup(ctx, exit_block, span);
            /* After emitting cleanup, switch_block to exit_block happens inside */
            switch_block(ctx, exit_block);
        } else {
            /* No defers: jump to exit block */
            if (!block_is_terminated(ctx->current_block)) {
                iron_lir_jump(ctx->current_func, ctx->current_block, exit_block->id, span);
            }
            switch_block(ctx, exit_block);
        }

        /* Emit the actual RETURN */
        if (!block_is_terminated(ctx->current_block)) {
            if (stmt->return_stmt.value) {
                IronLIR_ValueId val = lower_expr(ctx, stmt->return_stmt.value);
                iron_lir_return(ctx->current_func, ctx->current_block,
                                val, false, stmt->return_stmt.value->type, span);
            } else {
                iron_lir_return(ctx->current_func, ctx->current_block,
                                IRON_LIR_VALUE_INVALID, true, NULL, span);
            }
        }

        ctx->current_block = NULL; /* mark dead code */
        break;
    }

    case IRON_HIR_STMT_DEFER: {
        /* Push defer body onto defer stack at current depth */
        if (ctx->defer_depth == 0) {
            ctx->defer_depth = 1;
            ctx->defer_stacks = NULL;
            arrput(ctx->defer_stacks, (IronHIR_Block **)NULL);
        }
        /* Ensure stack has entry for current depth */
        while ((int)arrlen(ctx->defer_stacks) < ctx->defer_depth) {
            arrput(ctx->defer_stacks, (IronHIR_Block **)NULL);
        }
        arrput(ctx->defer_stacks[ctx->defer_depth - 1], stmt->defer.body);
        break;
    }

    case IRON_HIR_STMT_BLOCK: {
        lower_block_stmts(ctx, stmt->block.block);
        break;
    }

    case IRON_HIR_STMT_EXPR: {
        (void)lower_expr(ctx, stmt->expr_stmt.expr);
        break;
    }

    case IRON_HIR_STMT_FREE: {
        IronLIR_ValueId val = lower_expr(ctx, stmt->free_stmt.value);
        if (ctx->current_block && !block_is_terminated(ctx->current_block)) {
            iron_lir_free(ctx->current_func, ctx->current_block, val, span);
        }
        break;
    }

    case IRON_HIR_STMT_SPAWN: {
        /* Lifted spawn function */
        char lifted_name[64];
        snprintf(lifted_name, sizeof(lifted_name), "__spawn_%d", ctx->label_counter++);
        char *lifted_arena = (char *)iron_arena_alloc(ctx->lir_arena,
                                                       strlen(lifted_name) + 1, 1);
        memcpy(lifted_arena, lifted_name, strlen(lifted_name) + 1);

        if (ctx->current_block && !block_is_terminated(ctx->current_block)) {
            iron_lir_spawn(ctx->current_func, ctx->current_block,
                           lifted_arena, IRON_LIR_VALUE_INVALID,
                           stmt->spawn.handle_name,
                           iron_type_make_primitive(IRON_TYPE_VOID), span);
        }
        break;
    }

    case IRON_HIR_STMT_LEAK: {
        /* Intentional memory leak — just evaluate the expression */
        (void)lower_expr(ctx, stmt->leak.value);
        break;
    }

    default:
        break;
    }
}

static void lower_block_stmts(HIR_to_LIR_Ctx *ctx, IronHIR_Block *block) {
    if (!block) return;
    for (int i = 0; i < block->stmt_count; i++) {
        if (!ctx->current_block) break; /* dead code after return */
        lower_stmt(ctx, block->stmts[i]);
    }
}

/* ── Pass 1: Flatten an HIR function to pre-SSA CFG ────────────────────── */
static void flatten_func(HIR_to_LIR_Ctx *ctx, IronHIR_Func *hir_func) {
    if (hir_func->is_extern) return; /* extern functions have no body */

    Iron_Type *ret_type = hir_func->return_type;
    if (ret_type && ret_type->kind == IRON_TYPE_VOID) ret_type = NULL;

    /* Build LIR param array */
    int param_count = hir_func->param_count;
    IronLIR_Param *lir_params = NULL;
    if (param_count > 0) {
        lir_params = (IronLIR_Param *)iron_arena_alloc(
            ctx->lir_arena,
            (size_t)param_count * sizeof(IronLIR_Param),
            _Alignof(IronLIR_Param));
        for (int p = 0; p < param_count; p++) {
            lir_params[p].name = hir_func->params[p].name;
            lir_params[p].type = hir_func->params[p].type;
        }
    }

    /* Find or create LIR function */
    IronLIR_Func *lir_func = find_lir_func(ctx->lir_module, hir_func->name);
    if (!lir_func) {
        lir_func = iron_lir_func_create(ctx->lir_module, hir_func->name,
                                         lir_params, param_count, ret_type);
    }

    /* Set up context for this function */
    ctx->current_func = lir_func;

    /* Create entry block */
    IronLIR_Block *entry = iron_lir_block_create(lir_func, "entry");
    ctx->current_block = entry;
    ctx->entry_block   = entry;

    /* Reset per-function maps */
    hmfree(ctx->var_alloca_map);
    hmfree(ctx->val_binding_map);
    hmfree(ctx->param_map);
    ctx->var_alloca_map  = NULL;
    ctx->val_binding_map = NULL;
    ctx->param_map       = NULL;

    /* Reset defer stacks */
    if (ctx->defer_stacks) {
        for (int d = 0; d < ctx->defer_depth; d++) {
            arrfree(ctx->defer_stacks[d]);
        }
        arrfree(ctx->defer_stacks);
        ctx->defer_stacks = NULL;
    }
    ctx->defer_depth = 0;
    ctx->function_scope_depth = 0;

    /* Register parameters in param_map
     * LIR convention: param i has ValueId = i*2+1, alloca ValueId = i*2+2
     * We use alloca for params too (for consistency with existing lowering).
     * Actually, we just track param value IDs directly. */
    for (int p = 0; p < param_count; p++) {
        IronLIR_ValueId param_val = (IronLIR_ValueId)(p * 2 + 1);
        hmput(ctx->param_map, hir_func->params[p].var_id, param_val);
    }

    /* Lower function body */
    if (hir_func->body) {
        lower_block_stmts(ctx, hir_func->body);
    }

    /* Emit implicit void return if function doesn't end with a terminator */
    if (ctx->current_block && !block_is_terminated(ctx->current_block)) {
        Iron_Span zero_span = {0};
        iron_lir_return(lir_func, ctx->current_block,
                        IRON_LIR_VALUE_INVALID, true, NULL, zero_span);
    }
}

/* ── Pass 2: SSA construction via dominance frontiers ───────────────────── */

/* Named type for variable rename stack entries */
typedef struct { IronLIR_ValueId key; IronLIR_ValueId *value; } VarStackEntry; /* alloca_id -> stb_ds stack */

/* Recursively rename variables in dominator tree order */
static void ssa_rename_block(IronLIR_Func *fn, IronLIR_Block *block,
                              HirLir_DomEntry *idom,
                              IronLIR_Instr **value_table_snap,
                              VarStackEntry *var_stacks,
                              /* stb_ds array of pushed alloca_ids (per DFS frame) */
                              IronLIR_ValueId **pushed_per_block) {
    (void)idom;
    (void)value_table_snap;
    (void)pushed_per_block;

    /* For each instruction in block:
     * - LOAD from alloca: replace with top of var stack
     * - STORE to alloca: push stored value onto var stack
     * - PHI: push phi result onto var stack */
    for (int ii = 0; ii < block->instr_count; ii++) {
        IronLIR_Instr *instr = block->instrs[ii];
        if (!instr) continue;

        if (instr->kind == IRON_LIR_PHI) {
            /* PHI defines a new value — push it onto the stack for its alloca */
            /* We'll handle this in the phi insertion phase */
        } else if (instr->kind == IRON_LIR_LOAD) {
            IronLIR_ValueId ptr = instr->load.ptr;
            /* Check if ptr is an alloca */
            if (ptr != IRON_LIR_VALUE_INVALID &&
                (ptrdiff_t)ptr < (ptrdiff_t)arrlen(fn->value_table) &&
                fn->value_table[ptr] &&
                fn->value_table[ptr]->kind == IRON_LIR_ALLOCA) {
                /* Look up current definition */
                ptrdiff_t si = hmgeti(var_stacks, ptr);
                if (si >= 0 && arrlen(var_stacks[si].value) > 0) {
                    IronLIR_ValueId def = var_stacks[si].value[arrlen(var_stacks[si].value) - 1];
                    /* Replace this LOAD's id with def in all subsequent uses */
                    /* Mark this load as "replaced" so consumers use def */
                    /* For simplicity: patch the fn->value_table entry */
                    if (fn->value_table[instr->id]) {
                        /* Create a redirect: the load instruction still exists
                         * but we record the replacement in value_table */
                        fn->value_table[instr->id] = fn->value_table[def];
                    }
                }
            }
        } else if (instr->kind == IRON_LIR_STORE) {
            IronLIR_ValueId ptr = instr->store.ptr;
            if (ptr != IRON_LIR_VALUE_INVALID &&
                (ptrdiff_t)ptr < (ptrdiff_t)arrlen(fn->value_table) &&
                fn->value_table[ptr] &&
                fn->value_table[ptr]->kind == IRON_LIR_ALLOCA) {
                ptrdiff_t si = hmgeti(var_stacks, ptr);
                if (si < 0) {
                    hmput(var_stacks, ptr, NULL);
                    si = hmgeti(var_stacks, ptr);
                }
                arrput(var_stacks[si].value, instr->store.value);
            }
        }
    }
}

/* The full SSA construction algorithm for one function:
 * 1. rebuild_cfg_edges
 * 2. build_domtree
 * 3. compute dominance frontiers
 * 4. insert phi nodes at DF+ sites for each alloca variable
 * 5. rename (domtree walk) — replace alloca/load/store with direct values + phis */
static void ssa_construct_func(IronLIR_Func *fn) {
    if (!fn || fn->is_extern || fn->block_count == 0) return;

    /* Step 1: Build CFG edges */
    rebuild_cfg_edges(fn);

    /* Step 2: Build dominator tree */
    HirLir_DomEntry *idom = build_domtree(fn);
    if (!idom) return;

    /* Step 3: Compute dominance frontiers */
    IronLIR_BlockId **df = compute_dominance_frontiers(fn, idom);
    if (!df) {
        hmfree(idom);
        return;
    }

    /* Step 4: For each alloca variable, find def blocks, compute DF+ closure,
     * insert phi nodes */

    /* Collect all alloca ValueIds */
    IronLIR_ValueId *allocas = NULL;
    for (int bi = 0; bi < fn->block_count; bi++) {
        IronLIR_Block *blk = fn->blocks[bi];
        for (int ii = 0; ii < blk->instr_count; ii++) {
            IronLIR_Instr *instr = blk->instrs[ii];
            if (instr && instr->kind == IRON_LIR_ALLOCA) {
                arrput(allocas, instr->id);
            }
        }
    }

    /* For each alloca: find def blocks (blocks with STORE to alloca) */
    for (int ai = 0; ai < (int)arrlen(allocas); ai++) {
        IronLIR_ValueId alloca_id = allocas[ai];

        /* Find defining blocks (blocks that STORE to this alloca) */
        struct { IronLIR_BlockId key; bool value; } *def_blocks = NULL;
        struct { IronLIR_BlockId key; bool value; } *phi_placed  = NULL;

        for (int bi = 0; bi < fn->block_count; bi++) {
            IronLIR_Block *blk = fn->blocks[bi];
            for (int ii = 0; ii < blk->instr_count; ii++) {
                IronLIR_Instr *instr = blk->instrs[ii];
                if (instr && instr->kind == IRON_LIR_STORE &&
                    instr->store.ptr == alloca_id) {
                    hmput(def_blocks, blk->id, true);
                    break;
                }
            }
        }

        /* DF+ closure: iteratively propagate through DF */
        /* worklist = def_blocks initially */
        IronLIR_BlockId *worklist = NULL;
        for (int di = 0; di < (int)hmlen(def_blocks); di++) {
            arrput(worklist, def_blocks[di].key);
        }

        while (arrlen(worklist) > 0) {
            IronLIR_BlockId bid = worklist[arrlen(worklist) - 1];
            (void)arrpop(worklist);

            /* Find index of bid in fn->blocks[] */
            int block_idx = -1;
            for (int ri = 0; ri < fn->block_count; ri++) {
                if (fn->blocks[ri]->id == bid) { block_idx = ri; break; }
            }
            if (block_idx < 0) continue;

            /* For each block y in DF(bid): insert phi if not already placed */
            for (int di = 0; di < (int)arrlen(df[block_idx]); di++) {
                IronLIR_BlockId y_id = df[block_idx][di];
                if (hmgeti(phi_placed, y_id) >= 0) continue;
                hmput(phi_placed, y_id, true);

                /* Insert phi at beginning of block y */
                IronLIR_Block *y_blk = hirlir_find_block(fn, y_id);
                if (!y_blk) continue;

                /* Get type from alloca */
                Iron_Type *alloca_type = NULL;
                if (alloca_id < (IronLIR_ValueId)arrlen(fn->value_table) &&
                    fn->value_table[alloca_id]) {
                    alloca_type = fn->value_table[alloca_id]->alloca.alloc_type;
                }
                if (!alloca_type) continue;

                /* Create phi instruction — insert at front of y_blk */
                IronLIR_Instr *phi_instr = ARENA_ALLOC(fn->arena, IronLIR_Instr);
                memset(phi_instr, 0, sizeof(*phi_instr));
                phi_instr->kind = IRON_LIR_PHI;
                phi_instr->type = alloca_type;
                phi_instr->id   = fn->next_value_id++;
                while (arrlen(fn->value_table) <= (ptrdiff_t)phi_instr->id) {
                    arrput(fn->value_table, NULL);
                }
                fn->value_table[phi_instr->id] = phi_instr;

                /* Insert at front of y_blk->instrs (before existing instructions) */
                arrput(y_blk->instrs, NULL);
                y_blk->instr_count++;
                for (int k = y_blk->instr_count - 1; k > 0; k--) {
                    y_blk->instrs[k] = y_blk->instrs[k - 1];
                }
                y_blk->instrs[0] = phi_instr;

                /* Add phi's block to worklist if not already a def block */
                if (hmgeti(def_blocks, y_id) < 0) {
                    hmput(def_blocks, y_id, true);
                    arrput(worklist, y_id);
                }
            }
        }

        arrfree(worklist);
        hmfree(def_blocks);
        hmfree(phi_placed);
    }

    /* Step 5: Rename variables — walk dominator tree in preorder.
     * For each block: process instructions, then process successors.
     * Maintain a stack per alloca: stack[alloca_id] = stb_ds stack of current values. */

    /* Compute dominator tree children for each block (for preorder traversal) */
    /* children[block_id] = stb_ds array of block_ids that have bid as idom */
    struct { IronLIR_BlockId key; IronLIR_BlockId *value; } *dom_children = NULL;

    for (int bi = 0; bi < fn->block_count; bi++) {
        IronLIR_BlockId bid = fn->blocks[bi]->id;
        ptrdiff_t idx = hmgeti(idom, bid);
        if (idx < 0) continue;
        IronLIR_BlockId parent = idom[idx].value;
        if (parent == bid) continue; /* entry node */

        ptrdiff_t ci = hmgeti(dom_children, parent);
        if (ci < 0) {
            IronLIR_BlockId *empty_arr = NULL;
            hmput(dom_children, parent, empty_arr);
            ci = hmgeti(dom_children, parent);
        }
        arrput(dom_children[ci].value, bid);
    }

    /* Per-alloca value stacks: alloca_id -> stb_ds array of current SSA values */
    VarStackEntry *var_stacks = NULL;

    /* Iterative preorder DFS on dominator tree */
    typedef struct { IronLIR_BlockId bid; int child_idx; } DomFrame;
    DomFrame *dom_stack = NULL;

    if (fn->block_count > 0) {
        IronLIR_BlockId entry_id = fn->blocks[0]->id;
        DomFrame ef;
        ef.bid = entry_id;
        ef.child_idx = 0;
        arrput(dom_stack, ef);

        /* Process entry block first */
        {
            IronLIR_Block *blk = hirlir_find_block(fn, entry_id);
            if (blk) {
                ssa_rename_block(fn, blk, idom, fn->value_table,
                                  var_stacks, NULL);
            }
        }

        while (arrlen(dom_stack) > 0) {
            DomFrame *top = &dom_stack[arrlen(dom_stack) - 1];
            ptrdiff_t ci = hmgeti(dom_children, top->bid);
            IronLIR_BlockId *children = (ci >= 0) ? dom_children[ci].value : NULL;

            if (top->child_idx >= (int)arrlen(children)) {
                /* Pop frame */
                (void)arrpop(dom_stack);
            } else {
                IronLIR_BlockId child_id = children[top->child_idx++];
                IronLIR_Block *child_blk = hirlir_find_block(fn, child_id);
                if (child_blk) {
                    ssa_rename_block(fn, child_blk, idom, fn->value_table,
                                      var_stacks, NULL);
                }
                DomFrame cf;
                cf.bid = child_id;
                cf.child_idx = 0;
                arrput(dom_stack, cf);
            }
        }

        arrfree(dom_stack);
    }

    /* Fill phi incoming values by scanning predecessor blocks */
    for (int bi = 0; bi < fn->block_count; bi++) {
        IronLIR_Block *blk = fn->blocks[bi];
        for (int ii = 0; ii < blk->instr_count; ii++) {
            IronLIR_Instr *instr = blk->instrs[ii];
            if (!instr || instr->kind != IRON_LIR_PHI) continue;
            if (instr->phi.count > 0) continue; /* already has incoming values */

            /* Find the alloca this phi corresponds to:
             * Look for an alloca with the same type and find predecessors' last STORE */
            Iron_Type *phi_type = instr->type;
            for (int pi = 0; pi < (int)arrlen(blk->preds); pi++) {
                IronLIR_BlockId pred_id = blk->preds[pi];
                IronLIR_Block *pred_blk = hirlir_find_block(fn, pred_id);
                if (!pred_blk) continue;

                /* Find last STORE in pred_blk with matching alloca type */
                IronLIR_ValueId incoming_val = IRON_LIR_VALUE_INVALID;
                for (int k = pred_blk->instr_count - 1; k >= 0; k--) {
                    IronLIR_Instr *pi2 = pred_blk->instrs[k];
                    if (!pi2) continue;
                    if (pi2->kind == IRON_LIR_STORE) {
                        IronLIR_ValueId ptr = pi2->store.ptr;
                        if (ptr < (IronLIR_ValueId)arrlen(fn->value_table) &&
                            fn->value_table[ptr] &&
                            fn->value_table[ptr]->kind == IRON_LIR_ALLOCA &&
                            fn->value_table[ptr]->alloca.alloc_type == phi_type) {
                            incoming_val = pi2->store.value;
                            break;
                        }
                    }
                    if (pi2->kind == IRON_LIR_PHI && pi2->type == phi_type && pi2 != instr) {
                        incoming_val = pi2->id;
                        break;
                    }
                }

                if (incoming_val == IRON_LIR_VALUE_INVALID) {
                    /* Fallback: use a CONST_INT 0 or CONST_NULL */
                    IronLIR_Block *entry_blk = fn->blocks[0];
                    if (phi_type && phi_type->kind == IRON_TYPE_INT) {
                        Iron_Span zero_span = {0};
                        IronLIR_Instr *zero_c = iron_lir_const_int(fn, entry_blk, 0, phi_type, zero_span);
                        incoming_val = zero_c->id;
                    } else if (phi_type && phi_type->kind == IRON_TYPE_BOOL) {
                        Iron_Span zero_span = {0};
                        IronLIR_Instr *false_c = iron_lir_const_bool(fn, entry_blk, false, phi_type, zero_span);
                        incoming_val = false_c->id;
                    } else {
                        Iron_Span zero_span = {0};
                        IronLIR_Instr *null_c = iron_lir_const_null(fn, entry_blk, phi_type, zero_span);
                        incoming_val = null_c->id;
                    }
                }
                iron_lir_phi_add_incoming(instr, incoming_val, pred_id);
            }
        }
    }

    /* Cleanup */
    for (int bi = 0; bi < fn->block_count; bi++) {
        arrfree(df[bi]);
    }
    free(df);

    /* Free dom_children */
    for (int di = 0; di < (int)hmlen(dom_children); di++) {
        arrfree(dom_children[di].value);
    }
    hmfree(dom_children);

    /* Free var_stacks */
    for (int vi = 0; vi < (int)hmlen(var_stacks); vi++) {
        arrfree(var_stacks[vi].value);
    }
    hmfree(var_stacks);

    arrfree(allocas);
    hmfree(idom);
}

/* ── Main entry point ────────────────────────────────────────────────────── */

IronLIR_Module *iron_hir_to_lir(IronHIR_Module *hir, Iron_Program *program,
                                Iron_Scope *global_scope,
                                Iron_Arena *lir_arena, Iron_DiagList *diags) {
    if (!hir || !lir_arena) return NULL;

    /* Create LIR module */
    IronLIR_Module *lir_module = iron_lir_module_create(lir_arena, hir->name);

    /* Set up context */
    HIR_to_LIR_Ctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.hir          = hir;
    ctx.program      = program;
    ctx.global_scope = global_scope;
    ctx.lir_arena    = lir_arena;
    ctx.diags        = diags;
    ctx.lir_module   = lir_module;
    ctx.label_counter = 0;

    /* ── Pass 0: Register type declarations from AST ── */
    lower_type_decls_from_ast(&ctx);

    /* ── Pass 1: Flatten HIR functions to pre-SSA CFG ── */
    for (int fi = 0; fi < hir->func_count; fi++) {
        IronHIR_Func *hir_func = hir->funcs[fi];
        flatten_func(&ctx, hir_func);
    }

    /* ── Pass 2: SSA construction for each function ── */
    for (int fi = 0; fi < lir_module->func_count; fi++) {
        IronLIR_Func *fn = lir_module->funcs[fi];
        ssa_construct_func(fn);
    }

    /* ── Cleanup context ── */
    hmfree(ctx.var_alloca_map);
    hmfree(ctx.val_binding_map);
    hmfree(ctx.param_map);
    if (ctx.defer_stacks) {
        for (int d = 0; d < ctx.defer_depth; d++) {
            arrfree(ctx.defer_stacks[d]);
        }
        arrfree(ctx.defer_stacks);
    }

    /* ── Verify output ── */
    if (diags) {
        Iron_Arena verify_arena = iron_arena_create(64 * 1024);
        iron_lir_verify(lir_module, diags, &verify_arena);
        iron_arena_free(&verify_arena);
    }

    return lir_module;
}
