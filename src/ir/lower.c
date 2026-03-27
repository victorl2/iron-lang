/* lower.c — Two-pass AST-to-IR lowering orchestration.
 *
 * Pass 1 (lower_module_decls): register all module-level declarations
 * Pass 2 (lower_func_bodies):  lower each function body
 * Post-pass (lower_lift_pending): lift lambdas/spawn/pfor to top-level functions
 *
 * Mirrors the structure of codegen.c but emits IronIR instead of C text.
 */

#include "ir/lower_internal.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* ── Helper implementations ──────────────────────────────────────────────── */

IronIR_Block *new_block(IronIR_LowerCtx *ctx, const char *label) {
    return iron_ir_block_create(ctx->current_func, label);
}

void switch_block(IronIR_LowerCtx *ctx, IronIR_Block *block) {
    ctx->current_block = block;
}

IronIR_ValueId emit_poison(IronIR_LowerCtx *ctx, Iron_Type *type, Iron_Span span) {
    char msg[256];
    snprintf(msg, sizeof(msg), "unsupported or erroneous construct during lowering");
    iron_diag_emit(ctx->diags, ctx->ir_arena, IRON_DIAG_ERROR,
                   IRON_ERR_LOWER_UNSUPPORTED, span, msg,
                   "this construct is not yet supported by the IR lowering pass");
    IronIR_Instr *p = iron_ir_poison(ctx->current_func, ctx->current_block, type, span);
    return p->id;
}

void push_defer_scope(IronIR_LowerCtx *ctx) {
    ctx->defer_depth++;
    /* Grow the defer_stacks array if needed */
    while ((int)arrlen(ctx->defer_stacks) < ctx->defer_depth) {
        Iron_Node **empty = NULL;
        arrput(ctx->defer_stacks, empty);
    }
    /* Reset this level's list to empty */
    ctx->defer_stacks[ctx->defer_depth - 1] = NULL;
}

void pop_defer_scope(IronIR_LowerCtx *ctx) {
    if (ctx->defer_depth > 0) {
        arrfree(ctx->defer_stacks[ctx->defer_depth - 1]);
        ctx->defer_stacks[ctx->defer_depth - 1] = NULL;
        ctx->defer_depth--;
    }
}

void emit_defers_ir(IronIR_LowerCtx *ctx, int target_depth) {
    /* Emit deferred calls in reverse order from current scope outward to target_depth.
     * Mirrors emit_defers() in gen_stmts.c. */
    for (int d = ctx->defer_depth - 1; d >= target_depth; d--) {
        Iron_Node **stack = ctx->defer_stacks[d];
        int count = (int)arrlen(stack);
        for (int i = count - 1; i >= 0; i--) {
            lower_expr(ctx, stack[i]);
        }
    }
}

void lower_block(IronIR_LowerCtx *ctx, Iron_Block *block) {
    push_defer_scope(ctx);
    for (int i = 0; i < block->stmt_count; i++) {
        /* Stop lowering once the current block is terminated (e.g. after a return).
         * current_block == NULL means we're in dead code after a return/jump. */
        if (!ctx->current_block) break;
        lower_stmt(ctx, block->stmts[i]);
    }
    /* Only emit defers if we still have a live block to emit into */
    if (ctx->current_block) {
        emit_defers_ir(ctx, ctx->defer_depth - 1);
    }
    pop_defer_scope(ctx);
}

/* lower_stmt() is fully implemented in lower_stmts.c (Plan 08-02).
 * The declaration is in lower_internal.h. */

/* ── Pass 1 and Post-pass: Implemented in lower_types.c ─────────────────── */
/* lower_module_decls() and lower_lift_pending() are defined in lower_types.c */

/* ── Pass 2: Function body lowering ──────────────────────────────────────── */

static IronIR_Func *find_func_by_name(IronIR_Module *mod, const char *name) {
    for (int i = 0; i < mod->func_count; i++) {
        if (strcmp(mod->funcs[i]->name, name) == 0) {
            return mod->funcs[i];
        }
    }
    return NULL;
}

