/* lower_exprs.c — Expression lowering from AST to IR.
 * Implements lower_expr() — the main expression lowering dispatch function.
 * Mirrors gen_exprs.c but produces IronIR instructions instead of C text.
 *
 * Handles all IRON_NODE_* expression kinds for Phase 8 Plan 01.
 */

#include "ir/lower_internal.h"
#include "lexer/lexer.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>

/* ── Helper: extract resolved_type from any expression node ─────────────── */

/* Each expression subtype stores resolved_type as its third field (after span
 * and kind). We extract it by casting to a common layout struct.
 * This is safe for all IRON_NODE_*_LIT, IRON_NODE_IDENT, IRON_NODE_BINARY, etc.
 * since the type checker annotates all expression nodes with resolved_type. */
typedef struct {
    Iron_Span          span;
    Iron_NodeKind      kind;
    struct Iron_Type  *resolved_type;
} Iron_ExprNode;  /* common layout prefix for all expression nodes */

static Iron_Type *expr_type(Iron_Node *node) {
    if (!node) return NULL;
    /* All expression nodes have resolved_type as the third field */
    return ((Iron_ExprNode *)node)->resolved_type;
}

/* ── Short-circuit AND/OR helpers ────────────────────────────────────────── */

static IronIR_ValueId lower_short_circuit_and(IronIR_LowerCtx *ctx,
                                                Iron_BinaryExpr *bin,
                                                Iron_Node *node) {
    IronIR_Func  *fn   = ctx->current_func;
    Iron_Span     span = node->span;

    IronIR_Block *eval_rhs  = new_block(ctx, "and_rhs");
    IronIR_Block *short_blk = new_block(ctx, "and_short");
    IronIR_Block *merge     = new_block(ctx, "and_merge");

    IronIR_ValueId lhs_val  = lower_expr(ctx, bin->left);
    IronIR_Block  *lhs_exit = ctx->current_block;  /* may have changed block */
    iron_ir_branch(fn, lhs_exit, lhs_val, eval_rhs->id, short_blk->id, span);

    switch_block(ctx, eval_rhs);
    IronIR_ValueId rhs_val  = lower_expr(ctx, bin->right);
    IronIR_Block  *rhs_exit = ctx->current_block;
    iron_ir_jump(fn, rhs_exit, merge->id, span);

    switch_block(ctx, short_blk);
    Iron_Type     *bool_type = iron_type_make_primitive(IRON_TYPE_BOOL);
    IronIR_ValueId false_val = iron_ir_const_bool(fn, short_blk, false, bool_type, span)->id;
    iron_ir_jump(fn, short_blk, merge->id, span);

    switch_block(ctx, merge);
    IronIR_Instr  *phi = iron_ir_phi(fn, merge, bool_type, span);
    iron_ir_phi_add_incoming(phi, rhs_val,   rhs_exit->id);
    iron_ir_phi_add_incoming(phi, false_val, short_blk->id);
    return phi->id;
}

static IronIR_ValueId lower_short_circuit_or(IronIR_LowerCtx *ctx,
                                               Iron_BinaryExpr *bin,
                                               Iron_Node *node) {
    IronIR_Func  *fn   = ctx->current_func;
    Iron_Span     span = node->span;

    IronIR_Block *eval_rhs  = new_block(ctx, "or_rhs");
    IronIR_Block *short_blk = new_block(ctx, "or_short");
    IronIR_Block *merge     = new_block(ctx, "or_merge");

    IronIR_ValueId lhs_val  = lower_expr(ctx, bin->left);
    IronIR_Block  *lhs_exit = ctx->current_block;
    /* OR: true -> short-circuit, false -> eval rhs */
    iron_ir_branch(fn, lhs_exit, lhs_val, short_blk->id, eval_rhs->id, span);

    switch_block(ctx, eval_rhs);
    IronIR_ValueId rhs_val  = lower_expr(ctx, bin->right);
    IronIR_Block  *rhs_exit = ctx->current_block;
    iron_ir_jump(fn, rhs_exit, merge->id, span);

    switch_block(ctx, short_blk);
    Iron_Type     *bool_type = iron_type_make_primitive(IRON_TYPE_BOOL);
    IronIR_ValueId true_val  = iron_ir_const_bool(fn, short_blk, true, bool_type, span)->id;
    iron_ir_jump(fn, short_blk, merge->id, span);

    switch_block(ctx, merge);
    IronIR_Instr  *phi = iron_ir_phi(fn, merge, bool_type, span);
    iron_ir_phi_add_incoming(phi, rhs_val,  rhs_exit->id);
    iron_ir_phi_add_incoming(phi, true_val, short_blk->id);
    return phi->id;
}

