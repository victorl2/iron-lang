/* lower_stmts.c — Statement lowering from AST to IR.
 * Mirrors gen_stmts.c but produces IronIR instructions instead of C text.
 *
 * Implements the full lower_stmt() dispatch for all IRON_NODE_* statement kinds.
 * This replaces the stub lower_stmt() that was in lower.c (Plan 08-01).
 */

#include "ir/lower_internal.h"
#include "lexer/lexer.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* ── Helper: check whether a block already has a terminator ─────────────── */

static bool block_is_terminated(IronIR_Block *block) {
    /* NULL block means dead code after a return — treat as already terminated */
    if (!block) return true;
    if (block->instr_count == 0) return false;
    return iron_ir_is_terminator(block->instrs[block->instr_count - 1]->kind);
}

/* ── lower_stmt ──────────────────────────────────────────────────────────── */

void lower_stmt(IronIR_LowerCtx *ctx, Iron_Node *node) {
    if (!node) return;
    if (!ctx->current_func || !ctx->current_block) return;

    IronIR_Func *fn  = ctx->current_func;
    Iron_Span    span = node->span;

    /* If current block is already terminated, any new statement is dead code.
     * We still lower declarations to keep the val/var maps consistent but
     * skip control-flow emission. */

    switch (node->kind) {

        /* ── Val declaration (immutable binding, no alloca) ──────────────── */
        case IRON_NODE_VAL_DECL: {
            Iron_ValDecl *vd = (Iron_ValDecl *)node;
            IronIR_ValueId init_val = IRON_IR_VALUE_INVALID;
            if (vd->init) init_val = lower_expr(ctx, vd->init);
            shput(ctx->val_binding_map, vd->name, init_val);
            break;
        }

        /* ── Var declaration (mutable, alloca in entry block) ────────────── */
        case IRON_NODE_VAR_DECL: {
            Iron_VarDecl *vd = (Iron_VarDecl *)node;
            /* Alloca MUST go in the entry block (blocks[0]), not the current
             * block, to avoid alloca inside loop bodies (research Pitfall 1). */
            IronIR_Block *entry = fn->blocks[0];
            IronIR_Instr *slot  = iron_ir_alloca(fn, entry,
                                                  vd->declared_type, vd->name,
                                                  vd->span);
            shput(ctx->var_alloca_map, vd->name, slot->id);
            if (vd->init) {
                IronIR_ValueId init_val = lower_expr(ctx, vd->init);
                iron_ir_store(fn, ctx->current_block, slot->id, init_val, vd->span);
            }
            break;
        }

        /* ── Assignment ──────────────────────────────────────────────────── */
        case IRON_NODE_ASSIGN: {
            Iron_AssignStmt *a = (Iron_AssignStmt *)node;
            IronIR_ValueId rhs = lower_expr(ctx, a->value);
            if (a->target->kind == IRON_NODE_IDENT) {
                Iron_Ident *id = (Iron_Ident *)a->target;
                IronIR_ValueId slot = shget(ctx->var_alloca_map, id->name);
                iron_ir_store(fn, ctx->current_block, slot, rhs, span);
            } else if (a->target->kind == IRON_NODE_FIELD_ACCESS) {
                Iron_FieldAccess *fa = (Iron_FieldAccess *)a->target;
                IronIR_ValueId obj = lower_expr(ctx, fa->object);
                iron_ir_set_field(fn, ctx->current_block, obj, fa->field, rhs, span);
                /* For value-type structs stored in a var alloca, the SET_FIELD
                 * modifies a loaded copy.  We must store the modified copy back
                 * to the alloca so subsequent loads see the updated value. */
                if (fa->object->kind == IRON_NODE_IDENT) {
                    Iron_Ident *obj_id = (Iron_Ident *)fa->object;
                    ptrdiff_t slot_idx = shgeti(ctx->var_alloca_map, obj_id->name);
                    if (slot_idx >= 0) {
                        IronIR_ValueId slot = ctx->var_alloca_map[slot_idx].value;
                        iron_ir_store(fn, ctx->current_block, slot, obj, span);
                    }
                }
            } else if (a->target->kind == IRON_NODE_INDEX) {
                Iron_IndexExpr *idx = (Iron_IndexExpr *)a->target;
                IronIR_ValueId arr   = lower_expr(ctx, idx->object);
                IronIR_ValueId index = lower_expr(ctx, idx->index);
                iron_ir_set_index(fn, ctx->current_block, arr, index, rhs, span);
                /* For arrays stored in a var alloca, the SET_INDEX modifies
                 * the loaded copy via pointer.  Store back to the alloca so
                 * subsequent loads see the updated array.  This mirrors the
                 * SET_FIELD store-back pattern above. */
                if (idx->object->kind == IRON_NODE_IDENT) {
                    Iron_Ident *arr_id = (Iron_Ident *)idx->object;
                    ptrdiff_t slot_idx = shgeti(ctx->var_alloca_map, arr_id->name);
                    if (slot_idx >= 0) {
                        IronIR_ValueId slot = ctx->var_alloca_map[slot_idx].value;
                        iron_ir_store(fn, ctx->current_block, slot, arr, span);
                    }
                }
            } else {
                emit_poison(ctx, NULL, span);
            }
            break;
        }

        /* ── If / else-if / else ─────────────────────────────────────────── */
        case IRON_NODE_IF: {
            Iron_IfStmt *is = (Iron_IfStmt *)node;

            /* Allocate merge block upfront; used as final continuation */
            IronIR_Block *merge_block = new_block(ctx, "if_merge");

            /* Lower the primary condition */
            IronIR_ValueId cond_val = lower_expr(ctx, is->condition);

            /* Build chain of if/elif conditions.
             * Each condition branches to its then-block or the next condition
             * block (or the else/merge block if no more elifs). */

            IronIR_Block *then_block = new_block(ctx, "if_then");

            /* Determine the "else target" for the primary condition */
            IronIR_Block *else_target;
            if (is->elif_count > 0) {
                else_target = new_block(ctx, "if_elif_cond");
            } else if (is->else_body) {
                else_target = new_block(ctx, "if_else");
            } else {
                /* No else: the false branch falls through to merge */
                else_target = merge_block;
            }

            /* Emit primary branch */
            iron_ir_branch(fn, ctx->current_block, cond_val,
                           then_block->id, else_target->id, span);

            /* Lower then-body.
             * After lower_block returns, ctx->current_block is either the live
             * continuation block or NULL (dead code after a return).
             * block_is_terminated(NULL) == true, so the guard below is safe. */
            switch_block(ctx, then_block);
            lower_block(ctx, (Iron_Block *)is->body);
            if (!block_is_terminated(ctx->current_block)) {
                iron_ir_jump(fn, ctx->current_block, merge_block->id, span);
            }

            /* Lower elif chains */
            IronIR_Block *cur_elif_cond_block = else_target;
            for (int i = 0; i < is->elif_count; i++) {
                /* At this point cur_elif_cond_block is the condition block */
                switch_block(ctx, cur_elif_cond_block);
                IronIR_ValueId elif_cond = lower_expr(ctx, is->elif_conds[i]);

                IronIR_Block *elif_then = new_block(ctx, "if_elif_then");

                IronIR_Block *next_else;
                if (i + 1 < is->elif_count) {
                    next_else = new_block(ctx, "if_elif_cond");
                } else if (is->else_body) {
                    next_else = new_block(ctx, "if_else");
                } else {
                    next_else = merge_block;
                }

                iron_ir_branch(fn, cur_elif_cond_block, elif_cond,
                               elif_then->id, next_else->id, span);

                switch_block(ctx, elif_then);
                lower_block(ctx, (Iron_Block *)is->elif_bodies[i]);
                if (!block_is_terminated(ctx->current_block)) {
                    iron_ir_jump(fn, ctx->current_block, merge_block->id, span);
                }

                cur_elif_cond_block = next_else;
            }

            /* Lower else body (cur_elif_cond_block now points to else block
             * if there is one, otherwise it IS merge_block) */
            if (is->else_body) {
                switch_block(ctx, cur_elif_cond_block);
                lower_block(ctx, (Iron_Block *)is->else_body);
                if (!block_is_terminated(ctx->current_block)) {
                    iron_ir_jump(fn, ctx->current_block, merge_block->id, span);
                }
            }

            /* Continue at merge.
             * If all branches terminated (returns), merge_block will have zero
             * instructions — emit_c.c emits __builtin_unreachable() for those. */
            switch_block(ctx, merge_block);
            break;
        }

        /* ── While loop ──────────────────────────────────────────────────── */
        case IRON_NODE_WHILE: {
            Iron_WhileStmt *ws = (Iron_WhileStmt *)node;

            IronIR_Block *header = new_block(ctx, "while_header");
            IronIR_Block *body   = new_block(ctx, "while_body");
            IronIR_Block *exit   = new_block(ctx, "while_exit");

            /* Jump from current block to header */
            iron_ir_jump(fn, ctx->current_block, header->id, span);

            /* Header: lower condition, branch to body or exit.
             * NOTE: lower_expr may switch ctx->current_block (e.g. for `and`/`or`
             * short-circuit expressions that emit multiple basic blocks).  The
             * branch must be appended to ctx->current_block — the merge block at
             * the end of the condition evaluation — not to `header`. */
            switch_block(ctx, header);
            IronIR_ValueId cond_val = lower_expr(ctx, ws->condition);
            iron_ir_branch(fn, ctx->current_block, cond_val, body->id, exit->id, span);

            /* Save outer loop context */
            IronIR_Block *old_exit     = ctx->loop_exit_block;
            IronIR_Block *old_continue = ctx->loop_continue_block;
            int           old_depth    = ctx->loop_scope_depth;
            ctx->loop_exit_block     = exit;
            ctx->loop_continue_block = header;
            ctx->loop_scope_depth    = ctx->defer_depth;

            /* Body: lower body statements */
            switch_block(ctx, body);
            lower_block(ctx, (Iron_Block *)ws->body);
            if (!block_is_terminated(ctx->current_block)) {
                /* Back-edge to header */
                iron_ir_jump(fn, ctx->current_block, header->id, span);
            }

            /* Restore outer loop context */
            ctx->loop_exit_block     = old_exit;
            ctx->loop_continue_block = old_continue;
            ctx->loop_scope_depth    = old_depth;

            /* Continue at exit */
            switch_block(ctx, exit);
            break;
        }

        /* ── For loop ────────────────────────────────────────────────────── */
        case IRON_NODE_FOR: {
            Iron_ForStmt *fs = (Iron_ForStmt *)node;

            if (fs->is_parallel) {
                /* Parallel for: defer to Plan 08-03 lifting pass.
                 * Push a LiftPending descriptor and emit IRON_IR_PARALLEL_FOR. */
                char chunk_name_tmp[128];
                snprintf(chunk_name_tmp, sizeof(chunk_name_tmp),
                         "__pfor_%d", ctx->module->parallel_counter++);
                /* Arena-dup the name FIRST so the pointer survives past this frame */
                const char *chunk_name = iron_arena_strdup(ctx->ir_arena,
                                                            chunk_name_tmp,
                                                            strlen(chunk_name_tmp));
                LiftPending lp;
                lp.kind          = LIFT_PARALLEL_FOR;
                lp.ast_node      = node;
                lp.lifted_name   = chunk_name;
                lp.enclosing_func = ctx->current_func_name;
                arrput(ctx->pending_lifts, lp);

                /* Emit parallel_for instruction (range_val = iterable value) */
                IronIR_ValueId range_val = lower_expr(ctx, fs->iterable);
                iron_ir_parallel_for(fn, ctx->current_block,
                                     fs->var_name, range_val, chunk_name,
                                     IRON_IR_VALUE_INVALID,  /* pool_val: default */
                                     NULL, 0, span);
            } else {
                /* Sequential for: lower as while loop pattern.
                 *
                 * Two variants:
                 *  A) Integer bound (for i in n):
                 *     var_name = counter (0..n-1)
                 *  B) Array iteration (for x in arr):
                 *     hidden index counter, var_name = arr[index]
                 *
                 * Pattern for both:
                 *   entry:      alloca for loop_var (index for A, item for B)
                 *   pre_header: store 0 into index slot
                 *   header:     load index, compare < bound, branch body/exit
                 *   body:       [B: load item = arr[index]; store item slot]
                 *               lower body stmts, jump to increment
                 *   increment:  load, add 1, store, jump to header
                 *   exit:       continue here
                 */
                IronIR_Block *entry      = fn->blocks[0];
                Iron_Type    *int_type   = iron_type_make_primitive(IRON_TYPE_INT);
                Iron_Type    *bool_type  = iron_type_make_primitive(IRON_TYPE_BOOL);

                /* Detect array-iteration: check the AST iterable's resolved type */
                typedef struct { Iron_Span span; Iron_NodeKind kind; Iron_Type *resolved_type; } ExprNode;
                Iron_Type *iter_type = ((ExprNode *)fs->iterable)->resolved_type;
                bool is_array_iter = (iter_type && iter_type->kind == IRON_TYPE_ARRAY);

                if (is_array_iter) {
                    /* Array iteration: for x in arr { ... }
                     * Use a hidden index and bind var_name to arr[index] each iteration.
                     */
                    char idx_name[128];
                    snprintf(idx_name, sizeof(idx_name), "__for_idx_%p", (void *)node);

                    /* Alloca for hidden index */
                    IronIR_Instr *idx_slot = iron_ir_alloca(fn, entry,
                                                              int_type, idx_name, span);

                    /* Alloca for the item (var_name) */
                    Iron_Type *elem_type = iter_type->array.elem;
                    IronIR_Instr *item_slot = iron_ir_alloca(fn, entry,
                                                               elem_type, fs->var_name, span);
                    shput(ctx->var_alloca_map, fs->var_name, item_slot->id);

                    IronIR_Block *pre_header = new_block(ctx, "for_init");
                    IronIR_Block *header     = new_block(ctx, "for_header");
                    IronIR_Block *body_blk   = new_block(ctx, "for_body");
                    IronIR_Block *increment  = new_block(ctx, "for_incr");
                    IronIR_Block *exit_blk   = new_block(ctx, "for_exit");

                    iron_ir_jump(fn, ctx->current_block, pre_header->id, span);

                    /* Pre-header: store 0 into index */
                    switch_block(ctx, pre_header);
                    IronIR_Instr *zero = iron_ir_const_int(fn, pre_header, 0, int_type, span);
                    iron_ir_store(fn, pre_header, idx_slot->id, zero->id, span);
                    /* Evaluate array once in pre_header */
                    IronIR_ValueId arr_val = lower_expr(ctx, fs->iterable);
                    iron_ir_jump(fn, pre_header, header->id, span);

                    /* Header: load index, get arr.count, compare */
                    switch_block(ctx, header);
                    IronIR_Instr *idx_cur = iron_ir_load(fn, header, idx_slot->id, int_type, span);
                    /* Access arr.count field for the bound */
                    IronIR_Instr *bound_instr = iron_ir_get_field(fn, header,
                                                                    arr_val, "count",
                                                                    int_type, span);
                    IronIR_Instr *cmp = iron_ir_binop(fn, header, IRON_IR_LT,
                                                       idx_cur->id, bound_instr->id,
                                                       bool_type, span);
                    iron_ir_branch(fn, header, cmp->id, body_blk->id, exit_blk->id, span);

                    /* Save outer loop context */
                    IronIR_Block *old_exit     = ctx->loop_exit_block;
                    IronIR_Block *old_continue = ctx->loop_continue_block;
                    int           old_depth    = ctx->loop_scope_depth;
                    ctx->loop_exit_block     = exit_blk;
                    ctx->loop_continue_block = increment;
                    ctx->loop_scope_depth    = ctx->defer_depth;

                    /* Body: get item at current index, store into item slot */
                    switch_block(ctx, body_blk);
                    IronIR_Instr *idx_body = iron_ir_load(fn, body_blk,
                                                           idx_slot->id, int_type, span);
                    IronIR_Instr *item = iron_ir_get_index(fn, body_blk,
                                                            arr_val, idx_body->id,
                                                            elem_type, span);
                    iron_ir_store(fn, body_blk, item_slot->id, item->id, span);
                    lower_block(ctx, (Iron_Block *)fs->body);
                    if (!block_is_terminated(ctx->current_block)) {
                        iron_ir_jump(fn, ctx->current_block, increment->id, span);
                    }

                    /* Increment: load idx, add 1, store */
                    switch_block(ctx, increment);
                    IronIR_Instr *idx_inc = iron_ir_load(fn, increment, idx_slot->id, int_type, span);
                    IronIR_Instr *one_inc = iron_ir_const_int(fn, increment, 1, int_type, span);
                    IronIR_Instr *next_idx = iron_ir_binop(fn, increment, IRON_IR_ADD,
                                                            idx_inc->id, one_inc->id,
                                                            int_type, span);
                    iron_ir_store(fn, increment, idx_slot->id, next_idx->id, span);
                    iron_ir_jump(fn, increment, header->id, span);

                    /* Restore outer loop context */
                    ctx->loop_exit_block     = old_exit;
                    ctx->loop_continue_block = old_continue;
                    ctx->loop_scope_depth    = old_depth;

                    switch_block(ctx, exit_blk);
                } else {
                    /* Integer bound: for i in n { ... }
                     * Loop variable = counter (0..n-1) */
                    /* Alloca for loop variable in entry block */
                    IronIR_Instr *slot = iron_ir_alloca(fn, entry,
                                                         int_type, fs->var_name, span);
                    shput(ctx->var_alloca_map, fs->var_name, slot->id);

                    IronIR_Block *pre_header = new_block(ctx, "for_init");
                    IronIR_Block *header     = new_block(ctx, "for_header");
                    IronIR_Block *body_blk   = new_block(ctx, "for_body");
                    IronIR_Block *increment  = new_block(ctx, "for_incr");
                    IronIR_Block *exit       = new_block(ctx, "for_exit");

                    /* Jump to pre_header */
                    iron_ir_jump(fn, ctx->current_block, pre_header->id, span);

                    /* Pre-header: store initial value 0 */
                    switch_block(ctx, pre_header);
                    IronIR_Instr *zero = iron_ir_const_int(fn, pre_header, 0, int_type, span);
                    iron_ir_store(fn, pre_header, slot->id, zero->id, span);
                    iron_ir_jump(fn, pre_header, header->id, span);

                    /* Header: load counter, get bound, compare */
                    switch_block(ctx, header);
                    IronIR_Instr *counter = iron_ir_load(fn, header, slot->id, int_type, span);
                    IronIR_ValueId bound  = lower_expr(ctx, fs->iterable);
                    IronIR_Instr *cmp     = iron_ir_binop(fn, header, IRON_IR_LT,
                                                           counter->id, bound,
                                                           bool_type, span);
                    iron_ir_branch(fn, header, cmp->id, body_blk->id, exit->id, span);

                    /* Save outer loop context */
                    IronIR_Block *old_exit     = ctx->loop_exit_block;
                    IronIR_Block *old_continue = ctx->loop_continue_block;
                    int           old_depth    = ctx->loop_scope_depth;
                    ctx->loop_exit_block     = exit;
                    ctx->loop_continue_block = increment;
                    ctx->loop_scope_depth    = ctx->defer_depth;

                    /* Body */
                    switch_block(ctx, body_blk);
                    lower_block(ctx, (Iron_Block *)fs->body);
                    if (!block_is_terminated(ctx->current_block)) {
                        iron_ir_jump(fn, ctx->current_block, increment->id, span);
                    }

                    /* Increment: load, add 1, store, jump back to header */
                    switch_block(ctx, increment);
                    IronIR_Instr *cur_val = iron_ir_load(fn, increment, slot->id, int_type, span);
                    IronIR_Instr *one     = iron_ir_const_int(fn, increment, 1, int_type, span);
                    IronIR_Instr *next_val = iron_ir_binop(fn, increment, IRON_IR_ADD,
                                                            cur_val->id, one->id,
                                                            int_type, span);
                    iron_ir_store(fn, increment, slot->id, next_val->id, span);
                    iron_ir_jump(fn, increment, header->id, span);

                    /* Restore outer loop context */
                    ctx->loop_exit_block     = old_exit;
                    ctx->loop_continue_block = old_continue;
                    ctx->loop_scope_depth    = old_depth;

                    /* Continue at exit */
                    switch_block(ctx, exit);
                }
            }
            break;
        }

        /* ── Match / switch ──────────────────────────────────────────────── */
        case IRON_NODE_MATCH: {
            Iron_MatchStmt *ms = (Iron_MatchStmt *)node;

            /* Lower subject */
            IronIR_ValueId subject_val = lower_expr(ctx, ms->subject);

            /* Create join block */
            IronIR_Block *join_block = new_block(ctx, "match_join");

            /* Create default block (else body or direct jump to join) */
            IronIR_Block *default_block;
            if (ms->else_body) {
                default_block = new_block(ctx, "match_else");
            } else {
                default_block = join_block;
            }

            /* Build case_values and case_blocks arrays */
            int            *case_values = NULL;  /* stb_ds dynamic array */
            IronIR_BlockId *case_blocks = NULL;  /* stb_ds dynamic array */

            for (int i = 0; i < ms->case_count; i++) {
                Iron_MatchCase *mc = (Iron_MatchCase *)ms->cases[i];
                IronIR_Block *arm_block = new_block(ctx, "match_arm");

                /* Determine the case value from the pattern.
                 * For enum integer patterns: the pattern is an IRON_NODE_INT_LIT.
                 * For other patterns we use the index as a fallback. */
                int case_val = i;  /* default: discriminant = arm index */
                if (mc->pattern && mc->pattern->kind == IRON_NODE_INT_LIT) {
                    Iron_IntLit *il = (Iron_IntLit *)mc->pattern;
                    /* Parse the string value to an int */
                    case_val = (int)strtol(il->value, NULL, 10);
                }

                arrput(case_values, case_val);
                arrput(case_blocks, arm_block->id);
            }

            /* Emit switch terminator */
            iron_ir_switch(fn, ctx->current_block, subject_val,
                           default_block->id,
                           case_values, case_blocks,
                           ms->case_count, span);

            /* Lower each arm */
            for (int i = 0; i < ms->case_count; i++) {
                Iron_MatchCase *mc = (Iron_MatchCase *)ms->cases[i];
                /* Find the arm block we created (it's at index i + offset in fn->blocks) */
                /* case_blocks[i] is the BlockId — look up the IronIR_Block* */
                IronIR_Block *arm_block = NULL;
                for (int b = 0; b < fn->block_count; b++) {
                    if (fn->blocks[b]->id == case_blocks[i]) {
                        arm_block = fn->blocks[b];
                        break;
                    }
                }
                if (!arm_block) continue;

                switch_block(ctx, arm_block);
                if (mc->body) {
                    lower_block(ctx, (Iron_Block *)mc->body);
                }
                /* ctx->current_block may be NULL after a return in the arm body */
                if (ctx->current_block && !block_is_terminated(ctx->current_block)) {
                    iron_ir_jump(fn, ctx->current_block, join_block->id, span);
                }
            }

            /* Lower else/default body */
            if (ms->else_body) {
                switch_block(ctx, default_block);
                lower_block(ctx, (Iron_Block *)ms->else_body);
                /* ctx->current_block may be NULL after a return in the else body */
                if (ctx->current_block && !block_is_terminated(ctx->current_block)) {
                    iron_ir_jump(fn, ctx->current_block, join_block->id, span);
                }
            }

            arrfree(case_values);
            arrfree(case_blocks);

            switch_block(ctx, join_block);
            break;
        }

        /* ── Return ──────────────────────────────────────────────────────── */
        case IRON_NODE_RETURN: {
            Iron_ReturnStmt *rs = (Iron_ReturnStmt *)node;

            /* CRITICAL: emit defers before the return terminator */
            emit_defers_ir(ctx, ctx->function_scope_depth);

            if (rs->value) {
                IronIR_ValueId ret_val = lower_expr(ctx, rs->value);
                iron_ir_return(fn, ctx->current_block, ret_val, false,
                               fn->return_type, span);
            } else {
                iron_ir_return(fn, ctx->current_block, IRON_IR_VALUE_INVALID,
                               true, NULL, span);
            }

            /* Mark current_block as NULL to indicate dead code after this return.
             * Any subsequent lower_stmt calls in this block will early-exit via the
             * guard at the top of lower_stmt. This avoids creating unterminated
             * dead blocks that the verifier would flag. */
            ctx->current_block = NULL;
            break;
        }

        /* ── Defer ───────────────────────────────────────────────────────── */
        case IRON_NODE_DEFER: {
            Iron_DeferStmt *ds = (Iron_DeferStmt *)node;
            /* Push the deferred expression onto the current defer scope's stack.
             * It is NOT lowered now; emit_defers_ir() lowers it at scope exit. */
            if (ctx->defer_depth > 0) {
                arrput(ctx->defer_stacks[ctx->defer_depth - 1], ds->expr);
            }
            break;
        }

        /* ── Free (explicit memory deallocation) ─────────────────────────── */
        case IRON_NODE_FREE: {
            Iron_FreeStmt *fs = (Iron_FreeStmt *)node;
            IronIR_ValueId val = lower_expr(ctx, fs->expr);
            iron_ir_free(fn, ctx->current_block, val, span);
            break;
        }

        /* ── Leak (no-op annotation) ─────────────────────────────────────── */
        case IRON_NODE_LEAK: {
            /* Semantic annotation only — tells the compiler to skip auto-free.
             * Lower the inner expression (so side effects are preserved) but
             * discard the resulting ValueId. */
            Iron_LeakStmt *ls = (Iron_LeakStmt *)node;
            if (ls->expr) lower_expr(ctx, ls->expr);
            break;
        }

        /* ── Spawn (concurrency — placeholder for Plan 08-03) ────────────── */
        case IRON_NODE_SPAWN: {
            Iron_SpawnStmt *ss = (Iron_SpawnStmt *)node;

            /* Generate lifted function name */
            char lifted_name[128];
            const char *task_name = ss->name ? ss->name : "task";
            snprintf(lifted_name, sizeof(lifted_name),
                     "__spawn_%s_%d", task_name, ctx->module->spawn_counter++);

            /* Push LiftPending descriptor for post-pass lifting */
            LiftPending lp;
            lp.kind           = LIFT_SPAWN;
            lp.ast_node       = node;
            lp.lifted_name    = iron_arena_strdup(ctx->ir_arena, lifted_name, strlen(lifted_name));
            lp.enclosing_func = ctx->current_func_name;
            arrput(ctx->pending_lifts, lp);

            /* Lower pool expression if present */
            IronIR_ValueId pool_val = IRON_IR_VALUE_INVALID;
            if (ss->pool_expr) {
                pool_val = lower_expr(ctx, ss->pool_expr);
            }

            /* Emit IRON_IR_SPAWN instruction */
            Iron_Type *void_type = iron_type_make_primitive(IRON_TYPE_VOID);
            iron_ir_spawn(fn, ctx->current_block,
                          lp.lifted_name, pool_val,
                          ss->handle_name ? ss->handle_name : "",
                          void_type, span);
            break;
        }

        /* ── Nested block ────────────────────────────────────────────────── */
        case IRON_NODE_BLOCK: {
            Iron_Block *blk = (Iron_Block *)node;
            lower_block(ctx, blk);
            break;
        }

        /* ── Default: expression-as-statement ────────────────────────────── */
        default: {
            /* Any expression appearing as a statement: lower it and discard
             * the value. This handles bare function calls like `foo();`. */
            lower_expr(ctx, node);
            break;
        }
    }
}