static void lower_func_body(IronIR_LowerCtx *ctx, Iron_FuncDecl *fd) {
    if (!fd->body) return;  /* extern func — no body */

    IronIR_Func *fn = find_func_by_name(ctx->module, fd->name);
    if (!fn) return;  /* should not happen if Pass 1 registered it */

    ctx->current_func = fn;

    /* Reset per-function maps */
    shfree(ctx->val_binding_map);
    shfree(ctx->var_alloca_map);
    shfree(ctx->param_map);
    ctx->val_binding_map = NULL;
    ctx->var_alloca_map  = NULL;
    ctx->param_map       = NULL;

    /* Create entry block */
    IronIR_Block *entry = iron_ir_block_create(fn, "entry");
    ctx->current_block = entry;

    ctx->function_scope_depth = ctx->defer_depth;
    ctx->current_func_name    = fd->name;

    /* Assign ValueIds for parameters.
     * Iron params are immutable — we use the alloca+load model for consistency
     * so that IRON_NODE_IDENT lookups work uniformly through var_alloca_map.
     * Each param gets:
     *   1. A synthetic ValueId (manually bumped, NULL in value_table) representing
     *      the argument value.
     *   2. An alloca in the entry block.
     *   3. A store of the param value into the alloca.
     *   4. The alloca's ValueId stored in var_alloca_map[param.name].
     *
     * Param types: use fn->params[p].type which was set by lower_module_decls
     * from the type annotation. Iron_Param has no declared_type field. */
    for (int p = 0; p < fd->param_count; p++) {
        Iron_Param *ap = (Iron_Param *)fd->params[p];
        /* Use the type stored on the IrFunc param (set by lower_module_decls) */
        Iron_Type  *pt = (p < fn->param_count) ? fn->params[p].type : NULL;

        /* Allocate a synthetic ValueId for the param argument value */
        IronIR_ValueId param_val_id = fn->next_value_id++;
        while (arrlen(fn->value_table) <= (ptrdiff_t)param_val_id) {
            arrput(fn->value_table, NULL);
        }
        /* NULL in value_table = synthetic param value (not backed by an instr) */
        fn->value_table[param_val_id] = NULL;

        /* Record in param_map */
        shput(ctx->param_map, ap->name, param_val_id);

        /* Create alloca for this param */
        IronIR_Instr *slot = iron_ir_alloca(fn, entry, pt, ap->name, ap->span);

        /* Store param value into alloca */
        iron_ir_store(fn, entry, slot->id, param_val_id, ap->span);

        /* Record alloca in var_alloca_map so IRON_NODE_IDENT loads from it */
        shput(ctx->var_alloca_map, ap->name, slot->id);
    }

    /* Lower the function body */
    Iron_Block *body = (Iron_Block *)fd->body;
    push_defer_scope(ctx);
    for (int i = 0; i < body->stmt_count; i++) {
        if (!ctx->current_block) break;  /* dead code after return */
        lower_stmt(ctx, body->stmts[i]);
    }
    if (ctx->current_block) {
        emit_defers_ir(ctx, ctx->function_scope_depth);
    }
    pop_defer_scope(ctx);

    /* If current_block is not terminated, emit implicit void return (void functions only).
     * For non-void functions we don't add an implicit return — the programmer must
     * provide explicit returns. Leaving the block unterminated means the verifier
     * will catch the missing terminator. (In practice, all control paths end with
     * an explicit return in well-typed Iron programs.) */
    if (fn->return_type == NULL) {
        if (ctx->current_block && ctx->current_block->instr_count > 0) {
            IronIR_Instr *last = ctx->current_block->instrs[ctx->current_block->instr_count - 1];
            if (!iron_ir_is_terminator(last->kind)) {
                iron_ir_return(fn, ctx->current_block, IRON_IR_VALUE_INVALID, true,
                               NULL, fd->span);
            }
        } else if (ctx->current_block && ctx->current_block->instr_count == 0) {
            iron_ir_return(fn, ctx->current_block, IRON_IR_VALUE_INVALID, true,
                           NULL, fd->span);
        }
    }
}

static void lower_func_bodies(IronIR_LowerCtx *ctx) {
    /* Lower top-level function declarations */
    for (int i = 0; i < ctx->program->decl_count; i++) {
        Iron_Node *decl = ctx->program->decls[i];
        if (decl->kind == IRON_NODE_FUNC_DECL) {
            lower_func_body(ctx, (Iron_FuncDecl *)decl);
        }
    }
    /* Method declarations on object types are handled in Plan 08-03 */
}

/* ── Public API ──────────────────────────────────────────────────────────── */

IronIR_Module *iron_ir_lower(Iron_Program *program, Iron_Scope *global_scope,
                              Iron_Arena *ir_arena, Iron_DiagList *diags) {
    if (!program || !ir_arena || !diags) return NULL;

    /* Create the module */
    IronIR_Module *module = iron_ir_module_create(ir_arena, "module");

    /* Initialize lowering context */
    IronIR_LowerCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.program      = program;
    ctx.global_scope = global_scope;
    ctx.ir_arena     = ir_arena;
    ctx.diags        = diags;
    ctx.module       = module;

    /* Pass 1: register declarations */
    lower_module_decls(&ctx);

    /* Pass 2: lower function bodies */
    lower_func_bodies(&ctx);

    /* Post-pass: lift pending lambdas/spawn/pfor */
    lower_lift_pending(&ctx);

    /* Clean up context-owned stb_ds arrays */
    arrfree(ctx.pending_lifts);
    if (ctx.defer_stacks) {
        for (int d = 0; d < (int)arrlen(ctx.defer_stacks); d++) {
            arrfree(ctx.defer_stacks[d]);
        }
        arrfree(ctx.defer_stacks);
    }
    shfree(ctx.val_binding_map);
    shfree(ctx.var_alloca_map);
    shfree(ctx.param_map);

    /* Return NULL if any errors occurred */
    if (diags->error_count > 0) {
        return NULL;
    }
    return module;
}