/* ── Main dispatch ───────────────────────────────────────────────────────── */

IronIR_ValueId lower_expr(IronIR_LowerCtx *ctx, Iron_Node *node) {
    if (!node) return IRON_IR_VALUE_INVALID;

    IronIR_Func  *fn   = ctx->current_func;
    Iron_Span     span = node->span;

    switch (node->kind) {

    /* ── Literal constants (INSTR-01) ────────────────────────────────────── */

    case IRON_NODE_INT_LIT: {
        Iron_IntLit *lit = (Iron_IntLit *)node;
        int64_t val = (int64_t)strtoll(lit->value, NULL, 0);
        return iron_ir_const_int(fn, ctx->current_block, val,
                                 lit->resolved_type, span)->id;
    }

    case IRON_NODE_FLOAT_LIT: {
        Iron_FloatLit *lit = (Iron_FloatLit *)node;
        double val = strtod(lit->value, NULL);
        return iron_ir_const_float(fn, ctx->current_block, val,
                                   lit->resolved_type, span)->id;
    }

    case IRON_NODE_BOOL_LIT: {
        Iron_BoolLit *lit = (Iron_BoolLit *)node;
        return iron_ir_const_bool(fn, ctx->current_block, lit->value,
                                  lit->resolved_type, span)->id;
    }

    case IRON_NODE_STRING_LIT: {
        Iron_StringLit *lit = (Iron_StringLit *)node;
        return iron_ir_const_string(fn, ctx->current_block, lit->value,
                                    lit->resolved_type, span)->id;
    }

    case IRON_NODE_NULL_LIT: {
        Iron_NullLit *lit = (Iron_NullLit *)node;
        return iron_ir_const_null(fn, ctx->current_block,
                                  lit->resolved_type, span)->id;
    }

    /* ── Identifier (INSTR-04) ───────────────────────────────────────────── */

    case IRON_NODE_IDENT: {
        Iron_Ident *id = (Iron_Ident *)node;
        /* Check val_binding_map first (direct SSA value, no load needed) */
        int idx = shgeti(ctx->val_binding_map, id->name);
        if (idx >= 0) {
            return ctx->val_binding_map[idx].value;
        }
        /* Check var_alloca_map (mutable var — emit load from alloca) */
        idx = shgeti(ctx->var_alloca_map, id->name);
        if (idx >= 0) {
            IronIR_ValueId slot = ctx->var_alloca_map[idx].value;
            return iron_ir_load(fn, ctx->current_block, slot,
                                id->resolved_type, span)->id;
        }
        /* Unresolved identifier */
        char msg[256];
        snprintf(msg, sizeof(msg), "unresolved identifier '%s' during IR lowering", id->name);
        iron_diag_emit(ctx->diags, ctx->ir_arena, IRON_DIAG_ERROR,
                       IRON_ERR_LOWER_UNRESOLVED_IDENT, span, msg,
                       "ensure the identifier is declared before use");
        return emit_poison(ctx, id->resolved_type, span);
    }

    /* ── Binary expressions (INSTR-02, short-circuit) ────────────────────── */

    case IRON_NODE_BINARY: {
        Iron_BinaryExpr *bin = (Iron_BinaryExpr *)node;
        Iron_OpKind op = bin->op;

        /* Short-circuit AND */
        if (op == IRON_TOK_AND) {
            return lower_short_circuit_and(ctx, bin, node);
        }
        /* Short-circuit OR */
        if (op == IRON_TOK_OR) {
            return lower_short_circuit_or(ctx, bin, node);
        }

        /* Arithmetic and comparison: map token to IR kind */
        IronIR_InstrKind ir_kind;
        switch (op) {
            case IRON_TOK_PLUS:       ir_kind = IRON_IR_ADD;  break;
            case IRON_TOK_MINUS:      ir_kind = IRON_IR_SUB;  break;
            case IRON_TOK_STAR:       ir_kind = IRON_IR_MUL;  break;
            case IRON_TOK_SLASH:      ir_kind = IRON_IR_DIV;  break;
            case IRON_TOK_PERCENT:    ir_kind = IRON_IR_MOD;  break;
            case IRON_TOK_EQUALS:     ir_kind = IRON_IR_EQ;   break;
            case IRON_TOK_NOT_EQUALS: ir_kind = IRON_IR_NEQ;  break;
            case IRON_TOK_LESS:       ir_kind = IRON_IR_LT;   break;
            case IRON_TOK_LESS_EQ:    ir_kind = IRON_IR_LTE;  break;
            case IRON_TOK_GREATER:    ir_kind = IRON_IR_GT;   break;
            case IRON_TOK_GREATER_EQ: ir_kind = IRON_IR_GTE;  break;
            default:
                return emit_poison(ctx, bin->resolved_type, span);
        }

        IronIR_ValueId left_val  = lower_expr(ctx, bin->left);
        IronIR_ValueId right_val = lower_expr(ctx, bin->right);
        return iron_ir_binop(fn, ctx->current_block, ir_kind,
                             left_val, right_val, bin->resolved_type, span)->id;
    }

    /* ── Unary expressions (INSTR-03) ────────────────────────────────────── */

    case IRON_NODE_UNARY: {
        Iron_UnaryExpr *un = (Iron_UnaryExpr *)node;
        IronIR_InstrKind ir_kind;
        switch (un->op) {
            case IRON_TOK_MINUS: ir_kind = IRON_IR_NEG; break;
            case IRON_TOK_NOT:   ir_kind = IRON_IR_NOT; break;
            default:
                return emit_poison(ctx, un->resolved_type, span);
        }
        IronIR_ValueId operand_val = lower_expr(ctx, un->operand);
        return iron_ir_unop(fn, ctx->current_block, ir_kind,
                            operand_val, un->resolved_type, span)->id;
    }

    /* ── Field access (INSTR-05) ─────────────────────────────────────────── */

    case IRON_NODE_FIELD_ACCESS: {
        Iron_FieldAccess *fa = (Iron_FieldAccess *)node;
        IronIR_ValueId obj_val = lower_expr(ctx, fa->object);
        return iron_ir_get_field(fn, ctx->current_block, obj_val,
                                 fa->field, fa->resolved_type, span)->id;
    }

    /* ── Index access (INSTR-06) ─────────────────────────────────────────── */

    case IRON_NODE_INDEX: {
        Iron_IndexExpr *idx = (Iron_IndexExpr *)node;
        IronIR_ValueId arr_val = lower_expr(ctx, idx->object);
        IronIR_ValueId idx_val = lower_expr(ctx, idx->index);
        return iron_ir_get_index(fn, ctx->current_block, arr_val, idx_val,
                                 idx->resolved_type, span)->id;
    }

    /* ── Construct (INSTR-07) ────────────────────────────────────────────── */

    case IRON_NODE_CONSTRUCT: {
        Iron_ConstructExpr *con = (Iron_ConstructExpr *)node;
        IronIR_ValueId *field_vals = NULL;
        for (int i = 0; i < con->arg_count; i++) {
            IronIR_ValueId v = lower_expr(ctx, con->args[i]);
            arrput(field_vals, v);
        }
        IronIR_ValueId result = iron_ir_construct(fn, ctx->current_block,
                                                   con->resolved_type,
                                                   field_vals, con->arg_count,
                                                   span)->id;
        arrfree(field_vals);
        return result;
    }

    /* ── Array literal (INSTR-08) ────────────────────────────────────────── */

    case IRON_NODE_ARRAY_LIT: {
        Iron_ArrayLit *al = (Iron_ArrayLit *)node;
        IronIR_ValueId *elements = NULL;
        for (int i = 0; i < al->element_count; i++) {
            IronIR_ValueId v = lower_expr(ctx, al->elements[i]);
            arrput(elements, v);
        }
        /* Determine elem_type from resolved_type (array element type) */
        Iron_Type *elem_type = NULL;
        if (al->resolved_type && al->resolved_type->kind == IRON_TYPE_ARRAY) {
            elem_type = al->resolved_type->array.elem;
        }
        IronIR_ValueId result = iron_ir_array_lit(fn, ctx->current_block,
                                                   elem_type, elements, al->element_count,
                                                   al->resolved_type, span)->id;
        arrfree(elements);
        return result;
    }

    /* ── IS expression (INSTR-09) ────────────────────────────────────────── */

    case IRON_NODE_IS: {
        Iron_IsExpr *is = (Iron_IsExpr *)node;
        IronIR_ValueId val = lower_expr(ctx, is->expr);
        /* IS checks: if checking for nullability, emit is_null/is_not_null.
         * Otherwise emit a cast for type narrowing. */
        if (is->type_name && strcmp(is->type_name, "Null") == 0) {
            return iron_ir_is_null(fn, ctx->current_block, val, span)->id;
        }
        /* Type narrowing: emit cast to result type */
        return iron_ir_cast(fn, ctx->current_block, val,
                            is->resolved_type, span)->id;
    }

    /* ── Heap allocation (INSTR-09/MEM-01) ──────────────────────────────── */

    case IRON_NODE_HEAP: {
        Iron_HeapExpr *heap = (Iron_HeapExpr *)node;
        IronIR_ValueId inner_val = lower_expr(ctx, heap->inner);
        return iron_ir_heap_alloc(fn, ctx->current_block, inner_val,
                                  heap->auto_free, heap->escapes,
                                  heap->resolved_type, span)->id;
    }

    /* ── RC allocation (MEM-02) ──────────────────────────────────────────── */

    case IRON_NODE_RC: {
        Iron_RcExpr *rc = (Iron_RcExpr *)node;
        IronIR_ValueId inner_val = lower_expr(ctx, rc->inner);
        return iron_ir_rc_alloc(fn, ctx->current_block, inner_val,
                                rc->resolved_type, span)->id;
    }

    /* ── Function call (INSTR-13) ────────────────────────────────────────── */

    case IRON_NODE_CALL: {
        Iron_CallExpr *call = (Iron_CallExpr *)node;

        /* Direct call: callee is a simple identifier */
        if (call->callee && call->callee->kind == IRON_NODE_IDENT) {
            Iron_Ident *callee_id = (Iron_Ident *)call->callee;
            /* Emit func_ref for the callee name, then call via func_ptr */
            IronIR_ValueId func_ptr = iron_ir_func_ref(fn, ctx->current_block,
                                                        callee_id->name,
                                                        NULL, span)->id;
            /* Lower arguments */
            IronIR_ValueId *args = NULL;
            for (int i = 0; i < call->arg_count; i++) {
                IronIR_ValueId v = lower_expr(ctx, call->args[i]);
                arrput(args, v);
            }
            IronIR_ValueId result = iron_ir_call(fn, ctx->current_block,
                                                  NULL, func_ptr,
                                                  args, call->arg_count,
                                                  call->resolved_type, span)->id;
            arrfree(args);
            return result;
        }

        /* Indirect call: callee is an expression (lambda, method, fn pointer) */
        IronIR_ValueId func_ptr_val = lower_expr(ctx, call->callee);
        IronIR_ValueId *args = NULL;
        for (int i = 0; i < call->arg_count; i++) {
            IronIR_ValueId v = lower_expr(ctx, call->args[i]);
            arrput(args, v);
        }
        IronIR_ValueId result = iron_ir_call(fn, ctx->current_block,
                                              NULL, func_ptr_val,
                                              args, call->arg_count,
                                              call->resolved_type, span)->id;
        arrfree(args);
        return result;
    }

    /* ── Method call (INSTR-13) ──────────────────────────────────────────── */

    case IRON_NODE_METHOD_CALL: {
        Iron_MethodCallExpr *mc = (Iron_MethodCallExpr *)node;

        /* AUTO-STATIC CHECK: If the receiver is a type name (IRON_SYM_TYPE),
         * emit a direct C function call without self pointer.
         * Convention: <lowercase_type>_<method>(args...)
         * The emit_c.c mangle_func_name() will add the Iron_ prefix at emission.
         * This matches the stdlib C API: Iron_math_sin, Iron_io_read_file, etc. */
        if (mc->object->kind == IRON_NODE_IDENT) {
            Iron_Ident *obj_id = (Iron_Ident *)mc->object;
            if (obj_id->resolved_sym &&
                obj_id->resolved_sym->sym_kind == IRON_SYM_TYPE) {
                /* Build the function name: lowercase(TypeName)_methodName */
                const char *tname = obj_id->name;
                size_t tlen = strlen(tname);
                size_t mlen = strlen(mc->method);
                size_t total = tlen + 1 + mlen + 1;  /* type_method\0 */
                char *func_name = (char *)iron_arena_alloc(ctx->ir_arena, total,
                                                            _Alignof(char));
                /* Lowercase the type name */
                for (size_t k = 0; k < tlen; k++) {
                    func_name[k] = (char)tolower((unsigned char)tname[k]);
                }
                func_name[tlen] = '_';
                memcpy(func_name + tlen + 1, mc->method, mlen + 1);

                /* Emit func_ref for the auto-static function name */
                IronIR_ValueId func_ptr = iron_ir_func_ref(fn, ctx->current_block,
                                                            func_name, NULL, span)->id;

                /* Lower arguments (NO receiver for auto-static) */
                IronIR_ValueId *args = NULL;
                for (int i = 0; i < mc->arg_count; i++) {
                    IronIR_ValueId v = lower_expr(ctx, mc->args[i]);
                    arrput(args, v);
                }

                IronIR_ValueId result = iron_ir_call(fn, ctx->current_block,
                                                      NULL, func_ptr,
                                                      args, mc->arg_count,
                                                      mc->resolved_type, span)->id;
                arrfree(args);
                return result;
            }
        }

        /* Instance method call: receiver is an object instance */
        IronIR_ValueId obj_val = lower_expr(ctx, mc->object);

        /* Build args: receiver first, then explicit args */
        IronIR_ValueId *args = NULL;
        arrput(args, obj_val);
        for (int i = 0; i < mc->arg_count; i++) {
            IronIR_ValueId v = lower_expr(ctx, mc->args[i]);
            arrput(args, v);
        }
        int total_args = 1 + mc->arg_count;

        /* Emit method name as func_ref */
        IronIR_ValueId func_ptr = iron_ir_func_ref(fn, ctx->current_block,
                                                    mc->method, NULL, span)->id;
        IronIR_ValueId result = iron_ir_call(fn, ctx->current_block,
                                              NULL, func_ptr,
                                              args, total_args,
                                              mc->resolved_type, span)->id;
        arrfree(args);
        return result;
    }

    /* ── Interpolated string (INSTR-11) ──────────────────────────────────── */

    case IRON_NODE_INTERP_STRING: {
        Iron_InterpString *is = (Iron_InterpString *)node;
        IronIR_ValueId *parts = NULL;
        for (int i = 0; i < is->part_count; i++) {
            IronIR_ValueId v = lower_expr(ctx, is->parts[i]);
            arrput(parts, v);
        }
        IronIR_ValueId result = iron_ir_interp_string(fn, ctx->current_block,
                                                       parts, is->part_count,
                                                       is->resolved_type, span)->id;
        arrfree(parts);
        return result;
    }

    /* ── Slice expression (INSTR-12) ─────────────────────────────────────── */

    case IRON_NODE_SLICE: {
        Iron_SliceExpr *sl = (Iron_SliceExpr *)node;
        IronIR_ValueId arr_val   = lower_expr(ctx, sl->object);
        IronIR_ValueId start_val = IRON_IR_VALUE_INVALID;
        IronIR_ValueId end_val   = IRON_IR_VALUE_INVALID;
        if (sl->start) {
            start_val = lower_expr(ctx, sl->start);
        } else {
            /* Default start = 0 */
            Iron_Type *int_t = iron_type_make_primitive(IRON_TYPE_INT);
            start_val = iron_ir_const_int(fn, ctx->current_block, 0, int_t, span)->id;
        }
        if (sl->end) {
            end_val = lower_expr(ctx, sl->end);
        } else {
            /* Omitted end — use 0 as sentinel (emitter handles "to-end" slices) */
            Iron_Type *int_t = iron_type_make_primitive(IRON_TYPE_INT);
            end_val = iron_ir_const_int(fn, ctx->current_block, 0, int_t, span)->id;
        }
        return iron_ir_slice(fn, ctx->current_block, arr_val, start_val, end_val,
                             sl->resolved_type, span)->id;
    }

    /* ── Lambda (CONC-01 partial) ────────────────────────────────────────── */

    case IRON_NODE_LAMBDA: {
        Iron_LambdaExpr *lam = (Iron_LambdaExpr *)node;
        /* Assign lifted name using module counter */
        char name_buf[64];
        snprintf(name_buf, sizeof(name_buf), "__lambda_%d",
                 ctx->module->lambda_counter++);
        /* Allocate arena-persistent name */
        size_t name_len = strlen(name_buf) + 1;
        char *lifted_name = (char *)iron_arena_alloc(ctx->ir_arena, name_len, 1);
        memcpy(lifted_name, name_buf, name_len);

        /* Push LiftPending descriptor for post-pass */
        LiftPending lp;
        lp.kind = LIFT_LAMBDA;
        lp.ast_node = (Iron_Node *)lam;
        lp.lifted_name = lifted_name;
        lp.enclosing_func = ctx->current_func_name;
        arrput(ctx->pending_lifts, lp);

        /* Emit make_closure (captures filled during lift pass in Plan 08-03) */
        return iron_ir_make_closure(fn, ctx->current_block, lifted_name,
                                    NULL, 0, lam->resolved_type, span)->id;
    }

    /* ── Await (CONC-04 partial) ─────────────────────────────────────────── */

    case IRON_NODE_AWAIT: {
        Iron_AwaitExpr *aw = (Iron_AwaitExpr *)node;
        IronIR_ValueId handle_val = lower_expr(ctx, aw->handle);
        return iron_ir_await(fn, ctx->current_block, handle_val,
                             aw->resolved_type, span)->id;
    }

    /* ── Comptime ────────────────────────────────────────────────────────── */

    case IRON_NODE_COMPTIME: {
        /* Comptime is evaluated before IR lowering; lower the inner expression. */
        Iron_ComptimeExpr *ct = (Iron_ComptimeExpr *)node;
        if (ct->inner) {
            return lower_expr(ctx, ct->inner);
        }
        return emit_poison(ctx, ct->resolved_type, span);
    }

    /* ── Error node ──────────────────────────────────────────────────────── */

    case IRON_NODE_ERROR:
        return emit_poison(ctx, expr_type(node), span);

    /* ── Default: unsupported expression ─────────────────────────────────── */

    default: {
        char msg[256];
        snprintf(msg, sizeof(msg), "unsupported expression node kind %d during lowering",
                 (int)node->kind);
        iron_diag_emit(ctx->diags, ctx->ir_arena, IRON_DIAG_ERROR,
                       IRON_ERR_LOWER_UNSUPPORTED, span, msg,
                       "this expression type is not yet supported");
        return emit_poison(ctx, expr_type(node), span);
    }

    } /* end switch */
}
