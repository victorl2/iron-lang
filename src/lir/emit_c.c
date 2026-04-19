/* emit_c.c — IR-to-C emission backend for the Iron compiler.
 *
 * Implements iron_lir_emit_c():
 *   1. Include directives
 *   3. Forward declarations (typedef struct Iron_Foo Iron_Foo;)
 *   4. Struct bodies (topologically sorted) + interface vtables
 *   5. Enum definitions
 *   6. Global constants (none in IR path — handled during lowering)
 *   7. Function prototypes
 *   8. Lifted function bodies (lambda_, spawn_, parallel_)
 *   9. Function implementations
 *  10. main() wrapper
 */

#include "lir/emit_c.h"
#include "lir/emit_helpers.h"
#include "lir/emit_structs.h"
#include "lir/emit_split.h"
#include "lir/emit_fusion.h"
#include "lir/lir.h"
#include "lir/lir_optimize.h"
#include "lir/layout_analysis.h"
#include "lir/value_range.h"
#include "util/strbuf.h"
#include "util/arena.h"
#include "parser/ast.h"
#include "analyzer/types.h"
#include "analyzer/iface_collect.h"
#include "diagnostics/diagnostics.h"
#include "vendor/stb_ds.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <assert.h>

/* ── Stack-array tracking helpers ────────────────────────────────────────── */

/* Check if a ValueId is known to be a stack-represented array.
 * Returns the original ARRAY_LIT ValueId, or 0 if not a stack array. */
static IronLIR_ValueId get_stack_array_origin(EmitCtx *ctx, IronLIR_ValueId id) {
    if (!ctx->opt_info->stack_array_ids) return IRON_LIR_VALUE_INVALID;
    ptrdiff_t idx = hmgeti(ctx->opt_info->stack_array_ids, id);
    if (idx >= 0) return ctx->opt_info->stack_array_ids[idx].value;
    return IRON_LIR_VALUE_INVALID;
}

/* Register a ValueId as a stack array, with the given origin ARRAY_LIT id. */
static void mark_stack_array(EmitCtx *ctx, IronLIR_ValueId id,
                              IronLIR_ValueId origin) {
    hmput(ctx->opt_info->stack_array_ids, id, origin);
}

/* (resolve_stack_array_origin removed — pre-scan in emit_func_body handles propagation) */

/* Forward declaration -- emit_instr and emit_expr_to_buf are mutually recursive.
 * emit_expr_to_buf is non-static: also called from emit_fusion.c. */
void emit_expr_to_buf(Iron_StrBuf *sb, IronLIR_ValueId vid,
                       IronLIR_Func *fn, EmitCtx *ctx,
                       IronLIR_BlockId use_block_id, int depth);

/* ── Capture-field assignment helper ─────────────────────────────────────── */

/* Emit the RHS of a capture field assignment, handling the stack-array case.
 *
 * When a small array literal is captured, the LIR value is a stack-repr array
 * (elem_type _vN[] = {...}; int64_t _vN_len = N;).  The env/ctx struct field
 * is declared as Iron_List_elem_type (the full runtime list struct).  A plain
 * `_env->field = _vN;` is a type error in C.  Instead we emit:
 *     _env->field = (Iron_List_int64_t){ .items = _vN, .count = _vN_len };
 * For non-array captures the call degenerates to just emitting the value name.
 *
 * cap_type is the Iron_Type of the capture (from CaptureEntry.type); may be NULL.
 */
static void emit_capture_rhs(Iron_StrBuf *sb, IronLIR_ValueId cap_vid,
                              Iron_CaptureEntry *cap, EmitCtx *ctx) {
    /* Check if this is a stack-array value */
    IronLIR_ValueId sa_origin = get_stack_array_origin(ctx, cap_vid);
    if (sa_origin != IRON_LIR_VALUE_INVALID &&
        cap->type && cap->type->kind == IRON_TYPE_ARRAY) {
        /* Build the Iron_List struct type name, e.g. Iron_List_int64_t */
        Iron_Type *arr_type = cap->type; /* already [T] */
        const char *list_type = emit_type_to_c(arr_type, ctx);
        /* Emit compound-literal: (Iron_List_T){ .items = _vN, .count = _vN_len } */
        iron_strbuf_appendf(sb, "(%s){ .items = ", list_type);
        emit_val(sb, cap_vid);
        iron_strbuf_appendf(sb, ", .count = ");
        emit_val(sb, cap_vid);
        iron_strbuf_appendf(sb, "_len }");
    } else {
        emit_val(sb, cap_vid);
    }
}

/* ── Expression inlining recursive helper ─────────────────────────────────── */

/* Recursively build a C expression string for vid.
 * If vid is inline-eligible (single-use pure, same block), reconstructs the
 * producing instruction as a sub-expression. Otherwise emits `_vN`.
 * Always parenthesizes compound expressions for safety. */
void emit_expr_to_buf(Iron_StrBuf *sb, IronLIR_ValueId vid,
                       IronLIR_Func *fn, EmitCtx *ctx,
                       IronLIR_BlockId use_block_id, int depth) {
    if (vid == IRON_LIR_VALUE_INVALID) {
        iron_strbuf_appendf(sb, "_v_invalid");
        return;
    }

    /* Step 1: Check inline eligibility */
    if (!ctx->inline_eligible || hmgeti(ctx->inline_eligible, vid) < 0) {
        emit_val(sb, vid);
        return;
    }

    /* Step 2: Block-boundary check — prefer inlining within same block.
     * However, if the value IS inline-eligible (its declaration was suppressed),
     * we MUST inline it regardless of block boundary, since emit_val would
     * reference an undeclared variable. This is safe because inline-eligible
     * values have exactly one use and passed ordering-hazard checks. */
    if (ctx->value_block) {
        ptrdiff_t vb_idx = hmgeti(ctx->value_block, vid);
        if (vb_idx < 0 || (IronLIR_BlockId)ctx->value_block[vb_idx].value != use_block_id) {
            /* Only bail out if the value has a declaration (not inline-eligible) */
            if (!ctx->inline_eligible || hmgeti(ctx->inline_eligible, vid) < 0) {
                emit_val(sb, vid);
                return;
            }
            /* Otherwise: value is inline-eligible with no declaration — must inline */
        }
    }

    /* Step 3: Stack-array exclusion */
    if (ctx->opt_info->stack_array_ids &&
        hmgeti(ctx->opt_info->stack_array_ids, vid) >= 0) {
        emit_val(sb, vid);
        return;
    }

    /* Step 4: Look up the producing instruction */
    if (vid >= (IronLIR_ValueId)arrlen(fn->value_table) || fn->value_table[vid] == NULL) {
        emit_val(sb, vid);
        return;
    }
    IronLIR_Instr *instr = fn->value_table[vid];

    /* Step 4b: Never inline CALL — emit_expr_to_buf doesn't handle array
     * parameter splitting or other call-specific emit logic. */
    if (instr->kind == IRON_LIR_CALL) {
        emit_val(sb, vid);
        return;
    }

    /* Step 5: Deep-expression anchor comment */
    if (depth > 3) {
        iron_strbuf_appendf(sb, "/* _v%u */ ", vid);
    }

    /* Step 6: Build expression based on instruction kind */
    switch ((int)(instr->kind)) {

    /* Binary ops */
    case IRON_LIR_ADD:
        iron_strbuf_appendf(sb, "(");
        emit_expr_to_buf(sb, instr->binop.left,  fn, ctx, use_block_id, depth+1);
        iron_strbuf_appendf(sb, " + ");
        emit_expr_to_buf(sb, instr->binop.right, fn, ctx, use_block_id, depth+1);
        iron_strbuf_appendf(sb, ")");
        break;
    case IRON_LIR_SUB:
        iron_strbuf_appendf(sb, "(");
        emit_expr_to_buf(sb, instr->binop.left,  fn, ctx, use_block_id, depth+1);
        iron_strbuf_appendf(sb, " - ");
        emit_expr_to_buf(sb, instr->binop.right, fn, ctx, use_block_id, depth+1);
        iron_strbuf_appendf(sb, ")");
        break;
    case IRON_LIR_MUL:
        iron_strbuf_appendf(sb, "(");
        emit_expr_to_buf(sb, instr->binop.left,  fn, ctx, use_block_id, depth+1);
        iron_strbuf_appendf(sb, " * ");
        emit_expr_to_buf(sb, instr->binop.right, fn, ctx, use_block_id, depth+1);
        iron_strbuf_appendf(sb, ")");
        break;
    case IRON_LIR_DIV:
        iron_strbuf_appendf(sb, "(");
        emit_expr_to_buf(sb, instr->binop.left,  fn, ctx, use_block_id, depth+1);
        iron_strbuf_appendf(sb, " / ");
        emit_expr_to_buf(sb, instr->binop.right, fn, ctx, use_block_id, depth+1);
        iron_strbuf_appendf(sb, ")");
        break;
    case IRON_LIR_MOD:
        iron_strbuf_appendf(sb, "(");
        emit_expr_to_buf(sb, instr->binop.left,  fn, ctx, use_block_id, depth+1);
        iron_strbuf_appendf(sb, " %% ");
        emit_expr_to_buf(sb, instr->binop.right, fn, ctx, use_block_id, depth+1);
        iron_strbuf_appendf(sb, ")");
        break;
    case IRON_LIR_EQ: {
        /* Phase 59 01d: tuple equality — element-wise && of primitive
         * comparisons or iron_string_equals() for string elements. The
         * typechecker has already guaranteed matching arity + types.
         * Phase 59 P05: also handle plain `String == String` via
         * iron_string_equals() rather than the broken pointer-compare
         * `==` that the previous fall-through emitted. */
        Iron_Type *lty = emit_get_value_type(fn, instr->binop.left);
        if (lty && lty->kind == IRON_TYPE_TUPLE) {
            if (depth > 0) iron_strbuf_appendf(sb, "(");
            iron_strbuf_appendf(sb, "(");
            int nc = lty->tuple.elem_count;
            for (int i = 0; i < nc; i++) {
                if (i > 0) iron_strbuf_appendf(sb, " && ");
                const Iron_Type *et = lty->tuple.elem_types[i];
                bool is_string = et && et->kind == IRON_TYPE_STRING;
                if (is_string) {
                    iron_strbuf_appendf(sb, "iron_string_equals(&(");
                    emit_expr_to_buf(sb, instr->binop.left, fn, ctx, use_block_id, depth+1);
                    iron_strbuf_appendf(sb, ").v%d, &(", i);
                    emit_expr_to_buf(sb, instr->binop.right, fn, ctx, use_block_id, depth+1);
                    iron_strbuf_appendf(sb, ").v%d)", i);
                } else {
                    iron_strbuf_appendf(sb, "(");
                    emit_expr_to_buf(sb, instr->binop.left, fn, ctx, use_block_id, depth+1);
                    iron_strbuf_appendf(sb, ").v%d == (", i);
                    emit_expr_to_buf(sb, instr->binop.right, fn, ctx, use_block_id, depth+1);
                    iron_strbuf_appendf(sb, ").v%d", i);
                }
            }
            iron_strbuf_appendf(sb, ")");
            if (depth > 0) iron_strbuf_appendf(sb, ")");
            break;
        }
        if (lty && lty->kind == IRON_TYPE_STRING) {
            iron_strbuf_appendf(sb, "iron_string_equals(&(");
            emit_expr_to_buf(sb, instr->binop.left,  fn, ctx, use_block_id, depth+1);
            iron_strbuf_appendf(sb, "), &(");
            emit_expr_to_buf(sb, instr->binop.right, fn, ctx, use_block_id, depth+1);
            iron_strbuf_appendf(sb, "))");
            break;
        }
        if (depth > 0) iron_strbuf_appendf(sb, "(");
        emit_expr_to_buf(sb, instr->binop.left,  fn, ctx, use_block_id, depth+1);
        iron_strbuf_appendf(sb, " == ");
        emit_expr_to_buf(sb, instr->binop.right, fn, ctx, use_block_id, depth+1);
        if (depth > 0) iron_strbuf_appendf(sb, ")");
        break;
    }
    case IRON_LIR_NEQ: {
        /* Phase 59 01d: tuple inequality — !(elementwise ==).
         * Phase 59 P05: handle plain String != String via iron_string_equals. */
        Iron_Type *lty = emit_get_value_type(fn, instr->binop.left);
        if (lty && lty->kind == IRON_TYPE_TUPLE) {
            if (depth > 0) iron_strbuf_appendf(sb, "(");
            iron_strbuf_appendf(sb, "!(");
            int nc = lty->tuple.elem_count;
            for (int i = 0; i < nc; i++) {
                if (i > 0) iron_strbuf_appendf(sb, " && ");
                const Iron_Type *et = lty->tuple.elem_types[i];
                bool is_string = et && et->kind == IRON_TYPE_STRING;
                if (is_string) {
                    iron_strbuf_appendf(sb, "iron_string_equals(&(");
                    emit_expr_to_buf(sb, instr->binop.left, fn, ctx, use_block_id, depth+1);
                    iron_strbuf_appendf(sb, ").v%d, &(", i);
                    emit_expr_to_buf(sb, instr->binop.right, fn, ctx, use_block_id, depth+1);
                    iron_strbuf_appendf(sb, ").v%d)", i);
                } else {
                    iron_strbuf_appendf(sb, "(");
                    emit_expr_to_buf(sb, instr->binop.left, fn, ctx, use_block_id, depth+1);
                    iron_strbuf_appendf(sb, ").v%d == (", i);
                    emit_expr_to_buf(sb, instr->binop.right, fn, ctx, use_block_id, depth+1);
                    iron_strbuf_appendf(sb, ").v%d", i);
                }
            }
            iron_strbuf_appendf(sb, ")");
            if (depth > 0) iron_strbuf_appendf(sb, ")");
            break;
        }
        if (lty && lty->kind == IRON_TYPE_STRING) {
            iron_strbuf_appendf(sb, "(!iron_string_equals(&(");
            emit_expr_to_buf(sb, instr->binop.left,  fn, ctx, use_block_id, depth+1);
            iron_strbuf_appendf(sb, "), &(");
            emit_expr_to_buf(sb, instr->binop.right, fn, ctx, use_block_id, depth+1);
            iron_strbuf_appendf(sb, ")))");
            break;
        }
        if (depth > 0) iron_strbuf_appendf(sb, "(");
        emit_expr_to_buf(sb, instr->binop.left,  fn, ctx, use_block_id, depth+1);
        iron_strbuf_appendf(sb, " != ");
        emit_expr_to_buf(sb, instr->binop.right, fn, ctx, use_block_id, depth+1);
        if (depth > 0) iron_strbuf_appendf(sb, ")");
        break;
    }
    case IRON_LIR_LT:
        if (depth > 0) iron_strbuf_appendf(sb, "(");
        emit_expr_to_buf(sb, instr->binop.left,  fn, ctx, use_block_id, depth+1);
        iron_strbuf_appendf(sb, " < ");
        emit_expr_to_buf(sb, instr->binop.right, fn, ctx, use_block_id, depth+1);
        if (depth > 0) iron_strbuf_appendf(sb, ")");
        break;
    case IRON_LIR_LTE:
        if (depth > 0) iron_strbuf_appendf(sb, "(");
        emit_expr_to_buf(sb, instr->binop.left,  fn, ctx, use_block_id, depth+1);
        iron_strbuf_appendf(sb, " <= ");
        emit_expr_to_buf(sb, instr->binop.right, fn, ctx, use_block_id, depth+1);
        if (depth > 0) iron_strbuf_appendf(sb, ")");
        break;
    case IRON_LIR_GT:
        if (depth > 0) iron_strbuf_appendf(sb, "(");
        emit_expr_to_buf(sb, instr->binop.left,  fn, ctx, use_block_id, depth+1);
        iron_strbuf_appendf(sb, " > ");
        emit_expr_to_buf(sb, instr->binop.right, fn, ctx, use_block_id, depth+1);
        if (depth > 0) iron_strbuf_appendf(sb, ")");
        break;
    case IRON_LIR_GTE:
        if (depth > 0) iron_strbuf_appendf(sb, "(");
        emit_expr_to_buf(sb, instr->binop.left,  fn, ctx, use_block_id, depth+1);
        iron_strbuf_appendf(sb, " >= ");
        emit_expr_to_buf(sb, instr->binop.right, fn, ctx, use_block_id, depth+1);
        if (depth > 0) iron_strbuf_appendf(sb, ")");
        break;
    case IRON_LIR_AND:
        iron_strbuf_appendf(sb, "(");
        emit_expr_to_buf(sb, instr->binop.left,  fn, ctx, use_block_id, depth+1);
        iron_strbuf_appendf(sb, " && ");
        emit_expr_to_buf(sb, instr->binop.right, fn, ctx, use_block_id, depth+1);
        iron_strbuf_appendf(sb, ")");
        break;
    case IRON_LIR_OR:
        iron_strbuf_appendf(sb, "(");
        emit_expr_to_buf(sb, instr->binop.left,  fn, ctx, use_block_id, depth+1);
        iron_strbuf_appendf(sb, " || ");
        emit_expr_to_buf(sb, instr->binop.right, fn, ctx, use_block_id, depth+1);
        iron_strbuf_appendf(sb, ")");
        break;

    /* Bitwise binary ops */
    case IRON_LIR_SHL:
        iron_strbuf_appendf(sb, "(");
        emit_expr_to_buf(sb, instr->binop.left,  fn, ctx, use_block_id, depth+1);
        iron_strbuf_appendf(sb, " << ");
        emit_expr_to_buf(sb, instr->binop.right, fn, ctx, use_block_id, depth+1);
        iron_strbuf_appendf(sb, ")");
        break;
    case IRON_LIR_SHR:
        iron_strbuf_appendf(sb, "(");
        emit_expr_to_buf(sb, instr->binop.left,  fn, ctx, use_block_id, depth+1);
        iron_strbuf_appendf(sb, " >> ");
        emit_expr_to_buf(sb, instr->binop.right, fn, ctx, use_block_id, depth+1);
        iron_strbuf_appendf(sb, ")");
        break;
    case IRON_LIR_BAND:
        iron_strbuf_appendf(sb, "(");
        emit_expr_to_buf(sb, instr->binop.left,  fn, ctx, use_block_id, depth+1);
        iron_strbuf_appendf(sb, " & ");
        emit_expr_to_buf(sb, instr->binop.right, fn, ctx, use_block_id, depth+1);
        iron_strbuf_appendf(sb, ")");
        break;
    case IRON_LIR_BOR:
        iron_strbuf_appendf(sb, "(");
        emit_expr_to_buf(sb, instr->binop.left,  fn, ctx, use_block_id, depth+1);
        iron_strbuf_appendf(sb, " | ");
        emit_expr_to_buf(sb, instr->binop.right, fn, ctx, use_block_id, depth+1);
        iron_strbuf_appendf(sb, ")");
        break;
    case IRON_LIR_BXOR:
        iron_strbuf_appendf(sb, "(");
        emit_expr_to_buf(sb, instr->binop.left,  fn, ctx, use_block_id, depth+1);
        iron_strbuf_appendf(sb, " ^ ");
        emit_expr_to_buf(sb, instr->binop.right, fn, ctx, use_block_id, depth+1);
        iron_strbuf_appendf(sb, ")");
        break;

    /* Unary ops */
    case IRON_LIR_NEG:
        iron_strbuf_appendf(sb, "(-");
        emit_expr_to_buf(sb, instr->unop.operand, fn, ctx, use_block_id, depth+1);
        iron_strbuf_appendf(sb, ")");
        break;
    case IRON_LIR_NOT:
        iron_strbuf_appendf(sb, "(!");
        emit_expr_to_buf(sb, instr->unop.operand, fn, ctx, use_block_id, depth+1);
        iron_strbuf_appendf(sb, ")");
        break;
    case IRON_LIR_BNOT:
        iron_strbuf_appendf(sb, "(~");
        emit_expr_to_buf(sb, instr->unop.operand, fn, ctx, use_block_id, depth+1);
        iron_strbuf_appendf(sb, ")");
        break;

    /* Constants */
    case IRON_LIR_CONST_INT:
        iron_strbuf_appendf(sb, "((%s)%lldLL)",
                            emit_type_to_c(instr->type, ctx),
                            (long long)instr->const_int.value);
        break;
    case IRON_LIR_CONST_FLOAT:
        iron_strbuf_appendf(sb, "%g", instr->const_float.value);
        break;
    case IRON_LIR_CONST_BOOL:
        iron_strbuf_appendf(sb, "%s", instr->const_bool.value ? "true" : "false");
        break;
    case IRON_LIR_CONST_NULL:
        iron_strbuf_appendf(sb, "NULL");
        break;

    /* LOAD: pass through to the stored value (alloca variable) */
    case IRON_LIR_LOAD: {
        /* Check for param alias — inline as the param value */
        if (ctx->param_alias_ids) {
            ptrdiff_t pa_idx = hmgeti(ctx->param_alias_ids, instr->load.ptr);
            if (pa_idx >= 0) {
                emit_val(sb, ctx->param_alias_ids[pa_idx].value);
                break;
            }
        }
        /* Stack array load: can't inline as expression, fall back */
        if (get_stack_array_origin(ctx, instr->load.ptr) != IRON_LIR_VALUE_INVALID) {
            emit_val(sb, vid);
            break;
        }
        /* Interface alloca load: emit the LOAD result variable (not the alloca),
         * because the STORE applies wrapping that must be preserved */
        {
            IronLIR_Instr *ptr_instr2 = (instr->load.ptr < (IronLIR_ValueId)arrlen(fn->value_table))
                                       ? fn->value_table[instr->load.ptr] : NULL;
            if (ptr_instr2 && ptr_instr2->type &&
                ptr_instr2->type->kind == IRON_TYPE_INTERFACE) {
                emit_val(sb, vid);
                break;
            }
        }
        /* Regular load: inline as the alloca variable directly */
        emit_expr_to_buf(sb, instr->load.ptr, fn, ctx, use_block_id, depth+1);
        break;
    }

    /* CAST */
    case IRON_LIR_CAST: {
        const char *src_t = emit_type_to_c(instr->type, ctx);  /* type of input is cast.value's type */
        const char *dst_t = emit_type_to_c(instr->cast.target_type, ctx);
        (void)src_t; /* comment just uses type names — keep dst */
        iron_strbuf_appendf(sb, "(/* cast */ (%s)", dst_t);
        emit_expr_to_buf(sb, instr->cast.value, fn, ctx, use_block_id, depth+1);
        iron_strbuf_appendf(sb, ")");
        break;
    }

    /* GET_FIELD */
    case IRON_LIR_GET_FIELD: {
        /* Stack array .count is a special case — emits as _vN_len */
        if (get_stack_array_origin(ctx, instr->field.object) != IRON_LIR_VALUE_INVALID &&
            instr->field.field && strcmp(instr->field.field, "count") == 0) {
            emit_val(sb, instr->field.object);
            iron_strbuf_appendf(sb, "_len");
            break;
        }
        /* Type-name namespace access: Math.PI, Log.DEBUG, etc.
         * The object is a FUNC_REF (type name), not a runtime instance.
         * Emit Iron_TypeName_FieldName so the #define in the module header
         * resolves to the correct compile-time constant. */
        if (emit_val_is_type_ref(fn, instr->field.object)) {
            IronLIR_Instr *ref = fn->value_table[instr->field.object];
            const char *type_c = emit_resolve_func_c_name(ctx, ref->func_ref.func_name);
            iron_strbuf_appendf(sb, "%s_%s", type_c, instr->field.field);
            break;
        }
        bool obj_is_ptr = emit_val_is_heap_ptr(fn, instr->field.object);
        emit_expr_to_buf(sb, instr->field.object, fn, ctx, use_block_id, depth+1);
        iron_strbuf_appendf(sb, "%s%s", obj_is_ptr ? "->" : ".", instr->field.field);
        break;
    }

    /* GET_INDEX */
    case IRON_LIR_GET_INDEX: {
        /* Split collection GET_INDEX can't be inlined (needs switch statement) */
        if (ctx->split_collection_ids &&
            hmgeti(ctx->split_collection_ids, instr->index.array) >= 0) {
            emit_val(sb, vid);
            break;
        }
        IronLIR_ValueId sa_origin = get_stack_array_origin(ctx, instr->index.array);
        if (sa_origin != IRON_LIR_VALUE_INVALID) {
            emit_expr_to_buf(sb, instr->index.array, fn, ctx, use_block_id, depth+1);
            iron_strbuf_appendf(sb, "[");
            emit_expr_to_buf(sb, instr->index.index, fn, ctx, use_block_id, depth+1);
            iron_strbuf_appendf(sb, "]");
        } else {
            /* Check direct array type — also handles parameter values (NULL in value_table) */
            Iron_Type *arr_t_expr = emit_get_value_type(fn, instr->index.array);
            bool use_direct = (arr_t_expr && arr_t_expr->kind == IRON_TYPE_ARRAY);
            if (use_direct) {
                emit_expr_to_buf(sb, instr->index.array, fn, ctx, use_block_id, depth+1);
                iron_strbuf_appendf(sb, ".items[");
                emit_expr_to_buf(sb, instr->index.index, fn, ctx, use_block_id, depth+1);
                iron_strbuf_appendf(sb, "]");
            } else {
                /* Fall back — too complex to inline */
                emit_val(sb, vid);
            }
        }
        break;
    }

    /* CONSTRUCT */
    case IRON_LIR_CONSTRUCT: {
        const char *c_type = emit_type_to_c(instr->construct.type, ctx);
        /* Type name for comment */
        const char *type_name = c_type;
        if (instr->construct.type && instr->construct.type->kind == IRON_TYPE_OBJECT &&
            instr->construct.type->object.decl) {
            type_name = instr->construct.type->object.decl->name;
        }
        iron_strbuf_appendf(sb, "/* %s */ (%s){", type_name, c_type);
        /* ADT enum with payloads */
        if (instr->construct.type &&
            instr->construct.type->kind == IRON_TYPE_ENUM &&
            instr->construct.type->enu.decl &&
            instr->construct.type->enu.decl->has_payloads) {
            Iron_EnumDecl *adt_ed = instr->construct.type->enu.decl;
            const char *adt_mangled = instr->construct.type->enu.mangled_name
                ? instr->construct.type->enu.mangled_name
                : emit_mangle_name(adt_ed->name, ctx->arena);
            int variant_idx = 0;
            if (instr->construct.field_count > 0) {
                IronLIR_ValueId tag_vid = instr->construct.field_vals[0];
                if (tag_vid < (IronLIR_ValueId)arrlen(fn->value_table)) {
                    IronLIR_Instr *tag_instr = fn->value_table[tag_vid];
                    if (tag_instr && tag_instr->kind == IRON_LIR_CONST_INT) {
                        variant_idx = (int)tag_instr->const_int.value;
                    }
                }
            }
            if (variant_idx < 0 || variant_idx >= adt_ed->variant_count) variant_idx = 0;
            /* PROT-03 row 25 (AUDIT-01 M-severity): Iron_EnumVariant is a
             * sub-struct stored in a void** array. The loop bound
             * variant_count and the parser's allocation protocol guarantee
             * every entry is a non-NULL Iron_EnumVariant; the explicit
             * runtime asserts catch any future regression that violates the
             * invariant in Debug builds. */
            assert(variant_idx >= 0 && variant_idx < adt_ed->variant_count);
            assert(adt_ed->variants[variant_idx] != NULL);
            Iron_EnumVariant *adt_ev = (Iron_EnumVariant *)adt_ed->variants[variant_idx];
            iron_strbuf_appendf(sb, " .tag = %s_TAG_%s", adt_mangled, adt_ev->name);
            int payload_count = instr->construct.field_count - 1;
            if (payload_count > 0 && adt_ev->payload_count > 0) {
                iron_strbuf_appendf(sb, ", .data.%s = {", adt_ev->name);
                for (int pi = 0; pi < payload_count; pi++) {
                    if (pi > 0) iron_strbuf_appendf(sb, ", ");
                    else iron_strbuf_appendf(sb, " ");
                    iron_strbuf_appendf(sb, "._%d = ", pi);
                    emit_expr_to_buf(sb, instr->construct.field_vals[1 + pi], fn, ctx, use_block_id, depth+1);
                }
                iron_strbuf_appendf(sb, " }");
            }
        } else if (instr->construct.type &&
            instr->construct.type->kind == IRON_TYPE_OBJECT &&
            instr->construct.type->object.decl) {
            Iron_ObjectDecl *od = instr->construct.type->object.decl;
            int field_start = 0;
            if (od->extends_name) {
                if (instr->construct.field_count > 0) {
                    iron_strbuf_appendf(sb, " ._base = ");
                    emit_expr_to_buf(sb, instr->construct.field_vals[0], fn, ctx, use_block_id, depth+1);
                    field_start = 1;
                    if (instr->construct.field_count > 1) iron_strbuf_appendf(sb, ",");
                }
            }
            int od_field_idx = 0;
            int effective_field_count = instr->construct.field_count;
            if (effective_field_count > od->field_count + field_start) {
                effective_field_count = od->field_count + field_start;
            }
            for (int i = field_start; i < effective_field_count; i++) {
                if (i > field_start) iron_strbuf_appendf(sb, ",");
                if (od_field_idx < od->field_count) {
                    Iron_Field *f = (Iron_Field *)od->fields[od_field_idx++];
                    iron_strbuf_appendf(sb, " .%s = ", f->name);
                } else {
                    iron_strbuf_appendf(sb, " ");
                }
                emit_expr_to_buf(sb, instr->construct.field_vals[i], fn, ctx, use_block_id, depth+1);
            }
        /* Phase 59 01d: tuple construct — designated init with v0/v1/... */
        } else if (instr->construct.type &&
            instr->construct.type->kind == IRON_TYPE_TUPLE) {
            for (int i = 0; i < instr->construct.field_count; i++) {
                if (i > 0) iron_strbuf_appendf(sb, ",");
                iron_strbuf_appendf(sb, " .v%d = ", i);
                emit_expr_to_buf(sb, instr->construct.field_vals[i], fn, ctx, use_block_id, depth+1);
            }
        } else {
            for (int i = 0; i < instr->construct.field_count; i++) {
                if (i > 0) iron_strbuf_appendf(sb, ", ");
                else iron_strbuf_appendf(sb, " ");
                emit_expr_to_buf(sb, instr->construct.field_vals[i], fn, ctx, use_block_id, depth+1);
            }
        }
        iron_strbuf_appendf(sb, " }");
        break;
    }

    /* CALL (pure calls only — must be in inline_eligible to reach here) */
    case IRON_LIR_CALL: {
        /* Get callee name for comment */
        const char *callee_c = NULL;
        const char *callee_short = "call";
        if (instr->call.func_decl) {
            if (instr->call.func_decl->is_extern && instr->call.func_decl->extern_c_name) {
                callee_c = instr->call.func_decl->extern_c_name;
            } else {
                callee_c = emit_mangle_func_name(instr->call.func_decl->name, ctx->arena);
            }
            callee_short = instr->call.func_decl->name;
        } else {
            IronLIR_ValueId fptr = instr->call.func_ptr;
            if (fptr != IRON_LIR_VALUE_INVALID &&
                fptr < (IronLIR_ValueId)arrlen(fn->value_table) &&
                fn->value_table[fptr] != NULL &&
                fn->value_table[fptr]->kind == IRON_LIR_FUNC_REF) {
                const char *ir_name = fn->value_table[fptr]->func_ref.func_name;
                callee_c = emit_resolve_func_c_name(ctx, ir_name);
                callee_short = ir_name;
            }
        }
        if (!callee_c) {
            /* Can't inline indirect call with unknown target */
            emit_val(sb, vid);
            break;
        }
        iron_strbuf_appendf(sb, "/* %s */ %s(", callee_short, callee_c);
        for (int i = 0; i < instr->call.arg_count; i++) {
            if (i > 0) iron_strbuf_appendf(sb, ", ");
            /* Interface wrapping: if arg is concrete but callee is interface dispatch */
            IronLIR_ValueId arg_id = instr->call.args[i];
            Iron_Type *arg_type = (arg_id < (IronLIR_ValueId)arrlen(fn->value_table) &&
                                   fn->value_table[arg_id])
                                  ? fn->value_table[arg_id]->type : NULL;
            bool did_wrap = false;
            if (arg_type && arg_type->kind == IRON_TYPE_OBJECT &&
                arg_type->object.decl && ctx->iface_reg) {
                /* Check if callee is an interface dispatch function */
                for (int ri = 0; ri < (int)shlen(ctx->iface_reg->map); ri++) {
                    Iron_IfaceEntry *ent = &ctx->iface_reg->map[ri].value;
                    const char *im = emit_mangle_name(ent->iface_name, ctx->arena);
                    size_t imlen = strlen(im);
                    if (callee_c && strncmp(callee_c, im, imlen) == 0 &&
                        callee_c[imlen] == '_') {
                        /* This is an interface dispatch call — wrap the arg */
                        iron_strbuf_appendf(sb, "%s_from_%s(",
                            im, arg_type->object.decl->name);
                        emit_expr_to_buf(sb, arg_id, fn, ctx, use_block_id, depth+1);
                        iron_strbuf_appendf(sb, ")");
                        did_wrap = true;
                        break;
                    }
                }
            }
            if (!did_wrap) {
                emit_expr_to_buf(sb, arg_id, fn, ctx, use_block_id, depth+1);
            }
        }
        iron_strbuf_appendf(sb, ")");
        break;
    }

    /* IS_NULL / IS_NOT_NULL */
    case IRON_LIR_IS_NULL:
        iron_strbuf_appendf(sb, "(!");
        emit_expr_to_buf(sb, instr->null_check.value, fn, ctx, use_block_id, depth+1);
        iron_strbuf_appendf(sb, ".has_value)");
        break;
    case IRON_LIR_IS_NOT_NULL:
        iron_strbuf_appendf(sb, "(");
        emit_expr_to_buf(sb, instr->null_check.value, fn, ctx, use_block_id, depth+1);
        iron_strbuf_appendf(sb, ".has_value)");
        break;

    /* FUNC_REF */
    case IRON_LIR_FUNC_REF:
        iron_strbuf_appendf(sb, "%s",
                            emit_resolve_func_c_name(ctx, instr->func_ref.func_name));
        break;

    /* MAKE_CLOSURE / SLICE — complex multi-statement patterns, fall back */
    case IRON_LIR_MAKE_CLOSURE:
    case IRON_LIR_SLICE:
    /* -Wswitch-enum opt-out: expression-form emitter handles the operator
     * subset that can appear in a rvalue context; instruction kinds that
     * materialize via a named temporary (terminators, allocas, stores,
     * etc.) fall back to printing the value id, which the caller already
     * emitted as a separate statement earlier in emit_instr. */
    default:
        emit_val(sb, vid);
        break;
    }
}

/* emit_fused_chain moved to emit_fusion.c (Phase 52, Plan 03) */


void emit_instr(Iron_StrBuf *sb, IronLIR_Instr *instr,
                IronLIR_Func *fn, EmitCtx *ctx) {
    int ind = ctx->indent;

    /* Expression inlining: skip emission of inlineable values — they will be
     * reconstructed inline at their single use site by emit_expr_to_buf().
     * Exception: CALL instructions are never inlined (Step 4b prevents it),
     * so their declarations must not be suppressed. */
    if (instr->id != IRON_LIR_VALUE_INVALID &&
        ctx->inline_eligible &&
        hmgeti(ctx->inline_eligible, instr->id) >= 0 &&
        instr->kind != IRON_LIR_CALL) {
        return;  /* deferred to use site */
    }

    /* For backward-referenced values (hoisted to entry), emit as assignment
     * without the type prefix to avoid C redefinition errors. */
    bool is_hoisted = (ctx->phi_hoisted &&
                       hmgeti(ctx->phi_hoisted, instr->id) >= 0);

    switch (instr->kind) {

    /* ── Constants ──────────────────────────────────────────────────────── */

    case IRON_LIR_CONST_INT:
        emit_indent(sb, ind);
        if (!is_hoisted) iron_strbuf_appendf(sb, "%s ", emit_type_to_c(instr->type, ctx));
        emit_val(sb, instr->id);
        iron_strbuf_appendf(sb, " = (%s)%lldLL;\n",
                            emit_type_to_c(instr->type, ctx),
                            (long long)instr->const_int.value);
        break;

    case IRON_LIR_CONST_FLOAT:
        emit_indent(sb, ind);
        if (!is_hoisted) iron_strbuf_appendf(sb, "%s ", emit_type_to_c(instr->type, ctx));
        emit_val(sb, instr->id);
        iron_strbuf_appendf(sb, " = %g;\n", instr->const_float.value);
        break;

    case IRON_LIR_CONST_BOOL:
        emit_indent(sb, ind);
        if (!is_hoisted) iron_strbuf_appendf(sb, "bool ");
        emit_val(sb, instr->id);
        iron_strbuf_appendf(sb, " = %s;\n",
                            instr->const_bool.value ? "true" : "false");
        break;

    case IRON_LIR_CONST_STRING: {
        const char *sv = instr->const_str.value ? instr->const_str.value : "";
        size_t slen = strlen(sv);
        emit_indent(sb, ind);
        if (!is_hoisted) iron_strbuf_appendf(sb, "Iron_String ");
        emit_val(sb, instr->id);
        /* Escape the literal for C source embedding. Iron's lexer decodes
         * `\n` / `\t` / `\"` / `\\` into the raw byte, so `sv` may contain
         * newlines, quotes, backslashes, and arbitrary control characters
         * (e.g. inline GLSL with embedded newlines, D6 residual). Emitting
         * with `%s` broke on the first newline — the C compiler can't
         * parse a raw LF inside a string literal. Emit each byte through
         * a minimal C-escape table. */
        iron_strbuf_appendf(sb, " = iron_string_from_literal(\"");
        for (size_t i = 0; i < slen; i++) {
            unsigned char ch = (unsigned char)sv[i];
            switch (ch) {
                case '\n': iron_strbuf_appendf(sb, "\\n");  break;
                case '\r': iron_strbuf_appendf(sb, "\\r");  break;
                case '\t': iron_strbuf_appendf(sb, "\\t");  break;
                case '\\': iron_strbuf_appendf(sb, "\\\\"); break;
                case '"':  iron_strbuf_appendf(sb, "\\\""); break;
                default:
                    if (ch < 0x20 || ch == 0x7f) {
                        iron_strbuf_appendf(sb, "\\x%02x", ch);
                    } else {
                        iron_strbuf_appendf(sb, "%c", (int)ch);
                    }
                    break;
            }
        }
        iron_strbuf_appendf(sb, "\", %zu);\n", slen);
        break;
    }

    case IRON_LIR_CONST_NULL:
        emit_indent(sb, ind);
        if (instr->type && instr->type->kind == IRON_TYPE_OBJECT) {
            /* Zero-initialize struct so it can be used in PHI/assignment contexts */
            const char *c_type = emit_type_to_c(instr->type, ctx);
            if (!is_hoisted) iron_strbuf_appendf(sb, "%s ", c_type);
            emit_val(sb, instr->id);
            iron_strbuf_appendf(sb, " = {0};\n");
        } else if (instr->type && (instr->type->kind == IRON_TYPE_INT   ||
                                    instr->type->kind == IRON_TYPE_INT8  ||
                                    instr->type->kind == IRON_TYPE_INT16 ||
                                    instr->type->kind == IRON_TYPE_INT32 ||
                                    instr->type->kind == IRON_TYPE_INT64 ||
                                    instr->type->kind == IRON_TYPE_UINT  ||
                                    instr->type->kind == IRON_TYPE_UINT8 ||
                                    instr->type->kind == IRON_TYPE_UINT16||
                                    instr->type->kind == IRON_TYPE_UINT32||
                                    instr->type->kind == IRON_TYPE_UINT64)) {
            /* Scalar integer: emit type-correct zero instead of void* NULL */
            const char *c_type = emit_type_to_c(instr->type, ctx);
            if (!is_hoisted) iron_strbuf_appendf(sb, "%s ", c_type);
            emit_val(sb, instr->id);
            iron_strbuf_appendf(sb, " = 0;\n");
        } else {
            if (!is_hoisted) iron_strbuf_appendf(sb, "void* ");
            emit_val(sb, instr->id);
            iron_strbuf_appendf(sb, " = NULL;\n");
        }
        break;

    /* ── Arithmetic ─────────────────────────────────────────────────────── */

    case IRON_LIR_ADD:
        emit_indent(sb, ind);
        if (!is_hoisted) iron_strbuf_appendf(sb, "%s ", emit_type_to_c(instr->type, ctx));
        emit_val(sb, instr->id);
        iron_strbuf_appendf(sb, " = ");
        emit_expr_to_buf(sb, instr->binop.left, fn, ctx, ctx->current_block_id, 0);
        iron_strbuf_appendf(sb, " + ");
        emit_expr_to_buf(sb, instr->binop.right, fn, ctx, ctx->current_block_id, 0);
        iron_strbuf_appendf(sb, ";\n");
        break;

    case IRON_LIR_SUB:
        emit_indent(sb, ind);
        if (!is_hoisted) iron_strbuf_appendf(sb, "%s ", emit_type_to_c(instr->type, ctx));
        emit_val(sb, instr->id);
        iron_strbuf_appendf(sb, " = ");
        emit_expr_to_buf(sb, instr->binop.left, fn, ctx, ctx->current_block_id, 0);
        iron_strbuf_appendf(sb, " - ");
        emit_expr_to_buf(sb, instr->binop.right, fn, ctx, ctx->current_block_id, 0);
        iron_strbuf_appendf(sb, ";\n");
        break;

    case IRON_LIR_MUL:
        emit_indent(sb, ind);
        if (!is_hoisted) iron_strbuf_appendf(sb, "%s ", emit_type_to_c(instr->type, ctx));
        emit_val(sb, instr->id);
        iron_strbuf_appendf(sb, " = ");
        emit_expr_to_buf(sb, instr->binop.left, fn, ctx, ctx->current_block_id, 0);
        iron_strbuf_appendf(sb, " * ");
        emit_expr_to_buf(sb, instr->binop.right, fn, ctx, ctx->current_block_id, 0);
        iron_strbuf_appendf(sb, ";\n");
        break;

    case IRON_LIR_DIV:
        emit_indent(sb, ind);
        if (!is_hoisted) iron_strbuf_appendf(sb, "%s ", emit_type_to_c(instr->type, ctx));
        emit_val(sb, instr->id);
        iron_strbuf_appendf(sb, " = ");
        emit_expr_to_buf(sb, instr->binop.left, fn, ctx, ctx->current_block_id, 0);
        iron_strbuf_appendf(sb, " / ");
        emit_expr_to_buf(sb, instr->binop.right, fn, ctx, ctx->current_block_id, 0);
        iron_strbuf_appendf(sb, ";\n");
        break;

    case IRON_LIR_MOD:
        emit_indent(sb, ind);
        if (!is_hoisted) iron_strbuf_appendf(sb, "%s ", emit_type_to_c(instr->type, ctx));
        emit_val(sb, instr->id);
        iron_strbuf_appendf(sb, " = ");
        emit_expr_to_buf(sb, instr->binop.left, fn, ctx, ctx->current_block_id, 0);
        iron_strbuf_appendf(sb, " %% ");
        emit_expr_to_buf(sb, instr->binop.right, fn, ctx, ctx->current_block_id, 0);
        iron_strbuf_appendf(sb, ";\n");
        break;

    /* ── Comparison ─────────────────────────────────────────────────────── */

    case IRON_LIR_EQ: {
        /* Phase 59 01d: tuple equality at statement level.
         * Phase 59 P05: also handle plain `String == String` via
         * iron_string_equals(). */
        Iron_Type *lty = emit_get_value_type(fn, instr->binop.left);
        emit_indent(sb, ind);
        if (!is_hoisted) iron_strbuf_appendf(sb, "bool ");
        emit_val(sb, instr->id);
        iron_strbuf_appendf(sb, " = ");
        if (lty && lty->kind == IRON_TYPE_TUPLE) {
            iron_strbuf_appendf(sb, "(");
            int nc = lty->tuple.elem_count;
            for (int i = 0; i < nc; i++) {
                if (i > 0) iron_strbuf_appendf(sb, " && ");
                const Iron_Type *et = lty->tuple.elem_types[i];
                bool is_string = et && et->kind == IRON_TYPE_STRING;
                if (is_string) {
                    iron_strbuf_appendf(sb, "iron_string_equals(&(");
                    emit_expr_to_buf(sb, instr->binop.left, fn, ctx, ctx->current_block_id, 0);
                    iron_strbuf_appendf(sb, ").v%d, &(", i);
                    emit_expr_to_buf(sb, instr->binop.right, fn, ctx, ctx->current_block_id, 0);
                    iron_strbuf_appendf(sb, ").v%d)", i);
                } else {
                    iron_strbuf_appendf(sb, "(");
                    emit_expr_to_buf(sb, instr->binop.left, fn, ctx, ctx->current_block_id, 0);
                    iron_strbuf_appendf(sb, ").v%d == (", i);
                    emit_expr_to_buf(sb, instr->binop.right, fn, ctx, ctx->current_block_id, 0);
                    iron_strbuf_appendf(sb, ").v%d", i);
                }
            }
            iron_strbuf_appendf(sb, ")");
        } else if (lty && lty->kind == IRON_TYPE_STRING) {
            iron_strbuf_appendf(sb, "iron_string_equals(&(");
            emit_expr_to_buf(sb, instr->binop.left, fn, ctx, ctx->current_block_id, 0);
            iron_strbuf_appendf(sb, "), &(");
            emit_expr_to_buf(sb, instr->binop.right, fn, ctx, ctx->current_block_id, 0);
            iron_strbuf_appendf(sb, "))");
        } else {
            emit_expr_to_buf(sb, instr->binop.left, fn, ctx, ctx->current_block_id, 0);
            iron_strbuf_appendf(sb, " == ");
            emit_expr_to_buf(sb, instr->binop.right, fn, ctx, ctx->current_block_id, 0);
        }
        iron_strbuf_appendf(sb, ";\n");
        break;
    }

    case IRON_LIR_NEQ: {
        Iron_Type *lty = emit_get_value_type(fn, instr->binop.left);
        emit_indent(sb, ind);
        if (!is_hoisted) iron_strbuf_appendf(sb, "bool ");
        emit_val(sb, instr->id);
        iron_strbuf_appendf(sb, " = ");
        if (lty && lty->kind == IRON_TYPE_TUPLE) {
            iron_strbuf_appendf(sb, "!(");
            int nc = lty->tuple.elem_count;
            for (int i = 0; i < nc; i++) {
                if (i > 0) iron_strbuf_appendf(sb, " && ");
                const Iron_Type *et = lty->tuple.elem_types[i];
                bool is_string = et && et->kind == IRON_TYPE_STRING;
                if (is_string) {
                    iron_strbuf_appendf(sb, "iron_string_equals(&(");
                    emit_expr_to_buf(sb, instr->binop.left, fn, ctx, ctx->current_block_id, 0);
                    iron_strbuf_appendf(sb, ").v%d, &(", i);
                    emit_expr_to_buf(sb, instr->binop.right, fn, ctx, ctx->current_block_id, 0);
                    iron_strbuf_appendf(sb, ").v%d)", i);
                } else {
                    iron_strbuf_appendf(sb, "(");
                    emit_expr_to_buf(sb, instr->binop.left, fn, ctx, ctx->current_block_id, 0);
                    iron_strbuf_appendf(sb, ").v%d == (", i);
                    emit_expr_to_buf(sb, instr->binop.right, fn, ctx, ctx->current_block_id, 0);
                    iron_strbuf_appendf(sb, ").v%d", i);
                }
            }
            iron_strbuf_appendf(sb, ")");
        } else if (lty && lty->kind == IRON_TYPE_STRING) {
            iron_strbuf_appendf(sb, "(!iron_string_equals(&(");
            emit_expr_to_buf(sb, instr->binop.left, fn, ctx, ctx->current_block_id, 0);
            iron_strbuf_appendf(sb, "), &(");
            emit_expr_to_buf(sb, instr->binop.right, fn, ctx, ctx->current_block_id, 0);
            iron_strbuf_appendf(sb, ")))");
        } else {
            emit_expr_to_buf(sb, instr->binop.left, fn, ctx, ctx->current_block_id, 0);
            iron_strbuf_appendf(sb, " != ");
            emit_expr_to_buf(sb, instr->binop.right, fn, ctx, ctx->current_block_id, 0);
        }
        iron_strbuf_appendf(sb, ";\n");
        break;
    }

    case IRON_LIR_LT:
        emit_indent(sb, ind);
        if (!is_hoisted) iron_strbuf_appendf(sb, "bool ");
        emit_val(sb, instr->id);
        iron_strbuf_appendf(sb, " = ");
        emit_expr_to_buf(sb, instr->binop.left, fn, ctx, ctx->current_block_id, 0);
        iron_strbuf_appendf(sb, " < ");
        emit_expr_to_buf(sb, instr->binop.right, fn, ctx, ctx->current_block_id, 0);
        iron_strbuf_appendf(sb, ";\n");
        break;

    case IRON_LIR_LTE:
        emit_indent(sb, ind);
        if (!is_hoisted) iron_strbuf_appendf(sb, "bool ");
        emit_val(sb, instr->id);
        iron_strbuf_appendf(sb, " = ");
        emit_expr_to_buf(sb, instr->binop.left, fn, ctx, ctx->current_block_id, 0);
        iron_strbuf_appendf(sb, " <= ");
        emit_expr_to_buf(sb, instr->binop.right, fn, ctx, ctx->current_block_id, 0);
        iron_strbuf_appendf(sb, ";\n");
        break;

    case IRON_LIR_GT:
        emit_indent(sb, ind);
        if (!is_hoisted) iron_strbuf_appendf(sb, "bool ");
        emit_val(sb, instr->id);
        iron_strbuf_appendf(sb, " = ");
        emit_expr_to_buf(sb, instr->binop.left, fn, ctx, ctx->current_block_id, 0);
        iron_strbuf_appendf(sb, " > ");
        emit_expr_to_buf(sb, instr->binop.right, fn, ctx, ctx->current_block_id, 0);
        iron_strbuf_appendf(sb, ";\n");
        break;

    case IRON_LIR_GTE:
        emit_indent(sb, ind);
        if (!is_hoisted) iron_strbuf_appendf(sb, "bool ");
        emit_val(sb, instr->id);
        iron_strbuf_appendf(sb, " = ");
        emit_expr_to_buf(sb, instr->binop.left, fn, ctx, ctx->current_block_id, 0);
        iron_strbuf_appendf(sb, " >= ");
        emit_expr_to_buf(sb, instr->binop.right, fn, ctx, ctx->current_block_id, 0);
        iron_strbuf_appendf(sb, ";\n");
        break;

    /* ── Logical ────────────────────────────────────────────────────────── */

    case IRON_LIR_AND:
        emit_indent(sb, ind);
        if (!is_hoisted) iron_strbuf_appendf(sb, "bool ");
        emit_val(sb, instr->id);
        iron_strbuf_appendf(sb, " = ");
        emit_expr_to_buf(sb, instr->binop.left, fn, ctx, ctx->current_block_id, 0);
        iron_strbuf_appendf(sb, " && ");
        emit_expr_to_buf(sb, instr->binop.right, fn, ctx, ctx->current_block_id, 0);
        iron_strbuf_appendf(sb, ";\n");
        break;

    case IRON_LIR_OR:
        emit_indent(sb, ind);
        if (!is_hoisted) iron_strbuf_appendf(sb, "bool ");
        emit_val(sb, instr->id);
        iron_strbuf_appendf(sb, " = ");
        emit_expr_to_buf(sb, instr->binop.left, fn, ctx, ctx->current_block_id, 0);
        iron_strbuf_appendf(sb, " || ");
        emit_expr_to_buf(sb, instr->binop.right, fn, ctx, ctx->current_block_id, 0);
        iron_strbuf_appendf(sb, ";\n");
        break;

    /* ── Bitwise ────────────────────────────────────────────────────────── */

    case IRON_LIR_SHL:
        emit_indent(sb, ind);
        if (!is_hoisted) iron_strbuf_appendf(sb, "%s ", emit_type_to_c(instr->type, ctx));
        emit_val(sb, instr->id);
        iron_strbuf_appendf(sb, " = ");
        emit_expr_to_buf(sb, instr->binop.left, fn, ctx, ctx->current_block_id, 0);
        iron_strbuf_appendf(sb, " << ");
        emit_expr_to_buf(sb, instr->binop.right, fn, ctx, ctx->current_block_id, 0);
        iron_strbuf_appendf(sb, ";\n");
        break;

    case IRON_LIR_SHR:
        emit_indent(sb, ind);
        if (!is_hoisted) iron_strbuf_appendf(sb, "%s ", emit_type_to_c(instr->type, ctx));
        emit_val(sb, instr->id);
        iron_strbuf_appendf(sb, " = ");
        emit_expr_to_buf(sb, instr->binop.left, fn, ctx, ctx->current_block_id, 0);
        iron_strbuf_appendf(sb, " >> ");
        emit_expr_to_buf(sb, instr->binop.right, fn, ctx, ctx->current_block_id, 0);
        iron_strbuf_appendf(sb, ";\n");
        break;

    case IRON_LIR_BAND:
        emit_indent(sb, ind);
        if (!is_hoisted) iron_strbuf_appendf(sb, "%s ", emit_type_to_c(instr->type, ctx));
        emit_val(sb, instr->id);
        iron_strbuf_appendf(sb, " = ");
        emit_expr_to_buf(sb, instr->binop.left, fn, ctx, ctx->current_block_id, 0);
        iron_strbuf_appendf(sb, " & ");
        emit_expr_to_buf(sb, instr->binop.right, fn, ctx, ctx->current_block_id, 0);
        iron_strbuf_appendf(sb, ";\n");
        break;

    case IRON_LIR_BOR:
        emit_indent(sb, ind);
        if (!is_hoisted) iron_strbuf_appendf(sb, "%s ", emit_type_to_c(instr->type, ctx));
        emit_val(sb, instr->id);
        iron_strbuf_appendf(sb, " = ");
        emit_expr_to_buf(sb, instr->binop.left, fn, ctx, ctx->current_block_id, 0);
        iron_strbuf_appendf(sb, " | ");
        emit_expr_to_buf(sb, instr->binop.right, fn, ctx, ctx->current_block_id, 0);
        iron_strbuf_appendf(sb, ";\n");
        break;

    case IRON_LIR_BXOR:
        emit_indent(sb, ind);
        if (!is_hoisted) iron_strbuf_appendf(sb, "%s ", emit_type_to_c(instr->type, ctx));
        emit_val(sb, instr->id);
        iron_strbuf_appendf(sb, " = ");
        emit_expr_to_buf(sb, instr->binop.left, fn, ctx, ctx->current_block_id, 0);
        iron_strbuf_appendf(sb, " ^ ");
        emit_expr_to_buf(sb, instr->binop.right, fn, ctx, ctx->current_block_id, 0);
        iron_strbuf_appendf(sb, ";\n");
        break;

    /* ── Unary ──────────────────────────────────────────────────────────── */

    case IRON_LIR_NEG:
        emit_indent(sb, ind);
        if (!is_hoisted) iron_strbuf_appendf(sb, "%s ", emit_type_to_c(instr->type, ctx));
        emit_val(sb, instr->id);
        iron_strbuf_appendf(sb, " = -");
        emit_expr_to_buf(sb, instr->unop.operand, fn, ctx, ctx->current_block_id, 0);
        iron_strbuf_appendf(sb, ";\n");
        break;

    case IRON_LIR_NOT:
        emit_indent(sb, ind);
        if (!is_hoisted) iron_strbuf_appendf(sb, "bool ");
        emit_val(sb, instr->id);
        iron_strbuf_appendf(sb, " = !");
        emit_expr_to_buf(sb, instr->unop.operand, fn, ctx, ctx->current_block_id, 0);
        iron_strbuf_appendf(sb, ";\n");
        break;

    case IRON_LIR_BNOT:
        emit_indent(sb, ind);
        if (!is_hoisted) iron_strbuf_appendf(sb, "%s ", emit_type_to_c(instr->type, ctx));
        emit_val(sb, instr->id);
        iron_strbuf_appendf(sb, " = ~");
        emit_expr_to_buf(sb, instr->unop.operand, fn, ctx, ctx->current_block_id, 0);
        iron_strbuf_appendf(sb, ";\n");
        break;

    /* ── Memory ─────────────────────────────────────────────────────────── */

    case IRON_LIR_ALLOCA: {
        /* Skip alloca for read-only parameter aliases — no variable needed */
        if (hmgeti(ctx->param_alias_ids, instr->id) >= 0) break;

        /* Skip alloca for capture aliases — the variable is an env field.
         * The capture alias map maps alloca_id -> capture_index. */
        if (ctx->capture_alias_map && hmgeti(ctx->capture_alias_map, instr->id) >= 0)
            break;

        /* Skip alloca if it was already hoisted to function entry (phi_hoisted).
         * This handles the case where function inlining splits a block and moves
         * an alloca to a later block, but earlier blocks still reference it.
         * The declaration was pre-emitted at function entry; only a no-op here. */
        if (is_hoisted) break;

        /* Check if this alloca holds a stack array (determined by pre-scan) */
        IronLIR_ValueId sa_origin = get_stack_array_origin(ctx, instr->id);
        if (sa_origin != IRON_LIR_VALUE_INVALID) {
            /* Emit as pointer to element type so C array decays correctly */
            const char *elem_c = "int64_t"; /* fallback */
            if (instr->alloca.alloc_type &&
                instr->alloca.alloc_type->kind == IRON_TYPE_ARRAY &&
                instr->alloca.alloc_type->array.elem) {
                elem_c = emit_type_to_c(instr->alloca.alloc_type->array.elem, ctx);
            }
            /* PARAM-01: Check if origin is a const pointer param.
             * HIR pipeline: param i has val_id = i+1, so 1..param_count are
             * synthetic param IDs.  pi_idx = sa_origin - 1. */
            bool is_const_origin = false;
            if (sa_origin != IRON_LIR_VALUE_INVALID &&
                sa_origin >= 1 &&
                sa_origin <= (IronLIR_ValueId)fn->param_count) {
                int pi_idx = (int)(sa_origin - 1);
                if (pi_idx < fn->param_count) {
                    ArrayParamMode pm = emit_get_array_param_mode(ctx, fn->name, pi_idx);
                    if (pm == ARRAY_PARAM_CONST_PTR) is_const_origin = true;
                }
            }
            emit_indent(sb, ind);
            iron_strbuf_appendf(sb, "%s%s *", is_const_origin ? "const " : "", elem_c);
            emit_val(sb, instr->id);
            iron_strbuf_appendf(sb, ";\n");
            /* Also emit companion length variable for this alloca */
            emit_indent(sb, ind);
            iron_strbuf_appendf(sb, "int64_t ");
            emit_val(sb, instr->id);
            iron_strbuf_appendf(sb, "_len;\n");
        } else {
            /* Declare a C variable of the alloc_type */
            const char *c_type = emit_type_to_c(instr->alloca.alloc_type, ctx);
            emit_indent(sb, ind);
            iron_strbuf_appendf(sb, "%s ", c_type);
            emit_val(sb, instr->id);
            iron_strbuf_appendf(sb, ";\n");
        }
        break;
    }

    case IRON_LIR_LOAD: {
        /* Load from a capture alias: redirect to env field access */
        if (ctx->capture_alias_map) {
            ptrdiff_t ca_idx = hmgeti(ctx->capture_alias_map, instr->load.ptr);
            if (ca_idx >= 0) {
                int ci = ctx->capture_alias_map[ca_idx].value;
                Iron_CaptureEntry *cap = &ctx->current_captures[ci];
                emit_indent(sb, ind);
                if (!is_hoisted) {
                    const char *c_type = cap->type
                                         ? emit_type_to_c(cap->type, ctx)
                                         : "void*";
                    iron_strbuf_appendf(sb, "%s ", c_type);
                }
                emit_val(sb, instr->id);
                iron_strbuf_appendf(sb, " = ");
                if (cap->is_mutable) {
                    /* var capture: dereference pointer field */
                    iron_strbuf_appendf(sb, "*_e->%s;\n", cap->name);
                } else {
                    /* val capture: read field directly */
                    iron_strbuf_appendf(sb, "_e->%s;\n", cap->name);
                }
                break;
            }
        }

        /* Load from a read-only parameter alias: reference the param directly */
        {
            ptrdiff_t pa_idx = hmgeti(ctx->param_alias_ids, instr->load.ptr);
            if (pa_idx >= 0) {
                IronLIR_ValueId param_val = ctx->param_alias_ids[pa_idx].value;
                emit_indent(sb, ind);
                iron_strbuf_appendf(sb, "%s ", emit_type_to_c(instr->type, ctx));
                emit_val(sb, instr->id);
                iron_strbuf_appendf(sb, " = ");
                emit_val(sb, param_val);
                iron_strbuf_appendf(sb, ";\n");
                break;
            }
        }

        IronLIR_ValueId sa_origin = get_stack_array_origin(ctx, instr->load.ptr);
        if (sa_origin != IRON_LIR_VALUE_INVALID) {
            /* Loading from a stack-array alloca: emit as pointer copy */
            const char *elem_c = "int64_t"; /* fallback */
            if (instr->type && instr->type->kind == IRON_TYPE_ARRAY &&
                instr->type->array.elem) {
                elem_c = emit_type_to_c(instr->type->array.elem, ctx);
            }
            /* PARAM-01: preserve const qualifier from origin */
            bool is_const_origin = false;
            if (sa_origin != IRON_LIR_VALUE_INVALID && (sa_origin & 1) &&
                sa_origin <= (IronLIR_ValueId)(fn->param_count * 2)) {
                int pi_idx = (int)(sa_origin - 1) / 2;
                if (pi_idx < fn->param_count) {
                    ArrayParamMode pm = emit_get_array_param_mode(ctx, fn->name, pi_idx);
                    if (pm == ARRAY_PARAM_CONST_PTR) is_const_origin = true;
                }
            }
            emit_indent(sb, ind);
            iron_strbuf_appendf(sb, "%s%s *", is_const_origin ? "const " : "", elem_c);
            emit_val(sb, instr->id);
            iron_strbuf_appendf(sb, " = ");
            emit_val(sb, instr->load.ptr);
            iron_strbuf_appendf(sb, ";\n");
            /* Copy companion length */
            emit_indent(sb, ind);
            iron_strbuf_appendf(sb, "int64_t ");
            emit_val(sb, instr->id);
            iron_strbuf_appendf(sb, "_len = ");
            emit_val(sb, instr->load.ptr);
            iron_strbuf_appendf(sb, "_len;\n");
            /* Propagate stack-array status to loaded value */
            mark_stack_array(ctx, instr->id, sa_origin);
        } else {
            /* Load from the alloca variable — just copy it.
             * When the alloca is RC-typed (holds a heap/rc pointer), use the
             * alloca's type (T*) rather than the LOAD's inner type (T). */
            emit_indent(sb, ind);
            if (!is_hoisted) {
                Iron_Type *load_c_type = instr->type;
                IronLIR_ValueId ptr = instr->load.ptr;
                if (ptr != IRON_LIR_VALUE_INVALID &&
                    ptr < (IronLIR_ValueId)arrlen(fn->value_table) &&
                    fn->value_table[ptr] &&
                    fn->value_table[ptr]->kind == IRON_LIR_ALLOCA &&
                    fn->value_table[ptr]->alloca.alloc_type &&
                    fn->value_table[ptr]->alloca.alloc_type->kind == IRON_TYPE_RC) {
                    /* Alloca holds a pointer: use the alloca's RC type for C type */
                    load_c_type = fn->value_table[ptr]->alloca.alloc_type;
                }
                iron_strbuf_appendf(sb, "%s ", emit_type_to_c(load_c_type, ctx));
            }
            emit_val(sb, instr->id);
            iron_strbuf_appendf(sb, " = ");
            emit_val(sb, instr->load.ptr);
            iron_strbuf_appendf(sb, ";\n");
        }
        break;
    }

    case IRON_LIR_STORE: {
        /* Redirect store to capture alias → env field assignment */
        if (ctx->capture_alias_map) {
            ptrdiff_t ca_idx = hmgeti(ctx->capture_alias_map, instr->store.ptr);
            if (ca_idx >= 0) {
                int ci = ctx->capture_alias_map[ca_idx].value;
                Iron_CaptureEntry *cap = &ctx->current_captures[ci];
                if (cap->is_mutable) {
                    /* var capture: write through pointer field */
                    emit_indent(sb, ind);
                    iron_strbuf_appendf(sb, "*_e->%s = ", cap->name);
                    emit_expr_to_buf(sb, instr->store.value, fn, ctx, ctx->current_block_id, 0);
                    iron_strbuf_appendf(sb, ";\n");
                }
                /* val capture: immutable, skip store */
                break;
            }
        }

        /* Skip store into read-only parameter alias alloca */
        if (hmgeti(ctx->param_alias_ids, instr->store.ptr) >= 0) break;

        /* Check if we're storing a stack array into an alloca */
        IronLIR_ValueId sa_ptr = get_stack_array_origin(ctx, instr->store.ptr);
        IronLIR_ValueId sa_val = get_stack_array_origin(ctx, instr->store.value);
        if (sa_ptr != IRON_LIR_VALUE_INVALID && sa_val != IRON_LIR_VALUE_INVALID) {
            /* Store stack array pointer into alloca: ptr = value; ptr_len = value_len; */
            emit_indent(sb, ind);
            emit_val(sb, instr->store.ptr);
            iron_strbuf_appendf(sb, " = ");
            emit_expr_to_buf(sb, instr->store.value, fn, ctx, ctx->current_block_id, 0);
            iron_strbuf_appendf(sb, ";\n");
            /* Propagate companion length */
            emit_indent(sb, ind);
            emit_val(sb, instr->store.ptr);
            iron_strbuf_appendf(sb, "_len = ");
            emit_val(sb, sa_val);
            iron_strbuf_appendf(sb, "_len;\n");
        } else {
            /* Check for interface wrapping: concrete type → interface tagged union */
            Iron_Type *ptr_type = (instr->store.ptr < (IronLIR_ValueId)arrlen(fn->value_table) &&
                                   fn->value_table[instr->store.ptr])
                                  ? fn->value_table[instr->store.ptr]->type : NULL;
            Iron_Type *val_type = (instr->store.value < (IronLIR_ValueId)arrlen(fn->value_table) &&
                                   fn->value_table[instr->store.value])
                                  ? fn->value_table[instr->store.value]->type : NULL;
            bool needs_iface_wrap = false;
            const char *wrap_iface = NULL;
            const char *wrap_impl = NULL;
            if (ptr_type && val_type &&
                ptr_type->kind == IRON_TYPE_INTERFACE &&
                val_type->kind == IRON_TYPE_OBJECT &&
                ptr_type->interface.decl && val_type->object.decl) {
                wrap_iface = emit_mangle_name(ptr_type->interface.decl->name, ctx->arena);
                wrap_impl = val_type->object.decl->name;
                needs_iface_wrap = true;
            }

            /* Write into alloca variable: ptr = value (or wrapped) */
            emit_indent(sb, ind);
            emit_val(sb, instr->store.ptr);
            iron_strbuf_appendf(sb, " = ");
            if (needs_iface_wrap) {
                iron_strbuf_appendf(sb, "%s_from_%s(", wrap_iface, wrap_impl);
            }
            emit_expr_to_buf(sb, instr->store.value, fn, ctx, ctx->current_block_id, 0);
            if (needs_iface_wrap) {
                iron_strbuf_appendf(sb, ")");
            }
            iron_strbuf_appendf(sb, ";\n");
        }
        break;
    }

    /* ── Field / Index ──────────────────────────────────────────────────── */

    case IRON_LIR_GET_FIELD: {
        /* Phase 41: .count on split collection → ._total_count */
        if (instr->field.field && strcmp(instr->field.field, "count") == 0 &&
            ctx->split_collection_ids) {
            ptrdiff_t sp_idx = hmgeti(ctx->split_collection_ids, instr->field.object);
            if (sp_idx >= 0) {
                emit_indent(sb, ind);
                if (!is_hoisted) iron_strbuf_appendf(sb, "%s ", emit_type_to_c(instr->type, ctx));
                emit_val(sb, instr->id);
                iron_strbuf_appendf(sb, " = ");
                emit_val(sb, instr->field.object);
                iron_strbuf_appendf(sb, "._total_count;\n");
                break;
            }
        }
        /* Check if this is a .count access on a stack array (from len() builtin) */
        IronLIR_ValueId sa_origin = get_stack_array_origin(ctx, instr->field.object);
        if (sa_origin != IRON_LIR_VALUE_INVALID &&
            instr->field.field && strcmp(instr->field.field, "count") == 0) {
            /* Emit: int64_t _vN = _vOBJ_len; (companion length variable) */
            emit_indent(sb, ind);
            if (!is_hoisted) iron_strbuf_appendf(sb, "%s ", emit_type_to_c(instr->type, ctx));
            emit_val(sb, instr->id);
            iron_strbuf_appendf(sb, " = ");
            emit_val(sb, instr->field.object);
            iron_strbuf_appendf(sb, "_len;\n");
            break;
        }
        /* Type-name namespace access: Math.PI, Log.DEBUG, etc.
         * The object is a FUNC_REF (type name), not a runtime instance.
         * Emit Iron_TypeName_FieldName so the #define in the module header
         * resolves to the correct compile-time constant. */
        if (emit_val_is_type_ref(fn, instr->field.object)) {
            IronLIR_Instr *ref = fn->value_table[instr->field.object];
            const char *type_c = emit_resolve_func_c_name(ctx, ref->func_ref.func_name);
            emit_indent(sb, ind);
            if (!is_hoisted) iron_strbuf_appendf(sb, "%s ", emit_type_to_c(instr->type, ctx));
            emit_val(sb, instr->id);
            iron_strbuf_appendf(sb, " = %s_%s;\n", type_c, instr->field.field);
            break;
        }
        /* object.field or object->field for heap/rc pointers */
        emit_indent(sb, ind);
        if (!is_hoisted) iron_strbuf_appendf(sb, "%s ", emit_type_to_c(instr->type, ctx));
        emit_val(sb, instr->id);
        /* Use -> when the object value comes from a heap or rc allocation,
         * or when loaded from an alloca that holds a pointer (RC-typed alloca). */
        bool obj_is_ptr = emit_val_is_heap_ptr(fn, instr->field.object);

        /* Check if this field access is on a boxed recursive ADT slot:
         * field path format is "data.VariantName._N" — if slot N is boxed, wrap with *() */
        bool needs_deref = false;
        if (instr->field.field) {
            const char *last_dot = strrchr(instr->field.field, '.');
            if (last_dot && last_dot[1] == '_' && last_dot[2] >= '0' && last_dot[2] <= '9') {
                const char *data_pos = strstr(instr->field.field, "data.");
                if (data_pos) {
                    const char *vname_start = data_pos + 5; /* skip "data." */
                    const char *vname_end = strchr(vname_start, '.');
                    if (vname_end) {
                        int slot_idx = atoi(last_dot + 2);
                        /* Resolve the object's enum type through LOAD/ALLOCA */
                        Iron_Type *obj_enum_type = NULL;
                        IronLIR_ValueId obj_vid = instr->field.object;
                        /* First try direct type */
                        Iron_Type *direct_t = emit_get_value_type(fn, obj_vid);
                        if (direct_t && direct_t->kind == IRON_TYPE_ENUM) {
                            obj_enum_type = direct_t;
                        } else if (obj_vid != IRON_LIR_VALUE_INVALID &&
                                   obj_vid < (IronLIR_ValueId)arrlen(fn->value_table) &&
                                   fn->value_table[obj_vid] &&
                                   fn->value_table[obj_vid]->kind == IRON_LIR_LOAD) {
                            /* LOAD from ALLOCA: look at the alloca type */
                            IronLIR_ValueId ptr = fn->value_table[obj_vid]->load.ptr;
                            if (ptr != IRON_LIR_VALUE_INVALID &&
                                ptr < (IronLIR_ValueId)arrlen(fn->value_table) &&
                                fn->value_table[ptr] &&
                                fn->value_table[ptr]->kind == IRON_LIR_ALLOCA) {
                                Iron_Type *at = fn->value_table[ptr]->alloca.alloc_type;
                                if (at && at->kind == IRON_TYPE_ENUM) obj_enum_type = at;
                            }
                        }
                        if (obj_enum_type && obj_enum_type->enu.decl &&
                            obj_enum_type->enu.payload_is_boxed) {
                            Iron_EnumDecl *ged = obj_enum_type->enu.decl;
                            size_t vname_len = (size_t)(vname_end - vname_start);
                            char vname[128];
                            if (vname_len < sizeof(vname)) {
                                memcpy(vname, vname_start, vname_len);
                                vname[vname_len] = '\0';
                                for (int vi = 0; vi < ged->variant_count; vi++) {
                                    /* PROT-03 row 34 (AUDIT-01 M-severity):
                                     * loop-bound + non-NULL assert before the
                                     * Iron_EnumVariant cast from a void**
                                     * variants array. */
                                    assert(vi >= 0 && vi < ged->variant_count);
                                    assert(ged->variants[vi] != NULL);
                                    Iron_EnumVariant *gev = (Iron_EnumVariant *)ged->variants[vi];
                                    if (strcmp(gev->name, vname) == 0) {
                                        if (obj_enum_type->enu.payload_is_boxed[vi] &&
                                            slot_idx < gev->payload_count &&
                                            obj_enum_type->enu.payload_is_boxed[vi][slot_idx]) {
                                            needs_deref = true;
                                        }
                                        break;
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }

        if (needs_deref) {
            iron_strbuf_appendf(sb, " = *(");
            emit_expr_to_buf(sb, instr->field.object, fn, ctx, ctx->current_block_id, 0);
            iron_strbuf_appendf(sb, "%s%s);\n", obj_is_ptr ? "->" : ".", instr->field.field);
        } else {
            iron_strbuf_appendf(sb, " = ");
            emit_expr_to_buf(sb, instr->field.object, fn, ctx, ctx->current_block_id, 0);
            iron_strbuf_appendf(sb, "%s%s;\n", obj_is_ptr ? "->" : ".", instr->field.field);
        }
        break;
    }

    case IRON_LIR_SET_FIELD: {
        /* When the object is a LOAD from a mutable var-capture alloca, write
         * through the captured pointer: _e->capturename->fieldname = value.
         * Otherwise use the standard object.field or object->field emission. */
        bool wrote_via_capture = false;
        if (ctx->capture_alias_map) {
            IronLIR_ValueId obj_id = instr->field.object;
            if (obj_id != IRON_LIR_VALUE_INVALID &&
                obj_id < (IronLIR_ValueId)arrlen(fn->value_table) &&
                fn->value_table[obj_id] != NULL) {
                IronLIR_Instr *obj_instr = fn->value_table[obj_id];
                if (obj_instr->kind == IRON_LIR_LOAD) {
                    ptrdiff_t ca_idx = hmgeti(ctx->capture_alias_map, obj_instr->load.ptr);
                    if (ca_idx >= 0) {
                        int ci = ctx->capture_alias_map[ca_idx].value;
                        Iron_CaptureEntry *cap = &ctx->current_captures[ci];
                        if (cap->is_mutable) {
                            /* Write through the capture pointer to the original struct */
                            emit_indent(sb, ind);
                            iron_strbuf_appendf(sb, "_e->%s->%s = ", cap->name, instr->field.field);
                            emit_expr_to_buf(sb, instr->field.value, fn, ctx, ctx->current_block_id, 0);
                            iron_strbuf_appendf(sb, ";\n");
                            wrote_via_capture = true;
                        }
                    }
                }
            }
        }
        if (!wrote_via_capture) {
            /* object.field = value or object->field = value for heap/rc */
            emit_indent(sb, ind);
            emit_expr_to_buf(sb, instr->field.object, fn, ctx, ctx->current_block_id, 0);
            bool obj_is_ptr = emit_val_is_heap_ptr(fn, instr->field.object);
            iron_strbuf_appendf(sb, "%s%s = ", obj_is_ptr ? "->" : ".", instr->field.field);
            emit_expr_to_buf(sb, instr->field.value, fn, ctx, ctx->current_block_id, 0);
            iron_strbuf_appendf(sb, ";\n");
        }
        break;
    }

    case IRON_LIR_GET_INDEX: {
        /* Phase 49: Monomorphic collection — direct items[i] access, no tag dispatch.
         * Monomorphic collections are removed from split_collection_ids by the
         * collapse pass, so they fall through to the standard array.items[i] path
         * below.  The interface type is preserved (Iron_Shape tagged union) to
         * maintain type compatibility with downstream method dispatch. */
        /* Phase 41: GET_INDEX on split collection → order-based lookup */
        if (ctx->split_collection_ids) {
            ptrdiff_t sp_idx = hmgeti(ctx->split_collection_ids, instr->index.array);
            if (sp_idx >= 0) {
                const char *sp_iface = ctx->split_collection_ids[sp_idx].value;
                /* Look up interface entry for implementor names */
                Iron_IfaceEntry *sp_entry = NULL;
                if (ctx->iface_reg) {
                    for (int ri = 0; ri < (int)shlen(ctx->iface_reg->map); ri++) {
                        const char *mangled_check = emit_mangle_name(
                            ctx->iface_reg->map[ri].value.iface_name, ctx->arena);
                        if (strcmp(mangled_check, sp_iface) == 0) {
                            sp_entry = &ctx->iface_reg->map[ri].value;
                            break;
                        }
                    }
                }
                if (sp_entry) {
                    /* Emit: switch on order[idx].tag to select from correct sub-array */
                    emit_indent(sb, ind);
                    if (!is_hoisted) iron_strbuf_appendf(sb, "%s ", sp_iface);
                    emit_val(sb, instr->id);
                    iron_strbuf_appendf(sb, ";\n");
                    emit_indent(sb, ind);
                    iron_strbuf_appendf(sb, "switch (");
                    emit_expr_to_buf(sb, instr->index.array, fn, ctx, ctx->current_block_id, 0);
                    iron_strbuf_appendf(sb, "._order[");
                    emit_expr_to_buf(sb, instr->index.index, fn, ctx, ctx->current_block_id, 0);
                    iron_strbuf_appendf(sb, "].tag) {\n");
                    for (int ji = 0; ji < sp_entry->impl_count; ji++) {
                        Iron_IfaceImpl *impl2 = &sp_entry->impls[ji];
                        if (!impl2->is_alive) continue;
                        char lower_name[256];
                        {
                            size_t nl2 = strlen(impl2->type_name);
                            if (nl2 >= sizeof(lower_name)) nl2 = sizeof(lower_name) - 1;
                            for (size_t ci3 = 0; ci3 < nl2; ci3++)
                                lower_name[ci3] = (char)((impl2->type_name[ci3] >= 'A' &&
                                                           impl2->type_name[ci3] <= 'Z')
                                    ? impl2->type_name[ci3] + 32
                                    : impl2->type_name[ci3]);
                            lower_name[nl2] = '\0';
                        }
                        emit_indent(sb, ind + 1);
                        iron_strbuf_appendf(sb, "case %d: ", impl2->tag);
                        emit_val(sb, instr->id);
                        iron_strbuf_appendf(sb, " = %s_from_%s(",
                            sp_iface, impl2->type_name);
                        emit_expr_to_buf(sb, instr->index.array, fn, ctx, ctx->current_block_id, 0);
                        iron_strbuf_appendf(sb, ".%s_items[", lower_name);
                        emit_expr_to_buf(sb, instr->index.array, fn, ctx, ctx->current_block_id, 0);
                        iron_strbuf_appendf(sb, "._order[");
                        emit_expr_to_buf(sb, instr->index.index, fn, ctx, ctx->current_block_id, 0);
                        iron_strbuf_appendf(sb, "].idx]); break;\n");
                    }
                    emit_indent(sb, ind);
                    iron_strbuf_appendf(sb, "}\n");
                    break;
                }
            }
        }
        /* ARR-02: Check if the source array is stack-represented */
        IronLIR_ValueId sa_origin = get_stack_array_origin(ctx, instr->index.array);
        if (sa_origin != IRON_LIR_VALUE_INVALID) {
            /* Direct C indexing: result = array[index]; */
            emit_indent(sb, ind);
            if (!is_hoisted) iron_strbuf_appendf(sb, "%s ", emit_type_to_c(instr->type, ctx));
            emit_val(sb, instr->id);
            iron_strbuf_appendf(sb, " = ");
            emit_expr_to_buf(sb, instr->index.array, fn, ctx, ctx->current_block_id, 0);
            iron_strbuf_appendf(sb, "[");
            emit_expr_to_buf(sb, instr->index.index, fn, ctx, ctx->current_block_id, 0);
            iron_strbuf_appendf(sb, "];\n");
        } else {
            /* Check if the array has an array type — if so, inline .items[idx]
             * instead of calling _get() which is just return self->items[index] */
            Iron_Type *arr_t = emit_get_value_type(fn, instr->index.array);
            bool use_direct = (arr_t && arr_t->kind == IRON_TYPE_ARRAY);
            if (use_direct) {
                /* Direct field access: result = array.items[index]; */
                emit_indent(sb, ind);
                if (!is_hoisted) iron_strbuf_appendf(sb, "%s ", emit_type_to_c(instr->type, ctx));
                emit_val(sb, instr->id);
                iron_strbuf_appendf(sb, " = ");
                emit_expr_to_buf(sb, instr->index.array, fn, ctx, ctx->current_block_id, 0);
                iron_strbuf_appendf(sb, ".items[");
                emit_expr_to_buf(sb, instr->index.index, fn, ctx, ctx->current_block_id, 0);
                iron_strbuf_appendf(sb, "];\n");
            } else {
                /* Fallback: result = Iron_List_<suffix>_get(&array, index) */
                const char *list_type = "Iron_List_int64_t"; /* default fallback */
                if (arr_t) list_type = emit_type_to_c(arr_t, ctx);
                emit_indent(sb, ind);
                if (!is_hoisted) iron_strbuf_appendf(sb, "%s ", emit_type_to_c(instr->type, ctx));
                emit_val(sb, instr->id);
                iron_strbuf_appendf(sb, " = %s_get(&", list_type);
                emit_expr_to_buf(sb, instr->index.array, fn, ctx, ctx->current_block_id, 0);
                iron_strbuf_appendf(sb, ", ");
                emit_expr_to_buf(sb, instr->index.index, fn, ctx, ctx->current_block_id, 0);
                iron_strbuf_appendf(sb, ");\n");
            }
        }
        break;
    }

    case IRON_LIR_SET_INDEX: {
        /* ARR-02: Check if the target array is stack-represented */
        IronLIR_ValueId sa_origin = get_stack_array_origin(ctx, instr->index.array);
        if (sa_origin != IRON_LIR_VALUE_INVALID) {
            /* Direct C indexing: array[index] = value; */
            emit_indent(sb, ind);
            emit_expr_to_buf(sb, instr->index.array, fn, ctx, ctx->current_block_id, 0);
            iron_strbuf_appendf(sb, "[");
            emit_expr_to_buf(sb, instr->index.index, fn, ctx, ctx->current_block_id, 0);
            iron_strbuf_appendf(sb, "] = ");
            emit_expr_to_buf(sb, instr->index.value, fn, ctx, ctx->current_block_id, 0);
            iron_strbuf_appendf(sb, ";\n");
        } else {
            /* Check if the array has an array type — if so, inline .items[idx]
             * instead of calling _set() which is just self->items[index] = item */
            Iron_Type *arr_t = emit_get_value_type(fn, instr->index.array);
            bool use_direct = (arr_t && arr_t->kind == IRON_TYPE_ARRAY);
            if (use_direct) {
                /* Direct field access: array.items[index] = value; */
                emit_indent(sb, ind);
                emit_expr_to_buf(sb, instr->index.array, fn, ctx, ctx->current_block_id, 0);
                iron_strbuf_appendf(sb, ".items[");
                emit_expr_to_buf(sb, instr->index.index, fn, ctx, ctx->current_block_id, 0);
                iron_strbuf_appendf(sb, "] = ");
                emit_expr_to_buf(sb, instr->index.value, fn, ctx, ctx->current_block_id, 0);
                iron_strbuf_appendf(sb, ";\n");
            } else {
                /* Fallback: Iron_List_<suffix>_set(&array, index, value) */
                const char *list_type = "Iron_List_int64_t"; /* default fallback */
                if (arr_t) list_type = emit_type_to_c(arr_t, ctx);
                emit_indent(sb, ind);
                iron_strbuf_appendf(sb, "%s_set(&", list_type);
                emit_expr_to_buf(sb, instr->index.array, fn, ctx, ctx->current_block_id, 0);
                iron_strbuf_appendf(sb, ", ");
                emit_expr_to_buf(sb, instr->index.index, fn, ctx, ctx->current_block_id, 0);
                iron_strbuf_appendf(sb, ", ");
                emit_expr_to_buf(sb, instr->index.value, fn, ctx, ctx->current_block_id, 0);
                iron_strbuf_appendf(sb, ");\n");
            }
        }
        break;
    }

    /* ── Call ───────────────────────────────────────────────────────────── */

    case IRON_LIR_CALL: {
        /* Phase 49: Fusion chain dispatch — skip interior nodes, emit fused loop at terminal */
        if (ctx->fusion_chain_member) {
            ptrdiff_t fc_idx = hmgeti(ctx->fusion_chain_member, instr->id);
            if (fc_idx >= 0) {
                int chain_idx = ctx->fusion_chain_member[fc_idx].value;
                ptrdiff_t fp_idx = hmgeti(ctx->fusion_chain_position, instr->id);
                int pos = (fp_idx >= 0) ? ctx->fusion_chain_position[fp_idx].value : 0;
                FusionChain *chain = &ctx->fusion_chains[chain_idx];
                if (pos < chain->node_count - 1) {
                    return;  /* Interior node — skip emission entirely (fused at terminal) */
                }
                /* Terminal node — emit fused loop */
                emit_fused_chain(ctx, sb, fn, chain, instr, ctx->indent);
                break;  /* skip normal CALL emission */
            }
        }
        /* Check for __builtin_fill(count, value) -> stack array or Iron_List_T */
        {
            IronLIR_ValueId fptr = instr->call.func_ptr;
            if (fptr != IRON_LIR_VALUE_INVALID &&
                fptr < (IronLIR_ValueId)arrlen(fn->value_table) &&
                fn->value_table[fptr] != NULL &&
                fn->value_table[fptr]->kind == IRON_LIR_FUNC_REF &&
                strcmp(fn->value_table[fptr]->func_ref.func_name,
                       "__builtin_fill") == 0 &&
                instr->call.arg_count == 2) {

                /* Check if count is a compile-time constant <= 256 AND
                 * this fill() result is NOT used as an escaping value
                 * (i.e., it's in the stack_array_ids map from pre-scan). */
                IronLIR_ValueId count_id = instr->call.args[0];
                IronLIR_Instr *count_instr = NULL;
                if (count_id < (IronLIR_ValueId)arrlen(fn->value_table))
                    count_instr = fn->value_table[count_id];
                bool use_stack = false;
                int64_t fill_count = 0;
                if (count_instr && count_instr->kind == IRON_LIR_CONST_INT &&
                    count_instr->const_int.value > 0 &&
                    count_instr->const_int.value <= 256) {
                    /* Check pre-scan marked this as stack-eligible */
                    IronLIR_ValueId sa = get_stack_array_origin(ctx, instr->id);
                    if (sa != IRON_LIR_VALUE_INVALID) {
                        use_stack = true;
                        fill_count = count_instr->const_int.value;
                    }
                }

                if (use_stack) {
                    /* Emit as stack array with fill loop (constant count) */
                    const char *elem_type = "int64_t";
                    if (instr->type && instr->type->kind == IRON_TYPE_ARRAY &&
                        instr->type->array.elem)
                        elem_type = emit_type_to_c(instr->type->array.elem, ctx);
                    emit_indent(sb, ind);
                    iron_strbuf_appendf(sb, "%s ", elem_type);
                    emit_val(sb, instr->id);
                    iron_strbuf_appendf(sb, "[%lld];\n", (long long)fill_count);
                    emit_indent(sb, ind);
                    iron_strbuf_appendf(sb, "for (int64_t _fill_i = 0; _fill_i < %lld; _fill_i++) ",
                                         (long long)fill_count);
                    emit_val(sb, instr->id);
                    iron_strbuf_appendf(sb, "[_fill_i] = ");
                    emit_expr_to_buf(sb, instr->call.args[1], fn, ctx, ctx->current_block_id, 0);
                    iron_strbuf_appendf(sb, ";\n");
                    /* Emit companion length variable */
                    emit_indent(sb, ind);
                    iron_strbuf_appendf(sb, "int64_t ");
                    emit_val(sb, instr->id);
                    iron_strbuf_appendf(sb, "_len = %lld;\n", (long long)fill_count);
                    /* Mark as stack array */
                    mark_stack_array(ctx, instr->id, instr->id);
                } else {
                    /* Check if dynamic-count fill is stack-eligible (VLA) */
                    IronLIR_ValueId sa = get_stack_array_origin(ctx, instr->id);
                    if (sa != IRON_LIR_VALUE_INVALID) {
                        /* VLA path: stack-allocated variable-length array.
                         * Use alloca() instead of C99 VLA to avoid
                         * "goto bypasses VLA initialization" errors. */
                        const char *elem_type = "int64_t";
                        if (instr->type && instr->type->kind == IRON_TYPE_ARRAY &&
                            instr->type->array.elem)
                            elem_type = emit_type_to_c(instr->type->array.elem, ctx);
                        emit_indent(sb, ind);
                        iron_strbuf_appendf(sb, "%s *", elem_type);
                        emit_val(sb, instr->id);
                        iron_strbuf_appendf(sb, " = (%s *)alloca(sizeof(%s) * ",
                                            elem_type, elem_type);
                        emit_expr_to_buf(sb, instr->call.args[0], fn, ctx, ctx->current_block_id, 0);
                        iron_strbuf_appendf(sb, ");\n");
                        emit_indent(sb, ind);
                        iron_strbuf_appendf(sb, "for (int64_t _fill_i = 0; _fill_i < ");
                        emit_expr_to_buf(sb, instr->call.args[0], fn, ctx, ctx->current_block_id, 0);
                        iron_strbuf_appendf(sb, "; _fill_i++) ");
                        emit_val(sb, instr->id);
                        iron_strbuf_appendf(sb, "[_fill_i] = ");
                        emit_expr_to_buf(sb, instr->call.args[1], fn, ctx, ctx->current_block_id, 0);
                        iron_strbuf_appendf(sb, ";\n");
                        /* Emit companion length variable */
                        emit_indent(sb, ind);
                        iron_strbuf_appendf(sb, "int64_t ");
                        emit_val(sb, instr->id);
                        iron_strbuf_appendf(sb, "_len = ");
                        emit_expr_to_buf(sb, instr->call.args[0], fn, ctx, ctx->current_block_id, 0);
                        iron_strbuf_appendf(sb, ";\n");
                        /* Mark as stack array */
                        mark_stack_array(ctx, instr->id, instr->id);
                    } else {
                        /* Heap path: create() + push loop */
                        const char *list_type = emit_type_to_c(instr->type, ctx);
                        emit_indent(sb, ind);
                        iron_strbuf_appendf(sb, "%s ", list_type);
                        emit_val(sb, instr->id);
                        iron_strbuf_appendf(sb, " = %s_create();\n", list_type);
                        emit_indent(sb, ind);
                        iron_strbuf_appendf(sb, "for (int64_t _fill_i = 0; _fill_i < ");
                        emit_expr_to_buf(sb, instr->call.args[0], fn, ctx, ctx->current_block_id, 0);
                        iron_strbuf_appendf(sb, "; _fill_i++) {\n");
                        emit_indent(sb, ind + 1);
                        iron_strbuf_appendf(sb, "%s_push(&", list_type);
                        emit_val(sb, instr->id);
                        iron_strbuf_appendf(sb, ", ");
                        emit_expr_to_buf(sb, instr->call.args[1], fn, ctx, ctx->current_block_id, 0);
                        iron_strbuf_appendf(sb, ");\n");
                        emit_indent(sb, ind);
                        iron_strbuf_appendf(sb, "}\n");
                    }
                }
                break;
            }
        }

        /* ── Split collection method dispatch (Phase 47) ────────────────────
         * When a collection method (map, filter, reduce, forEach, sum) is called
         * on a split collection, emit inline per-item iteration using the _order
         * array instead of calling the Iron_List_*_method() C runtime function
         * (which only works on flat arrays). */
        if (ctx->split_collection_ids && instr->call.arg_count >= 1) {
            IronLIR_ValueId self_arg = instr->call.args[0];
            ptrdiff_t sp_idx = hmgeti(ctx->split_collection_ids, self_arg);
            if (sp_idx >= 0) {
                /* Identify which collection method is being called */
                const char *fn_name = NULL;
                IronLIR_ValueId fptr = instr->call.func_ptr;
                if (fptr != IRON_LIR_VALUE_INVALID &&
                    fptr < (IronLIR_ValueId)arrlen(fn->value_table) &&
                    fn->value_table[fptr] != NULL &&
                    fn->value_table[fptr]->kind == IRON_LIR_FUNC_REF) {
                    fn_name = emit_resolve_func_c_name(ctx, fn->value_table[fptr]->func_ref.func_name);
                }
                const char *coll_method = NULL;
                if (fn_name && strncmp(fn_name, "Iron_List_", 10) == 0) {
                    const char *suffix = strrchr(fn_name, '_');
                    if (suffix) {
                        if (strcmp(suffix, "_map") == 0) coll_method = "map";
                        else if (strcmp(suffix, "_filter") == 0) coll_method = "filter";
                        else if (strcmp(suffix, "_reduce") == 0) coll_method = "reduce";
                        else if (strcmp(suffix, "_forEach") == 0) coll_method = "forEach";
                        else if (strcmp(suffix, "_sum") == 0) coll_method = "sum";
                        else if (strcmp(suffix, "_push") == 0) coll_method = "push";   /* Phase 55: PUSH-01 */
                        else if (strcmp(suffix, "_len") == 0) coll_method = "len";     /* Phase 55: PUSH-01 (len) */
                        else if (strcmp(suffix, "_pop") == 0) coll_method = "pop";     /* Phase 55: PUSH-01 (pop) */
                        else if (strcmp(suffix, "_get") == 0) coll_method = "get";     /* Phase 55: PUSH-01 (get) */
                        else if (strcmp(suffix, "_set") == 0) coll_method = "set";     /* Phase 55: PUSH-01 (set) */
                    }
                }
                if (coll_method) {
                    /* Get interface info */
                    const char *sp_iface = ctx->split_collection_ids[sp_idx].value;
                    Iron_IfaceEntry *sp_entry = NULL;
                    if (ctx->iface_reg) {
                        for (int ri = 0; ri < (int)shlen(ctx->iface_reg->map); ri++) {
                            const char *mc2 = emit_mangle_name(
                                ctx->iface_reg->map[ri].value.iface_name, ctx->arena);
                            if (strcmp(mc2, sp_iface) == 0) {
                                sp_entry = &ctx->iface_reg->map[ri].value;
                                break;
                            }
                        }
                    }
                    if (sp_entry) {
                        /* Build the ordered item-fetch switch body.
                         * Each concrete type gets a case that wraps the sub-array item
                         * in the interface tagged union. */
                        if (strcmp(coll_method, "map") == 0 && instr->call.arg_count >= 2) {
                            /* map: create result list, iterate _order, call lambda, push */
                            const char *result_list_type = emit_type_to_c(instr->type, ctx);
                            const char *result_elem_type = (instr->type && instr->type->kind == IRON_TYPE_ARRAY && instr->type->array.elem)
                                ? emit_type_to_c(instr->type->array.elem, ctx) : "int64_t";
                            emit_indent(sb, ind);
                            if (!is_hoisted) iron_strbuf_appendf(sb, "%s ", result_list_type);
                            emit_val(sb, instr->id);
                            iron_strbuf_appendf(sb, " = %s_create();\n", result_list_type);
                            emit_indent(sb, ind);
                            iron_strbuf_appendf(sb, "{\n");
                            emit_indent(sb, ind + 1);
                            iron_strbuf_appendf(sb, "typedef %s (*_SpMapFn)(void *, %s);\n",
                                result_elem_type, sp_iface);
                            emit_indent(sb, ind + 1);
                            iron_strbuf_appendf(sb, "_SpMapFn _sp_map_fn; memcpy(&_sp_map_fn, &");
                            emit_expr_to_buf(sb, instr->call.args[1], fn, ctx, ctx->current_block_id, 0);
                            iron_strbuf_appendf(sb, ".fn, sizeof(_sp_map_fn));\n");
                            emit_indent(sb, ind + 1);
                            iron_strbuf_appendf(sb, "for (int64_t _oi = 0; _oi < ");
                            emit_val(sb, self_arg);
                            iron_strbuf_appendf(sb, "._order_count; _oi++) {\n");
                            /* Build tagged union item from _order */
                            emit_indent(sb, ind + 2);
                            iron_strbuf_appendf(sb, "%s _sp_item;\n", sp_iface);
                            emit_indent(sb, ind + 2);
                            iron_strbuf_appendf(sb, "switch (");
                            emit_val(sb, self_arg);
                            iron_strbuf_appendf(sb, "._order[_oi].tag) {\n");
                            for (int ji = 0; ji < sp_entry->impl_count; ji++) {
                                Iron_IfaceImpl *impl = &sp_entry->impls[ji];
                                if (!impl->is_alive) continue;
                                char lower_name[256];
                                {
                                    size_t nl2 = strlen(impl->type_name);
                                    if (nl2 >= sizeof(lower_name)) nl2 = sizeof(lower_name) - 1;
                                    for (size_t ci3 = 0; ci3 < nl2; ci3++)
                                        lower_name[ci3] = (char)((impl->type_name[ci3] >= 'A' &&
                                                                   impl->type_name[ci3] <= 'Z')
                                            ? impl->type_name[ci3] + 32
                                            : impl->type_name[ci3]);
                                    lower_name[nl2] = '\0';
                                }
                                emit_indent(sb, ind + 3);
                                iron_strbuf_appendf(sb, "case %d: _sp_item = %s_from_%s(",
                                    ji, sp_iface, impl->type_name);
                                emit_val(sb, self_arg);
                                iron_strbuf_appendf(sb, ".%s_items[", lower_name);
                                emit_val(sb, self_arg);
                                iron_strbuf_appendf(sb, "._order[_oi].idx]); break;\n");
                            }
                            emit_indent(sb, ind + 3);
                            iron_strbuf_appendf(sb, "default: memset(&_sp_item, 0, sizeof(_sp_item)); break;\n");
                            emit_indent(sb, ind + 2);
                            iron_strbuf_appendf(sb, "}\n");
                            /* Call lambda and push result */
                            emit_indent(sb, ind + 2);
                            iron_strbuf_appendf(sb, "%s_push(&", result_list_type);
                            emit_val(sb, instr->id);
                            iron_strbuf_appendf(sb, ", _sp_map_fn(");
                            emit_expr_to_buf(sb, instr->call.args[1], fn, ctx, ctx->current_block_id, 0);
                            iron_strbuf_appendf(sb, ".env, _sp_item));\n");
                            emit_indent(sb, ind + 1);
                            iron_strbuf_appendf(sb, "}\n");
                            emit_indent(sb, ind);
                            iron_strbuf_appendf(sb, "}\n");
                            break;
                        } else if (strcmp(coll_method, "filter") == 0 && instr->call.arg_count >= 2) {
                            /* filter: create result split list, iterate _order,
                             * call predicate, conditionally push to result */
                            emit_indent(sb, ind);
                            if (!is_hoisted) iron_strbuf_appendf(sb, "Iron_SplitList_%s ", sp_iface);
                            emit_val(sb, instr->id);
                            iron_strbuf_appendf(sb, " = {0};\n");
                            /* Track result as split collection */
                            hmput(ctx->split_collection_ids, instr->id, sp_iface);
                            emit_indent(sb, ind);
                            iron_strbuf_appendf(sb, "{\n");
                            emit_indent(sb, ind + 1);
                            iron_strbuf_appendf(sb, "typedef bool (*_SpFilterFn)(void *, %s);\n", sp_iface);
                            emit_indent(sb, ind + 1);
                            iron_strbuf_appendf(sb, "_SpFilterFn _sp_filter_fn; memcpy(&_sp_filter_fn, &");
                            emit_expr_to_buf(sb, instr->call.args[1], fn, ctx, ctx->current_block_id, 0);
                            iron_strbuf_appendf(sb, ".fn, sizeof(_sp_filter_fn));\n");
                            emit_indent(sb, ind + 1);
                            iron_strbuf_appendf(sb, "for (int64_t _oi = 0; _oi < ");
                            emit_val(sb, self_arg);
                            iron_strbuf_appendf(sb, "._order_count; _oi++) {\n");
                            /* Build tagged union item */
                            emit_indent(sb, ind + 2);
                            iron_strbuf_appendf(sb, "%s _sp_item;\n", sp_iface);
                            emit_indent(sb, ind + 2);
                            iron_strbuf_appendf(sb, "switch (");
                            emit_val(sb, self_arg);
                            iron_strbuf_appendf(sb, "._order[_oi].tag) {\n");
                            for (int ji = 0; ji < sp_entry->impl_count; ji++) {
                                Iron_IfaceImpl *impl = &sp_entry->impls[ji];
                                if (!impl->is_alive) continue;
                                char lower_name[256];
                                {
                                    size_t nl2 = strlen(impl->type_name);
                                    if (nl2 >= sizeof(lower_name)) nl2 = sizeof(lower_name) - 1;
                                    for (size_t ci3 = 0; ci3 < nl2; ci3++)
                                        lower_name[ci3] = (char)((impl->type_name[ci3] >= 'A' &&
                                                                   impl->type_name[ci3] <= 'Z')
                                            ? impl->type_name[ci3] + 32
                                            : impl->type_name[ci3]);
                                    lower_name[nl2] = '\0';
                                }
                                emit_indent(sb, ind + 3);
                                iron_strbuf_appendf(sb, "case %d: _sp_item = %s_from_%s(",
                                    ji, sp_iface, impl->type_name);
                                emit_val(sb, self_arg);
                                iron_strbuf_appendf(sb, ".%s_items[", lower_name);
                                emit_val(sb, self_arg);
                                iron_strbuf_appendf(sb, "._order[_oi].idx]); break;\n");
                            }
                            emit_indent(sb, ind + 3);
                            iron_strbuf_appendf(sb, "default: break;\n");
                            emit_indent(sb, ind + 2);
                            iron_strbuf_appendf(sb, "}\n");
                            /* Call predicate and conditionally push */
                            emit_indent(sb, ind + 2);
                            iron_strbuf_appendf(sb, "if (_sp_filter_fn(");
                            emit_expr_to_buf(sb, instr->call.args[1], fn, ctx, ctx->current_block_id, 0);
                            iron_strbuf_appendf(sb, ".env, _sp_item)) {\n");
                            emit_indent(sb, ind + 3);
                            iron_strbuf_appendf(sb, "switch (");
                            emit_val(sb, self_arg);
                            iron_strbuf_appendf(sb, "._order[_oi].tag) {\n");
                            for (int ji = 0; ji < sp_entry->impl_count; ji++) {
                                Iron_IfaceImpl *impl = &sp_entry->impls[ji];
                                if (!impl->is_alive) continue;
                                char lower_name[256];
                                {
                                    size_t nl2 = strlen(impl->type_name);
                                    if (nl2 >= sizeof(lower_name)) nl2 = sizeof(lower_name) - 1;
                                    for (size_t ci3 = 0; ci3 < nl2; ci3++)
                                        lower_name[ci3] = (char)((impl->type_name[ci3] >= 'A' &&
                                                                   impl->type_name[ci3] <= 'Z')
                                            ? impl->type_name[ci3] + 32
                                            : impl->type_name[ci3]);
                                    lower_name[nl2] = '\0';
                                }
                                emit_indent(sb, ind + 4);
                                iron_strbuf_appendf(sb, "case %d: Iron_SplitList_%s_push_%s(&",
                                    ji, sp_iface, impl->type_name);
                                emit_val(sb, instr->id);
                                iron_strbuf_appendf(sb, ", ");
                                emit_val(sb, self_arg);
                                iron_strbuf_appendf(sb, ".%s_items[", lower_name);
                                emit_val(sb, self_arg);
                                iron_strbuf_appendf(sb, "._order[_oi].idx]); break;\n");
                            }
                            emit_indent(sb, ind + 3);
                            iron_strbuf_appendf(sb, "}\n");
                            emit_indent(sb, ind + 2);
                            iron_strbuf_appendf(sb, "}\n");
                            emit_indent(sb, ind + 1);
                            iron_strbuf_appendf(sb, "}\n");
                            emit_indent(sb, ind);
                            iron_strbuf_appendf(sb, "}\n");
                            break;
                        } else if (strcmp(coll_method, "reduce") == 0 && instr->call.arg_count >= 3) {
                            /* reduce(init, f): accumulate across all items in order */
                            const char *acc_type = emit_type_to_c(instr->type, ctx);
                            emit_indent(sb, ind);
                            if (!is_hoisted) iron_strbuf_appendf(sb, "%s ", acc_type);
                            emit_val(sb, instr->id);
                            iron_strbuf_appendf(sb, " = ");
                            emit_expr_to_buf(sb, instr->call.args[1], fn, ctx, ctx->current_block_id, 0);
                            iron_strbuf_appendf(sb, ";\n");
                            emit_indent(sb, ind);
                            iron_strbuf_appendf(sb, "{\n");
                            emit_indent(sb, ind + 1);
                            iron_strbuf_appendf(sb, "typedef %s (*_SpReduceFn)(void *, %s, %s);\n",
                                acc_type, acc_type, sp_iface);
                            emit_indent(sb, ind + 1);
                            iron_strbuf_appendf(sb, "_SpReduceFn _sp_reduce_fn; memcpy(&_sp_reduce_fn, &");
                            emit_expr_to_buf(sb, instr->call.args[2], fn, ctx, ctx->current_block_id, 0);
                            iron_strbuf_appendf(sb, ".fn, sizeof(_sp_reduce_fn));\n");
                            emit_indent(sb, ind + 1);
                            iron_strbuf_appendf(sb, "for (int64_t _oi = 0; _oi < ");
                            emit_val(sb, self_arg);
                            iron_strbuf_appendf(sb, "._order_count; _oi++) {\n");
                            /* Build tagged union item */
                            emit_indent(sb, ind + 2);
                            iron_strbuf_appendf(sb, "%s _sp_item;\n", sp_iface);
                            emit_indent(sb, ind + 2);
                            iron_strbuf_appendf(sb, "switch (");
                            emit_val(sb, self_arg);
                            iron_strbuf_appendf(sb, "._order[_oi].tag) {\n");
                            for (int ji = 0; ji < sp_entry->impl_count; ji++) {
                                Iron_IfaceImpl *impl = &sp_entry->impls[ji];
                                if (!impl->is_alive) continue;
                                char lower_name[256];
                                {
                                    size_t nl2 = strlen(impl->type_name);
                                    if (nl2 >= sizeof(lower_name)) nl2 = sizeof(lower_name) - 1;
                                    for (size_t ci3 = 0; ci3 < nl2; ci3++)
                                        lower_name[ci3] = (char)((impl->type_name[ci3] >= 'A' &&
                                                                   impl->type_name[ci3] <= 'Z')
                                            ? impl->type_name[ci3] + 32
                                            : impl->type_name[ci3]);
                                    lower_name[nl2] = '\0';
                                }
                                emit_indent(sb, ind + 3);
                                iron_strbuf_appendf(sb, "case %d: _sp_item = %s_from_%s(",
                                    ji, sp_iface, impl->type_name);
                                emit_val(sb, self_arg);
                                iron_strbuf_appendf(sb, ".%s_items[", lower_name);
                                emit_val(sb, self_arg);
                                iron_strbuf_appendf(sb, "._order[_oi].idx]); break;\n");
                            }
                            emit_indent(sb, ind + 3);
                            iron_strbuf_appendf(sb, "default: memset(&_sp_item, 0, sizeof(_sp_item)); break;\n");
                            emit_indent(sb, ind + 2);
                            iron_strbuf_appendf(sb, "}\n");
                            emit_indent(sb, ind + 2);
                            emit_val(sb, instr->id);
                            iron_strbuf_appendf(sb, " = _sp_reduce_fn(");
                            emit_expr_to_buf(sb, instr->call.args[2], fn, ctx, ctx->current_block_id, 0);
                            iron_strbuf_appendf(sb, ".env, ");
                            emit_val(sb, instr->id);
                            iron_strbuf_appendf(sb, ", _sp_item);\n");
                            emit_indent(sb, ind + 1);
                            iron_strbuf_appendf(sb, "}\n");
                            emit_indent(sb, ind);
                            iron_strbuf_appendf(sb, "}\n");
                            break;
                        } else if (strcmp(coll_method, "forEach") == 0 && instr->call.arg_count >= 2) {
                            /* forEach: iterate _order, call lambda on each item */
                            emit_indent(sb, ind);
                            iron_strbuf_appendf(sb, "{\n");
                            emit_indent(sb, ind + 1);
                            iron_strbuf_appendf(sb, "typedef void (*_SpForEachFn)(void *, %s);\n", sp_iface);
                            emit_indent(sb, ind + 1);
                            iron_strbuf_appendf(sb, "_SpForEachFn _sp_each_fn; memcpy(&_sp_each_fn, &");
                            emit_expr_to_buf(sb, instr->call.args[1], fn, ctx, ctx->current_block_id, 0);
                            iron_strbuf_appendf(sb, ".fn, sizeof(_sp_each_fn));\n");
                            emit_indent(sb, ind + 1);
                            iron_strbuf_appendf(sb, "for (int64_t _oi = 0; _oi < ");
                            emit_val(sb, self_arg);
                            iron_strbuf_appendf(sb, "._order_count; _oi++) {\n");
                            emit_indent(sb, ind + 2);
                            iron_strbuf_appendf(sb, "%s _sp_item;\n", sp_iface);
                            emit_indent(sb, ind + 2);
                            iron_strbuf_appendf(sb, "switch (");
                            emit_val(sb, self_arg);
                            iron_strbuf_appendf(sb, "._order[_oi].tag) {\n");
                            for (int ji = 0; ji < sp_entry->impl_count; ji++) {
                                Iron_IfaceImpl *impl = &sp_entry->impls[ji];
                                if (!impl->is_alive) continue;
                                char lower_name[256];
                                {
                                    size_t nl2 = strlen(impl->type_name);
                                    if (nl2 >= sizeof(lower_name)) nl2 = sizeof(lower_name) - 1;
                                    for (size_t ci3 = 0; ci3 < nl2; ci3++)
                                        lower_name[ci3] = (char)((impl->type_name[ci3] >= 'A' &&
                                                                   impl->type_name[ci3] <= 'Z')
                                            ? impl->type_name[ci3] + 32
                                            : impl->type_name[ci3]);
                                    lower_name[nl2] = '\0';
                                }
                                emit_indent(sb, ind + 3);
                                iron_strbuf_appendf(sb, "case %d: _sp_item = %s_from_%s(",
                                    ji, sp_iface, impl->type_name);
                                emit_val(sb, self_arg);
                                iron_strbuf_appendf(sb, ".%s_items[", lower_name);
                                emit_val(sb, self_arg);
                                iron_strbuf_appendf(sb, "._order[_oi].idx]); break;\n");
                            }
                            emit_indent(sb, ind + 3);
                            iron_strbuf_appendf(sb, "default: break;\n");
                            emit_indent(sb, ind + 2);
                            iron_strbuf_appendf(sb, "}\n");
                            emit_indent(sb, ind + 2);
                            iron_strbuf_appendf(sb, "_sp_each_fn(");
                            emit_expr_to_buf(sb, instr->call.args[1], fn, ctx, ctx->current_block_id, 0);
                            iron_strbuf_appendf(sb, ".env, _sp_item);\n");
                            emit_indent(sb, ind + 1);
                            iron_strbuf_appendf(sb, "}\n");
                            emit_indent(sb, ind);
                            iron_strbuf_appendf(sb, "}\n");
                            break;
                        } else if (strcmp(coll_method, "sum") == 0) {
                            /* sum: not meaningful on interface arrays (no + operator) */
                            /* Emit zero-initialized result */
                            emit_indent(sb, ind);
                            if (!is_hoisted) iron_strbuf_appendf(sb, "%s ", emit_type_to_c(instr->type, ctx));
                            emit_val(sb, instr->id);
                            iron_strbuf_appendf(sb, " = 0;\n");
                            break;
                        } else if (strcmp(coll_method, "push") == 0 && instr->call.arg_count >= 2) {
                            /* Phase 55 / PUSH-01: .push() on interface split collection.
                             *
                             * The HIR->LIR lowering uniformly mangles array method calls to
                             * "Iron_List_<elem>_<method>", producing names like
                             * "Iron_List_Iron_Shape_push" that do not exist for interface
                             * arrays (which lower to Iron_SplitList with per-type push
                             * functions "Iron_SplitList_<Iface>_push_<Type>"). Without this
                             * branch, the call falls through to generic emission and the
                             * bogus name leaks into the generated C as an undeclared function.
                             *
                             * Two dispatch modes:
                             *   (a) pushed value has concrete object type -> emit a direct
                             *       per-type push call. Mirrors the array-literal path at
                             *       emit_c.c:~2575 and the fusion terminal at emit_fusion.c.
                             *   (b) pushed value has interface (tagged union) type -> emit a
                             *       runtime switch over the value's .tag dispatching to the
                             *       matching per-type push, reading the concrete payload out
                             *       of the tagged union's data.<TypeName> member. */
                            IronLIR_ValueId push_val = instr->call.args[1];
                            Iron_Type *et = emit_get_value_type(fn, push_val);
                            if (et && et->kind == IRON_TYPE_OBJECT && et->object.decl) {
                                /* --- Mode (a): concrete object arg --- */
                                emit_indent(sb, ind);
                                iron_strbuf_appendf(sb, "Iron_SplitList_%s_push_%s(&",
                                    sp_iface, et->object.decl->name);
                                emit_val(sb, self_arg);
                                iron_strbuf_appendf(sb, ", ");
                                emit_expr_to_buf(sb, push_val, fn, ctx, ctx->current_block_id, 0);
                                iron_strbuf_appendf(sb, ");\n");
                                break;
                            } else if (et && et->kind == IRON_TYPE_INTERFACE) {
                                /* --- Mode (b): interface-typed arg, runtime tag dispatch --- */
                                emit_indent(sb, ind);
                                iron_strbuf_appendf(sb, "{\n");
                                emit_indent(sb, ind + 1);
                                iron_strbuf_appendf(sb, "%s _sp_push_val = ", sp_iface);
                                emit_expr_to_buf(sb, push_val, fn, ctx, ctx->current_block_id, 0);
                                iron_strbuf_appendf(sb, ";\n");
                                emit_indent(sb, ind + 1);
                                iron_strbuf_appendf(sb, "switch (_sp_push_val.tag) {\n");
                                for (int ji = 0; ji < sp_entry->impl_count; ji++) {
                                    Iron_IfaceImpl *impl = &sp_entry->impls[ji];
                                    if (!impl->is_alive) continue;
                                    /* Honor indirect (pointer-indirected) variants used for
                                     * large payloads. See emit_structs.c:278-311. */
                                    char ikey[512];
                                    snprintf(ikey, sizeof(ikey), "%s:%s",
                                             sp_iface, impl->type_name);
                                    bool is_indirect = (ctx->indirect_variants &&
                                                        shgeti(ctx->indirect_variants, ikey) >= 0);
                                    emit_indent(sb, ind + 2);
                                    iron_strbuf_appendf(sb,
                                        "case %d: Iron_SplitList_%s_push_%s(&",
                                        impl->tag, sp_iface, impl->type_name);
                                    emit_val(sb, self_arg);
                                    iron_strbuf_appendf(sb,
                                        ", %s_sp_push_val.data.%s); break;\n",
                                        is_indirect ? "*" : "",
                                        impl->type_name);
                                }
                                emit_indent(sb, ind + 2);
                                iron_strbuf_appendf(sb, "default: break;\n");
                                emit_indent(sb, ind + 1);
                                iron_strbuf_appendf(sb, "}\n");
                                emit_indent(sb, ind);
                                iron_strbuf_appendf(sb, "}\n");
                                break;
                            }
                            /* Fall-through: pushed value has neither concrete nor interface
                             * type — defer to generic emission (will produce the legacy bogus
                             * name and fail loudly at C compile time, which is strictly
                             * better than silently miscompiling). */
                        } else if (strcmp(coll_method, "len") == 0) {
                            /* Phase 55 / PUSH-01: .len() on interface split collection.
                             *
                             * Returns `_total_count` directly. The Iron_SplitList struct
                             * always emits `_total_count` (see emit_split.c:370), which
                             * is authoritative for the whole collection (sum across all
                             * per-type sub-arrays). No per-type fan-out is required.
                             *
                             * Without this branch, the call falls through to generic
                             * direct-call emission and the bogus
                             * "Iron_List_Iron_<Iface>_len" name leaks into the generated
                             * C as an undeclared function. */
                            emit_indent(sb, ind);
                            if (!is_hoisted) {
                                iron_strbuf_appendf(sb, "%s ",
                                    emit_type_to_c(instr->type, ctx));
                            }
                            emit_val(sb, instr->id);
                            iron_strbuf_appendf(sb, " = ");
                            emit_val(sb, self_arg);
                            iron_strbuf_appendf(sb, "._total_count;\n");
                            break;
                        } else if (strcmp(coll_method, "pop") == 0) {
                            /* Phase 55 / PUSH-01: .pop() on interface split collection.
                             *
                             * Consult _order[_total_count - 1] to recover the
                             * (tag, sub_index) of the most recently pushed element,
                             * dispatch via a tag switch to the matching per-type
                             * sub-array, read the element, wrap it back into the
                             * interface tagged union via Iron_<Iface>_from_<Type>,
                             * decrement per-type count + _order_count + _total_count.
                             *
                             * Scoping (per 55-CONTEXT.md):
                             *   - AoS implementors: full implementation.
                             *   - SoA implementors: SoA sub-arrays are per-field, not
                             *     per-element, so the simple <lower>_items[idx] read
                             *     does not apply. If ANY alive impl is SoA, emit a
                             *     defensive fallback (zero-initialize the result and
                             *     leave a comment) rather than miscompiling.
                             *   - Indirect variants: Iron_<Iface>_from_<Type> handles
                             *     the heap allocation internally, so passing the
                             *     by-value struct read from <lower>_items is correct
                             *     regardless of indirection. */
                            const char *pop_type = emit_type_to_c(instr->type, ctx);

                            /* Check for any SoA implementor — defensive fallback. */
                            bool any_soa = false;
                            for (int ji = 0; ji < sp_entry->impl_count; ji++) {
                                Iron_IfaceImpl *impl = &sp_entry->impls[ji];
                                if (!impl->is_alive) continue;
                                char soa_key_tmp[768];
                                snprintf(soa_key_tmp, sizeof(soa_key_tmp),
                                    "%s:%s", sp_iface, impl->type_name);
                                if (ctx->soa_types &&
                                    shgeti(ctx->soa_types, soa_key_tmp) >= 0) {
                                    any_soa = true;
                                    break;
                                }
                            }

                            emit_indent(sb, ind);
                            if (!is_hoisted) {
                                iron_strbuf_appendf(sb, "%s ", pop_type);
                            }
                            emit_val(sb, instr->id);
                            iron_strbuf_appendf(sb, ";\n");

                            if (any_soa) {
                                /* SoA fallback: known limitation — pop on split
                                 * collections whose implementors use SoA layout is
                                 * not yet supported. Zero-init the result and leave
                                 * the collection state untouched. */
                                emit_indent(sb, ind);
                                iron_strbuf_appendf(sb,
                                    "memset(&");
                                emit_val(sb, instr->id);
                                iron_strbuf_appendf(sb, ", 0, sizeof(");
                                emit_val(sb, instr->id);
                                iron_strbuf_appendf(sb, ")); /* SoA pop not yet supported */\n");
                                break;
                            }

                            emit_indent(sb, ind);
                            iron_strbuf_appendf(sb, "{\n");
                            /* Compute last-index once */
                            emit_indent(sb, ind + 1);
                            iron_strbuf_appendf(sb, "int64_t _sp_pop_i = ");
                            emit_val(sb, self_arg);
                            iron_strbuf_appendf(sb, "._total_count - 1;\n");
                            /* Tag switch over _order[_sp_pop_i].tag */
                            emit_indent(sb, ind + 1);
                            iron_strbuf_appendf(sb, "switch (");
                            emit_val(sb, self_arg);
                            iron_strbuf_appendf(sb, "._order[_sp_pop_i].tag) {\n");
                            for (int ji = 0; ji < sp_entry->impl_count; ji++) {
                                Iron_IfaceImpl *impl = &sp_entry->impls[ji];
                                if (!impl->is_alive) continue;
                                /* Lowercase type name for sub-array field access */
                                char lower_name[256];
                                {
                                    size_t nl2 = strlen(impl->type_name);
                                    if (nl2 >= sizeof(lower_name)) nl2 = sizeof(lower_name) - 1;
                                    for (size_t ci3 = 0; ci3 < nl2; ci3++)
                                        lower_name[ci3] = (char)((impl->type_name[ci3] >= 'A' &&
                                                                   impl->type_name[ci3] <= 'Z')
                                            ? impl->type_name[ci3] + 32
                                            : impl->type_name[ci3]);
                                    lower_name[nl2] = '\0';
                                }
                                emit_indent(sb, ind + 2);
                                iron_strbuf_appendf(sb, "case %d: ", impl->tag);
                                emit_val(sb, instr->id);
                                iron_strbuf_appendf(sb, " = %s_from_%s(",
                                    sp_iface, impl->type_name);
                                emit_val(sb, self_arg);
                                iron_strbuf_appendf(sb, ".%s_items[", lower_name);
                                emit_val(sb, self_arg);
                                iron_strbuf_appendf(sb, "._order[_sp_pop_i].idx]); ");
                                emit_val(sb, self_arg);
                                iron_strbuf_appendf(sb, ".%s_count--; break;\n", lower_name);
                            }
                            emit_indent(sb, ind + 2);
                            iron_strbuf_appendf(sb, "default: memset(&");
                            emit_val(sb, instr->id);
                            iron_strbuf_appendf(sb, ", 0, sizeof(");
                            emit_val(sb, instr->id);
                            iron_strbuf_appendf(sb, ")); break;\n");
                            emit_indent(sb, ind + 1);
                            iron_strbuf_appendf(sb, "}\n");
                            /* Decrement order and total counts */
                            emit_indent(sb, ind + 1);
                            emit_val(sb, self_arg);
                            iron_strbuf_appendf(sb, "._order_count--;\n");
                            emit_indent(sb, ind + 1);
                            emit_val(sb, self_arg);
                            iron_strbuf_appendf(sb, "._total_count--;\n");
                            emit_indent(sb, ind);
                            iron_strbuf_appendf(sb, "}\n");
                            break;
                        } else if (strcmp(coll_method, "get") == 0 && instr->call.arg_count >= 2) {
                            /* Phase 55 / PUSH-01: .get(i) on interface split collection.
                             *
                             * Consult _order[i] to recover the (tag, sub_index) of the
                             * element at logical index i, dispatch via a tag switch to
                             * the matching per-type sub-array, read the element, wrap
                             * it back into the interface tagged union via
                             * Iron_<Iface>_from_<Type>, and return as the result.
                             *
                             * Semantics mirror .pop() except NO count decrement and NO
                             * _order shrink — this is a pure read.
                             *
                             * Scoping (per 55-CONTEXT.md, same as pop):
                             *   - AoS implementors: full implementation.
                             *   - SoA implementors: the simple <lower>_items[idx] read
                             *     does not apply to per-field SoA storage. If ANY alive
                             *     impl is SoA, emit a defensive fallback (zero-init the
                             *     result and leave a comment) rather than miscompiling.
                             *   - Indirect variants: Iron_<Iface>_from_<Type> handles
                             *     the heap allocation internally, so passing the by-value
                             *     struct read from <lower>_items is correct regardless
                             *     of indirection. */
                            IronLIR_ValueId idx_val = instr->call.args[1];
                            const char *get_type = emit_type_to_c(instr->type, ctx);

                            /* Check for any SoA implementor — defensive fallback. */
                            bool any_soa = false;
                            for (int ji = 0; ji < sp_entry->impl_count; ji++) {
                                Iron_IfaceImpl *impl = &sp_entry->impls[ji];
                                if (!impl->is_alive) continue;
                                char soa_key_tmp[768];
                                snprintf(soa_key_tmp, sizeof(soa_key_tmp),
                                    "%s:%s", sp_iface, impl->type_name);
                                if (ctx->soa_types &&
                                    shgeti(ctx->soa_types, soa_key_tmp) >= 0) {
                                    any_soa = true;
                                    break;
                                }
                            }

                            emit_indent(sb, ind);
                            if (!is_hoisted) {
                                iron_strbuf_appendf(sb, "%s ", get_type);
                            }
                            emit_val(sb, instr->id);
                            iron_strbuf_appendf(sb, ";\n");

                            if (any_soa) {
                                /* SoA fallback: known limitation — get on split
                                 * collections whose implementors use SoA layout is
                                 * not yet supported. Zero-init the result. */
                                emit_indent(sb, ind);
                                iron_strbuf_appendf(sb, "memset(&");
                                emit_val(sb, instr->id);
                                iron_strbuf_appendf(sb, ", 0, sizeof(");
                                emit_val(sb, instr->id);
                                iron_strbuf_appendf(sb, ")); /* SoA get not yet supported */\n");
                                break;
                            }

                            emit_indent(sb, ind);
                            iron_strbuf_appendf(sb, "{\n");
                            /* Compute index once */
                            emit_indent(sb, ind + 1);
                            iron_strbuf_appendf(sb, "int64_t _sp_get_i = ");
                            emit_expr_to_buf(sb, idx_val, fn, ctx, ctx->current_block_id, 0);
                            iron_strbuf_appendf(sb, ";\n");
                            /* Tag switch over _order[_sp_get_i].tag */
                            emit_indent(sb, ind + 1);
                            iron_strbuf_appendf(sb, "switch (");
                            emit_val(sb, self_arg);
                            iron_strbuf_appendf(sb, "._order[_sp_get_i].tag) {\n");
                            for (int ji = 0; ji < sp_entry->impl_count; ji++) {
                                Iron_IfaceImpl *impl = &sp_entry->impls[ji];
                                if (!impl->is_alive) continue;
                                /* Lowercase type name for sub-array field access */
                                char lower_name[256];
                                {
                                    size_t nl2 = strlen(impl->type_name);
                                    if (nl2 >= sizeof(lower_name)) nl2 = sizeof(lower_name) - 1;
                                    for (size_t ci3 = 0; ci3 < nl2; ci3++)
                                        lower_name[ci3] = (char)((impl->type_name[ci3] >= 'A' &&
                                                                   impl->type_name[ci3] <= 'Z')
                                            ? impl->type_name[ci3] + 32
                                            : impl->type_name[ci3]);
                                    lower_name[nl2] = '\0';
                                }
                                emit_indent(sb, ind + 2);
                                iron_strbuf_appendf(sb, "case %d: ", impl->tag);
                                emit_val(sb, instr->id);
                                iron_strbuf_appendf(sb, " = %s_from_%s(",
                                    sp_iface, impl->type_name);
                                emit_val(sb, self_arg);
                                iron_strbuf_appendf(sb, ".%s_items[", lower_name);
                                emit_val(sb, self_arg);
                                iron_strbuf_appendf(sb, "._order[_sp_get_i].idx]); break;\n");
                            }
                            emit_indent(sb, ind + 2);
                            iron_strbuf_appendf(sb, "default: memset(&");
                            emit_val(sb, instr->id);
                            iron_strbuf_appendf(sb, ", 0, sizeof(");
                            emit_val(sb, instr->id);
                            iron_strbuf_appendf(sb, ")); break;\n");
                            emit_indent(sb, ind + 1);
                            iron_strbuf_appendf(sb, "}\n");
                            emit_indent(sb, ind);
                            iron_strbuf_appendf(sb, "}\n");
                            break;
                        } else if (strcmp(coll_method, "set") == 0 && instr->call.arg_count >= 3) {
                            /* Phase 55 / PUSH-01: .set(i, v) on interface split collection.
                             *
                             * Semantics (same-type in-place overwrite, per 55-CONTEXT.md
                             * "Claude's Discretion" on set):
                             *
                             *   - If the new value v has a concrete static type T that
                             *     matches the tag of the slot at _order[i], write in
                             *     place into the <lower>_items sub-array at the
                             *     recovered sub-index. Guard the write with a runtime
                             *     tag check so a mismatch (e.g. slot currently holds a
                             *     different concrete type) is a silent no-op rather
                             *     than a stale-cross-type write.
                             *
                             *   - KNOWN LIMITATION: different-type writes (the runtime
                             *     tag check fails) are a silent no-op. Supporting
                             *     cross-type in-place overwrite would require rebuilding
                             *     the _order[] array and shuffling per-type sub-arrays,
                             *     which is non-trivial. Document and defer to a future
                             *     phase if users request it.
                             *
                             *   - KNOWN LIMITATION: interface-typed new values
                             *     (v has IRON_TYPE_INTERFACE, e.g. the result of a
                             *     function returning Shape) are a silent no-op with a
                             *     C comment. Same reasoning — would require per-case
                             *     tag dispatch on the write side, deferred.
                             *
                             * Scoping (per 55-CONTEXT.md, same as pop/get):
                             *   - AoS implementors: full same-type in-place write.
                             *   - SoA implementors: no-op with a comment.
                             *
                             * The guarded write approach is strictly simpler and safer
                             * than runtime assertions — users who hit the limitation
                             * see the collection remain unchanged rather than a crash. */
                            IronLIR_ValueId idx_val = instr->call.args[1];
                            IronLIR_ValueId set_val = instr->call.args[2];
                            Iron_Type *vt = emit_get_value_type(fn, set_val);

                            /* Check for any SoA implementor — defensive fallback. */
                            bool any_soa = false;
                            for (int ji = 0; ji < sp_entry->impl_count; ji++) {
                                Iron_IfaceImpl *impl = &sp_entry->impls[ji];
                                if (!impl->is_alive) continue;
                                char soa_key_tmp[768];
                                snprintf(soa_key_tmp, sizeof(soa_key_tmp),
                                    "%s:%s", sp_iface, impl->type_name);
                                if (ctx->soa_types &&
                                    shgeti(ctx->soa_types, soa_key_tmp) >= 0) {
                                    any_soa = true;
                                    break;
                                }
                            }

                            if (any_soa) {
                                /* SoA fallback: known limitation. */
                                emit_indent(sb, ind);
                                iron_strbuf_appendf(sb,
                                    "/* Phase 55: .set() on split collection with SoA "
                                    "implementor — not yet supported, no-op */\n");
                                break;
                            }

                            if (vt && vt->kind == IRON_TYPE_OBJECT && vt->object.decl) {
                                /* Find the tag for this concrete type among alive impls. */
                                int target_tag = -1;
                                Iron_IfaceImpl *target_impl = NULL;
                                for (int ji = 0; ji < sp_entry->impl_count; ji++) {
                                    Iron_IfaceImpl *impl = &sp_entry->impls[ji];
                                    if (!impl->is_alive) continue;
                                    if (strcmp(impl->type_name,
                                              vt->object.decl->name) == 0) {
                                        target_tag = impl->tag;
                                        target_impl = impl;
                                        break;
                                    }
                                }
                                if (target_tag >= 0 && target_impl != NULL) {
                                    /* Lowercase type name for sub-array field access */
                                    char lower_name[256];
                                    {
                                        size_t nl2 = strlen(target_impl->type_name);
                                        if (nl2 >= sizeof(lower_name)) nl2 = sizeof(lower_name) - 1;
                                        for (size_t ci3 = 0; ci3 < nl2; ci3++)
                                            lower_name[ci3] = (char)((target_impl->type_name[ci3] >= 'A' &&
                                                                       target_impl->type_name[ci3] <= 'Z')
                                                ? target_impl->type_name[ci3] + 32
                                                : target_impl->type_name[ci3]);
                                        lower_name[nl2] = '\0';
                                    }
                                    emit_indent(sb, ind);
                                    iron_strbuf_appendf(sb, "{\n");
                                    emit_indent(sb, ind + 1);
                                    iron_strbuf_appendf(sb, "int64_t _sp_set_i = ");
                                    emit_expr_to_buf(sb, idx_val, fn, ctx, ctx->current_block_id, 0);
                                    iron_strbuf_appendf(sb, ";\n");
                                    /* Guarded write: slot's current tag must match the
                                     * concrete type of the new value. Different-type
                                     * writes fall through to the else-arm (no-op). */
                                    emit_indent(sb, ind + 1);
                                    iron_strbuf_appendf(sb, "if (");
                                    emit_val(sb, self_arg);
                                    iron_strbuf_appendf(sb,
                                        "._order[_sp_set_i].tag == %d) {\n", target_tag);
                                    emit_indent(sb, ind + 2);
                                    emit_val(sb, self_arg);
                                    iron_strbuf_appendf(sb, ".%s_items[", lower_name);
                                    emit_val(sb, self_arg);
                                    iron_strbuf_appendf(sb, "._order[_sp_set_i].idx] = ");
                                    emit_expr_to_buf(sb, set_val, fn, ctx, ctx->current_block_id, 0);
                                    iron_strbuf_appendf(sb, ";\n");
                                    emit_indent(sb, ind + 1);
                                    iron_strbuf_appendf(sb, "}\n");
                                    /* Different-type set is a no-op: known limitation. */
                                    emit_indent(sb, ind + 1);
                                    iron_strbuf_appendf(sb,
                                        "/* else: different-type .set() is a no-op in "
                                        "Phase 55 (known limitation) */\n");
                                    emit_indent(sb, ind);
                                    iron_strbuf_appendf(sb, "}\n");
                                    break;
                                }
                                /* Concrete type is not an alive implementor of this
                                 * interface — fall through to the generic no-op comment. */
                            }
                            /* Interface-typed new value, or unresolvable type:
                             * known limitation, silent no-op with C comment. */
                            emit_indent(sb, ind);
                            iron_strbuf_appendf(sb,
                                "/* Phase 55: .set() with non-concrete or non-matching "
                                "type — no-op (known limitation) */\n");
                            break;
                        }
                    }
                }
            }
        }

        bool is_void = (instr->type == NULL ||
                        instr->type->kind == IRON_TYPE_VOID);

        emit_indent(sb, ind);
        if (!is_void) {
            if (!is_hoisted) {
                iron_strbuf_appendf(sb, "%s ", emit_type_to_c(instr->type, ctx));
            }
            emit_val(sb, instr->id);
            iron_strbuf_appendf(sb, " = ");
        }

        bool has_env_arg = false;  /* set true when closure .env is emitted as first arg */
        if (instr->call.func_decl) {
            /* Direct call: use the mangled function name */
            Iron_FuncDecl *fd = instr->call.func_decl;
            if (fd->is_extern && fd->extern_c_name) {
                iron_strbuf_appendf(sb, "%s(", fd->extern_c_name);
            } else {
                iron_strbuf_appendf(sb, "%s(", emit_mangle_func_name(fd->name, ctx->arena));
            }
        } else {
            /* Indirect call: check if func_ptr is a FUNC_REF — if so, emit
             * a direct call to avoid the invalid (void (*)(...)) cast pattern. */
            IronLIR_ValueId fptr = instr->call.func_ptr;
            bool emitted_direct = false;
            if (fptr != IRON_LIR_VALUE_INVALID &&
                fptr < (IronLIR_ValueId)arrlen(fn->value_table) &&
                fn->value_table[fptr] != NULL &&
                fn->value_table[fptr]->kind == IRON_LIR_FUNC_REF) {
                /* Direct call via known function name — honor extern_c_name */
                const char *c_name = emit_resolve_func_c_name(
                    ctx, fn->value_table[fptr]->func_ref.func_name);
                iron_strbuf_appendf(sb, "%s(", c_name);
                emitted_direct = true;
            }
            if (!emitted_direct) {
                /* True indirect call through an Iron_Closure value.
                 * Dispatch through .fn with .env as the first argument.
                 * All IRON_TYPE_FUNC values are now Iron_Closure fat pointers. */
                const char *ret_c = (instr->type && instr->type->kind != IRON_TYPE_VOID)
                    ? emit_type_to_c(instr->type, ctx)
                    : "void";
                /* Check if fptr value has IRON_TYPE_FUNC type (Iron_Closure).
                 * If so, dispatch through .fn field passing .env as first arg.
                 * fptr_instr may be NULL for synthetic param values — check the
                 * function's param type array in that case. */
                bool is_closure_call = false;
                if (fptr != IRON_LIR_VALUE_INVALID &&
                    fptr < (IronLIR_ValueId)arrlen(fn->value_table)) {
                    IronLIR_Instr *fptr_instr = fn->value_table[fptr];
                    if (fptr_instr && fptr_instr->type &&
                        fptr_instr->type->kind == IRON_TYPE_FUNC) {
                        is_closure_call = true;
                    }
                    /* Synthetic param values have NULL backing instruction.
                     * Param value IDs are assigned sequentially: param 0 → ID 1,
                     * param 1 → ID 2, etc. Check the function's param type array. */
                    if (!fptr_instr && fn->params &&
                        fptr >= 1 && fptr <= (IronLIR_ValueId)fn->param_count) {
                        int pi = (int)(fptr - 1);
                        if (fn->params[pi].type &&
                            fn->params[pi].type->kind == IRON_TYPE_FUNC) {
                            is_closure_call = true;
                        }
                    }
                }
                if (is_closure_call) {
                    /* All lifted lambda functions use a uniform calling convention:
                     * fn(void *_env, arg0, arg1, ...). Always pass closure.env as
                     * the first argument, regardless of capture count. Non-capturing
                     * closures have env=NULL and their lifted function ignores it. */
                    bool needs_env_arg = false;
                    if (fptr != IRON_LIR_VALUE_INVALID &&
                        fptr < (IronLIR_ValueId)arrlen(fn->value_table)) {
                        IronLIR_Instr *fptr_instr = fn->value_table[fptr];
                        if (fptr_instr && fptr_instr->kind == IRON_LIR_MAKE_CLOSURE) {
                            /* All closures (capturing or not) use env-first convention */
                            needs_env_arg = true;
                        }
                        /* Also check if we loaded a MAKE_CLOSURE value via LOAD or
                         * retrieved one via GET_FIELD. All Iron_Closure values carry
                         * a .env field and all lambda functions accept void* as their
                         * first argument. Always dispatch through .fn(.env, ...). */
                        if (!needs_env_arg && fptr_instr &&
                            (fptr_instr->kind == IRON_LIR_LOAD ||
                             fptr_instr->kind == IRON_LIR_GET_FIELD)) {
                            needs_env_arg = true;
                        }
                        /* Synthetic param values (NULL backing instruction) are
                         * Iron_Closure parameters — always pass .env since the caller
                         * may supply a capturing or non-capturing closure. */
                        if (!needs_env_arg && !fptr_instr) {
                            needs_env_arg = true;
                        }
                    }

                    /* Re-lookup fptr_instr for direct call resolution */
                    IronLIR_Instr *fptr_instr2 = (fptr != IRON_LIR_VALUE_INVALID &&
                        fptr < (IronLIR_ValueId)arrlen(fn->value_table))
                        ? fn->value_table[fptr] : NULL;

                    if (needs_env_arg) {
                        /* Capturing closure: dispatch through .fn with .env as first arg.
                         * Using (void*, ...) allows extra typed arguments.
                         * Use emit_expr_to_buf for fptr so inline-eligible values
                         * (e.g. GET_FIELD of a func field) are emitted correctly. */
                        iron_strbuf_appendf(sb, "((%s (*)(void*, ...))", ret_c);
                        emit_expr_to_buf(sb, fptr, fn, ctx, ctx->current_block_id, 0);
                        iron_strbuf_appendf(sb, ".fn)(");
                        emit_expr_to_buf(sb, fptr, fn, ctx, ctx->current_block_id, 0);
                        iron_strbuf_appendf(sb, ".env");
                        /* .env is already emitted as first arg; subsequent args get comma prefix */
                        has_env_arg = true;
                    } else {
                        /* Non-capturing closure: call the lifted function directly
                         * by name to avoid unprototyped cast warnings. */
                        const char *lifted_name = NULL;
                        if (fptr_instr2 && fptr_instr2->kind == IRON_LIR_MAKE_CLOSURE) {
                            lifted_name = fptr_instr2->make_closure.lifted_func_name;
                        }
                        if (lifted_name) {
                            const char *c_name = emit_mangle_func_name(lifted_name, ctx->arena);
                            iron_strbuf_appendf(sb, "%s(", c_name);
                        } else {
                            /* Unknown callee — use void* cast as fallback */
                            iron_strbuf_appendf(sb, "((void (*)(void))");
                            emit_val(sb, fptr);
                            iron_strbuf_appendf(sb, ".fn)(");
                        }
                    }
                } else {
                    /* Fallback: direct call without prototype */
                    iron_strbuf_appendf(sb, "((void (*)(void))");
                    emit_val(sb, fptr);
                    iron_strbuf_appendf(sb, ")(");
                }
            }
        }

        /* Determine if this is a true C extern call — only those need Iron_String
         * arguments converted to const char* via iron_string_cstr().
         * Stdlib stubs (extern but no extern_c_name) expect Iron_String directly. */
        bool is_extern_call = false;
        if (instr->call.func_decl && instr->call.func_decl->is_extern &&
            instr->call.func_decl->extern_c_name) {
            is_extern_call = true;
        } else if (!instr->call.func_decl) {
            /* Indirect call: check if the FUNC_REF target is extern */
            IronLIR_ValueId fptr = instr->call.func_ptr;
            if (fptr != IRON_LIR_VALUE_INVALID &&
                fptr < (IronLIR_ValueId)arrlen(fn->value_table) &&
                fn->value_table[fptr] != NULL &&
                fn->value_table[fptr]->kind == IRON_LIR_FUNC_REF) {
                const char *ref_name = fn->value_table[fptr]->func_ref.func_name;
                for (int fi = 0; fi < ctx->module->func_count; fi++) {
                    if (strcmp(ctx->module->funcs[fi]->name, ref_name) == 0 &&
                        ctx->module->funcs[fi]->is_extern &&
                        ctx->module->funcs[fi]->extern_c_name) {
                        is_extern_call = true;
                        break;
                    }
                }
            }
        }

        /* PARAM-01/02: Resolve callee IR name for pointer-mode check */
        const char *callee_ir_name = NULL;
        if (instr->call.func_decl && !instr->call.func_decl->is_extern) {
            callee_ir_name = instr->call.func_decl->name;
        } else if (!instr->call.func_decl) {
            IronLIR_ValueId fptr2 = instr->call.func_ptr;
            if (fptr2 != IRON_LIR_VALUE_INVALID &&
                fptr2 < (IronLIR_ValueId)arrlen(fn->value_table) &&
                fn->value_table[fptr2] != NULL &&
                fn->value_table[fptr2]->kind == IRON_LIR_FUNC_REF) {
                const char *rn2 = fn->value_table[fptr2]->func_ref.func_name;
                IronLIR_Func *cf = emit_find_ir_func(ctx, rn2);
                if (cf && !cf->is_extern) callee_ir_name = rn2;
            }
        }

        /* Receiver ABI: the HIR→LIR lowering layer sets `self_by_addr` on
         * calls whose target C function takes its first argument by pointer
         * (collection method calls, mutating Timer methods). The old code
         * here used a prefix-strncmp heuristic on the C name — that was a
         * footgun because any future extern matching the prefix but taking
         * a value receiver would silently miscompile. Metadata on the LIR
         * call instruction is authoritative. See lir.h:IRON_LIR_CALL.
         * self_by_addr + hir_to_lir.c for the two population sites. */
        bool self_by_addr = instr->call.self_by_addr;

        bool first_arg = !has_env_arg;  /* false when .env was already emitted */
        for (int i = 0; i < instr->call.arg_count; i++) {
            if (!first_arg) iron_strbuf_appendf(sb, ", ");
            first_arg = false;
            IronLIR_ValueId arg_id = instr->call.args[i];

            /* Pointer-receiver self: emit &arg for the first argument */
            if (self_by_addr && i == 0) {
                iron_strbuf_appendf(sb, "&");
                emit_expr_to_buf(sb, arg_id, fn, ctx, ctx->current_block_id, 0);
                continue;
            }

            /* PARAM-01/02: Check if callee expects pointer+length */
            ArrayParamMode callee_pmode = ARRAY_PARAM_LIST;
            if (callee_ir_name)
                callee_pmode = emit_get_array_param_mode(ctx, callee_ir_name, i);

            if (callee_pmode == ARRAY_PARAM_CONST_PTR ||
                callee_pmode == ARRAY_PARAM_MUT_PTR) {
                IronLIR_ValueId sa_origin = get_stack_array_origin(ctx, arg_id);
                if (sa_origin != IRON_LIR_VALUE_INVALID) {
                    /* Stack array: pass pointer + companion length */
                    emit_val(sb, arg_id);
                    iron_strbuf_appendf(sb, ", ");
                    emit_val(sb, arg_id);
                    iron_strbuf_appendf(sb, "_len");
                } else {
                    /* Iron_List_T: extract .items and .count */
                    emit_val(sb, arg_id);
                    iron_strbuf_appendf(sb, ".items, ");
                    emit_val(sb, arg_id);
                    iron_strbuf_appendf(sb, ".count");
                }
                continue;
            }

            /* For extern calls, apply FFI type conversions:
             * - Iron_String → const char* via iron_string_cstr()
             * - int64_t → int via (int) cast when extern param is Int
             * - Float64 → float via (float) cast when extern param is Float32
             * This bridges Iron's uniform types to C's native types.
             *
             * PROT-02 / Phase 66 Plan 02 Step 5: rewritten from an if-chain
             * on arg_type->kind to an exhaustive switch under the
             * -Werror=switch-enum strictness posture. Semantics are
             * byte-for-byte identical to the pre-switch form. */
            bool is_string_arg = false;
            bool needs_int_cast = false;
            bool needs_float_cast = false;
            if (is_extern_call && arg_id != IRON_LIR_VALUE_INVALID &&
                arg_id < (IronLIR_ValueId)arrlen(fn->value_table) &&
                fn->value_table[arg_id] != NULL) {
                Iron_Type *arg_type = fn->value_table[arg_id]->type;

                /* Resolve the extern's corresponding C parameter type, if any. */
                Iron_Type *pt = NULL;
                if (arg_type) {
                    const char *ext_name = NULL;
                    if (instr->call.func_decl && instr->call.func_decl->extern_c_name)
                        ext_name = instr->call.func_decl->name;
                    if (ext_name) {
                        for (int ei = 0; ei < ctx->module->extern_decl_count; ei++) {
                            IronLIR_ExternDecl *ed = ctx->module->extern_decls[ei];
                            if (strcmp(ed->iron_name, ext_name) == 0 &&
                                i < ed->param_count && ed->param_types[i]) {
                                pt = ed->param_types[i];
                                break;
                            }
                        }
                    }
                }

                if (arg_type) {
                    switch ((int)(arg_type->kind)) {
                        case IRON_TYPE_STRING:
                            is_string_arg = true;
                            break;
                        case IRON_TYPE_INT:
                        case IRON_TYPE_INT64:
                            if (pt && (pt->kind == IRON_TYPE_INT ||
                                       pt->kind == IRON_TYPE_INT32)) {
                                needs_int_cast = true;
                            }
                            break;
                        case IRON_TYPE_FLOAT64:
                            if (pt && pt->kind == IRON_TYPE_FLOAT32) {
                                needs_float_cast = true;
                            }
                            break;
                        /* -Wswitch-enum opt-out: the FFI cast bridge only
                         * rewrites STRING, INT/INT64, and FLOAT64 arguments.
                         * Every other Iron_TypeKind (OBJECT, INTERFACE,
                         * ENUM, ARRAY, BOOL, FLOAT32, int/uint variants
                         * other than INT/INT64, etc.) passes through to the
                         * generic emit path below unchanged — the Iron
                         * runtime value already has the same layout as the
                         * corresponding extern C parameter type. */
                        default:
                            break;
                    }
                }
            }
            if (is_string_arg) {
                iron_strbuf_appendf(sb, "iron_string_cstr(&");
                emit_expr_to_buf(sb, arg_id, fn, ctx, ctx->current_block_id, 0);
                iron_strbuf_appendf(sb, ")");
            } else if (needs_int_cast) {
                iron_strbuf_appendf(sb, "(int)(");
                emit_expr_to_buf(sb, arg_id, fn, ctx, ctx->current_block_id, 0);
                iron_strbuf_appendf(sb, ")");
            } else if (needs_float_cast) {
                iron_strbuf_appendf(sb, "(float)(");
                emit_expr_to_buf(sb, arg_id, fn, ctx, ctx->current_block_id, 0);
                iron_strbuf_appendf(sb, ")");
            } else {
                /* Heap-pointer deref: if the argument is a heap/rc pointer but the
                 * callee's corresponding parameter expects a value type (IRON_TYPE_OBJECT),
                 * dereference the pointer so the value is passed by value as expected. */
                bool needs_deref = false;
                if (emit_val_is_heap_ptr(fn, arg_id) && callee_ir_name) {
                    IronLIR_Func *callee_fn = emit_find_ir_func(ctx, callee_ir_name);
                    if (callee_fn && i < callee_fn->param_count) {
                        Iron_Type *param_t = callee_fn->params[i].type;
                        if (param_t && param_t->kind == IRON_TYPE_OBJECT) {
                            needs_deref = true;
                        }
                    }
                }
                if (needs_deref) {
                    iron_strbuf_appendf(sb, "(*");
                    emit_expr_to_buf(sb, arg_id, fn, ctx, ctx->current_block_id, 0);
                    iron_strbuf_appendf(sb, ")");
                } else {
                    /* Interface wrapping: if arg is concrete object and the
                     * callee parameter expects an interface type, wrap it */
                    Iron_Type *arg_t2 = (arg_id < (IronLIR_ValueId)arrlen(fn->value_table) &&
                                         fn->value_table[arg_id])
                                        ? fn->value_table[arg_id]->type : NULL;
                    bool wrapped = false;
                    if (arg_t2 && arg_t2->kind == IRON_TYPE_OBJECT &&
                        arg_t2->object.decl && ctx->iface_reg) {
                        /* Check callee parameter type — look up the IR function */
                        Iron_Type *param_iface_t = NULL;
                        if (callee_ir_name) {
                            IronLIR_Func *callee_fn2 = emit_find_ir_func(ctx, callee_ir_name);
                            if (callee_fn2 && i < callee_fn2->param_count) {
                                Iron_Type *pt2 = callee_fn2->params[i].type;
                                if (pt2 && pt2->kind == IRON_TYPE_INTERFACE &&
                                    pt2->interface.decl) {
                                    param_iface_t = pt2;
                                }
                            }
                        }
                        /* Also check dispatch function name pattern */
                        if (!param_iface_t) {
                            const char *callee_resolved = NULL;
                            IronLIR_ValueId fp = instr->call.func_ptr;
                            if (fp != IRON_LIR_VALUE_INVALID &&
                                fp < (IronLIR_ValueId)arrlen(fn->value_table) &&
                                fn->value_table[fp] &&
                                fn->value_table[fp]->kind == IRON_LIR_FUNC_REF) {
                                callee_resolved = emit_resolve_func_c_name(ctx,
                                    fn->value_table[fp]->func_ref.func_name);
                            }
                            if (callee_resolved) {
                                for (int ri2 = 0; ri2 < (int)shlen(ctx->iface_reg->map); ri2++) {
                                    Iron_IfaceEntry *ent = &ctx->iface_reg->map[ri2].value;
                                    size_t nl = strlen(ent->iface_name);
                                    char *lp = (char *)iron_arena_alloc(ctx->arena, 5 + nl + 1, 1);
                                    if (!lp) iron_oom_abort("emit_c.c:emit_instr iface_lower");
                                    memcpy(lp, "Iron_", 5);
                                    for (size_t ci2 = 0; ci2 < nl; ci2++) {
                                        char ch = ent->iface_name[ci2];
                                        lp[5+ci2] = (ch >= 'A' && ch <= 'Z')
                                            ? (char)(ch + ('a' - 'A')) : ch;
                                    }
                                    lp[5+nl] = '\0';
                                    if (strncmp(callee_resolved, lp, 5+nl) == 0 &&
                                        callee_resolved[5+nl] == '_') {
                                        param_iface_t = ent->iface_type;
                                        if (!param_iface_t && ent->iface_decl) {
                                            /* Use interface name to find the type */
                                            param_iface_t = (Iron_Type *)1; /* sentinel */
                                        }
                                        break;
                                    }
                                }
                            }
                        }
                        if (param_iface_t) {
                            /* Find which interface this object implements */
                            for (int ri2 = 0; ri2 < (int)shlen(ctx->iface_reg->map); ri2++) {
                                Iron_IfaceEntry *ent = &ctx->iface_reg->map[ri2].value;
                                for (int ji2 = 0; ji2 < ent->impl_count; ji2++) {
                                    if (strcmp(ent->impls[ji2].type_name,
                                              arg_t2->object.decl->name) == 0) {
                                        const char *im2 = emit_mangle_name(ent->iface_name, ctx->arena);
                                        iron_strbuf_appendf(sb, "%s_from_%s(",
                                            im2, arg_t2->object.decl->name);
                                        emit_expr_to_buf(sb, arg_id, fn, ctx,
                                                         ctx->current_block_id, 0);
                                        iron_strbuf_appendf(sb, ")");
                                        wrapped = true;
                                        break;
                                    }
                                }
                                if (wrapped) break;
                            }
                        }
                    }
                    if (!wrapped) {
                        emit_expr_to_buf(sb, arg_id, fn, ctx, ctx->current_block_id, 0);
                    }
                }
            }
        }
        iron_strbuf_appendf(sb, ");\n");
        break;
    }

    /* ── Control flow ───────────────────────────────────────────────────── */

    case IRON_LIR_JUMP:
        emit_indent(sb, ind);
        iron_strbuf_appendf(sb, "goto %s;\n",
                            emit_resolve_label(fn, instr->jump.target, ctx->arena));
        break;

    case IRON_LIR_BRANCH:
        emit_indent(sb, ind);
        iron_strbuf_appendf(sb, "if (");
        emit_expr_to_buf(sb, instr->branch.cond, fn, ctx, ctx->current_block_id, 0);
        iron_strbuf_appendf(sb, ") goto %s; else goto %s;\n",
                            emit_resolve_label(fn, instr->branch.then_block, ctx->arena),
                            emit_resolve_label(fn, instr->branch.else_block, ctx->arena));
        break;

    case IRON_LIR_SWITCH: {
        emit_indent(sb, ind);
        iron_strbuf_appendf(sb, "switch (");
        emit_expr_to_buf(sb, instr->sw.subject, fn, ctx, ctx->current_block_id, 0);
        iron_strbuf_appendf(sb, ") {\n");
        for (int i = 0; i < instr->sw.case_count; i++) {
            emit_indent(sb, ind + 1);
            iron_strbuf_appendf(sb, "case %d: goto %s;\n",
                                instr->sw.case_values[i],
                                emit_resolve_label(fn, instr->sw.case_blocks[i], ctx->arena));
        }
        emit_indent(sb, ind + 1);
        iron_strbuf_appendf(sb, "default: goto %s;\n",
                            emit_resolve_label(fn, instr->sw.default_block, ctx->arena));
        emit_indent(sb, ind);
        iron_strbuf_appendf(sb, "}\n");
        break;
    }

    case IRON_LIR_RETURN: {
        /* COLL-04: Emit _free() for non-escaping heap arrays before return.
         * We iterate over all unique original ARRAY_LIT ids tracked in
         * heap_array_ids and free those that haven't escaped. */
        if (ctx->opt_info->heap_array_ids) {
            /* Collect unique original ARRAY_LIT ids to avoid double-free */
            struct { IronLIR_ValueId key; bool value; } *freed = NULL;
            for (ptrdiff_t hi = 0; hi < hmlen(ctx->opt_info->heap_array_ids); hi++) {
                IronLIR_ValueId orig = ctx->opt_info->heap_array_ids[hi].value;
                /* Skip if already freed, if this array escapes, or if it's a stack array */
                if (hmgeti(freed, orig) >= 0) continue;
                if (hmgeti(ctx->opt_info->escaped_heap_ids, orig) >= 0) continue;
                if (hmgeti(ctx->opt_info->stack_array_ids, orig) >= 0) continue;
                hmput(freed, orig, true);

                /* Look up the original instruction to get the list type */
                if (orig < (IronLIR_ValueId)arrlen(fn->value_table) &&
                    fn->value_table[orig] != NULL) {
                    IronLIR_Instr *orig_instr = fn->value_table[orig];
                    /* (debug removed) */
                    const char *list_type = NULL;
                    if (orig_instr->kind == IRON_LIR_ARRAY_LIT) {
                        Iron_Type *arr_type = iron_type_make_array(
                            ctx->arena, orig_instr->array_lit.elem_type, -1);
                        list_type = emit_type_to_c(arr_type, ctx);
                    } else if (orig_instr->type &&
                               orig_instr->type->kind == IRON_TYPE_ARRAY) {
                        /* __builtin_fill result */
                        list_type = emit_type_to_c(orig_instr->type, ctx);
                    }
                    if (list_type &&
                        get_stack_array_origin(ctx, orig) == IRON_LIR_VALUE_INVALID) {
                        emit_indent(sb, ind);
                        iron_strbuf_appendf(sb, "%s_free(&_v%u);\n",
                                            list_type, (unsigned)orig);
                    }
                }
            }
            hmfree(freed);
        }

        /* Phase 38: Free recursive ADT locals that are NOT the returned value */
        if (ctx->adt_boxed_allocas) {
            IronLIR_ValueId ret_alloca = IRON_LIR_VALUE_INVALID;
            /* Identify which alloca holds the returned value (chase LOAD -> alloca) */
            if (!instr->ret.is_void) {
                IronLIR_ValueId rv = instr->ret.value;
                if (rv != IRON_LIR_VALUE_INVALID &&
                    rv < (IronLIR_ValueId)arrlen(fn->value_table) &&
                    fn->value_table[rv] != NULL &&
                    fn->value_table[rv]->kind == IRON_LIR_LOAD) {
                    ret_alloca = fn->value_table[rv]->load.ptr;
                }
            }
            for (ptrdiff_t ai = 0; ai < hmlen(ctx->adt_boxed_allocas); ai++) {
                IronLIR_ValueId alloca_id = ctx->adt_boxed_allocas[ai].key;
                Iron_Type *atype = ctx->adt_boxed_allocas[ai].value;
                /* Skip the alloca that holds the returned value */
                if (alloca_id == ret_alloca) continue;
                const char *atype_c = emit_type_to_c(atype, ctx);
                emit_indent(sb, ind);
                iron_strbuf_appendf(sb, "%s_free(&_v%u);\n",
                                    atype_c, (unsigned)alloca_id);
            }
        }

        emit_indent(sb, ind);
        if (instr->ret.is_void) {
            iron_strbuf_appendf(sb, "return;\n");
        } else {
            /* Check if return needs interface wrapping:
             * function returns interface, value is concrete */
            bool ret_wrapped = false;
            if (fn->return_type && fn->return_type->kind == IRON_TYPE_INTERFACE &&
                fn->return_type->interface.decl && ctx->iface_reg) {
                IronLIR_ValueId rv = instr->ret.value;
                Iron_Type *rv_type = (rv < (IronLIR_ValueId)arrlen(fn->value_table) &&
                                      fn->value_table[rv])
                                     ? fn->value_table[rv]->type : NULL;
                if (rv_type && rv_type->kind == IRON_TYPE_OBJECT && rv_type->object.decl) {
                    const char *iface_m = emit_mangle_name(
                        fn->return_type->interface.decl->name, ctx->arena);
                    iron_strbuf_appendf(sb, "return %s_from_%s(",
                        iface_m, rv_type->object.decl->name);
                    emit_expr_to_buf(sb, instr->ret.value, fn, ctx, ctx->current_block_id, 0);
                    iron_strbuf_appendf(sb, ");\n");
                    ret_wrapped = true;
                }
            }
            if (!ret_wrapped) {
                iron_strbuf_appendf(sb, "return ");
                emit_expr_to_buf(sb, instr->ret.value, fn, ctx, ctx->current_block_id, 0);
                iron_strbuf_appendf(sb, ";\n");
            }
        }
        break;
    }

    /* ── Cast ───────────────────────────────────────────────────────────── */

    case IRON_LIR_CAST: {
        const char *target_c = emit_type_to_c(instr->cast.target_type, ctx);
        emit_indent(sb, ind);
        if (!is_hoisted) iron_strbuf_appendf(sb, "%s ", target_c);
        emit_val(sb, instr->id);
        iron_strbuf_appendf(sb, " = (%s)", target_c);
        emit_expr_to_buf(sb, instr->cast.value, fn, ctx, ctx->current_block_id, 0);
        iron_strbuf_appendf(sb, ";\n");
        break;
    }

    /* ── Memory management ──────────────────────────────────────────────── */

    case IRON_LIR_HEAP_ALLOC: {
        /* The inner_val is an already-constructed value; wrap it in a pointer.
         * instr->type is the inner (value) type, e.g. Iron_Data.
         * The heap result is a pointer: Iron_Data *_vN = malloc(sizeof(Iron_Data));
         * *_vN = inner_val;
         *
         * FIX-01 rank 3 (Phase 67-02): pre-Phase-67 ironc emitted a bare
         * `T *_vN = malloc(...); *_vN = val;` sequence with zero NULL check,
         * so every compiled Iron program that used `heap` silently segfaulted
         * under OOM. The guard below routes OOM into iron_oom_abort which
         * prints a bisectable diagnostic before abort(3). */
        const char *val_type = emit_type_to_c(instr->type, ctx);
        emit_indent(sb, ind);
        iron_strbuf_appendf(sb, "%s *", val_type);
        emit_val(sb, instr->id);
        iron_strbuf_appendf(sb, " = (%s *)malloc(sizeof(%s));\n", val_type, val_type);
        /* FIX-01 rank 3: OOM guard before dereference */
        emit_indent(sb, ind);
        iron_strbuf_appendf(sb, "if (!");
        emit_val(sb, instr->id);
        iron_strbuf_appendf(sb, ") iron_oom_abort(\"emit_c HEAP_ALLOC\");\n");
        /* Store the inner value into the allocated memory */
        emit_indent(sb, ind);
        iron_strbuf_appendf(sb, "*");
        emit_val(sb, instr->id);
        iron_strbuf_appendf(sb, " = ");
        emit_expr_to_buf(sb, instr->heap_alloc.inner_val, fn, ctx, ctx->current_block_id, 0);
        iron_strbuf_appendf(sb, ";\n");
        break;
    }

    case IRON_LIR_RC_ALLOC: {
        /* rc allocates via malloc (simplified: no actual ref-count tracking yet).
         * instr->type is IRON_TYPE_RC wrapping an inner type; result is a pointer
         * to the inner type (e.g. rc Config -> Iron_Config *).
         *
         * FIX-01 rank 4 (Phase 67-02): same pattern as rank 3 above — pre-fix
         * the emitted C dereferenced the malloc result without a NULL check,
         * so every `rc` use was one OOM away from silent segfault. */
        const char *val_type = NULL;
        if (instr->type && instr->type->kind == IRON_TYPE_RC && instr->type->rc.inner) {
            val_type = emit_type_to_c(instr->type->rc.inner, ctx);
        } else {
            val_type = emit_type_to_c(instr->type, ctx);
        }
        emit_indent(sb, ind);
        iron_strbuf_appendf(sb, "%s *", val_type);
        emit_val(sb, instr->id);
        iron_strbuf_appendf(sb, " = (%s *)malloc(sizeof(%s));\n", val_type, val_type);
        /* FIX-01 rank 4: OOM guard before dereference */
        emit_indent(sb, ind);
        iron_strbuf_appendf(sb, "if (!");
        emit_val(sb, instr->id);
        iron_strbuf_appendf(sb, ") iron_oom_abort(\"emit_c RC_ALLOC\");\n");
        emit_indent(sb, ind);
        iron_strbuf_appendf(sb, "*");
        emit_val(sb, instr->id);
        iron_strbuf_appendf(sb, " = ");
        emit_expr_to_buf(sb, instr->rc_alloc.inner_val, fn, ctx, ctx->current_block_id, 0);
        iron_strbuf_appendf(sb, ";\n");
        break;
    }

    case IRON_LIR_FREE:
        emit_indent(sb, ind);
        iron_strbuf_appendf(sb, "free(");
        emit_expr_to_buf(sb, instr->free_instr.value, fn, ctx, ctx->current_block_id, 0);
        iron_strbuf_appendf(sb, ");\n");
        break;

    /* ── Construct ──────────────────────────────────────────────────────── */

    case IRON_LIR_CONSTRUCT: {
        /* Emit: T _vN = { .field0 = _vA, .field1 = _vB, ... }; */
        const char *c_type = emit_type_to_c(instr->construct.type, ctx);

        /* ADT enum with payloads: emit { .tag = T_TAG_V, .data.V = { payload... } } */
        if (instr->construct.type &&
            instr->construct.type->kind == IRON_TYPE_ENUM &&
            instr->construct.type->enu.decl &&
            instr->construct.type->enu.decl->has_payloads) {
            Iron_EnumDecl *adt_ed = instr->construct.type->enu.decl;
            const char *adt_mangled = instr->construct.type->enu.mangled_name
                ? instr->construct.type->enu.mangled_name
                : emit_mangle_name(adt_ed->name, ctx->arena);
            /* Determine variant index from first field value (tag int constant) */
            int variant_idx = 0;
            if (instr->construct.field_count > 0) {
                IronLIR_ValueId tag_vid = instr->construct.field_vals[0];
                if (tag_vid < (IronLIR_ValueId)arrlen(fn->value_table)) {
                    IronLIR_Instr *tag_instr = fn->value_table[tag_vid];
                    if (tag_instr && tag_instr->kind == IRON_LIR_CONST_INT) {
                        variant_idx = (int)tag_instr->const_int.value;
                    }
                }
            }
            if (variant_idx < 0 || variant_idx >= adt_ed->variant_count)
                variant_idx = 0;
            /* PROT-03 row 26 (AUDIT-01 M-severity): same pattern as row 25 —
             * loop-bound + non-NULL assert before the Iron_EnumVariant cast
             * from the void** variants array in the CONSTRUCT statement
             * emitter. */
            assert(variant_idx >= 0 && variant_idx < adt_ed->variant_count);
            assert(adt_ed->variants[variant_idx] != NULL);
            Iron_EnumVariant *adt_ev = (Iron_EnumVariant *)adt_ed->variants[variant_idx];
            int payload_count = instr->construct.field_count - 1;

            /* Pre-emit malloc for boxed fields (before struct literal) */
            bool *pib = NULL;
            if (instr->construct.type->enu.payload_is_boxed &&
                instr->construct.type->enu.payload_is_boxed[variant_idx]) {
                pib = instr->construct.type->enu.payload_is_boxed[variant_idx];
            }
            if (pib && payload_count > 0 && adt_ev->payload_count > 0) {
                for (int pi = 0; pi < payload_count && pi < adt_ev->payload_count; pi++) {
                    if (pib[pi]) {
                        Iron_Type ***vpt_box = instr->construct.type->enu.variant_payload_types;
                        const char *field_type = (vpt_box && vpt_box[variant_idx] && vpt_box[variant_idx][pi])
                            ? emit_type_to_c(vpt_box[variant_idx][pi], ctx) : "void";
                        emit_indent(sb, ind);
                        iron_strbuf_appendf(sb, "%s *__box_%u_%d = (%s *)malloc(sizeof(%s));\n",
                            field_type, (unsigned)instr->id, pi, field_type, field_type);
                        /* FIX-02 Phase 67-02: OOM guard on boxed ADT field */
                        emit_indent(sb, ind);
                        iron_strbuf_appendf(sb,
                            "if (!__box_%u_%d) iron_oom_abort(\"emit_c boxed ADT\");\n",
                            (unsigned)instr->id, pi);
                        emit_indent(sb, ind);
                        iron_strbuf_appendf(sb, "*__box_%u_%d = ", (unsigned)instr->id, pi);
                        emit_expr_to_buf(sb, instr->construct.field_vals[1 + pi], fn, ctx, ctx->current_block_id, 0);
                        iron_strbuf_appendf(sb, ";\n");
                    }
                }
            }

            emit_indent(sb, ind);
            iron_strbuf_appendf(sb, "%s ", c_type);
            emit_val(sb, instr->id);
            iron_strbuf_appendf(sb, " = {");

            /* Emit .tag */
            iron_strbuf_appendf(sb, " .tag = %s_TAG_%s", adt_mangled, adt_ev->name);
            /* Emit .data.VariantName = { payload... } if any payloads */
            if (payload_count > 0 && adt_ev->payload_count > 0) {
                iron_strbuf_appendf(sb, ", .data.%s = {", adt_ev->name);
                for (int pi = 0; pi < payload_count && pi < adt_ev->payload_count; pi++) {
                    if (pi > 0) iron_strbuf_appendf(sb, ", ");
                    else iron_strbuf_appendf(sb, " ");
                    iron_strbuf_appendf(sb, "._%d = ", pi);
                    if (pib && pib[pi]) {
                        iron_strbuf_appendf(sb, "__box_%u_%d", (unsigned)instr->id, pi);
                    } else {
                        emit_expr_to_buf(sb, instr->construct.field_vals[1 + pi], fn, ctx, ctx->current_block_id, 0);
                    }
                }
                iron_strbuf_appendf(sb, " }");
            }
        /* Use field names from the object decl if available */
        } else if (instr->construct.type &&
            instr->construct.type->kind == IRON_TYPE_OBJECT &&
            instr->construct.type->object.decl) {
            emit_indent(sb, ind);
            iron_strbuf_appendf(sb, "%s ", c_type);
            emit_val(sb, instr->id);
            iron_strbuf_appendf(sb, " = {");
            Iron_ObjectDecl *od = instr->construct.type->object.decl;
            int field_start = 0;
            /* If the object has a parent, first field is _base */
            if (od->extends_name) {
                if (instr->construct.field_count > 0) {
                    iron_strbuf_appendf(sb, " ._base = ");
                    emit_expr_to_buf(sb, instr->construct.field_vals[0], fn, ctx, ctx->current_block_id, 0);
                    field_start = 1;
                    if (instr->construct.field_count > 1)
                        iron_strbuf_appendf(sb, ",");
                }
            }
            int od_field_idx = 0;
            int effective_field_count = instr->construct.field_count;
            if (effective_field_count > od->field_count + field_start) {
                effective_field_count = od->field_count + field_start;
            }
            for (int i = field_start; i < effective_field_count; i++) {
                if (i > field_start) iron_strbuf_appendf(sb, ",");
                if (od_field_idx < od->field_count) {
                    Iron_Field *f = (Iron_Field *)od->fields[od_field_idx++];
                    iron_strbuf_appendf(sb, " .%s = ", f->name);
                } else {
                    iron_strbuf_appendf(sb, " ");
                }
                emit_expr_to_buf(sb, instr->construct.field_vals[i], fn, ctx, ctx->current_block_id, 0);
            }
        /* Phase 59 01d: tuple construct — emit C99 designated init
         * with field names v0, v1, ... matching the typedef produced
         * by emit_ensure_tuple. */
        } else if (instr->construct.type &&
            instr->construct.type->kind == IRON_TYPE_TUPLE) {
            emit_indent(sb, ind);
            iron_strbuf_appendf(sb, "%s ", c_type);
            emit_val(sb, instr->id);
            iron_strbuf_appendf(sb, " = {");
            for (int i = 0; i < instr->construct.field_count; i++) {
                if (i > 0) iron_strbuf_appendf(sb, ",");
                iron_strbuf_appendf(sb, " .v%d = ", i);
                emit_expr_to_buf(sb, instr->construct.field_vals[i], fn, ctx, ctx->current_block_id, 0);
            }
        } else {
            emit_indent(sb, ind);
            iron_strbuf_appendf(sb, "%s ", c_type);
            emit_val(sb, instr->id);
            iron_strbuf_appendf(sb, " = {");
            /* Fallback: positional initialization */
            for (int i = 0; i < instr->construct.field_count; i++) {
                if (i > 0) iron_strbuf_appendf(sb, ", ");
                else iron_strbuf_appendf(sb, " ");
                emit_expr_to_buf(sb, instr->construct.field_vals[i], fn, ctx, ctx->current_block_id, 0);
            }
        }
        iron_strbuf_appendf(sb, " };\n");
        break;
    }

    /* ── Array literal ──────────────────────────────────────────────────── */

    case IRON_LIR_ARRAY_LIT: {
        /* Check if this is an interface-typed array (elements need wrapping) */
        bool is_iface_array = instr->array_lit.elem_type &&
                              instr->array_lit.elem_type->kind == IRON_TYPE_INTERFACE;
        const char *iface_mangled = NULL;
        if (is_iface_array) {
            iface_mangled = emit_mangle_name(
                instr->array_lit.elem_type->interface.decl->name, ctx->arena);
        }

        /* Phase 49: Monomorphic collection — emit as standard Iron_List with tagged
         * union elements instead of Iron_SplitList.  Monomorphic collections are
         * removed from split_collection_ids by the Phase 49 collapse pass, so the
         * Phase 41 split check below will NOT match.  The code falls through to the
         * standard (non-split) Iron_List emission path which wraps each element via
         * <Iface>_from_<Type>().  This preserves type compatibility with downstream
         * method dispatch while eliminating split collection overhead (no per-type
         * sub-arrays, no _order array, no tag dispatch in for-loop iteration). */

        /* Phase 41: Interface arrays use split collection instead of stack/heap array */
        if (is_iface_array && ctx->iface_reg) {
            /* Emit as Iron_SplitList_<Iface> with per-type push calls.
             * Phase 53: If hoisted (phi_hoisted), skip the type prefix since
             * the declaration was pre-emitted at function entry. Use a
             * compound literal for the zero-init assignment. */
            bool is_hoisted = ctx->phi_hoisted &&
                              hmgeti(ctx->phi_hoisted, instr->id) >= 0;
            emit_indent(sb, ind);
            if (is_hoisted) {
                emit_val(sb, instr->id);
                iron_strbuf_appendf(sb, " = (Iron_SplitList_%s){0};\n", iface_mangled);
            } else {
                iron_strbuf_appendf(sb, "Iron_SplitList_%s ", iface_mangled);
                emit_val(sb, instr->id);
                iron_strbuf_appendf(sb, " = {0};\n");
            }
            /* Push each element to the correct type sub-array */
            for (int i = 0; i < instr->array_lit.element_count; i++) {
                IronLIR_ValueId eid = instr->array_lit.elements[i];
                Iron_Type *et = emit_get_value_type(fn, eid);
                if (et && et->kind == IRON_TYPE_OBJECT && et->object.decl) {
                    emit_indent(sb, ind);
                    iron_strbuf_appendf(sb, "Iron_SplitList_%s_push_%s(&",
                        iface_mangled, et->object.decl->name);
                    emit_val(sb, instr->id);
                    iron_strbuf_appendf(sb, ", ");
                    emit_expr_to_buf(sb, eid, fn, ctx, ctx->current_block_id, 0);
                    iron_strbuf_appendf(sb, ");\n");
                }
            }
            /* Track this ValueId as a split collection */
            const char *iface_mangled_copy = iron_arena_strdup(ctx->arena, iface_mangled, strlen(iface_mangled));
            if (!iface_mangled_copy) iron_oom_abort("emit_c.c:emit_instr array_lit split iface_mangled");
            hmput(ctx->split_collection_ids, instr->id, iface_mangled_copy);
        } else if (instr->array_lit.use_stack_repr) {
            /* ARR-01: Emit as C stack array for known-size arrays (<= 256 elements).
             * Produces: elem_type _vN[] = {v0, v1, ...};
             *           int64_t _vN_len = count; */
            const char *elem_c_type = emit_type_to_c(instr->array_lit.elem_type, ctx);
            emit_indent(sb, ind);
            iron_strbuf_appendf(sb, "%s ", elem_c_type);
            emit_val(sb, instr->id);
            iron_strbuf_appendf(sb, "[] = {");
            for (int i = 0; i < instr->array_lit.element_count; i++) {
                if (i > 0) iron_strbuf_appendf(sb, ", ");
                emit_expr_to_buf(sb, instr->array_lit.elements[i], fn, ctx, ctx->current_block_id, 0);
            }
            iron_strbuf_appendf(sb, "};\n");
            /* Emit companion length variable */
            emit_indent(sb, ind);
            iron_strbuf_appendf(sb, "int64_t ");
            emit_val(sb, instr->id);
            iron_strbuf_appendf(sb, "_len = %d;\n", instr->array_lit.element_count);
            /* Register this value as a stack array for downstream use */
            mark_stack_array(ctx, instr->id, instr->id);
        } else {
            /* Create a type-specific Iron_List_<suffix> and push each element.
             * e.g. [Int] -> Iron_List_int64_t_create(), Iron_List_int64_t_push() */
            Iron_Type *arr_type = iron_type_make_array(ctx->arena, instr->array_lit.elem_type, -1);
            const char *list_type = emit_type_to_c(arr_type, ctx);
            emit_indent(sb, ind);
            iron_strbuf_appendf(sb, "%s ", list_type);
            emit_val(sb, instr->id);
            iron_strbuf_appendf(sb, " = %s_create();\n", list_type);
            for (int i = 0; i < instr->array_lit.element_count; i++) {
                emit_indent(sb, ind);
                iron_strbuf_appendf(sb, "%s_push(&", list_type);
                emit_val(sb, instr->id);
                iron_strbuf_appendf(sb, ", ");
                /* Wrap concrete elements into tagged union for interface arrays */
                if (is_iface_array) {
                    IronLIR_ValueId eid = instr->array_lit.elements[i];
                    Iron_Type *et = emit_get_value_type(fn, eid);
                    if (et && et->kind == IRON_TYPE_OBJECT && et->object.decl) {
                        iron_strbuf_appendf(sb, "%s_from_%s(",
                            iface_mangled, et->object.decl->name);
                        emit_expr_to_buf(sb, eid, fn, ctx, ctx->current_block_id, 0);
                        iron_strbuf_appendf(sb, ")");
                    } else {
                        emit_expr_to_buf(sb, eid, fn, ctx, ctx->current_block_id, 0);
                    }
                } else {
                    emit_expr_to_buf(sb, instr->array_lit.elements[i], fn, ctx, ctx->current_block_id, 0);
                }
                iron_strbuf_appendf(sb, ");\n");
            }
        }
        break;
    }

    /* ── Slice ──────────────────────────────────────────────────────────── */

    case IRON_LIR_SLICE: {
        emit_indent(sb, ind);
        iron_strbuf_appendf(sb, "Iron_List ");
        emit_val(sb, instr->id);
        iron_strbuf_appendf(sb, " = iron_list_slice(");
        emit_expr_to_buf(sb, instr->slice.array, fn, ctx, ctx->current_block_id, 0);
        iron_strbuf_appendf(sb, ", ");
        if (instr->slice.start == IRON_LIR_VALUE_INVALID) {
            iron_strbuf_appendf(sb, "0");
        } else {
            emit_expr_to_buf(sb, instr->slice.start, fn, ctx, ctx->current_block_id, 0);
        }
        iron_strbuf_appendf(sb, ", ");
        if (instr->slice.end == IRON_LIR_VALUE_INVALID) {
            iron_strbuf_appendf(sb, "-1");
        } else {
            emit_expr_to_buf(sb, instr->slice.end, fn, ctx, ctx->current_block_id, 0);
        }
        iron_strbuf_appendf(sb, ");\n");
        break;
    }

    /* ── Null checks ────────────────────────────────────────────────────── */

    case IRON_LIR_IS_NULL:
        emit_indent(sb, ind);
        if (!is_hoisted) iron_strbuf_appendf(sb, "bool ");
        emit_val(sb, instr->id);
        iron_strbuf_appendf(sb, " = !");
        emit_expr_to_buf(sb, instr->null_check.value, fn, ctx, ctx->current_block_id, 0);
        iron_strbuf_appendf(sb, ".has_value;\n");
        break;

    case IRON_LIR_IS_NOT_NULL:
        emit_indent(sb, ind);
        if (!is_hoisted) iron_strbuf_appendf(sb, "bool ");
        emit_val(sb, instr->id);
        iron_strbuf_appendf(sb, " = ");
        emit_expr_to_buf(sb, instr->null_check.value, fn, ctx, ctx->current_block_id, 0);
        iron_strbuf_appendf(sb, ".has_value;\n");
        break;

    /* ── String interpolation ───────────────────────────────────────────── */

    case IRON_LIR_INTERP_STRING: {
        /* Two-pass snprintf interpolation:
         *   Pass 1: measure with snprintf(NULL, 0, fmt, args...)
         *   Pass 2: allocate and fill with snprintf(buf, n+1, fmt, args...)
         * Each part is given a printf format specifier based on its type.
         * String parts use iron_string_cstr() to convert to C string.
         *
         * IMPORTANT: the result variable is declared BEFORE the inner block
         * so it remains in scope for subsequent instructions. */

        /* Declare result variable at outer scope */
        emit_indent(sb, ind);
        iron_strbuf_appendf(sb, "Iron_String ");
        emit_val(sb, instr->id);
        iron_strbuf_appendf(sb, ";\n");

        /* Open temporary block for buf variables */
        emit_indent(sb, ind);
        iron_strbuf_appendf(sb, "{\n");
        ind++;

        /* Build format string and collect conversion expressions */
        Iron_StrBuf fmt_sb   = iron_strbuf_create(64);
        Iron_StrBuf args_sb  = iron_strbuf_create(128);
        bool        first_arg = true;

        for (int i = 0; i < instr->interp_string.part_count; i++) {
            IronLIR_ValueId part_id = instr->interp_string.parts[i];
            /* Look up the type of this part via the function's value_table */
            Iron_Type *part_type = NULL;
            if (part_id < (IronLIR_ValueId)arrlen(fn->value_table) &&
                fn->value_table[part_id]) {
                part_type = fn->value_table[part_id]->type;
            }

            /* Determine format specifier and conversion */
            const char *fmt_spec = "%s";  /* default: string */
            if (part_type) {
                switch ((int)(part_type->kind)) {
                    case IRON_TYPE_INT:
                    case IRON_TYPE_INT8:
                    case IRON_TYPE_INT16:
                    case IRON_TYPE_INT32:
                    case IRON_TYPE_INT64:
                    case IRON_TYPE_UINT:
                    case IRON_TYPE_UINT8:
                    case IRON_TYPE_UINT16:
                    case IRON_TYPE_UINT32:
                    case IRON_TYPE_UINT64:
                        fmt_spec = "%lld";
                        break;
                    case IRON_TYPE_FLOAT:
                    case IRON_TYPE_FLOAT32:
                    case IRON_TYPE_FLOAT64:
                        fmt_spec = "%g";
                        break;
                    case IRON_TYPE_BOOL:
                        /* emitted as ternary inline */
                        fmt_spec = "%s";
                        break;
                    /* -Wswitch-enum opt-out: interp-string format selector
                     * only distinguishes integer / float / bool from the
                     * generic string path; every other kind goes through
                     * the caller's iron_string_cstr fallback. */
                    default:
                        fmt_spec = "%s";
                        break;
                }
            }

            iron_strbuf_appendf(&fmt_sb, "%s", fmt_spec);

            if (!first_arg) iron_strbuf_appendf(&args_sb, ", ");
            first_arg = false;

            if (part_type) {
                switch ((int)(part_type->kind)) {
                    case IRON_TYPE_INT:
                    case IRON_TYPE_INT8:
                    case IRON_TYPE_INT16:
                    case IRON_TYPE_INT32:
                    case IRON_TYPE_INT64:
                    case IRON_TYPE_UINT:
                    case IRON_TYPE_UINT8:
                    case IRON_TYPE_UINT16:
                    case IRON_TYPE_UINT32:
                    case IRON_TYPE_UINT64: {
                        /* Cast to long long for %lld */
                        Iron_StrBuf tmp = iron_strbuf_create(32);
                        emit_val(&tmp, part_id);
                        iron_strbuf_appendf(&args_sb, "(long long)(%s)",
                                            iron_strbuf_get(&tmp));
                        iron_strbuf_free(&tmp);
                        break;
                    }
                    case IRON_TYPE_FLOAT:
                    case IRON_TYPE_FLOAT32:
                    case IRON_TYPE_FLOAT64: {
                        Iron_StrBuf tmp = iron_strbuf_create(32);
                        emit_val(&tmp, part_id);
                        iron_strbuf_appendf(&args_sb, "(double)(%s)",
                                            iron_strbuf_get(&tmp));
                        iron_strbuf_free(&tmp);
                        break;
                    }
                    case IRON_TYPE_BOOL: {
                        /* "true" / "false" ternary */
                        Iron_StrBuf tmp = iron_strbuf_create(32);
                        emit_val(&tmp, part_id);
                        iron_strbuf_appendf(&args_sb, "(%s ? \"true\" : \"false\")",
                                            iron_strbuf_get(&tmp));
                        iron_strbuf_free(&tmp);
                        break;
                    }
                    case IRON_TYPE_STRING: {
                        /* iron_string_cstr() converts to const char* */
                        Iron_StrBuf tmp = iron_strbuf_create(32);
                        emit_val(&tmp, part_id);
                        iron_strbuf_appendf(&args_sb, "iron_string_cstr(&%s)",
                                            iron_strbuf_get(&tmp));
                        iron_strbuf_free(&tmp);
                        break;
                    }
                    /* -Wswitch-enum opt-out: interp-string argument emitter
                     * handles primitives + String explicitly; every other
                     * Iron_TypeKind (OBJECT, INTERFACE, ENUM, ARRAY, etc.)
                     * is coerced through iron_string_cstr which looks up
                     * the object's to_string() method. */
                    default: {
                        /* Generic: try cstr conversion */
                        Iron_StrBuf tmp = iron_strbuf_create(32);
                        emit_val(&tmp, part_id);
                        iron_strbuf_appendf(&args_sb, "iron_string_cstr(&%s)",
                                            iron_strbuf_get(&tmp));
                        iron_strbuf_free(&tmp);
                        break;
                    }
                }
            } else {
                /* No type info: fall back to cstr */
                Iron_StrBuf tmp = iron_strbuf_create(32);
                emit_val(&tmp, part_id);
                iron_strbuf_appendf(&args_sb, "iron_string_cstr(&%s)",
                                    iron_strbuf_get(&tmp));
                iron_strbuf_free(&tmp);
            }
        }

        const char *fmt_str  = iron_strbuf_get(&fmt_sb);
        const char *args_str = iron_strbuf_get(&args_sb);

        /* Pass 1: measure */
        emit_indent(sb, ind);
        if (instr->interp_string.part_count > 0) {
            iron_strbuf_appendf(sb,
                "int _interp_len_%u = snprintf(NULL, 0, \"%s\", %s);\n",
                instr->id, fmt_str, args_str);
        } else {
            iron_strbuf_appendf(sb,
                "int _interp_len_%u = 0;\n", instr->id);
        }

        /* Allocate buffer (len + 1 for NUL terminator) */
        emit_indent(sb, ind);
        iron_strbuf_appendf(sb,
            "char *_interp_buf_%u = (char *)malloc((size_t)(_interp_len_%u + 1));\n",
            instr->id, instr->id);
        /* FIX-02 Phase 67-02: OOM guard on interpolation buffer */
        emit_indent(sb, ind);
        iron_strbuf_appendf(sb,
            "if (!_interp_buf_%u) iron_oom_abort(\"emit_c interp string\");\n",
            instr->id);

        /* Pass 2: fill */
        emit_indent(sb, ind);
        if (instr->interp_string.part_count > 0) {
            iron_strbuf_appendf(sb,
                "snprintf(_interp_buf_%u, (size_t)(_interp_len_%u + 1), \"%s\", %s);\n",
                instr->id, instr->id, fmt_str, args_str);
        } else {
            iron_strbuf_appendf(sb, "_interp_buf_%u[0] = '\\0';\n", instr->id);
        }

        iron_strbuf_free(&fmt_sb);
        iron_strbuf_free(&args_sb);

        /* Assign result to the outer-scoped variable */
        emit_indent(sb, ind);
        emit_val(sb, instr->id);
        iron_strbuf_appendf(sb,
            " = iron_string_from_cstr(_interp_buf_%u, (size_t)_interp_len_%u);\n",
            instr->id, instr->id);
        emit_indent(sb, ind);
        iron_strbuf_appendf(sb, "free(_interp_buf_%u);\n", instr->id);

        ind--;
        emit_indent(sb, ind);
        iron_strbuf_appendf(sb, "}\n");
        break;
    }

    /* ── Closures / function references ─────────────────────────────────── */

    case IRON_LIR_MAKE_CLOSURE: {
        /* Emit an Iron_Closure fat pointer.
         * Capturing closures: emit __lambda_N_env_t typedef into struct_bodies,
         * malloc+populate the env, and build Iron_Closure { .env = ..., .fn = ... }.
         * Non-capturing closures: Iron_Closure { .env = NULL, .fn = cast_fn }. */
        const char *func_name = instr->make_closure.lifted_func_name;
        int cap_count = instr->make_closure.capture_count;
        Iron_CaptureEntry *cap_meta = instr->make_closure.capture_metadata;

        if (cap_count > 0 && cap_meta) {
            /* Env struct type name: <func_name>_env_t */
            Iron_StrBuf env_type_sb = iron_strbuf_create(64);
            iron_strbuf_appendf(&env_type_sb, "%s_env_t", func_name);
            const char *env_type = iron_arena_strdup(ctx->arena,
                                                      iron_strbuf_get(&env_type_sb),
                                                      env_type_sb.len);
            if (!env_type) iron_oom_abort("emit_c.c:emit_instr MAKE_CLOSURE env_type");
            iron_strbuf_free(&env_type_sb);

            /* Emit typedef into struct_bodies (deduplicated via mono_registry) */
            if (shgeti(ctx->mono_registry, (char *)env_type) < 0) {
                shput(ctx->mono_registry, (char *)env_type, true);
                iron_strbuf_appendf(&ctx->struct_bodies, "typedef struct {\n");
                for (int ci = 0; ci < cap_count; ci++) {
                    const char *field_type = cap_meta[ci].type
                                             ? emit_type_to_c(cap_meta[ci].type, ctx)
                                             : "void*";
                    if (cap_meta[ci].is_mutable) {
                        /* var capture: store pointer to outer variable */
                        iron_strbuf_appendf(&ctx->struct_bodies,
                                            "    %s *%s;\n",
                                            field_type, cap_meta[ci].name);
                    } else {
                        /* val capture: store value copy */
                        iron_strbuf_appendf(&ctx->struct_bodies,
                                            "    %s %s;\n",
                                            field_type, cap_meta[ci].name);
                    }
                }
                iron_strbuf_appendf(&ctx->struct_bodies, "} %s;\n\n", env_type);
            }

            /* Allocate env struct */
            emit_indent(sb, ind);
            iron_strbuf_appendf(sb, "%s *_env_%u = (%s *)malloc(sizeof(%s));\n",
                                env_type, instr->id, env_type, env_type);
            /* FIX-02 Phase 67-02: OOM guard on closure env (A) */
            emit_indent(sb, ind);
            iron_strbuf_appendf(sb,
                "if (!_env_%u) iron_oom_abort(\"emit_c closure env (A)\");\n",
                instr->id);

            /* Populate env fields */
            for (int ci = 0; ci < cap_count; ci++) {
                emit_indent(sb, ind);
                iron_strbuf_appendf(sb, "_env_%u->%s = ", instr->id, cap_meta[ci].name);
                if (cap_meta[ci].is_mutable) {
                    /* var capture: store address of the outer alloca variable */
                    iron_strbuf_appendf(sb, "&");
                    emit_val(sb, instr->make_closure.captures[ci]);
                } else {
                    /* val capture: store loaded value */
                    emit_val(sb, instr->make_closure.captures[ci]);
                }
                iron_strbuf_appendf(sb, ";\n");
            }

            /* Build Iron_Closure with env */
            emit_indent(sb, ind);
            iron_strbuf_appendf(sb, "Iron_Closure ");
            emit_val(sb, instr->id);
            iron_strbuf_appendf(sb,
                " = { .env = _env_%u, .fn = (void(*)(void*))%s };\n",
                instr->id, func_name);
        } else {
            /* Non-capturing: Iron_Closure with NULL env */
            emit_indent(sb, ind);
            iron_strbuf_appendf(sb, "Iron_Closure ");
            emit_val(sb, instr->id);
            iron_strbuf_appendf(sb,
                " = { .env = NULL, .fn = (void(*)(void*))%s };\n",
                func_name);
        }
        break;
    }

    case IRON_LIR_FUNC_REF: {
        /* FUNC_REF values are resolved directly from the value_table at CALL
         * sites (see the IRON_LIR_CALL case), so the emitted void* cast is
         * dead code.  Skip emission entirely for all FUNC_REF instructions. */
        break;
    }

    /* ── Concurrency ────────────────────────────────────────────────────── */

    case IRON_LIR_SPAWN: {
        const char *func_name   = instr->spawn.lifted_func_name;
        const char *c_func_name = emit_mangle_func_name(func_name, ctx->arena);
        int cap_count = instr->spawn.capture_count;
        Iron_CaptureEntry *cap_meta = instr->spawn.capture_metadata;

        /* If there are captures, emit an env struct typedef. The env struct is
         * heap-allocated and passed as void* to the wrapper. The lifted spawn
         * function receives void* _env as its first parameter (like lambdas). */
        const char *env_type = NULL;
        if (cap_count > 0 && cap_meta) {
            /* Env struct type: <func_name>_env_t */
            Iron_StrBuf env_type_sb = iron_strbuf_create(64);
            iron_strbuf_appendf(&env_type_sb, "%s_env_t", func_name);
            env_type = iron_arena_strdup(ctx->arena,
                                         iron_strbuf_get(&env_type_sb),
                                         env_type_sb.len);
            if (!env_type) iron_oom_abort("emit_c.c:emit_instr SPAWN env_type");
            iron_strbuf_free(&env_type_sb);

            /* Emit env struct typedef (deduplicated) */
            if (shgeti(ctx->mono_registry, (char *)env_type) < 0) {
                shput(ctx->mono_registry, (char *)env_type, true);
                iron_strbuf_appendf(&ctx->struct_bodies, "typedef struct {\n");
                for (int ci = 0; ci < cap_count; ci++) {
                    const char *field_type = cap_meta[ci].type
                                             ? emit_type_to_c(cap_meta[ci].type, ctx)
                                             : "void*";
                    if (cap_meta[ci].is_mutable) {
                        iron_strbuf_appendf(&ctx->struct_bodies,
                            "    %s *%s;\n", field_type, cap_meta[ci].name);
                    } else {
                        iron_strbuf_appendf(&ctx->struct_bodies,
                            "    %s %s;\n", field_type, cap_meta[ci].name);
                    }
                }
                iron_strbuf_appendf(&ctx->struct_bodies, "} %s;\n\n", env_type);
            }
        }

        if (instr->type && instr->type->kind != IRON_TYPE_VOID) {
            /* Handled spawn: generate a wrapper that captures the result into
             * the handle's result field, then start the thread.
             *
             * For non-capturing spawns: arg is the handle itself (self-ref pattern).
             * For capturing spawns (void return): arg is the env struct; use
             *   Iron_handle_create(wrapper, env_arg) for the handle.
             * For capturing spawns (non-void return): embed result ptr in env struct.
             */

            /* Build a deterministic wrapper name */
            Iron_StrBuf wrapper_sb = iron_strbuf_create(64);
            iron_strbuf_appendf(&wrapper_sb, "%s_wrapper", c_func_name);
            const char *wrapper_name = iron_arena_strdup(ctx->arena,
                                                          iron_strbuf_get(&wrapper_sb),
                                                          wrapper_sb.len);
            if (!wrapper_name) iron_oom_abort("emit_c.c:emit_instr SPAWN wrapper_name");
            iron_strbuf_free(&wrapper_sb);

            /* Look up the lifted function to determine its return type */
            IronLIR_Func *lifted_fn = NULL;
            for (int fi = 0; fi < ctx->module->func_count; fi++) {
                if (ctx->module->funcs[fi]->name &&
                    strcmp(ctx->module->funcs[fi]->name, func_name) == 0) {
                    lifted_fn = ctx->module->funcs[fi];
                    break;
                }
            }

            bool has_return = lifted_fn && lifted_fn->return_type &&
                              lifted_fn->return_type->kind != IRON_TYPE_VOID;
            const char *ret_c = has_return
                ? emit_type_to_c(lifted_fn->return_type, ctx)
                : NULL;

            /* Emit wrapper into lifted_funcs section */
            iron_strbuf_appendf(&ctx->lifted_funcs,
                "static void %s(void *_arg) {\n", wrapper_name);

            if (cap_count > 0 && cap_meta && env_type) {
                /* Capturing spawn: arg is the env struct.
                 * For non-void return spawns, embed result in a wrapper struct. */
                if (has_return && ret_c) {
                    /* Build a result-bearing arg struct: { env_t env; ret_t result; } */
                    Iron_StrBuf rarg_sb = iron_strbuf_create(64);
                    iron_strbuf_appendf(&rarg_sb, "%s_rarg_t", func_name);
                    const char *rarg_type = iron_arena_strdup(ctx->arena,
                        iron_strbuf_get(&rarg_sb), rarg_sb.len);
                    if (!rarg_type) iron_oom_abort("emit_c.c:emit_instr SPAWN rarg_type");
                    iron_strbuf_free(&rarg_sb);

                    if (shgeti(ctx->mono_registry, (char *)rarg_type) < 0) {
                        shput(ctx->mono_registry, (char *)rarg_type, true);
                        iron_strbuf_appendf(&ctx->struct_bodies,
                            "typedef struct { %s env; %s result; } %s;\n\n",
                            env_type, ret_c, rarg_type);
                    }

                    iron_strbuf_appendf(&ctx->lifted_funcs,
                        "    %s *_ra = (%s *)_arg;\n", rarg_type, rarg_type);
                    iron_strbuf_appendf(&ctx->lifted_funcs,
                        "    %s *_e = &_ra->env;\n", env_type);
                    iron_strbuf_appendf(&ctx->lifted_funcs,
                        "    _ra->result = %s(_e);\n", c_func_name);
                } else {
                    iron_strbuf_appendf(&ctx->lifted_funcs,
                        "    %s *_e = (%s *)_arg;\n", env_type, env_type);
                    iron_strbuf_appendf(&ctx->lifted_funcs,
                        "    %s(_e);\n", c_func_name);
                    iron_strbuf_appendf(&ctx->lifted_funcs, "    free(_arg);\n");
                }
            } else {
                /* Non-capturing spawn: arg is the handle itself (self-ref) */
                iron_strbuf_appendf(&ctx->lifted_funcs,
                    "    Iron_Handle *_h = (Iron_Handle *)_arg;\n");
                if (has_return && ret_c) {
                    iron_strbuf_appendf(&ctx->lifted_funcs,
                        "    %s _result = %s();\n", ret_c, c_func_name);
                    iron_strbuf_appendf(&ctx->lifted_funcs,
                        "    _h->result = (void *)(intptr_t)_result;\n");
                } else {
                    iron_strbuf_appendf(&ctx->lifted_funcs,
                        "    %s();\n", c_func_name);
                }
            }

            iron_strbuf_appendf(&ctx->lifted_funcs, "}\n\n");

            if (cap_count > 0 && cap_meta && env_type) {
                /* Capturing handled spawn: allocate env, populate, use Iron_handle_create */
                emit_indent(sb, ind);
                iron_strbuf_appendf(sb, "%s *_env_%u = (%s *)malloc(sizeof(%s));\n",
                    env_type, instr->id, env_type, env_type);
                /* FIX-02 Phase 67-02: OOM guard on closure env (B) */
                emit_indent(sb, ind);
                iron_strbuf_appendf(sb,
                    "if (!_env_%u) iron_oom_abort(\"emit_c closure env (B)\");\n",
                    instr->id);
                /* Populate env fields */
                for (int ci = 0; ci < cap_count; ci++) {
                    emit_indent(sb, ind);
                    iron_strbuf_appendf(sb, "_env_%u->%s = ", instr->id, cap_meta[ci].name);
                    if (cap_meta[ci].is_mutable) {
                        iron_strbuf_appendf(sb, "&");
                        emit_val(sb, instr->spawn.captures[ci]);
                    } else {
                        emit_capture_rhs(sb, instr->spawn.captures[ci], &cap_meta[ci], ctx);
                    }
                    iron_strbuf_appendf(sb, ";\n");
                }
                /* Use Iron_handle_create: wrapper receives env as arg */
                emit_indent(sb, ind);
                iron_strbuf_appendf(sb, "Iron_Handle *");
                emit_val(sb, instr->id);
                iron_strbuf_appendf(sb, " = Iron_handle_create("
                                    "(void (*)(void *))%s, _env_%u);\n",
                                    wrapper_name, instr->id);
            } else {
                /* Non-capturing handled spawn: use iron_handle_create_self_ref */
                emit_indent(sb, ind);
                iron_strbuf_appendf(sb, "Iron_Handle *");
                emit_val(sb, instr->id);
                iron_strbuf_appendf(sb, " = iron_handle_create_self_ref("
                                    "(void (*)(void *))%s);\n", wrapper_name);
            }
        } else {
            /* Fire-and-forget spawn -- use pool_submit */
            if (cap_count > 0 && cap_meta && env_type) {
                /* Capturing fire-and-forget: allocate env, populate, submit */
                emit_indent(sb, ind);
                iron_strbuf_appendf(sb, "{\n");
                int inner = ind + 1;
                emit_indent(sb, inner);
                iron_strbuf_appendf(sb, "%s *_env_%u = (%s *)malloc(sizeof(%s));\n",
                    env_type, instr->id, env_type, env_type);
                /* FIX-02 Phase 67-02: OOM guard on closure env (C) */
                emit_indent(sb, inner);
                iron_strbuf_appendf(sb,
                    "if (!_env_%u) iron_oom_abort(\"emit_c closure env (C)\");\n",
                    instr->id);
                for (int ci = 0; ci < cap_count; ci++) {
                    emit_indent(sb, inner);
                    iron_strbuf_appendf(sb, "_env_%u->%s = ", instr->id, cap_meta[ci].name);
                    if (cap_meta[ci].is_mutable) {
                        iron_strbuf_appendf(sb, "&");
                        emit_val(sb, instr->spawn.captures[ci]);
                    } else {
                        emit_capture_rhs(sb, instr->spawn.captures[ci], &cap_meta[ci], ctx);
                    }
                    iron_strbuf_appendf(sb, ";\n");
                }
                /* Emit a simple wrapper that calls with env and frees */
                Iron_StrBuf ff_wrapper_sb = iron_strbuf_create(64);
                iron_strbuf_appendf(&ff_wrapper_sb, "%s_ff_wrapper", c_func_name);
                const char *ff_wrapper = iron_arena_strdup(ctx->arena,
                                                            iron_strbuf_get(&ff_wrapper_sb),
                                                            ff_wrapper_sb.len);
                if (!ff_wrapper) iron_oom_abort("emit_c.c:emit_instr SPAWN ff_wrapper");
                iron_strbuf_free(&ff_wrapper_sb);

                if (shgeti(ctx->mono_registry, (char *)ff_wrapper) < 0) {
                    shput(ctx->mono_registry, (char *)ff_wrapper, true);
                    iron_strbuf_appendf(&ctx->lifted_funcs,
                        "static void %s(void *_arg) {\n", ff_wrapper);
                    iron_strbuf_appendf(&ctx->lifted_funcs,
                        "    %s *_e = (%s *)_arg;\n", env_type, env_type);
                    iron_strbuf_appendf(&ctx->lifted_funcs,
                        "    %s(_e);\n", c_func_name);
                    iron_strbuf_appendf(&ctx->lifted_funcs,
                        "    free(_arg);\n");
                    iron_strbuf_appendf(&ctx->lifted_funcs, "}\n\n");
                }

                emit_indent(sb, inner);
                iron_strbuf_appendf(sb, "Iron_pool_submit(Iron_global_pool, %s, _env_%u);\n",
                    ff_wrapper, instr->id);
                emit_indent(sb, ind);
                iron_strbuf_appendf(sb, "}\n");
            } else {
                /* Non-capturing fire-and-forget */
                emit_indent(sb, ind);
                iron_strbuf_appendf(sb, "Iron_pool_submit(");
                if (instr->spawn.pool_val == IRON_LIR_VALUE_INVALID) {
                    iron_strbuf_appendf(sb, "Iron_global_pool");
                } else {
                    emit_val(sb, instr->spawn.pool_val);
                }
                iron_strbuf_appendf(sb, ", (void (*)(void *))%s, NULL);\n", c_func_name);
            }
        }
        break;
    }

    case IRON_LIR_PARALLEL_FOR: {
        /* Range-splitting parallel-for pattern using Iron_pool_submit.
         *
         * Without captures: chunk function has signature void chunk(int64_t i)
         * With captures: chunk function has signature void chunk(void *_env, int64_t i)
         *   and the ctx struct embeds the env fields.
         *
         * We emit:
         *   1. Optionally an env struct typedef for captured variables.
         *   2. A context struct: { int64_t start; int64_t end; [env fields] }
         *   3. A wrapper function looping [start,end) calling chunk for each i.
         *   4. Inline range-splitting logic that malloc's a ctx per chunk + submits.
         *   5. Iron_pool_barrier at the end.
         */
        const char *chunk_func = instr->parallel_for.chunk_func_name;
        int pfor_cap_count = instr->parallel_for.capture_count;
        Iron_CaptureEntry *pfor_cap_meta = instr->parallel_for.capture_metadata;

        /* Derive context struct name and wrapper name from chunk func name */
        Iron_StrBuf ctx_type_sb = iron_strbuf_create(64);
        iron_strbuf_appendf(&ctx_type_sb, "%s_ctx", chunk_func);
        const char *ctx_type = iron_arena_strdup(ctx->arena,
                                                   iron_strbuf_get(&ctx_type_sb),
                                                   ctx_type_sb.len);
        if (!ctx_type) iron_oom_abort("emit_c.c:emit_instr PARALLEL_FOR ctx_type");
        iron_strbuf_free(&ctx_type_sb);

        Iron_StrBuf wrapper_sb = iron_strbuf_create(64);
        iron_strbuf_appendf(&wrapper_sb, "%s_wrapper", chunk_func);
        const char *wrapper_name = iron_arena_strdup(ctx->arena,
                                                       iron_strbuf_get(&wrapper_sb),
                                                       wrapper_sb.len);
        if (!wrapper_name) iron_oom_abort("emit_c.c:emit_instr PARALLEL_FOR wrapper_name");
        iron_strbuf_free(&wrapper_sb);

        /* Emit context struct typedef into lifted_funcs section.
         * For capturing pfor, embed env fields directly in the ctx struct. */
        iron_strbuf_appendf(&ctx->lifted_funcs, "typedef struct {\n");
        iron_strbuf_appendf(&ctx->lifted_funcs, "    int64_t start;\n");
        iron_strbuf_appendf(&ctx->lifted_funcs, "    int64_t end;\n");
        if (pfor_cap_count > 0 && pfor_cap_meta) {
            for (int ci = 0; ci < pfor_cap_count; ci++) {
                const char *field_type = pfor_cap_meta[ci].type
                                         ? emit_type_to_c(pfor_cap_meta[ci].type, ctx)
                                         : "void*";
                if (pfor_cap_meta[ci].is_mutable) {
                    iron_strbuf_appendf(&ctx->lifted_funcs,
                        "    %s *%s;\n", field_type, pfor_cap_meta[ci].name);
                } else {
                    iron_strbuf_appendf(&ctx->lifted_funcs,
                        "    %s %s;\n", field_type, pfor_cap_meta[ci].name);
                }
            }
        }
        iron_strbuf_appendf(&ctx->lifted_funcs, "} %s;\n\n", ctx_type);

        /* Resolve the mangled C name for the chunk function. */
        const char *chunk_c_name = emit_mangle_func_name(chunk_func, ctx->arena);

        /* Emit wrapper function into lifted_funcs section.
         * For captures, build a pseudo-env and pass it as first arg. */
        iron_strbuf_appendf(&ctx->lifted_funcs,
            "static void %s(void *_arg) {\n", wrapper_name);
        iron_strbuf_appendf(&ctx->lifted_funcs,
            "    %s *_c = (%s *)_arg;\n", ctx_type, ctx_type);
        iron_strbuf_appendf(&ctx->lifted_funcs,
            "    int64_t _start = _c->start;\n");
        iron_strbuf_appendf(&ctx->lifted_funcs,
            "    int64_t _end   = _c->end;\n");
        if (pfor_cap_count > 0 && pfor_cap_meta) {
            /* Build a local env struct to pass to the chunk function */
            Iron_StrBuf env_type_sb = iron_strbuf_create(64);
            iron_strbuf_appendf(&env_type_sb, "%s_env_t", chunk_func);
            const char *env_type = iron_arena_strdup(ctx->arena,
                                                      iron_strbuf_get(&env_type_sb),
                                                      env_type_sb.len);
            if (!env_type) iron_oom_abort("emit_c.c:emit_instr PARALLEL_FOR pfor_env_type");
            iron_strbuf_free(&env_type_sb);

            /* Emit env struct typedef (deduplicated) */
            if (shgeti(ctx->mono_registry, (char *)env_type) < 0) {
                shput(ctx->mono_registry, (char *)env_type, true);
                /* Pre-declare it in struct_bodies */
                iron_strbuf_appendf(&ctx->struct_bodies, "typedef struct {\n");
                for (int ci = 0; ci < pfor_cap_count; ci++) {
                    const char *field_type = pfor_cap_meta[ci].type
                                             ? emit_type_to_c(pfor_cap_meta[ci].type, ctx)
                                             : "void*";
                    if (pfor_cap_meta[ci].is_mutable) {
                        iron_strbuf_appendf(&ctx->struct_bodies,
                            "    %s *%s;\n", field_type, pfor_cap_meta[ci].name);
                    } else {
                        iron_strbuf_appendf(&ctx->struct_bodies,
                            "    %s %s;\n", field_type, pfor_cap_meta[ci].name);
                    }
                }
                iron_strbuf_appendf(&ctx->struct_bodies, "} %s;\n\n", env_type);
            }

            iron_strbuf_appendf(&ctx->lifted_funcs,
                "    %s _local_env;\n", env_type);
            for (int ci = 0; ci < pfor_cap_count; ci++) {
                iron_strbuf_appendf(&ctx->lifted_funcs,
                    "    _local_env.%s = _c->%s;\n",
                    pfor_cap_meta[ci].name, pfor_cap_meta[ci].name);
            }
            iron_strbuf_appendf(&ctx->lifted_funcs,
                "    free(_arg);\n");
            iron_strbuf_appendf(&ctx->lifted_funcs,
                "    for (int64_t _i = _start; _i < _end; _i++) {\n");
            iron_strbuf_appendf(&ctx->lifted_funcs,
                "        %s(&_local_env, _i);\n", chunk_c_name);
            iron_strbuf_appendf(&ctx->lifted_funcs,
                "    }\n");
        } else {
            iron_strbuf_appendf(&ctx->lifted_funcs,
                "    free(_arg);\n");
            iron_strbuf_appendf(&ctx->lifted_funcs,
                "    for (int64_t _i = _start; _i < _end; _i++) {\n");
            iron_strbuf_appendf(&ctx->lifted_funcs,
                "        %s(_i);\n", chunk_c_name);
            iron_strbuf_appendf(&ctx->lifted_funcs,
                "    }\n");
        }
        iron_strbuf_appendf(&ctx->lifted_funcs, "}\n\n");

        /* Emit inline range-splitting and submission loop */
        emit_indent(sb, ind);
        iron_strbuf_appendf(sb, "{\n");
        int inner = ind + 1;

        /* _total = range value */
        emit_indent(sb, inner);
        iron_strbuf_appendf(sb, "int64_t _total = ");
        emit_val(sb, instr->parallel_for.range_val);
        iron_strbuf_appendf(sb, ";\n");

        /* Thread count and chunk size */
        emit_indent(sb, inner);
        iron_strbuf_appendf(sb,
            "int64_t _nthreads = (int64_t)Iron_pool_thread_count(Iron_global_pool);\n");
        emit_indent(sb, inner);
        iron_strbuf_appendf(sb, "if (_nthreads < 1) _nthreads = 1;\n");
        emit_indent(sb, inner);
        iron_strbuf_appendf(sb,
            "int64_t _chunk_size = (_total + _nthreads - 1) / _nthreads;\n");
        emit_indent(sb, inner);
        iron_strbuf_appendf(sb, "if (_chunk_size < 1) _chunk_size = 1;\n");

        /* Submission loop */
        emit_indent(sb, inner);
        iron_strbuf_appendf(sb,
            "for (int64_t _c = 0; _c < _total; _c += _chunk_size) {\n");
        int inner2 = inner + 1;
        emit_indent(sb, inner2);
        iron_strbuf_appendf(sb,
            "int64_t _end = (_c + _chunk_size > _total) ? _total : _c + _chunk_size;\n");
        emit_indent(sb, inner2);
        iron_strbuf_appendf(sb,
            "%s *_pctx = (%s *)malloc(sizeof(%s));\n",
            ctx_type, ctx_type, ctx_type);
        /* FIX-02 Phase 67-02: OOM guard on parallel-for ctx */
        emit_indent(sb, inner2);
        iron_strbuf_appendf(sb,
            "if (!_pctx) iron_oom_abort(\"emit_c parallel-for ctx\");\n");
        emit_indent(sb, inner2);
        iron_strbuf_appendf(sb, "_pctx->start = _c;\n");
        emit_indent(sb, inner2);
        iron_strbuf_appendf(sb, "_pctx->end = _end;\n");
        /* Populate env fields in ctx */
        if (pfor_cap_count > 0 && pfor_cap_meta) {
            for (int ci = 0; ci < pfor_cap_count; ci++) {
                emit_indent(sb, inner2);
                iron_strbuf_appendf(sb, "_pctx->%s = ", pfor_cap_meta[ci].name);
                if (pfor_cap_meta[ci].is_mutable) {
                    iron_strbuf_appendf(sb, "&");
                    emit_val(sb, instr->parallel_for.captures[ci]);
                } else {
                    emit_capture_rhs(sb, instr->parallel_for.captures[ci],
                                     &pfor_cap_meta[ci], ctx);
                }
                iron_strbuf_appendf(sb, ";\n");
            }
        }
        emit_indent(sb, inner2);
        iron_strbuf_appendf(sb,
            "Iron_pool_submit(Iron_global_pool, %s, _pctx);\n", wrapper_name);
        emit_indent(sb, inner);
        iron_strbuf_appendf(sb, "}\n");

        /* Barrier */
        emit_indent(sb, inner);
        iron_strbuf_appendf(sb, "Iron_pool_barrier(Iron_global_pool);\n");

        /* Close scope */
        emit_indent(sb, ind);
        iron_strbuf_appendf(sb, "}\n");
        break;
    }

    case IRON_LIR_AWAIT:
        emit_indent(sb, ind);
        if (instr->type) {
            iron_strbuf_appendf(sb, "%s ", emit_type_to_c(instr->type, ctx));
            emit_val(sb, instr->id);
            iron_strbuf_appendf(sb, " = (%s)iron_future_await(",
                                emit_type_to_c(instr->type, ctx));
        } else {
            iron_strbuf_appendf(sb, "iron_future_await(");
        }
        emit_val(sb, instr->await.handle);
        iron_strbuf_appendf(sb, ");\n");
        break;

    /* ── SSA Phi (should be eliminated) ─────────────────────────────────── */

    case IRON_LIR_PHI:
        /* Should never reach here — phi_eliminate() runs before emission */
        emit_indent(sb, ind);
        iron_strbuf_appendf(sb, "/* ERROR: phi not eliminated _v%u */\n",
                            instr->id);
        break;

    /* ── Poison ─────────────────────────────────────────────────────────── */

    case IRON_LIR_POISON:
        /* Skip poison instructions — they represent undefined behavior */
        emit_indent(sb, ind);
        iron_strbuf_appendf(sb, "/* poison */\n");
        break;

    /* ── Sentinel ───────────────────────────────────────────────────────── */

    case IRON_LIR_INSTR_COUNT:
        /* Should never appear */
        break;
    }

    (void)emit_type_is_pointer; /* suppress unused warning */
}

/* ── Function emission ────────────────────────────────────────────────────── */

/* Determine if a function is a "lifted" function (any __ prefix: __pfor_, __lambda_, __spawn_, etc.) */
static bool is_lifted_func(const char *name) {
    if (!name) return false;
    return strncmp(name, "__", 2) == 0;
}

void emit_func_signature(Iron_StrBuf *sb, IronLIR_Func *fn,
                         EmitCtx *ctx, bool with_newline) {
    const char *ret_c = fn->return_type
                        ? emit_type_to_c(fn->return_type, ctx)
                        : "void";
    const char *c_name = fn->is_extern && fn->extern_c_name
                        ? fn->extern_c_name
                        : emit_mangle_func_name(fn->name, ctx->arena);
    iron_strbuf_appendf(sb, "%s %s(", ret_c, c_name);
    for (int i = 0; i < fn->param_count; i++) {
        if (i > 0) iron_strbuf_appendf(sb, ", ");
        /* HIR pipeline param ID convention:
         * All param synthetic IDs are allocated contiguously first (IDs 1..N),
         * then allocas follow (IDs N+1..2N).  So param i has synthetic id = i+1.
         * The C signature uses the synthetic id so the STORE in the entry
         * block (store alloca_slot = param_val_id) resolves correctly. */
        Iron_Type *pt = fn->params[i].type;
        int param_val_id = i + 1;
        /* PARAM-01/02: Check if this array param uses pointer mode */
        ArrayParamMode pmode = ARRAY_PARAM_LIST;
        if (pt && pt->kind == IRON_TYPE_ARRAY)
            pmode = emit_get_array_param_mode(ctx, fn->name, i);
        if (pmode == ARRAY_PARAM_CONST_PTR) {
            const char *elem_c = emit_type_to_c(pt->array.elem, ctx);
            iron_strbuf_appendf(sb, "const %s *_v%d, int64_t _v%d_len",
                                elem_c, param_val_id, param_val_id);
        } else if (pmode == ARRAY_PARAM_MUT_PTR) {
            const char *elem_c = emit_type_to_c(pt->array.elem, ctx);
            iron_strbuf_appendf(sb, "%s *_v%d, int64_t _v%d_len",
                                elem_c, param_val_id, param_val_id);
        } else if (pt) {
            iron_strbuf_appendf(sb, "%s _v%d",
                                emit_type_to_c(pt, ctx), param_val_id);
        } else {
            iron_strbuf_appendf(sb, "void* _v%d", param_val_id);
        }
    }
    if (fn->param_count == 0) {
        iron_strbuf_appendf(sb, "void");
    }
    iron_strbuf_appendf(sb, ")");
    if (with_newline) {
        iron_strbuf_appendf(sb, ";\n");
    }
}

void emit_func_body(EmitCtx *ctx, IronLIR_Func *fn) {
    /* Choose target buffer: lifted functions go to lifted_funcs */
    Iron_StrBuf *sb = is_lifted_func(fn->name)
                      ? &ctx->lifted_funcs
                      : &ctx->implementations;

    /* Reset per-function ADT boxed-alloca tracking map (Phase 38) */
    hmfree(ctx->adt_boxed_allocas);
    ctx->adt_boxed_allocas = NULL;

    /* Reset per-function stack-array tracking map */
    hmfree(ctx->opt_info->stack_array_ids);
    ctx->opt_info->stack_array_ids = NULL;

    /* Reset per-function heap-array lifecycle tracking */
    hmfree(ctx->opt_info->heap_array_ids);
    ctx->opt_info->heap_array_ids = NULL;
    hmfree(ctx->opt_info->escaped_heap_ids);
    ctx->opt_info->escaped_heap_ids = NULL;

    /* Reset per-function parameter alias tracking */
    hmfree(ctx->param_alias_ids);
    ctx->param_alias_ids = NULL;

    /* Reset per-function split collection tracking */
    hmfree(ctx->split_collection_ids);
    ctx->split_collection_ids = NULL;

    /* Reset per-function fusion chain tracking */
    if (ctx->fusion_chains) {
        for (int fci = 0; fci < (int)arrlen(ctx->fusion_chains); fci++) {
            arrfree(ctx->fusion_chains[fci].nodes);
        }
        arrfree(ctx->fusion_chains);
        ctx->fusion_chains = NULL;
    }
    hmfree(ctx->fusion_chain_member);
    ctx->fusion_chain_member = NULL;
    hmfree(ctx->fusion_chain_position);
    ctx->fusion_chain_position = NULL;

    /* Pre-scan: identify allocas that receive stack arrays via STORE.
     * Build a mapping from STORE(alloca_ptr, stack_array_val) so that
     * the alloca can be emitted as elem_type* instead of Iron_List_T,
     * and LOADs from it are propagated as stack array references.
     * We also build the set: stack-array-literal IDs. */
    {
        /* Phase A: collect all stack-array literal IDs and fill() with constant count */
        struct { IronLIR_ValueId key; IronLIR_ValueId value; } *sa_pre = NULL;
        for (int bi = 0; bi < fn->block_count; bi++) {
            IronLIR_Block *block = fn->blocks[bi];
            for (int ii = 0; ii < block->instr_count; ii++) {
                IronLIR_Instr *instr = block->instrs[ii];
                if (instr->kind == IRON_LIR_ARRAY_LIT && instr->array_lit.use_stack_repr) {
                    hmput(sa_pre, instr->id, instr->id);
                }
                /* __builtin_fill — all calls (constant or dynamic count) */
                if (instr->kind == IRON_LIR_CALL && instr->call.arg_count == 2 &&
                    instr->type && instr->type->kind == IRON_TYPE_ARRAY) {
                    IronLIR_ValueId fptr = instr->call.func_ptr;
                    if (fptr != IRON_LIR_VALUE_INVALID &&
                        fptr < (IronLIR_ValueId)arrlen(fn->value_table) &&
                        fn->value_table[fptr] != NULL &&
                        fn->value_table[fptr]->kind == IRON_LIR_FUNC_REF &&
                        strcmp(fn->value_table[fptr]->func_ref.func_name,
                               "__builtin_fill") == 0) {
                        /* Skip if revoked by escape analysis */
                        if (hmgeti(ctx->opt_info->revoked_fill_ids, instr->id) < 0) {
                            hmput(sa_pre, instr->id, instr->id);
                        }
                    }
                }
            }
        }
        /* Phase A2 (PARAM-01/02): inject pointer-mode array params so
         * propagation in Phase B reaches allocas receiving param values. */
        for (int ppi = 0; ppi < fn->param_count; ppi++) {
            Iron_Type *ppt = fn->params[ppi].type;
            if (!ppt || ppt->kind != IRON_TYPE_ARRAY) continue;
            ArrayParamMode pmode = emit_get_array_param_mode(ctx, fn->name, ppi);
            if (pmode == ARRAY_PARAM_CONST_PTR || pmode == ARRAY_PARAM_MUT_PTR) {
                IronLIR_ValueId pvid = (IronLIR_ValueId)(ppi + 1);
                hmput(sa_pre, pvid, pvid);
            }
        }
        /* Phase B: propagate through STORE and LOAD chains */
        for (int bi = 0; bi < fn->block_count; bi++) {
            IronLIR_Block *block = fn->blocks[bi];
            for (int ii = 0; ii < block->instr_count; ii++) {
                IronLIR_Instr *instr = block->instrs[ii];
                if (instr->kind == IRON_LIR_STORE) {
                    ptrdiff_t vi = hmgeti(sa_pre, instr->store.value);
                    if (vi >= 0) hmput(sa_pre, instr->store.ptr, sa_pre[vi].value);
                } else if (instr->kind == IRON_LIR_LOAD) {
                    ptrdiff_t vi = hmgeti(sa_pre, instr->load.ptr);
                    if (vi >= 0) hmput(sa_pre, instr->id, sa_pre[vi].value);
                }
            }
        }
        /* Seed the ctx map with the pre-scan results */
        for (ptrdiff_t i = 0; i < hmlen(sa_pre); i++) {
            hmput(ctx->opt_info->stack_array_ids, sa_pre[i].key, sa_pre[i].value);
        }
        hmfree(sa_pre);
    }

    /* ── Heap-array lifecycle pre-scan (COLL-04) ────────────────────────────
     * Collect all heap-allocated ARRAY_LIT instructions (!use_stack_repr)
     * and __builtin_fill CALL instructions.  Propagate through STORE/LOAD
     * chains.  Then determine which escape (via RETURN, SET_FIELD,
     * CONSTRUCT, CALL arg, or MAKE_CLOSURE capture). */
    {
        struct { IronLIR_ValueId key; IronLIR_ValueId value; } *ha_pre = NULL;

        /* Phase A: collect all heap-array literal IDs and builtin_fill results */
        for (int bi = 0; bi < fn->block_count; bi++) {
            IronLIR_Block *block = fn->blocks[bi];
            for (int ii = 0; ii < block->instr_count; ii++) {
                IronLIR_Instr *instr = block->instrs[ii];
                if (instr->kind == IRON_LIR_ARRAY_LIT && !instr->array_lit.use_stack_repr) {
                    hmput(ha_pre, instr->id, instr->id);
                }
                /* __builtin_fill calls produce heap lists ONLY if not stack-eligible */
                if (instr->kind == IRON_LIR_CALL && instr->type &&
                    instr->type->kind == IRON_TYPE_ARRAY) {
                    IronLIR_ValueId fptr = instr->call.func_ptr;
                    if (fptr != IRON_LIR_VALUE_INVALID &&
                        fptr < (IronLIR_ValueId)arrlen(fn->value_table) &&
                        fn->value_table[fptr] != NULL &&
                        fn->value_table[fptr]->kind == IRON_LIR_FUNC_REF &&
                        strcmp(fn->value_table[fptr]->func_ref.func_name,
                               "__builtin_fill") == 0) {
                        /* Only track as heap if NOT in stack_array_ids (already stack) */
                        if (hmgeti(ctx->opt_info->stack_array_ids, instr->id) < 0) {
                            hmput(ha_pre, instr->id, instr->id);
                        }
                    }
                }
            }
        }

        /* Phase B: propagate through STORE/LOAD chains */
        for (int bi = 0; bi < fn->block_count; bi++) {
            IronLIR_Block *block = fn->blocks[bi];
            for (int ii = 0; ii < block->instr_count; ii++) {
                IronLIR_Instr *instr = block->instrs[ii];
                if (instr->kind == IRON_LIR_STORE) {
                    ptrdiff_t vi = hmgeti(ha_pre, instr->store.value);
                    if (vi >= 0) hmput(ha_pre, instr->store.ptr, ha_pre[vi].value);
                } else if (instr->kind == IRON_LIR_LOAD) {
                    ptrdiff_t vi = hmgeti(ha_pre, instr->load.ptr);
                    if (vi >= 0) hmput(ha_pre, instr->id, ha_pre[vi].value);
                }
            }
        }

        /* Phase B2: remove any ha_pre entries whose origin is a stack array.
         * This handles fill() calls that were promoted to stack arrays. */
        {
            IronLIR_ValueId *to_remove = NULL;
            for (ptrdiff_t i = 0; i < hmlen(ha_pre); i++) {
                IronLIR_ValueId orig = ha_pre[i].value;
                if (hmgeti(ctx->opt_info->stack_array_ids, orig) >= 0) {
                    arrput(to_remove, ha_pre[i].key);
                }
            }
            for (int i = 0; i < (int)arrlen(to_remove); i++) {
                hmdel(ha_pre, to_remove[i]);
            }
            arrfree(to_remove);
        }

        /* Phase C: mark escaping arrays */
        for (int bi = 0; bi < fn->block_count; bi++) {
            IronLIR_Block *block = fn->blocks[bi];
            for (int ii = 0; ii < block->instr_count; ii++) {
                IronLIR_Instr *instr = block->instrs[ii];
                /* Escapes via RETURN */
                if (instr->kind == IRON_LIR_RETURN && !instr->ret.is_void) {
                    ptrdiff_t vi = hmgeti(ha_pre, instr->ret.value);
                    if (vi >= 0) hmput(ctx->opt_info->escaped_heap_ids, ha_pre[vi].value, true);
                }
                /* Escapes via SET_FIELD (stored into object) */
                if (instr->kind == IRON_LIR_SET_FIELD) {
                    ptrdiff_t vi = hmgeti(ha_pre, instr->field.value);
                    if (vi >= 0) hmput(ctx->opt_info->escaped_heap_ids, ha_pre[vi].value, true);
                }
                /* Escapes via CONSTRUCT (embedded in struct) */
                if (instr->kind == IRON_LIR_CONSTRUCT) {
                    for (int fi2 = 0; fi2 < instr->construct.field_count; fi2++) {
                        ptrdiff_t vi = hmgeti(ha_pre, instr->construct.field_vals[fi2]);
                        if (vi >= 0) hmput(ctx->opt_info->escaped_heap_ids, ha_pre[vi].value, true);
                    }
                }
                /* Escapes via CALL argument (passed to another function) */
                if (instr->kind == IRON_LIR_CALL) {
                    for (int ai = 0; ai < instr->call.arg_count; ai++) {
                        ptrdiff_t vi = hmgeti(ha_pre, instr->call.args[ai]);
                        if (vi >= 0) hmput(ctx->opt_info->escaped_heap_ids, ha_pre[vi].value, true);
                    }
                }
                /* Escapes via MAKE_CLOSURE capture */
                if (instr->kind == IRON_LIR_MAKE_CLOSURE) {
                    for (int ci = 0; ci < instr->make_closure.capture_count; ci++) {
                        ptrdiff_t vi = hmgeti(ha_pre, instr->make_closure.captures[ci]);
                        if (vi >= 0) hmput(ctx->opt_info->escaped_heap_ids, ha_pre[vi].value, true);
                    }
                }
            }
        }

        /* Seed the ctx heap_array_ids map (skip stack arrays) */
        for (ptrdiff_t i = 0; i < hmlen(ha_pre); i++) {
            IronLIR_ValueId orig = ha_pre[i].value;
            if (hmgeti(ctx->opt_info->stack_array_ids, orig) >= 0) continue;
            hmput(ctx->opt_info->heap_array_ids, ha_pre[i].key, ha_pre[i].value);
        }
        hmfree(ha_pre);
    }

    /* ── ADT boxed-alloca pre-scan (Phase 38) ─────────────────────────────
     * Collect ALLOCA instructions whose alloc_type is a recursive ADT enum
     * (i.e. payload_is_boxed is non-NULL and at least one slot is boxed).
     * These locals will be freed at every RETURN site (unless returned). */
    {
        for (int bi = 0; bi < fn->block_count; bi++) {
            IronLIR_Block *block = fn->blocks[bi];
            for (int ii = 0; ii < block->instr_count; ii++) {
                IronLIR_Instr *in = block->instrs[ii];
                if (in->kind != IRON_LIR_ALLOCA) continue;
                Iron_Type *atype = in->alloca.alloc_type;
                if (!atype || atype->kind != IRON_TYPE_ENUM) continue;
                if (!atype->enu.payload_is_boxed) continue;
                Iron_EnumDecl *aed = atype->enu.decl;
                if (!aed) continue;
                bool any_boxed = false;
                for (int avi = 0; avi < aed->variant_count && !any_boxed; avi++) {
                    if (!atype->enu.payload_is_boxed[avi]) continue;
                    /* PROT-03 unenumerated bonus (AUDIT-01 M-severity sibling
                     * of rows 25/26/34): loop-bound + non-NULL assert before
                     * the Iron_EnumVariant cast in the boxed-alloca scan. */
                    assert(avi >= 0 && avi < aed->variant_count);
                    assert(aed->variants[avi] != NULL);
                    Iron_EnumVariant *aev = (Iron_EnumVariant *)aed->variants[avi];
                    for (int ak = 0; ak < aev->payload_count; ak++) {
                        if (atype->enu.payload_is_boxed[avi][ak]) {
                            any_boxed = true;
                            break;
                        }
                    }
                }
                if (any_boxed) {
                    hmput(ctx->adt_boxed_allocas, in->id, atype);
                }
            }
        }
    }

    /* ── Read-only parameter alias pre-scan ────────────────────────────────
     * Detect parameter allocas that are:
     *   1. Stored to exactly once (the initial param store in the entry block)
     *   2. Never stored to again in any other block
     *   3. Not already handled by pointer-mode array params (stack_array_ids)
     * For such allocas, we skip the alloca decl and store, and make loads
     * reference the parameter value ID directly. */
    {
        for (int pi = 0; pi < fn->param_count; pi++) {
            /* HIR pipeline: params 1..N, allocas N+1..2N */
            IronLIR_ValueId param_val = (IronLIR_ValueId)(pi + 1);
            IronLIR_ValueId alloca_id = (IronLIR_ValueId)(fn->param_count + pi + 1);

            /* Skip if this alloca is already tracked as a stack array (pointer-mode param) */
            if (hmgeti(ctx->opt_info->stack_array_ids, alloca_id) >= 0) continue;

            /* Count stores to this alloca across ALL blocks */
            int store_count = 0;
            for (int bi = 0; bi < fn->block_count; bi++) {
                IronLIR_Block *block = fn->blocks[bi];
                for (int ii = 0; ii < block->instr_count; ii++) {
                    IronLIR_Instr *instr = block->instrs[ii];
                    if (instr->kind == IRON_LIR_STORE &&
                        instr->store.ptr == alloca_id) {
                        store_count++;
                    }
                }
            }

            /* Only alias if stored exactly once (the initial param store) */
            if (store_count == 1) {
                hmput(ctx->param_alias_ids, alloca_id, param_val);
            }
        }
    }

    /* ── Expression inlining pre-scan ────────────────────────────────────────
     * Build per-function inline_eligible and value_block maps.
     * Only runs when the optimizer has populated func_purity (module-wide analysis). */
    {
        /* Reset per-function inlining maps */
        hmfree(ctx->inline_eligible);
        ctx->inline_eligible = NULL;
        hmfree(ctx->value_block);
        ctx->value_block = NULL;

        if (ctx->opt_info && ctx->opt_info->func_purity != NULL) {
            /* Compute per-function analysis using ir_optimize helpers */
            hmfree(ctx->opt_info->use_counts);
            ctx->opt_info->use_counts = NULL;
            iron_lir_compute_use_counts(fn, ctx->opt_info);

            hmfree(ctx->opt_info->value_block);
            ctx->opt_info->value_block = NULL;
            iron_lir_compute_value_block(fn, ctx->opt_info);

            hmfree(ctx->opt_info->inline_eligible);
            ctx->opt_info->inline_eligible = NULL;
            iron_lir_compute_inline_eligible(fn, ctx->opt_info);

            /* Copy maps to ctx for emit_instr/emit_expr_to_buf access */
            ctx->inline_eligible = ctx->opt_info->inline_eligible;
            ctx->value_block     = ctx->opt_info->value_block;
        }
    }

    /* -- Phase 49: Pre-populate split_collection_ids for ARRAY_LIT instructions
     * (needed before fusion chain detection so chains on split collections
     * can be identified as split).  The Phase 41 scan below is a duplicate
     * but safe — hmput is idempotent for same key. */
    if (ctx->iface_reg) {
        for (int bi = 0; bi < fn->block_count; bi++) {
            IronLIR_Block *blk = fn->blocks[bi];
            for (int ii = 0; ii < blk->instr_count; ii++) {
                IronLIR_Instr *in2 = blk->instrs[ii];
                if (in2->kind == IRON_LIR_ARRAY_LIT &&
                    in2->array_lit.elem_type &&
                    in2->array_lit.elem_type->kind == IRON_TYPE_INTERFACE &&
                    in2->array_lit.elem_type->interface.decl) {
                    const char *im = emit_mangle_name(
                        in2->array_lit.elem_type->interface.decl->name, ctx->arena);
                    const char *im_copy = iron_arena_strdup(ctx->arena, im, strlen(im));
                    if (!im_copy) iron_oom_abort("emit_c.c:emit_func_body array_lit iface_mangled (A)");
                    hmput(ctx->split_collection_ids, in2->id, im_copy);
                }
                /* Phase 53: CALL instructions returning interface-typed arrays.
                 * The callee returns Iron_SplitList_<Iface>, so the CALL result
                 * must also be tracked as a split collection. */
                if (in2->kind == IRON_LIR_CALL &&
                    in2->id != IRON_LIR_VALUE_INVALID &&
                    in2->type && in2->type->kind == IRON_TYPE_ARRAY &&
                    in2->type->array.elem &&
                    in2->type->array.elem->kind == IRON_TYPE_INTERFACE &&
                    in2->type->array.elem->interface.decl) {
                    const char *im = emit_mangle_name(
                        in2->type->array.elem->interface.decl->name, ctx->arena);
                    const char *im_copy = iron_arena_strdup(ctx->arena, im, strlen(im));
                    if (!im_copy) iron_oom_abort("emit_c.c:emit_func_body call_result iface_mangled (A)");
                    hmput(ctx->split_collection_ids, in2->id, im_copy);
                }
            }
        }
        /* Propagate split_collection_ids through STORE/LOAD chains so that
         * fusion chain detection can find split sources after val assignment. */
        for (int bi = 0; bi < fn->block_count; bi++) {
            IronLIR_Block *blk = fn->blocks[bi];
            for (int ii = 0; ii < blk->instr_count; ii++) {
                IronLIR_Instr *in2 = blk->instrs[ii];
                if (in2->kind == IRON_LIR_STORE) {
                    ptrdiff_t si = hmgeti(ctx->split_collection_ids, in2->store.value);
                    if (si >= 0) hmput(ctx->split_collection_ids, in2->store.ptr,
                                       ctx->split_collection_ids[si].value);
                } else if (in2->kind == IRON_LIR_LOAD) {
                    ptrdiff_t si = hmgeti(ctx->split_collection_ids, in2->load.ptr);
                    if (si >= 0) hmput(ctx->split_collection_ids, in2->id,
                                       ctx->split_collection_ids[si].value);
                }
            }
        }
    }

    /* -- Phase 49: Fusion chain detection pre-scan ----------------------------
     * Identify sequences of fusible collection method CALLs where each
     * method's result feeds directly (or through STORE/LOAD) into the next
     * method's self argument.  Build fusion_chains array and membership map.
     */
    {
        /* Step A: Collect all fusible CALL instructions */
        typedef struct {
            IronLIR_ValueId call_vid;
            const char *method;
            IronLIR_ValueId self_arg;
            IronLIR_ValueId *lambda_args;
            int lambda_arg_count;
            IronLIR_ValueId init_arg;
        } FusibleCallInfo;
        FusibleCallInfo *fusible_calls = NULL;

        for (int bi = 0; bi < fn->block_count; bi++) {
            IronLIR_Block *block = fn->blocks[bi];
            for (int ii = 0; ii < block->instr_count; ii++) {
                IronLIR_Instr *instr = block->instrs[ii];
                if (instr->kind != IRON_LIR_CALL || instr->call.arg_count < 1) continue;

                IronLIR_ValueId fptr = instr->call.func_ptr;
                if (fptr == IRON_LIR_VALUE_INVALID ||
                    fptr >= (IronLIR_ValueId)arrlen(fn->value_table) ||
                    fn->value_table[fptr] == NULL ||
                    fn->value_table[fptr]->kind != IRON_LIR_FUNC_REF) continue;

                const char *fname = emit_resolve_func_c_name(ctx, fn->value_table[fptr]->func_ref.func_name);
                if (!fname || strncmp(fname, "Iron_List_", 10) != 0) continue;

                const char *suffix = strrchr(fname, '_');
                if (!suffix) continue;

                const char *method = NULL;
                if (strcmp(suffix, "_map") == 0) method = "map";
                else if (strcmp(suffix, "_filter") == 0) method = "filter";
                else if (strcmp(suffix, "_reduce") == 0) method = "reduce";
                else if (strcmp(suffix, "_forEach") == 0) method = "forEach";
                else if (strcmp(suffix, "_sum") == 0) method = "sum";
                if (!method) continue;

                FusibleCallInfo fci;
                fci.call_vid = instr->id;
                fci.method = method;
                fci.self_arg = instr->call.args[0];
                fci.lambda_args = NULL;
                fci.lambda_arg_count = 0;
                fci.init_arg = IRON_LIR_VALUE_INVALID;

                if (strcmp(method, "reduce") == 0 && instr->call.arg_count >= 3) {
                    fci.init_arg = instr->call.args[1];
                    for (int ai = 2; ai < instr->call.arg_count; ai++)
                        arrput(fci.lambda_args, instr->call.args[ai]);
                    fci.lambda_arg_count = instr->call.arg_count - 2;
                } else if (strcmp(method, "sum") != 0 && instr->call.arg_count >= 2) {
                    for (int ai = 1; ai < instr->call.arg_count; ai++)
                        arrput(fci.lambda_args, instr->call.args[ai]);
                    fci.lambda_arg_count = instr->call.arg_count - 1;
                }

                arrput(fusible_calls, fci);
            }
        }

        if (arrlen(fusible_calls) > 0) {
            /* Step B: Build result-to-call reverse map */
            struct { IronLIR_ValueId key; int value; } *result_to_call = NULL;
            for (int i = 0; i < (int)arrlen(fusible_calls); i++) {
                hmput(result_to_call, fusible_calls[i].call_vid, i);
            }

            /* Step C: Propagate through STORE/LOAD chains */
            struct { IronLIR_ValueId key; IronLIR_ValueId value; } *val_origin = NULL;
            for (int i = 0; i < (int)arrlen(fusible_calls); i++) {
                hmput(val_origin, fusible_calls[i].call_vid, fusible_calls[i].call_vid);
            }
            for (int bi = 0; bi < fn->block_count; bi++) {
                IronLIR_Block *block = fn->blocks[bi];
                for (int ii = 0; ii < block->instr_count; ii++) {
                    IronLIR_Instr *instr = block->instrs[ii];
                    if (instr->kind == IRON_LIR_STORE) {
                        ptrdiff_t vi = hmgeti(val_origin, instr->store.value);
                        if (vi >= 0) hmput(val_origin, instr->store.ptr, val_origin[vi].value);
                    } else if (instr->kind == IRON_LIR_LOAD) {
                        ptrdiff_t vi = hmgeti(val_origin, instr->load.ptr);
                        if (vi >= 0) hmput(val_origin, instr->id, val_origin[vi].value);
                    }
                }
            }

            /* Step D: Build chains by linking calls */
            /* next[i] = index of the fusible call that consumes call i's result, or -1 */
            int *next = (int *)calloc((size_t)arrlen(fusible_calls), sizeof(int));
            int *prev = (int *)calloc((size_t)arrlen(fusible_calls), sizeof(int));
            for (int i = 0; i < (int)arrlen(fusible_calls); i++) {
                next[i] = -1;
                prev[i] = -1;
            }
            for (int i = 0; i < (int)arrlen(fusible_calls); i++) {
                IronLIR_ValueId sa = fusible_calls[i].self_arg;
                ptrdiff_t oi = hmgeti(val_origin, sa);
                if (oi >= 0) {
                    IronLIR_ValueId origin_vid = val_origin[oi].value;
                    ptrdiff_t ri = hmgeti(result_to_call, origin_vid);
                    if (ri >= 0) {
                        int pred_idx = result_to_call[ri].value;
                        if (pred_idx != i) {
                            next[pred_idx] = i;
                            prev[i] = pred_idx;
                        }
                    }
                }
            }

            /* Step E: Use-count escape check — break chains at intermediate nodes with use_count > 1 */
            if (ctx->opt_info && ctx->opt_info->use_counts) {
                for (int i = 0; i < (int)arrlen(fusible_calls); i++) {
                    if (next[i] < 0) continue; /* terminal or isolated — skip */
                    IronLIR_ValueId cvid = fusible_calls[i].call_vid;
                    ptrdiff_t uc_idx = hmgeti(ctx->opt_info->use_counts, cvid);
                    int uc = (uc_idx >= 0) ? ctx->opt_info->use_counts[uc_idx].value : 0;
                    if (uc > 1) {
                        /* Intermediate result escapes — break chain */
                        int ni = next[i];
                        prev[ni] = -1;
                        next[i] = -1;
                    }
                    /* Also check STORE'd values: if a STORE writes this call result
                     * to an alloca that has multiple LOADs, the value escapes. */
                    for (ptrdiff_t vo = 0; vo < hmlen(val_origin); vo++) {
                        if (val_origin[vo].value == cvid && val_origin[vo].key != cvid) {
                            ptrdiff_t st_uc = hmgeti(ctx->opt_info->use_counts, val_origin[vo].key);
                            if (st_uc >= 0 && ctx->opt_info->use_counts[st_uc].value > 1) {
                                int ni = next[i];
                                if (ni >= 0) prev[ni] = -1;
                                next[i] = -1;
                                break;
                            }
                        }
                    }
                }
            }

            /* Walk chains: start from nodes with no predecessor */
            for (int i = 0; i < (int)arrlen(fusible_calls); i++) {
                if (prev[i] >= 0) continue; /* not a chain start */
                if (next[i] < 0) continue;  /* isolated node — no chain */

                /* Build chain from i -> next[i] -> ... */
                FusionChain chain;
                memset(&chain, 0, sizeof(chain));
                chain.nodes = NULL;
                chain.source = fusible_calls[i].self_arg;

                int cur = i;
                while (cur >= 0) {
                    FusionChainNode node;
                    node.call_vid = fusible_calls[cur].call_vid;
                    node.method = fusible_calls[cur].method;
                    node.self_arg = fusible_calls[cur].self_arg;
                    node.lambda_args = fusible_calls[cur].lambda_args;
                    node.lambda_arg_count = fusible_calls[cur].lambda_arg_count;
                    node.init_arg = fusible_calls[cur].init_arg;
                    arrput(chain.nodes, node);
                    cur = next[cur];
                }
                chain.node_count = (int)arrlen(chain.nodes);

                /* Step F: Filter out single-node chains */
                if (chain.node_count < 2) {
                    arrfree(chain.nodes);
                    continue;
                }

                /* Check if source is a split collection */
                ptrdiff_t sp_idx = hmgeti(ctx->split_collection_ids, chain.source);
                if (sp_idx >= 0) {
                    chain.is_split = true;
                    chain.sp_iface = ctx->split_collection_ids[sp_idx].value;
                } else {
                    /* Also check through val_origin in case source was stored/loaded */
                    ptrdiff_t src_oi = hmgeti(val_origin, chain.source);
                    if (src_oi >= 0) {
                        sp_idx = hmgeti(ctx->split_collection_ids, val_origin[src_oi].value);
                        if (sp_idx >= 0) {
                            chain.is_split = true;
                            chain.sp_iface = ctx->split_collection_ids[sp_idx].value;
                        }
                    }
                }

                int chain_idx = (int)arrlen(ctx->fusion_chains);
                arrput(ctx->fusion_chains, chain);

                for (int ni = 0; ni < chain.node_count; ni++) {
                    hmput(ctx->fusion_chain_member, chain.nodes[ni].call_vid, chain_idx);
                    hmput(ctx->fusion_chain_position, chain.nodes[ni].call_vid, ni);
                }
            }

            /* Step F2: Emit --warn-fusion-break diagnostics */
            if (ctx->warn_fusion_break) {
                for (int i = 0; i < (int)arrlen(fusible_calls); i++) {
                    IronLIR_ValueId cvid = fusible_calls[i].call_vid;
                    /* Check if this call's result goes to a non-fusible consumer */
                    if (next[i] < 0 && hmgeti(ctx->fusion_chain_member, cvid) < 0) {
                        /* This fusible call is not in any chain — check if its result
                         * feeds into another call at all (non-fusible break) */
                        bool feeds_fusible = false;
                        for (int j = 0; j < (int)arrlen(fusible_calls); j++) {
                            if (j == i) continue;
                            ptrdiff_t oi = hmgeti(val_origin, fusible_calls[j].self_arg);
                            if (oi >= 0 && val_origin[oi].value == cvid) {
                                feeds_fusible = true;
                                break;
                            }
                        }
                        if (!feeds_fusible && next[i] < 0 && prev[i] >= 0) {
                            /* Was in a chain but got broken — escape */
                            fprintf(stderr, "note: --warn-fusion-break: fusion chain broken: "
                                    "intermediate result of .%s() used elsewhere in function %s\n",
                                    fusible_calls[i].method, fn->name);
                        }
                    }
                    /* Check for use_count > 1 breaks */
                    if (ctx->opt_info && ctx->opt_info->use_counts) {
                        ptrdiff_t uc_idx = hmgeti(ctx->opt_info->use_counts, cvid);
                        int uc = (uc_idx >= 0) ? ctx->opt_info->use_counts[uc_idx].value : 0;
                        if (uc > 1 && hmgeti(ctx->fusion_chain_member, cvid) < 0) {
                            fprintf(stderr, "note: --warn-fusion-break: fusion chain broken: "
                                    "intermediate result of .%s() used elsewhere in function %s\n",
                                    fusible_calls[i].method, fn->name);
                        }
                    }
                }
            }

            hmfree(result_to_call);
            hmfree(val_origin);
            free(next);
            free(prev);
        }

        /* Free lambda_args for candidates not adopted into chains */
        for (int i = 0; i < (int)arrlen(fusible_calls); i++) {
            if (hmgeti(ctx->fusion_chain_member, fusible_calls[i].call_vid) < 0) {
                arrfree(fusible_calls[i].lambda_args);
            }
        }
        arrfree(fusible_calls);
    }

    emit_func_signature(sb, fn, ctx, false);
    iron_strbuf_appendf(sb, " {\n");

    ctx->indent = 1;

    /* ── Capture alias setup for lifted lambda functions ─────────────────────
     * For lifted functions with captures, build a map from alloca ValueId to
     * capture index, and emit the env cast prologue.
     * The _env parameter is _v1 (first parameter, synthetic ID 1). */
    hmfree(ctx->capture_alias_map);
    ctx->capture_alias_map    = NULL;
    ctx->current_captures     = NULL;
    ctx->current_capture_count = 0;

    if (fn->capture_count > 0 && fn->capture_metadata && fn->param_count > 0) {
        ctx->current_captures      = fn->capture_metadata;
        ctx->current_capture_count = fn->capture_count;

        /* Build capture_alias_map: alloca ValueId -> capture index.
         * Scan all ALLOCAs in the function; if name_hint matches a capture name,
         * record the mapping. */
        for (int bi = 0; bi < fn->block_count; bi++) {
            IronLIR_Block *blk = fn->blocks[bi];
            for (int ii = 0; ii < blk->instr_count; ii++) {
                IronLIR_Instr *ins = blk->instrs[ii];
                if (ins->kind != IRON_LIR_ALLOCA) continue;
                const char *hint = ins->alloca.name_hint;
                if (!hint) continue;
                for (int ci = 0; ci < fn->capture_count; ci++) {
                    if (strcmp(hint, fn->capture_metadata[ci].name) == 0) {
                        hmput(ctx->capture_alias_map, ins->id, ci);
                        break;
                    }
                }
            }
        }

        /* Emit env cast prologue: `<func_name>_env_t *_e = (<func_name>_env_t *)_v1;`
         * _v1 is the synthetic param ID for _env (first parameter). */
        emit_indent(sb, 1);
        iron_strbuf_appendf(sb, "%s_env_t *_e = (%s_env_t *)_v1;\n",
                            fn->name, fn->name);
    }

    /* Compute reachable blocks via BFS from entry to avoid emitting dead code
     * that triggers C compiler warnings (e.g. void return in non-void function). */
    bool *blk_reachable = (bool *)calloc((size_t)fn->block_count, sizeof(bool));
    if (blk_reachable && fn->block_count > 0) {
        IronLIR_BlockId *wl = NULL;
        blk_reachable[0] = true;
        arrput(wl, fn->blocks[0]->id);
        while (arrlen(wl) > 0) {
            IronLIR_BlockId cid = arrpop(wl);
            for (int bi2 = 0; bi2 < fn->block_count; bi2++) {
                if (fn->blocks[bi2]->id != cid) continue;
                IronLIR_Block *cb = fn->blocks[bi2];
                for (int si = 0; si < (int)arrlen(cb->succs); si++) {
                    IronLIR_BlockId sid = cb->succs[si];
                    for (int bi3 = 0; bi3 < fn->block_count; bi3++) {
                        if (fn->blocks[bi3]->id == sid && !blk_reachable[bi3]) {
                            blk_reachable[bi3] = true;
                            arrput(wl, sid);
                        }
                    }
                }
                break;
            }
        }
        arrfree(wl);
    }

    /* Hoist backward-referenced values: detect values defined in later blocks
     * but used in earlier blocks (backward gotos in loops) and pre-declare them. */
    hmfree(ctx->phi_hoisted);
    ctx->phi_hoisted = NULL;
    if (ctx->value_block) {
        /* use_block_min[vid] = earliest block index where vid is used as operand.
         * Tracks both value operands (store.value, binop operands, etc.) and
         * pointer operands (load.ptr, store.ptr) for ALLOCA hoisting. */
        struct { IronLIR_ValueId key; int value; } *use_block_min = NULL;

        /* Helper macro: record that value V is used in block BI2 */
#define TRACK_USE(v, bi2_) do { \
    if ((v) != IRON_LIR_VALUE_INVALID) { \
        ptrdiff_t _idx = hmgeti(use_block_min, (v)); \
        if (_idx < 0 || (bi2_) < use_block_min[_idx].value) \
            hmput(use_block_min, (v), (bi2_)); \
    } \
} while (0)

        for (int bi2 = 0; bi2 < fn->block_count; bi2++) {
            IronLIR_Block *blk = fn->blocks[bi2];
            for (int ii = 0; ii < blk->instr_count; ii++) {
                IronLIR_Instr *in = blk->instrs[ii];
                /* Track all value operand uses for backward-reference detection. */
                switch ((int)(in->kind)) {
                case IRON_LIR_STORE:
                    TRACK_USE(in->store.value, bi2);
                    TRACK_USE(in->store.ptr, bi2);
                    break;
                case IRON_LIR_LOAD:
                    TRACK_USE(in->load.ptr, bi2);
                    break;
                case IRON_LIR_GET_INDEX:
                    TRACK_USE(in->index.array, bi2);
                    TRACK_USE(in->index.index, bi2);
                    break;
                case IRON_LIR_SET_INDEX:
                    TRACK_USE(in->index.array, bi2);
                    TRACK_USE(in->index.index, bi2);
                    TRACK_USE(in->index.value, bi2);
                    break;
                case IRON_LIR_ADD: case IRON_LIR_SUB: case IRON_LIR_MUL:
                case IRON_LIR_DIV: case IRON_LIR_MOD:
                case IRON_LIR_EQ: case IRON_LIR_NEQ: case IRON_LIR_LT:
                case IRON_LIR_LTE: case IRON_LIR_GT: case IRON_LIR_GTE:
                case IRON_LIR_AND: case IRON_LIR_OR:
                case IRON_LIR_SHL: case IRON_LIR_SHR:
                case IRON_LIR_BAND: case IRON_LIR_BOR: case IRON_LIR_BXOR:
                    TRACK_USE(in->binop.left, bi2);
                    TRACK_USE(in->binop.right, bi2);
                    break;
                case IRON_LIR_NEG: case IRON_LIR_NOT: case IRON_LIR_BNOT:
                    TRACK_USE(in->unop.operand, bi2);
                    break;
                case IRON_LIR_CALL:
                    for (int ai = 0; ai < in->call.arg_count; ai++)
                        TRACK_USE(in->call.args[ai], bi2);
                    break;
                case IRON_LIR_BRANCH:
                    TRACK_USE(in->branch.cond, bi2);
                    break;
                case IRON_LIR_RETURN:
                    if (!in->ret.is_void) TRACK_USE(in->ret.value, bi2);
                    break;
                case IRON_LIR_GET_FIELD:
                    TRACK_USE(in->field.object, bi2);
                    break;
                case IRON_LIR_SET_FIELD:
                    TRACK_USE(in->field.object, bi2);
                    TRACK_USE(in->field.value, bi2);
                    break;
                case IRON_LIR_INTERP_STRING:
                    for (int pi2 = 0; pi2 < in->interp_string.part_count; pi2++)
                        TRACK_USE(in->interp_string.parts[pi2], bi2);
                    break;
                case IRON_LIR_CAST:
                    TRACK_USE(in->cast.value, bi2);
                    break;
                case IRON_LIR_IS_NULL: case IRON_LIR_IS_NOT_NULL:
                    TRACK_USE(in->null_check.value, bi2);
                    break;
                /* -Wswitch-enum opt-out: use-site hoist tracker only needs to
                 * inspect opcodes that consume a typed operand; opcodes with
                 * no value dependency (labels, nop-like markers) are safely
                 * skipped. */
                default:
                    break;
                }
            }
        }
#undef TRACK_USE

        /* For each non-entry-block value used before its definition block, hoist */
        for (int bi2 = 1; bi2 < fn->block_count; bi2++) {
            IronLIR_Block *blk = fn->blocks[bi2];
            for (int ii = 0; ii < blk->instr_count; ii++) {
                IronLIR_Instr *in = blk->instrs[ii];
                if (in->id == IRON_LIR_VALUE_INVALID) continue;
                if (ctx->inline_eligible && hmgeti(ctx->inline_eligible, in->id) >= 0) continue;
                if (iron_lir_is_terminator(in->kind)) continue;

                ptrdiff_t um = hmgeti(use_block_min, in->id);
                if (um < 0 || use_block_min[um].value >= bi2) continue;

                if (in->kind == IRON_LIR_ALLOCA) {
                    /* Hoist ALLOCA: pre-declare at function entry, skip at definition site.
                     * Skip stack arrays (handled separately by fill_hoisted machinery). */
                    if (get_stack_array_origin(ctx, in->id) != IRON_LIR_VALUE_INVALID) continue;
                    if (!in->alloca.alloc_type) continue;
                    hmput(ctx->phi_hoisted, in->id, true);
                    const char *c_type = emit_type_to_c(in->alloca.alloc_type, ctx);
                    emit_indent(sb, 1);
                    iron_strbuf_appendf(sb, "%s _v%u;\n", c_type, in->id);
                } else {
                    /* Hoist any other value-producing instruction (LOAD, CALL, binop, etc.)
                     * that is used in an earlier block.  Pre-declare the result variable at
                     * function entry; at the definition site, emit assignment without type
                     * prefix (is_hoisted=true path in emit_instr). */
                    if (!in->type || in->type->kind == IRON_TYPE_VOID) continue;
                    /* Don't hoist stack arrays — they need special initialization. */
                    if (get_stack_array_origin(ctx, in->id) != IRON_LIR_VALUE_INVALID) continue;
                    /* Phase 49: Don't hoist chain-interior fusible calls (they emit no code) */
                    if (ctx->fusion_chain_member) {
                        ptrdiff_t fc_hoist = hmgeti(ctx->fusion_chain_member, in->id);
                        if (fc_hoist >= 0) {
                            int ci_h = ctx->fusion_chain_member[fc_hoist].value;
                            ptrdiff_t fp_h = hmgeti(ctx->fusion_chain_position, in->id);
                            int pos_h = (fp_h >= 0) ? ctx->fusion_chain_position[fp_h].value : 0;
                            if (pos_h < ctx->fusion_chains[ci_h].node_count - 1) continue;
                        }
                    }
                    hmput(ctx->phi_hoisted, in->id, true);
                    emit_indent(sb, 1);
                    iron_strbuf_appendf(sb, "%s _v%u;\n",
                        emit_type_to_c(in->type, ctx), in->id);
                }
            }
        }
        hmfree(use_block_min);
    }

    /* ── Phase 41: Pre-scan for interface-typed ARRAY_LITs and CALL results ─
     * Populate split_collection_ids before the block emission loop so that
     * split-eligible for-loops can be detected. */
    if (ctx->iface_reg) {
        for (int bi = 0; bi < fn->block_count; bi++) {
            IronLIR_Block *blk = fn->blocks[bi];
            for (int ii = 0; ii < blk->instr_count; ii++) {
                IronLIR_Instr *in2 = blk->instrs[ii];
                if (in2->kind == IRON_LIR_ARRAY_LIT &&
                    in2->array_lit.elem_type &&
                    in2->array_lit.elem_type->kind == IRON_TYPE_INTERFACE &&
                    in2->array_lit.elem_type->interface.decl) {
                    const char *im = emit_mangle_name(
                        in2->array_lit.elem_type->interface.decl->name, ctx->arena);
                    const char *im_copy = iron_arena_strdup(ctx->arena, im, strlen(im));
                    if (!im_copy) iron_oom_abort("emit_c.c:emit_func_body array_lit iface_mangled (B)");
                    hmput(ctx->split_collection_ids, in2->id, im_copy);
                }
                /* Phase 53: CALL returning interface array -> split collection */
                if (in2->kind == IRON_LIR_CALL &&
                    in2->id != IRON_LIR_VALUE_INVALID &&
                    in2->type && in2->type->kind == IRON_TYPE_ARRAY &&
                    in2->type->array.elem &&
                    in2->type->array.elem->kind == IRON_TYPE_INTERFACE &&
                    in2->type->array.elem->interface.decl) {
                    const char *im = emit_mangle_name(
                        in2->type->array.elem->interface.decl->name, ctx->arena);
                    const char *im_copy = iron_arena_strdup(ctx->arena, im, strlen(im));
                    if (!im_copy) iron_oom_abort("emit_c.c:emit_func_body call_result iface_mangled (B)");
                    hmput(ctx->split_collection_ids, in2->id, im_copy);
                }
            }
        }
    }

    /* ── Phase 49: Monomorphic collapse ─────────────────────────────────────
     * If a split collection has only one concrete type (monomorphic_collections),
     * remove it from split_collection_ids so it falls through to the plain
     * typed array emission path. */
    if (ctx->monomorphic_collections && ctx->split_collection_ids) {
        IronLIR_ValueId *to_unsplit = NULL;
        for (ptrdiff_t i = 0; i < hmlen(ctx->split_collection_ids); i++) {
            IronLIR_ValueId vid = ctx->split_collection_ids[i].key;
            ptrdiff_t mi = hmgeti(ctx->monomorphic_collections, vid);
            if (mi >= 0) {
                arrput(to_unsplit, vid);
            }
        }
        for (int i = 0; i < (int)arrlen(to_unsplit); i++) {
            hmdel(ctx->split_collection_ids, to_unsplit[i]);
        }
        arrfree(to_unsplit);
    }

    /* ── Phase 41: Detect split-eligible for-loops ─────────────────────────
     * Scan for for-loop patterns over split collections. For each detected
     * pattern, mark the blocks (pre, header, body, inc) as replaced by
     * per-type unordered loops.
     *
     * Structure: pre → header → body → inc → (back to header) / exit
     *   pre: GET_FIELD .count on split collection, JUMP to header
     *   header: LOAD idx, LOAD count, LT compare, BRANCH body/exit
     *   body: LOAD idx, GET_INDEX array[idx], STORE loop_var, <user code>, JUMP inc
     *   inc: LOAD idx, ADD 1, STORE idx, JUMP header
     */
    struct { int key; int value; } *split_replaced_blocks = NULL;  /* bi -> 1 */
    typedef struct {
        int pre_bi, header_bi, body_bi, inc_bi, exit_bi;
        IronLIR_ValueId iterable_vid;
        IronLIR_ValueId get_index_vid;
        int body_start_ii;  /* first user instruction index in body */
    } SplitLoopInfo;
    SplitLoopInfo *split_loops = NULL;

    if (ctx->split_collection_ids && hmlen(ctx->split_collection_ids) > 0) {
        for (int bi = 0; bi < fn->block_count; bi++) {
            IronLIR_Block *blk = fn->blocks[bi];
            if (!blk->label || strncmp(blk->label, "for_pre", 7) != 0) continue;
            /* Found a for_pre block. Check if it references a split collection via GET_FIELD .count */
            IronLIR_ValueId split_arr_vid = IRON_LIR_VALUE_INVALID;
            for (int ii = 0; ii < blk->instr_count; ii++) {
                IronLIR_Instr *in2 = blk->instrs[ii];
                if (in2->kind == IRON_LIR_GET_FIELD && in2->field.field &&
                    strcmp(in2->field.field, "count") == 0) {
                    if (hmgeti(ctx->split_collection_ids, in2->field.object) >= 0) {
                        split_arr_vid = in2->field.object;
                        break;
                    }
                }
            }
            if (split_arr_vid == IRON_LIR_VALUE_INVALID) continue;

            /* Find header block (JUMP target of pre) */
            IronLIR_Instr *pre_term = blk->instrs[blk->instr_count - 1];
            if (pre_term->kind != IRON_LIR_JUMP) continue;
            int header_bi = -1;
            for (int bi2 = 0; bi2 < fn->block_count; bi2++) {
                if (fn->blocks[bi2]->id == pre_term->jump.target) { header_bi = bi2; break; }
            }
            if (header_bi < 0) continue;

            /* Header should end with BRANCH to body (true) and exit (false) */
            IronLIR_Block *header = fn->blocks[header_bi];
            IronLIR_Instr *hdr_term = header->instrs[header->instr_count - 1];
            if (hdr_term->kind != IRON_LIR_BRANCH) continue;
            int body_bi2 = -1, exit_bi2 = -1;
            for (int bi2 = 0; bi2 < fn->block_count; bi2++) {
                if (fn->blocks[bi2]->id == hdr_term->branch.then_block) body_bi2 = bi2;
                if (fn->blocks[bi2]->id == hdr_term->branch.else_block) exit_bi2 = bi2;
            }
            if (body_bi2 < 0 || exit_bi2 < 0) continue;

            /* Body should have a simple structure: no BRANCH (no if/else inside) */
            IronLIR_Block *body = fn->blocks[body_bi2];
            IronLIR_Instr *body_term = body->instrs[body->instr_count - 1];
            if (body_term->kind != IRON_LIR_JUMP) continue;  /* Must be simple JUMP to inc */

            /* Find inc block (JUMP target of body) */
            int inc_bi2 = -1;
            for (int bi2 = 0; bi2 < fn->block_count; bi2++) {
                if (fn->blocks[bi2]->id == body_term->jump.target) { inc_bi2 = bi2; break; }
            }
            if (inc_bi2 < 0) continue;

            /* Find GET_INDEX instruction in body and the first user instruction */
            IronLIR_ValueId get_index_vid2 = IRON_LIR_VALUE_INVALID;
            int body_start2 = 0;
            for (int ii = 0; ii < body->instr_count; ii++) {
                IronLIR_Instr *in2 = body->instrs[ii];
                if (in2->kind == IRON_LIR_GET_INDEX &&
                    in2->index.array == split_arr_vid) {
                    get_index_vid2 = in2->id;
                    /* Skip GET_INDEX and the following STORE to loop var */
                    body_start2 = ii + 1;
                    if (body_start2 < body->instr_count &&
                        body->instrs[body_start2]->kind == IRON_LIR_STORE) {
                        body_start2++;
                    }
                    break;
                }
                /* Skip LOAD instructions that load the loop index */
                if (in2->kind == IRON_LIR_LOAD) continue;
            }
            if (get_index_vid2 == IRON_LIR_VALUE_INVALID) continue;

            /* Record this loop for replacement */
            SplitLoopInfo info = {
                .pre_bi = bi, .header_bi = header_bi,
                .body_bi = body_bi2, .inc_bi = inc_bi2,
                .exit_bi = exit_bi2,
                .iterable_vid = split_arr_vid,
                .get_index_vid = get_index_vid2,
                .body_start_ii = body_start2,
            };
            arrput(split_loops, info);
            hmput(split_replaced_blocks, bi, 1);
            hmput(split_replaced_blocks, header_bi, 1);
            hmput(split_replaced_blocks, body_bi2, 1);
            hmput(split_replaced_blocks, inc_bi2, 1);
        }
    }

    /* Phase 53: Hoist split loop iterable_vid declarations when the iterable
     * is defined in a block after the for-pre block (e.g., from an inlined
     * function return).  The standard hoisting mechanism may not catch these
     * because the split loop emission bypasses normal instruction references. */
    for (int si = 0; si < (int)arrlen(split_loops); si++) {
        IronLIR_ValueId ivid = split_loops[si].iterable_vid;
        if (ivid == IRON_LIR_VALUE_INVALID) continue;
        if (ctx->phi_hoisted && hmgeti(ctx->phi_hoisted, ivid) >= 0) continue;
        /* Find which block defines this ValueId */
        int def_block = -1;
        for (int db = 0; db < fn->block_count; db++) {
            IronLIR_Block *dblk = fn->blocks[db];
            for (int dii = 0; dii < dblk->instr_count; dii++) {
                if (dblk->instrs[dii]->id == ivid) { def_block = db; break; }
            }
            if (def_block >= 0) break;
        }
        if (def_block > split_loops[si].pre_bi) {
            /* Iterable defined after the for-pre block — hoist its declaration */
            IronLIR_Instr *ivin = (ivid < (IronLIR_ValueId)arrlen(fn->value_table))
                                  ? fn->value_table[ivid] : NULL;
            if (ivin && ivin->type) {
                hmput(ctx->phi_hoisted, ivid, true);
                emit_indent(sb, 1);
                iron_strbuf_appendf(sb, "%s _v%u;\n",
                    emit_type_to_c(ivin->type, ctx), ivid);
            }
        }
    }

    for (int bi = 0; bi < fn->block_count; bi++) {
        IronLIR_Block *block = fn->blocks[bi];
        ctx->current_block_id = block->id;  /* for block-boundary enforcement in emit_expr_to_buf */

        /* Emit block label — unique per block to avoid duplicate-label errors.
         * Unreachable blocks still need their label emitted (for goto targets
         * that might resolve elsewhere), but we follow with __builtin_unreachable(). */
        iron_strbuf_appendf(sb, "%s:;\n",
                            emit_make_block_label(block->id, block->label, ctx->arena));

        /* Phase 41: Check if this block is replaced by split iteration */
        if (hmgeti(split_replaced_blocks, bi) >= 0) {
            /* Check if this is the pre block of a split loop */
            SplitLoopInfo *matched = NULL;
            for (int si = 0; si < (int)arrlen(split_loops); si++) {
                if (split_loops[si].pre_bi == bi) { matched = &split_loops[si]; break; }
            }
            if (matched) {
                /* Emit per-type unordered loops */
                const char *sp_iface = ctx->split_collection_ids[
                    hmgeti(ctx->split_collection_ids, matched->iterable_vid)].value;
                Iron_IfaceEntry *sp_entry2 = NULL;
                if (ctx->iface_reg) {
                    for (int ri = 0; ri < (int)shlen(ctx->iface_reg->map); ri++) {
                        const char *mc = emit_mangle_name(
                            ctx->iface_reg->map[ri].value.iface_name, ctx->arena);
                        if (strcmp(mc, sp_iface) == 0) {
                            sp_entry2 = &ctx->iface_reg->map[ri].value;
                            break;
                        }
                    }
                }
                if (sp_entry2) {
                    IronLIR_Block *body_blk = fn->blocks[matched->body_bi];
                    /* Emit any non-GET_INDEX, non-STORE instructions from pre block
                     * (like initial value assignments) */
                    for (int ii = 0; ii < fn->blocks[bi]->instr_count; ii++) {
                        IronLIR_Instr *in2 = fn->blocks[bi]->instrs[ii];
                        if (in2->kind == IRON_LIR_JUMP) break;
                        if (in2->kind == IRON_LIR_GET_FIELD &&
                            in2->field.object == matched->iterable_vid) continue;
                        emit_instr(sb, in2, fn, ctx);
                    }

                    for (int ji = 0; ji < sp_entry2->impl_count; ji++) {
                        Iron_IfaceImpl *impl2 = &sp_entry2->impls[ji];
                        if (!impl2->is_alive) continue;
                        char lower_name[256];
                        {
                            size_t nl2 = strlen(impl2->type_name);
                            if (nl2 >= sizeof(lower_name)) nl2 = sizeof(lower_name) - 1;
                            for (size_t ci3 = 0; ci3 < nl2; ci3++)
                                lower_name[ci3] = (char)((impl2->type_name[ci3] >= 'A' &&
                                                           impl2->type_name[ci3] <= 'Z')
                                    ? impl2->type_name[ci3] + 32
                                    : impl2->type_name[ci3]);
                            lower_name[nl2] = '\0';
                        }
                        /* Phase 48: Check if this type uses reduced storage */
                        bool type_is_reduced = (shgeti(ctx->reduced_storage_types,
                                                       impl2->type_name) >= 0);
                        /* Phase 48-02: Check if this type uses SoA layout */
                        char loop_soa_key[768];
                        snprintf(loop_soa_key, sizeof(loop_soa_key), "%s:%s",
                            sp_iface, impl2->type_name);
                        bool type_is_soa = (shgeti(ctx->soa_types, loop_soa_key) >= 0);

                        /* Emit per-type for-loop */
                        emit_indent(sb, ctx->indent);
                        iron_strbuf_appendf(sb, "{ /* unordered split: %s */\n", impl2->type_name);
                        emit_indent(sb, ctx->indent + 1);
                        iron_strbuf_appendf(sb, "for (int64_t _sp_i = 0; _sp_i < ");
                        emit_val(sb, matched->iterable_vid);
                        iron_strbuf_appendf(sb, ".%s_count; _sp_i++) {\n", lower_name);
                        if (!type_is_soa) {
                        /* Prefetch hint — warm next cache line (AoS only) */
                        emit_indent(sb, ctx->indent + 2);
                        iron_strbuf_appendf(sb, "IRON_PREFETCH(&");
                        emit_val(sb, matched->iterable_vid);
                        iron_strbuf_appendf(sb, ".%s_items[_sp_i + 8]);\n", lower_name);
                        }

                        if (type_is_soa && impl2->decl) {
                            /* Phase 48-02: Reconstruct from SoA per-type field arrays.
                             * Each used field has its own array: <lower>_<field>[_sp_i] */
                            const char *im2 = emit_mangle_name(impl2->type_name, ctx->arena);
                            emit_indent(sb, ctx->indent + 2);
                            iron_strbuf_appendf(sb, "%s _tmp_%s = {0};\n", im2, lower_name);
                            Iron_ObjectDecl *od = impl2->decl;
                            IronLIR_ValueId *cvids = NULL;
                            if (ctx->split_collection_ids) {
                                for (ptrdiff_t si = 0; si < hmlen(ctx->split_collection_ids); si++) {
                                    if (strcmp(ctx->split_collection_ids[si].value, sp_iface) == 0)
                                        arrput(cvids, ctx->split_collection_ids[si].key);
                                }
                            }
                            for (int fi = 0; fi < od->field_count; fi++) {
                                Iron_Field *f = (Iron_Field *)od->fields[fi];
                                bool any_used = true;
                                if (arrlen(cvids) > 0) {
                                    any_used = false;
                                    for (int ci2 = 0; ci2 < (int)arrlen(cvids); ci2++) {
                                        if (iron_layout_is_field_used(&ctx->layout,
                                                cvids[ci2], f->name)) {
                                            any_used = true;
                                            break;
                                        }
                                    }
                                }
                                if (!any_used) continue;
                                /* Phase 50: Widening cast for SoA field read */
                                const char *narrowed_sw = iron_vr_get_narrowed_type(
                                    &ctx->value_range, impl2->type_name, f->name);
                                emit_indent(sb, ctx->indent + 2);
                                if (narrowed_sw) {
                                    const char *orig_type = "int64_t";
                                    if (f->type_ann) {
                                        Iron_TypeAnnotation *ta = (Iron_TypeAnnotation *)f->type_ann;
                                        if (!ta->is_func && !ta->is_nullable && !ta->is_array)
                                            orig_type = emit_annotation_to_c(ta->name, ctx);
                                    }
                                    iron_strbuf_appendf(sb, "_tmp_%s.%s = (%s)",
                                        lower_name, f->name, orig_type);
                                } else {
                                    iron_strbuf_appendf(sb, "_tmp_%s.%s = ",
                                        lower_name, f->name);
                                }
                                emit_val(sb, matched->iterable_vid);
                                iron_strbuf_appendf(sb, ".%s_%s[_sp_i];\n",
                                    lower_name, f->name);
                            }
                            arrfree(cvids);
                            emit_indent(sb, ctx->indent + 2);
                            iron_strbuf_appendf(sb, "%s ", sp_iface);
                            emit_val(sb, matched->get_index_vid);
                            iron_strbuf_appendf(sb, " = %s_from_%s(_tmp_%s);\n",
                                sp_iface, impl2->type_name, lower_name);
                        } else if (type_is_reduced && impl2->decl) {
                            /* Phase 48: Reconstruct from reduced storage */
                            const char *im2 = emit_mangle_name(impl2->type_name, ctx->arena);
                            emit_indent(sb, ctx->indent + 2);
                            iron_strbuf_appendf(sb, "%s _tmp_%s = {0};\n", im2, lower_name);
                            Iron_ObjectDecl *od = impl2->decl;
                            IronLIR_ValueId *cvids = NULL;
                            if (ctx->split_collection_ids) {
                                for (ptrdiff_t si = 0; si < hmlen(ctx->split_collection_ids); si++) {
                                    if (strcmp(ctx->split_collection_ids[si].value, sp_iface) == 0)
                                        arrput(cvids, ctx->split_collection_ids[si].key);
                                }
                            }
                            for (int fi = 0; fi < od->field_count; fi++) {
                                Iron_Field *f = (Iron_Field *)od->fields[fi];
                                bool any_used = false;
                                for (int ci2 = 0; ci2 < (int)arrlen(cvids); ci2++) {
                                    if (iron_layout_is_field_used(&ctx->layout,
                                            cvids[ci2], f->name)) {
                                        any_used = true;
                                        break;
                                    }
                                }
                                if (!any_used) continue;
                                /* Phase 50: Widening cast for AoS reduced field read */
                                const char *narrowed_aw = iron_vr_get_narrowed_type(
                                    &ctx->value_range, impl2->type_name, f->name);
                                emit_indent(sb, ctx->indent + 2);
                                if (narrowed_aw) {
                                    const char *orig_type = "int64_t";
                                    if (f->type_ann) {
                                        Iron_TypeAnnotation *ta = (Iron_TypeAnnotation *)f->type_ann;
                                        if (!ta->is_func && !ta->is_nullable && !ta->is_array)
                                            orig_type = emit_annotation_to_c(ta->name, ctx);
                                    }
                                    iron_strbuf_appendf(sb, "_tmp_%s.%s = (%s)",
                                        lower_name, f->name, orig_type);
                                } else {
                                    iron_strbuf_appendf(sb, "_tmp_%s.%s = ",
                                        lower_name, f->name);
                                }
                                emit_val(sb, matched->iterable_vid);
                                iron_strbuf_appendf(sb, ".%s_items[_sp_i].%s;\n",
                                    lower_name, f->name);
                            }
                            arrfree(cvids);
                            emit_indent(sb, ctx->indent + 2);
                            iron_strbuf_appendf(sb, "%s ", sp_iface);
                            emit_val(sb, matched->get_index_vid);
                            iron_strbuf_appendf(sb, " = %s_from_%s(_tmp_%s);\n",
                                sp_iface, impl2->type_name, lower_name);
                        } else {
                            /* Emit element wrapping assignment (original behavior) */
                            emit_indent(sb, ctx->indent + 2);
                            iron_strbuf_appendf(sb, "%s ", sp_iface);
                            emit_val(sb, matched->get_index_vid);
                            iron_strbuf_appendf(sb, " = %s_from_%s(",
                                sp_iface, impl2->type_name);
                            emit_val(sb, matched->iterable_vid);
                            iron_strbuf_appendf(sb, ".%s_items[_sp_i]);\n", lower_name);
                        }

                        /* Emit body instructions (user code) */
                        int saved_indent = ctx->indent;
                        ctx->indent = saved_indent + 2;
                        for (int ii = matched->body_start_ii; ii < body_blk->instr_count; ii++) {
                            IronLIR_Instr *in2 = body_blk->instrs[ii];
                            if (in2->kind == IRON_LIR_JUMP) break;  /* Skip jump to inc */
                            emit_instr(sb, in2, fn, ctx);
                        }
                        ctx->indent = saved_indent;
                        ctx->in_split_loop = false; /* restore after body */
                        emit_indent(sb, ctx->indent + 1);
                        iron_strbuf_appendf(sb, "}\n");
                        emit_indent(sb, ctx->indent);
                        iron_strbuf_appendf(sb, "}\n");
                    }
                    /* Jump to exit block */
                    iron_strbuf_appendf(sb, "    goto %s;\n",
                        emit_make_block_label(fn->blocks[matched->exit_bi]->id,
                                          fn->blocks[matched->exit_bi]->label,
                                          ctx->arena));
                }
            }
            /* For non-pre blocks in split loops (header, body, inc), emit as
             * unreachable since the per-type loops handle everything */
            else {
                iron_strbuf_appendf(sb, "    __builtin_unreachable();\n");
            }
            continue;
        }

        bool is_reachable = !blk_reachable || blk_reachable[bi];
        if (!is_reachable || block->instr_count == 0) {
            /* Dead / unreachable block — suppress all instructions and use
             * __builtin_unreachable() so C compilers don't warn about
             * missing returns and optimizers can prune the dead path. */
            iron_strbuf_appendf(sb, "    __builtin_unreachable();\n");
        } else {
            for (int ii = 0; ii < block->instr_count; ii++) {
                emit_instr(sb, block->instrs[ii], fn, ctx);
            }
        }
    }
    arrfree(split_loops);
    hmfree(split_replaced_blocks);

    if (blk_reachable) free(blk_reachable);

    ctx->indent = 0;
    iron_strbuf_appendf(sb, "}\n\n");

    /* Cleanup per-function inlining maps */
    ctx->inline_eligible = NULL;
    ctx->value_block = NULL;

    /* Cleanup per-function capture alias map */
    hmfree(ctx->capture_alias_map);
    ctx->capture_alias_map     = NULL;
    ctx->current_captures      = NULL;
    ctx->current_capture_count = 0;

    /* Note: opt_info maps are owned by opt_info and freed by iron_lir_optimize_info_free() */

    /* Cleanup per-function ADT boxed-alloca map (Phase 38) */
    hmfree(ctx->adt_boxed_allocas);
    ctx->adt_boxed_allocas = NULL;
}

/* ── Main entry point ─────────────────────────────────────────────────────── */

const char *iron_lir_emit_c(IronLIR_Module *module, Iron_Arena *arena,
                            Iron_DiagList *diags,
                            IronLIR_OptimizeInfo *opt_info,
                            Iron_IfaceRegistry *iface_reg,
                            bool warn_fusion_break,
                            bool report_compression) {
    if (!module) return NULL;
    if (diags && diags->error_count > 0) return NULL;

    /* Initialize context */
    EmitCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.arena         = arena;
    ctx.diags         = diags;
    ctx.module        = module;
    ctx.next_type_tag = 1;
    ctx.opt_info      = opt_info;
    ctx.iface_reg     = iface_reg;
    ctx.warn_fusion_break = warn_fusion_break;
    ctx.report_compression = report_compression;

    ctx.includes        = iron_strbuf_create(512);
    ctx.forward_decls   = iron_strbuf_create(256);
    ctx.struct_bodies   = iron_strbuf_create(1024);
    ctx.enum_defs       = iron_strbuf_create(256);
    ctx.global_consts   = iron_strbuf_create(64);
    ctx.prototypes      = iron_strbuf_create(512);
    ctx.lifted_funcs    = iron_strbuf_create(1024);
    ctx.implementations = iron_strbuf_create(4096);
    ctx.main_wrapper    = iron_strbuf_create(256);

    ctx.emitted_optionals = NULL;
    ctx.mono_registry     = NULL;
    ctx.indent            = 0;

    /* ── Phase 1: Includes ───────────────────────────────────────────────── */
    iron_strbuf_appendf(&ctx.includes,
                         "#include \"runtime/iron_runtime.h\"\n");
    iron_strbuf_appendf(&ctx.includes, "#include <stdint.h>\n");
    iron_strbuf_appendf(&ctx.includes, "#include <stdbool.h>\n");
    iron_strbuf_appendf(&ctx.includes, "#include <stdlib.h>\n");
    iron_strbuf_appendf(&ctx.includes, "#include <string.h>\n");
    iron_strbuf_appendf(&ctx.includes, "#include <stdio.h>\n");
    iron_strbuf_appendf(&ctx.includes, "#include \"stdlib/iron_math.h\"\n");
    iron_strbuf_appendf(&ctx.includes, "#include \"stdlib/iron_io.h\"\n");
    iron_strbuf_appendf(&ctx.includes, "#define IRON_TIMER_STRUCT_DEFINED\n");
    iron_strbuf_appendf(&ctx.includes, "#include \"stdlib/iron_time.h\"\n");
    iron_strbuf_appendf(&ctx.includes, "#include \"stdlib/iron_log.h\"\n");
    iron_strbuf_appendf(&ctx.includes, "#include \"stdlib/iron_hint.h\"\n");
    /* Phase 59 02: the TCP stdlib surface (Net.*, TcpSocket.*, TcpListener.*)
     * does NOT need an iron_net.h include here — the extern-stub prototype
     * path in Phase 3 below emits forward declarations for every empty-body
     * method stub (including Iron_net_tcp_dial etc.) into ctx.prototypes
     * which lands AFTER the struct_bodies section. That ordering lets the
     * prototypes reference compiler-emitted tuple types (Iron_Tuple_*) and
     * object structs (Iron_NetError/Iron_TcpSocket/Iron_TcpListener) without
     * double-defining them. iron_net.h stays a normal C header for unit
     * tests and iron_net.c. */

    /* Phase 44: Portable prefetch macro for split collection hot loops */
    iron_strbuf_appendf(&ctx.includes,
        "#ifdef __GNUC__\n"
        "  #define IRON_PREFETCH(addr) __builtin_prefetch(addr, 0, 3)\n"
        "#elif defined(_MSC_VER)\n"
        "  #include <xmmintrin.h>\n"
        "  #define IRON_PREFETCH(addr) _mm_prefetch((const char*)(addr), _MM_HINT_T0)\n"
        "#else\n"
        "  #define IRON_PREFETCH(addr) ((void)0)\n"
        "#endif\n\n");

    /* ── Phase 48: Module-level prescan for split collections & layout analysis */
    emit_prescan_split_collections(&ctx);

    /* ── Phase 53: Interprocedural return type collection ────────────────────
     * Pre-pass ("Phase 0"): For each function, collect the concrete types of
     * split collections returned via RETURN instructions.  This enables the
     * monomorphic detection scan below to recognise CALL results as single-type
     * collections at call sites.
     *
     * func_return_types: maps function name -> stb_ds array of concrete type
     * names found in the function's RETURN instructions. */
    struct { char *key; const char **value; } *func_return_types = NULL;

    if (ctx.split_collection_ids && hmlen(ctx.split_collection_ids) > 0 && ctx.iface_reg) {
        for (int fi = 0; fi < module->func_count; fi++) {
            IronLIR_Func *fn = module->funcs[fi];
            if (!fn || fn->is_extern || fn->block_count == 0) continue;

            /* Build local propagation map: vid -> origin ARRAY_LIT vid */
            struct { IronLIR_ValueId key; IronLIR_ValueId value; } *vid_origin = NULL;

            /* Find ARRAY_LIT instructions that are split collections */
            for (int bi = 0; bi < fn->block_count; bi++) {
                IronLIR_Block *blk = fn->blocks[bi];
                for (int ii = 0; ii < blk->instr_count; ii++) {
                    IronLIR_Instr *in2 = blk->instrs[ii];
                    if (in2->kind == IRON_LIR_ARRAY_LIT &&
                        hmgeti(ctx.split_collection_ids, in2->id) >= 0) {
                        hmput(vid_origin, in2->id, in2->id);
                    }
                }
            }
            /* Propagate through STORE/LOAD chains */
            for (int bi = 0; bi < fn->block_count; bi++) {
                IronLIR_Block *blk = fn->blocks[bi];
                for (int ii = 0; ii < blk->instr_count; ii++) {
                    IronLIR_Instr *in2 = blk->instrs[ii];
                    if (in2->kind == IRON_LIR_STORE) {
                        ptrdiff_t vi = hmgeti(vid_origin, in2->store.value);
                        if (vi >= 0) hmput(vid_origin, in2->store.ptr, vid_origin[vi].value);
                    } else if (in2->kind == IRON_LIR_LOAD) {
                        ptrdiff_t vi = hmgeti(vid_origin, in2->load.ptr);
                        if (vi >= 0) hmput(vid_origin, in2->id, vid_origin[vi].value);
                    }
                }
            }

            /* Check RETURN instructions for tracked split collections */
            for (int bi = 0; bi < fn->block_count; bi++) {
                IronLIR_Block *blk = fn->blocks[bi];
                for (int ii = 0; ii < blk->instr_count; ii++) {
                    IronLIR_Instr *in2 = blk->instrs[ii];
                    if (in2->kind == IRON_LIR_RETURN && !in2->ret.is_void) {
                        ptrdiff_t vi = hmgeti(vid_origin, in2->ret.value);
                        if (vi < 0) continue;
                        IronLIR_ValueId origin_vid = vid_origin[vi].value;
                        /* Look up the ARRAY_LIT instruction to extract concrete types */
                        IronLIR_Instr *arr_lit = NULL;
                        if (origin_vid < (IronLIR_ValueId)arrlen(fn->value_table))
                            arr_lit = fn->value_table[origin_vid];
                        if (!arr_lit || arr_lit->kind != IRON_LIR_ARRAY_LIT) continue;

                        /* Collect concrete types from the ARRAY_LIT elements */
                        const char **ret_types = NULL;
                        ptrdiff_t rt_idx = shgeti(func_return_types, fn->name);
                        if (rt_idx >= 0) ret_types = func_return_types[rt_idx].value;

                        for (int ei = 0; ei < arr_lit->array_lit.element_count; ei++) {
                            IronLIR_ValueId eid = arr_lit->array_lit.elements[ei];
                            Iron_Type *et = emit_get_value_type(fn, eid);
                            if (et && et->kind == IRON_TYPE_OBJECT && et->object.decl) {
                                const char *tname = et->object.decl->name;
                                bool found = false;
                                for (int ti = 0; ti < (int)arrlen(ret_types); ti++) {
                                    if (strcmp(ret_types[ti], tname) == 0) { found = true; break; }
                                }
                                if (!found) arrput(ret_types, tname);
                            }
                        }
                        shput(func_return_types, fn->name, ret_types);
                    }
                }
            }
            hmfree(vid_origin);
        }
    }

    /* ── Phase 49: Monomorphic collection detection ─────────────────────────
     * Whole-program scan: for each interface-typed ARRAY_LIT in split_collection_ids,
     * determine the set of concrete types pushed to it.  If exactly one concrete type
     * is found, mark the collection as monomorphic.
     *
     * Phase 53 extension: CALL results from functions with known return types
     * are also tracked as collections with the callee's concrete type set.
     * Collections passed as CALL args still escape (conservative for args).
     * Collections returned from a function are NOT escaped when tracked. */
    if (ctx.split_collection_ids && hmlen(ctx.split_collection_ids) > 0 && ctx.iface_reg) {
        /* Tracking map: collection ValueId -> set of concrete type names.
         * We use a simple approach: track (vid, type_name) pairs. */
        struct { IronLIR_ValueId key; const char **value; } *coll_types = NULL;

        for (int fi = 0; fi < module->func_count; fi++) {
            IronLIR_Func *fn = module->funcs[fi];
            if (!fn || fn->is_extern || fn->block_count == 0) continue;

            /* Build local propagation map for this function:
             * maps ValueIds to the original ARRAY_LIT ValueId through STORE/LOAD */
            struct { IronLIR_ValueId key; IronLIR_ValueId value; } *vid_origin = NULL;

            /* Phase A: Find ARRAY_LIT instructions that are split collections */
            for (int bi = 0; bi < fn->block_count; bi++) {
                IronLIR_Block *blk = fn->blocks[bi];
                for (int ii = 0; ii < blk->instr_count; ii++) {
                    IronLIR_Instr *in2 = blk->instrs[ii];
                    if (in2->kind == IRON_LIR_ARRAY_LIT &&
                        hmgeti(ctx.split_collection_ids, in2->id) >= 0) {
                        hmput(vid_origin, in2->id, in2->id);

                        /* Record concrete types from ARRAY_LIT elements */
                        for (int ei = 0; ei < in2->array_lit.element_count; ei++) {
                            IronLIR_ValueId eid = in2->array_lit.elements[ei];
                            Iron_Type *et = emit_get_value_type(fn, eid);
                            if (et && et->kind == IRON_TYPE_OBJECT && et->object.decl) {
                                /* Initialize type set for this collection if needed */
                                ptrdiff_t ct_idx = hmgeti(coll_types, in2->id);
                                if (ct_idx < 0) {
                                    const char **types = NULL;
                                    hmput(coll_types, in2->id, types);
                                    ct_idx = hmgeti(coll_types, in2->id);
                                }
                                /* Add type if not already present */
                                const char *tname = et->object.decl->name;
                                bool found = false;
                                for (int ti = 0; ti < (int)arrlen(coll_types[ct_idx].value); ti++) {
                                    if (strcmp(coll_types[ct_idx].value[ti], tname) == 0) {
                                        found = true;
                                        break;
                                    }
                                }
                                if (!found) {
                                    arrput(coll_types[ct_idx].value, tname);
                                }
                            }
                        }
                    }
                }
            }

            /* Phase A.1: Add CALL results from split_collection_ids using func_return_types */
            for (int bi = 0; bi < fn->block_count; bi++) {
                IronLIR_Block *blk = fn->blocks[bi];
                for (int ii = 0; ii < blk->instr_count; ii++) {
                    IronLIR_Instr *in2 = blk->instrs[ii];
                    if (in2->kind != IRON_LIR_CALL) continue;
                    if (hmgeti(ctx.split_collection_ids, in2->id) < 0) continue;
                    /* This CALL result is a split collection. Resolve callee name. */
                    const char *callee_name = NULL;
                    if (in2->call.func_decl)
                        callee_name = in2->call.func_decl->name;
                    else if (in2->call.func_ptr != IRON_LIR_VALUE_INVALID &&
                             in2->call.func_ptr < (IronLIR_ValueId)arrlen(fn->value_table) &&
                             fn->value_table[in2->call.func_ptr] &&
                             fn->value_table[in2->call.func_ptr]->kind == IRON_LIR_FUNC_REF)
                        callee_name = fn->value_table[in2->call.func_ptr]->func_ref.func_name;
                    if (!callee_name) continue;
                    ptrdiff_t rt_idx = shgeti(func_return_types, callee_name);
                    if (rt_idx < 0) continue;
                    /* Copy callee's return type set into coll_types for this CALL vid */
                    hmput(vid_origin, in2->id, in2->id);
                    const char **ret_types = func_return_types[rt_idx].value;
                    ptrdiff_t ct_idx = hmgeti(coll_types, in2->id);
                    if (ct_idx < 0) {
                        const char **types = NULL;
                        hmput(coll_types, in2->id, types);
                        ct_idx = hmgeti(coll_types, in2->id);
                    }
                    for (int rti = 0; rti < (int)arrlen(ret_types); rti++) {
                        bool found = false;
                        for (int ti = 0; ti < (int)arrlen(coll_types[ct_idx].value); ti++) {
                            if (strcmp(coll_types[ct_idx].value[ti], ret_types[rti]) == 0) {
                                found = true; break;
                            }
                        }
                        if (!found) arrput(coll_types[ct_idx].value, ret_types[rti]);
                    }
                }
            }

            /* Phase B: Propagate through STORE/LOAD chains */
            for (int bi = 0; bi < fn->block_count; bi++) {
                IronLIR_Block *blk = fn->blocks[bi];
                for (int ii = 0; ii < blk->instr_count; ii++) {
                    IronLIR_Instr *in2 = blk->instrs[ii];
                    if (in2->kind == IRON_LIR_STORE) {
                        ptrdiff_t vi = hmgeti(vid_origin, in2->store.value);
                        if (vi >= 0) hmput(vid_origin, in2->store.ptr, vid_origin[vi].value);
                    } else if (in2->kind == IRON_LIR_LOAD) {
                        ptrdiff_t vi = hmgeti(vid_origin, in2->load.ptr);
                        if (vi >= 0) hmput(vid_origin, in2->id, vid_origin[vi].value);
                    }
                }
            }

            /* Phase C: Check for escaping collections (passed as args to calls).
             * If a collection escapes, remove it from consideration for
             * monomorphic detection (the collection itself still emits as a
             * SplitList; escaping only prevents monomorphic collapse). */
            struct { IronLIR_ValueId key; bool value; } *escaped = NULL;
            for (int bi = 0; bi < fn->block_count; bi++) {
                IronLIR_Block *blk = fn->blocks[bi];
                for (int ii = 0; ii < blk->instr_count; ii++) {
                    IronLIR_Instr *in2 = blk->instrs[ii];
                    if (in2->kind == IRON_LIR_CALL) {
                        for (int ai = 0; ai < in2->call.arg_count; ai++) {
                            ptrdiff_t vi = hmgeti(vid_origin, in2->call.args[ai]);
                            if (vi >= 0) {
                                hmput(escaped, vid_origin[vi].value, true);
                            }
                        }
                    }
                    /* Also check RETURN, SET_FIELD, MAKE_CLOSURE */
                    if (in2->kind == IRON_LIR_RETURN && !in2->ret.is_void) {
                        ptrdiff_t vi = hmgeti(vid_origin, in2->ret.value);
                        if (vi >= 0) hmput(escaped, vid_origin[vi].value, true);
                    }
                    if (in2->kind == IRON_LIR_SET_FIELD) {
                        ptrdiff_t vi = hmgeti(vid_origin, in2->field.value);
                        if (vi >= 0) hmput(escaped, vid_origin[vi].value, true);
                    }
                    if (in2->kind == IRON_LIR_MAKE_CLOSURE) {
                        for (int ci2 = 0; ci2 < in2->make_closure.capture_count; ci2++) {
                            ptrdiff_t vi = hmgeti(vid_origin, in2->make_closure.captures[ci2]);
                            if (vi >= 0) hmput(escaped, vid_origin[vi].value, true);
                        }
                    }
                }
            }

            /* Remove escaped collections from coll_types */
            if (escaped) {
                for (ptrdiff_t i = 0; i < hmlen(escaped); i++) {
                    ptrdiff_t ct_idx = hmgeti(coll_types, escaped[i].key);
                    if (ct_idx >= 0) {
                        arrfree(coll_types[ct_idx].value);
                        hmdel(coll_types, escaped[i].key);
                    }
                }
                hmfree(escaped);
            }

            hmfree(vid_origin);
        }

        /* Phase D: Mark monomorphic collections (exactly 1 concrete type, non-empty) */
        for (ptrdiff_t i = 0; i < hmlen(coll_types); i++) {
            if (arrlen(coll_types[i].value) == 1) {
                hmput(ctx.monomorphic_collections, coll_types[i].key,
                      coll_types[i].value[0]);
            }
            arrfree(coll_types[i].value);
        }
        hmfree(coll_types);
    }

    /* ── Phase E (Phase 53): Parameter type collection from call sites ──────
     * For each function parameter that receives split collections from all
     * callers, collect the union of concrete types.  If exactly one concrete
     * type and the specialization heuristic approves, mark the parameter's
     * collection as monomorphic inside the callee. */
    if (ctx.split_collection_ids && hmlen(ctx.split_collection_ids) > 0 && ctx.iface_reg) {
        /* param_types: "func_name:param_idx" -> stb_ds array of type names */
        struct { char *key; const char **value; } *param_types = NULL;
        /* call_site_counts: "func_name" -> number of CALL sites found */
        struct { char *key; int value; } *call_site_counts = NULL;

        /* Scan ALL call instructions across all functions */
        for (int fi = 0; fi < module->func_count; fi++) {
            IronLIR_Func *fn = module->funcs[fi];
            if (!fn || fn->is_extern || fn->block_count == 0) continue;

            for (int bi = 0; bi < fn->block_count; bi++) {
                IronLIR_Block *blk = fn->blocks[bi];
                for (int ii = 0; ii < blk->instr_count; ii++) {
                    IronLIR_Instr *in2 = blk->instrs[ii];
                    if (in2->kind != IRON_LIR_CALL) continue;

                    /* Resolve callee name */
                    const char *callee_name = NULL;
                    if (in2->call.func_decl) {
                        callee_name = in2->call.func_decl->name;
                    } else if (in2->call.func_ptr != IRON_LIR_VALUE_INVALID &&
                               in2->call.func_ptr < (IronLIR_ValueId)arrlen(fn->value_table) &&
                               fn->value_table[in2->call.func_ptr] &&
                               fn->value_table[in2->call.func_ptr]->kind == IRON_LIR_FUNC_REF) {
                        callee_name = fn->value_table[in2->call.func_ptr]->func_ref.func_name;
                    }
                    if (!callee_name) continue;

                    /* Count call sites per function */
                    {
                        ptrdiff_t cs = shgeti(call_site_counts, callee_name);
                        int cnt = (cs >= 0) ? call_site_counts[cs].value + 1 : 1;
                        shput(call_site_counts, callee_name, cnt);
                    }

                    /* Check each argument for monomorphic collection info */
                    for (int ai = 0; ai < in2->call.arg_count; ai++) {
                        IronLIR_ValueId arg_vid = in2->call.args[ai];
                        /* Check if this argument has known monomorphic type */
                        ptrdiff_t mc_idx = hmgeti(ctx.monomorphic_collections, arg_vid);
                        if (mc_idx >= 0) {
                            /* Argument is a monomorphic collection with a known type */
                            const char *concrete_type = ctx.monomorphic_collections[mc_idx].value;
                            char param_key[512];
                            snprintf(param_key, sizeof(param_key), "%s:%d", callee_name, ai);
                            ptrdiff_t pt_idx = shgeti(param_types, param_key);
                            const char **types = (pt_idx >= 0) ? param_types[pt_idx].value : NULL;
                            bool found = false;
                            for (int ti = 0; ti < (int)arrlen(types); ti++) {
                                if (strcmp(types[ti], concrete_type) == 0) { found = true; break; }
                            }
                            if (!found) arrput(types, concrete_type);
                            shput(param_types, param_key, types);
                        }
                        /* Also check func_return_types for CALL results used as args */
                        if (mc_idx < 0) {
                            /* Check if the arg is a CALL result with known return types */
                            IronLIR_Instr *arg_instr = NULL;
                            if (arg_vid < (IronLIR_ValueId)arrlen(fn->value_table))
                                arg_instr = fn->value_table[arg_vid];
                            if (arg_instr && arg_instr->kind == IRON_LIR_CALL) {
                                const char *inner_callee = NULL;
                                if (arg_instr->call.func_decl)
                                    inner_callee = arg_instr->call.func_decl->name;
                                if (inner_callee) {
                                    ptrdiff_t rt_idx = shgeti(func_return_types, inner_callee);
                                    if (rt_idx >= 0) {
                                        char param_key[512];
                                        snprintf(param_key, sizeof(param_key), "%s:%d", callee_name, ai);
                                        ptrdiff_t pt_idx = shgeti(param_types, param_key);
                                        const char **types = (pt_idx >= 0) ? param_types[pt_idx].value : NULL;
                                        const char **ret_types = func_return_types[rt_idx].value;
                                        for (int rti = 0; rti < (int)arrlen(ret_types); rti++) {
                                            bool found = false;
                                            for (int ti = 0; ti < (int)arrlen(types); ti++) {
                                                if (strcmp(types[ti], ret_types[rti]) == 0) { found = true; break; }
                                            }
                                            if (!found) arrput(types, ret_types[rti]);
                                        }
                                        shput(param_types, param_key, types);
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }

        /* Apply specialization heuristic for single-type parameters */
        for (ptrdiff_t pi = 0; pi < shlen(param_types); pi++) {
            if (arrlen(param_types[pi].value) != 1) {
                arrfree(param_types[pi].value);
                continue;
            }
            /* Single concrete type for this parameter position */
            const char *concrete_type = param_types[pi].value[0];
            const char *param_key = param_types[pi].key;

            /* Parse "func_name:param_idx" */
            char func_name_buf[512];
            int param_idx = 0;
            {
                const char *colon = strrchr(param_key, ':');
                if (!colon) { arrfree(param_types[pi].value); continue; }
                size_t fnlen = (size_t)(colon - param_key);
                if (fnlen >= sizeof(func_name_buf)) fnlen = sizeof(func_name_buf) - 1;
                memcpy(func_name_buf, param_key, fnlen);
                func_name_buf[fnlen] = '\0';
                param_idx = atoi(colon + 1);
            }

            /* Find the callee function in the module */
            IronLIR_Func *callee_fn = emit_find_ir_func(&ctx, func_name_buf);
            if (!callee_fn || callee_fn->is_extern || callee_fn->block_count == 0) {
                arrfree(param_types[pi].value);
                continue;
            }

            /* Specialization heuristic: specialize ONLY when ALL conditions met:
             *   1. Function has <= 50 LIR instructions
             *   2. There are 1-2 call sites for this function
             *   3. Function body has at least 1 dispatch-related BRANCH/SWITCH */
            int total_instrs = 0;
            int dispatch_branches = 0;
            for (int bi = 0; bi < callee_fn->block_count; bi++) {
                total_instrs += callee_fn->blocks[bi]->instr_count;
                for (int ii = 0; ii < callee_fn->blocks[bi]->instr_count; ii++) {
                    IronLIR_Instr *in2 = callee_fn->blocks[bi]->instrs[ii];
                    if (in2->kind == IRON_LIR_SWITCH || in2->kind == IRON_LIR_BRANCH)
                        dispatch_branches++;
                }
            }

            ptrdiff_t cs_idx = shgeti(call_site_counts, func_name_buf);
            int num_call_sites = (cs_idx >= 0) ? call_site_counts[cs_idx].value : 0;

            bool eligible = (total_instrs <= 50) &&
                            (num_call_sites >= 1 && num_call_sites <= 2) &&
                            (dispatch_branches >= 1);

            if (!eligible) {
                /* Conservative: don't specialize large or multi-caller functions */
                if (ctx.warn_fusion_break) {
                    char diag_msg[512];
                    snprintf(diag_msg, sizeof(diag_msg),
                        "specialization heuristic rejected %s for %s "
                        "(instrs=%d, callers=%d, dispatch=%d)",
                        func_name_buf, concrete_type,
                        total_instrs, num_call_sites, dispatch_branches);
                    iron_diag_emit(ctx.diags, ctx.arena, IRON_DIAG_NOTE, 0,
                        (Iron_Span){NULL,0,0,0,0}, diag_msg, NULL);
                }
                arrfree(param_types[pi].value);
                continue;
            }

            /* Check specialization_registry to prevent duplicate bodies */
            if (ctx.specialization_registry) {
                char spec_key[512];
                snprintf(spec_key, sizeof(spec_key), "%s:%s", func_name_buf, concrete_type);
                if (shgeti(ctx.specialization_registry, spec_key) >= 0) {
                    arrfree(param_types[pi].value);
                    continue;  /* Already specialized for this type */
                }
            }

            /* Mark the callee's parameter collection as monomorphic.
             * The parameter ValueId in the callee is (param_idx + 1) per convention
             * (params get IDs 1..N, allocas get N+1..2N). */
            IronLIR_ValueId param_vid = (IronLIR_ValueId)(param_idx + 1);
            /* Also check the alloca for this parameter: param_idx alloca is at
             * id = param_count + param_idx + 1. */
            IronLIR_ValueId alloca_vid = (IronLIR_ValueId)(callee_fn->param_count + param_idx + 1);

            /* Propagate: mark both the param and its alloca chain */
            hmput(ctx.monomorphic_collections, param_vid, concrete_type);
            hmput(ctx.monomorphic_collections, alloca_vid, concrete_type);

            /* Also try to find LOAD from alloca in the callee's entry block */
            if (callee_fn->block_count > 0) {
                IronLIR_Block *entry_blk = callee_fn->blocks[0];
                for (int ii = 0; ii < entry_blk->instr_count; ii++) {
                    IronLIR_Instr *in2 = entry_blk->instrs[ii];
                    if (in2->kind == IRON_LIR_LOAD && in2->load.ptr == alloca_vid) {
                        hmput(ctx.monomorphic_collections, in2->id, concrete_type);
                    }
                }
            }

            arrfree(param_types[pi].value);
        }

        /* Clean up */
        for (ptrdiff_t pi = 0; pi < shlen(param_types); pi++) {
            arrfree(param_types[pi].value);
        }
        shfree(param_types);
        shfree(call_site_counts);
    }

    /* Clean up func_return_types */
    for (ptrdiff_t i = 0; i < shlen(func_return_types); i++) {
        arrfree(func_return_types[i].value);
    }
    shfree(func_return_types);

    /* Phase 49: Specialization registry initialization */
    ctx.specialization_registry = NULL;

    /* ── Phase 2: Type declarations (forward decls, structs, enums) ───────── */
    emit_type_decls(&ctx);

    /* ── Phase 3: Function prototypes ────────────────────────────────────── */
    for (int i = 0; i < module->func_count; i++) {
        IronLIR_Func *fn = module->funcs[i];
        if (fn->is_extern) continue;
        emit_func_signature(&ctx.prototypes, fn, &ctx, true);
    }

    /* Auto-generate C prototypes for extern functions whose parameter types
     * are resolved. Shared with emit_web_module so the web target also
     * declares raylib and other C library bindings. */
    emit_extern_prototypes(&ctx);

    /* Phase 59 02: emit extern prototypes for the Net/TcpSocket/TcpListener
     * stub functions so the generated C can call into iron_net.c without
     * needing the header include (which would conflict with the compiler-
     * emitted struct definitions for NetError/TcpSocket/TcpListener).
     *
     * These prototypes are hand-written rather than re-derived from LIR
     * because they also need to pre-emit prerequisite List/tuple typedefs
     * into ctx.struct_bodies in the right order (notably Iron_List_Iron_Address
     * before Iron_Tuple__Address__NetError for DNS). The generic auto-gen
     * block below handles functions with simpler type dependencies.
     *
     * They reach ctx.prototypes which is concatenated AFTER ctx.struct_bodies
     * in the final output. */
    {
        /* Collect the set of stub names we actually need to emit, gated on
         * the method-name mangling produced by hir_to_lir:
         *   Net.tcp_dial        → Iron_net_tcp_dial
         *   Net.tcp_listen      → Iron_net_tcp_listen
         *   TcpListener.accept  → Iron_tcplistener_accept
         *   TcpListener.close   → Iron_tcplistener_close
         *   TcpSocket.read      → Iron_tcpsocket_read
         *   TcpSocket.write     → Iron_tcpsocket_write
         *   TcpSocket.close     → Iron_tcpsocket_close
         * The `has_*` flags below gate emission so test binaries that
         * never touch net.iron still get byte-for-byte the same output as
         * before this Phase 59 02 change. */
        bool has_net_tcp_dial         = false;
        bool has_net_tcp_listen       = false;
        bool has_tcplistener_accept   = false;
        bool has_tcplistener_close    = false;
        bool has_tcpsocket_read       = false;
        bool has_tcpsocket_write      = false;
        bool has_tcpsocket_close      = false;
        /* Phase 59 P03: UDP + IP stub detectors */
        bool has_net_udp_bind         = false;
        bool has_net_udp_sendto_v4    = false;
        bool has_net_udp_sendto_v6    = false;
        bool has_udpsocket_close      = false;
        bool has_net_ipv4addr_parse   = false;
        bool has_net_ipv4addr_format  = false;
        bool has_net_ipv6addr_parse   = false;
        bool has_net_ipv6addr_format  = false;
        /* Phase 59 P04: DNS stub detector */
        bool has_net_lookup_host      = false;
        for (int fi = 0; fi < module->func_count; fi++) {
            IronLIR_Func *fn = module->funcs[fi];
            if (!fn || !fn->is_extern || fn->extern_c_name) continue;
            const char *mangled = emit_mangle_func_name(fn->name, ctx.arena);
            if (!mangled) continue;
            if (strcmp(mangled, "Iron_net_tcp_dial") == 0)         has_net_tcp_dial = true;
            else if (strcmp(mangled, "Iron_net_tcp_listen") == 0)  has_net_tcp_listen = true;
            else if (strcmp(mangled, "Iron_tcplistener_accept") == 0) has_tcplistener_accept = true;
            else if (strcmp(mangled, "Iron_tcplistener_close") == 0)  has_tcplistener_close = true;
            else if (strcmp(mangled, "Iron_tcpsocket_read") == 0)  has_tcpsocket_read = true;
            else if (strcmp(mangled, "Iron_tcpsocket_write") == 0) has_tcpsocket_write = true;
            else if (strcmp(mangled, "Iron_tcpsocket_close") == 0) has_tcpsocket_close = true;
            /* Phase 59 P03 — method mangling from hir_to_lir:
             *   Net.udp_bind         → Iron_net_udp_bind
             *   Net.udp_sendto_v4    → Iron_net_udp_sendto_v4
             *   Net.udp_sendto_v6    → Iron_net_udp_sendto_v6
             *   UdpSocket.close      → Iron_udpsocket_close
             *   IPv4Addr.parse       → Iron_ipv4addr_parse
             *   IPv4Addr.format      → Iron_ipv4addr_format
             *   IPv6Addr.parse       → Iron_ipv6addr_parse
             *   IPv6Addr.format      → Iron_ipv6addr_format
             * The actual C impls live with "Iron_net_" prefix for the
             * Net.* static methods and with the lowercased-type prefix
             * for instance methods. */
            else if (strcmp(mangled, "Iron_net_udp_bind") == 0)      has_net_udp_bind = true;
            else if (strcmp(mangled, "Iron_net_udp_sendto_v4") == 0) has_net_udp_sendto_v4 = true;
            else if (strcmp(mangled, "Iron_net_udp_sendto_v6") == 0) has_net_udp_sendto_v6 = true;
            else if (strcmp(mangled, "Iron_udpsocket_close") == 0)   has_udpsocket_close = true;
            else if (strcmp(mangled, "Iron_ipv4addr_parse") == 0)    has_net_ipv4addr_parse = true;
            else if (strcmp(mangled, "Iron_ipv4addr_format") == 0)   has_net_ipv4addr_format = true;
            else if (strcmp(mangled, "Iron_ipv6addr_parse") == 0)    has_net_ipv6addr_parse = true;
            else if (strcmp(mangled, "Iron_ipv6addr_format") == 0)   has_net_ipv6addr_format = true;
            /* Phase 59 P04: DNS. Net.lookup_host → Iron_net_lookup_host. */
            else if (strcmp(mangled, "Iron_net_lookup_host") == 0)   has_net_lookup_host = true;
        }
        /* If any net stub is referenced, make sure the three well-known
         * result tuple types are defined. The compiler only synthesises a
         * tuple typedef on demand (emit_ensure_tuple) from a USED tuple
         * type; if the user code only uses (TcpListener, NetError) the
         * (Int, NetError) typedef never lands. The prototypes below
         * reference all three, so we emit whatever is missing up-front
         * and register the mangled name in ctx.emitted_tuples so a
         * subsequent emit_ensure_tuple call is a no-op. */
        bool need_tcp = has_net_tcp_dial || has_net_tcp_listen ||
                        has_tcplistener_accept || has_tcplistener_close ||
                        has_tcpsocket_read || has_tcpsocket_write ||
                        has_tcpsocket_close;
        bool need_udp = has_net_udp_bind || has_net_udp_sendto_v4 ||
                        has_net_udp_sendto_v6 || has_udpsocket_close;
        bool need_ip  = has_net_ipv4addr_parse || has_net_ipv4addr_format ||
                        has_net_ipv6addr_parse || has_net_ipv6addr_format;
        bool need_dns = has_net_lookup_host;
        bool need_any = need_tcp || need_udp || need_ip || need_dns;
        if (need_tcp) {
            static const char *k_net_tuple_names[] = {
                "Iron_Tuple_TcpSocket_NetError",
                "Iron_Tuple_TcpListener_NetError",
                "Iron_Tuple_Int_NetError",
            };
            static const char *k_net_tuple_bodies[] = {
                "typedef struct { Iron_TcpSocket v0; Iron_NetError v1; } Iron_Tuple_TcpSocket_NetError;\n",
                "typedef struct { Iron_TcpListener v0; Iron_NetError v1; } Iron_Tuple_TcpListener_NetError;\n",
                "typedef struct { int64_t v0; Iron_NetError v1; } Iron_Tuple_Int_NetError;\n",
            };
            for (int ti = 0; ti < 3; ti++) {
                bool already = false;
                for (int ei = 0; ei < (int)arrlen(ctx.emitted_tuples); ei++) {
                    if (strcmp(ctx.emitted_tuples[ei], k_net_tuple_names[ti]) == 0) {
                        already = true;
                        break;
                    }
                }
                if (!already) {
                    iron_strbuf_appendf(&ctx.struct_bodies, "%s", k_net_tuple_bodies[ti]);
                    char *tuple_name_copy = iron_arena_strdup(ctx.arena,
                                                    k_net_tuple_names[ti],
                                                    strlen(k_net_tuple_names[ti]));
                    if (!tuple_name_copy) iron_oom_abort("emit_c.c:iron_lir_emit_c net_tuple_name");
                    arrput(ctx.emitted_tuples, tuple_name_copy);
                }
            }
        }
        /* Phase 59 P03: UDP result tuple typedefs. The (Int, NetError)
         * tuple is shared with TCP, so it's already emitted above when
         * need_tcp is true. If the user imports net but only touches UDP,
         * we still need (Int, NetError) for sendto returns, so emit it
         * unconditionally whenever need_udp is true. */
        if (need_udp) {
            static const char *k_udp_tuple_names[] = {
                "Iron_Tuple_UdpSocket_NetError",
                "Iron_Tuple_Int_NetError",
            };
            static const char *k_udp_tuple_bodies[] = {
                "typedef struct { Iron_UdpSocket v0; Iron_NetError v1; } Iron_Tuple_UdpSocket_NetError;\n",
                "typedef struct { int64_t v0; Iron_NetError v1; } Iron_Tuple_Int_NetError;\n",
            };
            for (int ti = 0; ti < 2; ti++) {
                bool already = false;
                for (int ei = 0; ei < (int)arrlen(ctx.emitted_tuples); ei++) {
                    if (strcmp(ctx.emitted_tuples[ei], k_udp_tuple_names[ti]) == 0) {
                        already = true;
                        break;
                    }
                }
                if (!already) {
                    iron_strbuf_appendf(&ctx.struct_bodies, "%s", k_udp_tuple_bodies[ti]);
                    char *tuple_name_copy = iron_arena_strdup(ctx.arena,
                                                    k_udp_tuple_names[ti],
                                                    strlen(k_udp_tuple_names[ti]));
                    if (!tuple_name_copy) iron_oom_abort("emit_c.c:iron_lir_emit_c udp_tuple_name");
                    arrput(ctx.emitted_tuples, tuple_name_copy);
                }
            }
        }
        /* Phase 59 P04: DNS result tuple + list typedef.
         *
         * Net.lookup_host returns (Iron_List_Iron_Address, Iron_NetError).
         * The Iron compiler emits the `struct Iron_Address` tagged-union
         * definition whenever net.iron declares `enum Address { V4(IPv4Addr),
         * V6(IPv6Addr) }`, but it does NOT emit the Iron_List_Iron_Address
         * struct or IRON_LIST_DECL/IMPL expansions because `[Address]`
         * only appears as a stub return type (no ARRAY_LIT triggers
         * emit_mono_list_decls). We emit everything here.
         *
         * The list type + IRON_LIST_DECL/IMPL are appended to
         * ctx.struct_bodies after the enum Address struct has already
         * been emitted by emit_type_decls (called at line 6052 above). */
        if (need_dns) {
            /* Iron_List_Iron_Address struct + DECL + IMPL. Deduped via
             * ctx.emitted_tuples (reuses that registry as a generic
             * emitted-symbol dedup). */
            const char *k_addr_list_name = "Iron_List_Iron_Address";
            bool list_already = false;
            for (int ei = 0; ei < (int)arrlen(ctx.emitted_tuples); ei++) {
                if (strcmp(ctx.emitted_tuples[ei], k_addr_list_name) == 0) {
                    list_already = true;
                    break;
                }
            }
            if (!list_already) {
                iron_strbuf_appendf(&ctx.struct_bodies,
                    "/* Phase 59 P04: Iron_List type for DNS [Address] return */\n"
                    "typedef struct Iron_List_Iron_Address {\n"
                    "    Iron_Address *items;\n"
                    "    int64_t       count;\n"
                    "    int64_t       capacity;\n"
                    "} Iron_List_Iron_Address;\n"
                    "IRON_LIST_DECL(Iron_Address, Iron_Address)\n"
                    "IRON_LIST_IMPL(Iron_Address, Iron_Address)\n\n");
                char *addr_list_copy = iron_arena_strdup(ctx.arena, k_addr_list_name,
                                                          strlen(k_addr_list_name));
                if (!addr_list_copy) iron_oom_abort("emit_c.c:iron_lir_emit_c addr_list_name");
                arrput(ctx.emitted_tuples, addr_list_copy);
            }
            /* Iron_Tuple__Address__NetError — the tuple type name is
             * produced by tuple_build_mangled_name in analyzer/types.c
             * via iron_type_to_string on each element followed by
             * non-identifier-character sanitisation:
             *   [Address] → "[Address]" → "_Address_"
             *   NetError  → "NetError"
             * Final: "Iron_Tuple__Address__NetError" (note the double
             * underscore between Tuple and Address — one from the tuple
             * separator, one from the sanitised `[`). */
            const char *k_addr_tuple_name = "Iron_Tuple__Address__NetError";
            bool tuple_already = false;
            for (int ei = 0; ei < (int)arrlen(ctx.emitted_tuples); ei++) {
                if (strcmp(ctx.emitted_tuples[ei], k_addr_tuple_name) == 0) {
                    tuple_already = true;
                    break;
                }
            }
            if (!tuple_already) {
                iron_strbuf_appendf(&ctx.struct_bodies,
                    "typedef struct { Iron_List_Iron_Address v0; Iron_NetError v1; } Iron_Tuple__Address__NetError;\n");
                char *addr_tuple_copy = iron_arena_strdup(ctx.arena, k_addr_tuple_name,
                                                           strlen(k_addr_tuple_name));
                if (!addr_tuple_copy) iron_oom_abort("emit_c.c:iron_lir_emit_c addr_tuple_name");
                arrput(ctx.emitted_tuples, addr_tuple_copy);
            }
        }

        /* Phase 59 P03: IPv4/IPv6 result tuple typedefs. */
        if (need_ip) {
            static const char *k_ip_tuple_names[] = {
                "Iron_Tuple_IPv4Addr_NetError",
                "Iron_Tuple_IPv6Addr_NetError",
            };
            static const char *k_ip_tuple_bodies[] = {
                "typedef struct { Iron_IPv4Addr v0; Iron_NetError v1; } Iron_Tuple_IPv4Addr_NetError;\n",
                "typedef struct { Iron_IPv6Addr v0; Iron_NetError v1; } Iron_Tuple_IPv6Addr_NetError;\n",
            };
            for (int ti = 0; ti < 2; ti++) {
                bool already = false;
                for (int ei = 0; ei < (int)arrlen(ctx.emitted_tuples); ei++) {
                    if (strcmp(ctx.emitted_tuples[ei], k_ip_tuple_names[ti]) == 0) {
                        already = true;
                        break;
                    }
                }
                if (!already) {
                    iron_strbuf_appendf(&ctx.struct_bodies, "%s", k_ip_tuple_bodies[ti]);
                    char *ip_tuple_copy = iron_arena_strdup(ctx.arena,
                                                    k_ip_tuple_names[ti],
                                                    strlen(k_ip_tuple_names[ti]));
                    if (!ip_tuple_copy) iron_oom_abort("emit_c.c:iron_lir_emit_c ip_tuple_name");
                    arrput(ctx.emitted_tuples, ip_tuple_copy);
                }
            }
        }
        (void)need_any;
        if (has_net_tcp_dial) {
            iron_strbuf_appendf(&ctx.prototypes,
                "Iron_Tuple_TcpSocket_NetError Iron_net_tcp_dial(Iron_String host, int64_t port, int64_t timeout);\n");
        }
        if (has_net_tcp_listen) {
            iron_strbuf_appendf(&ctx.prototypes,
                "Iron_Tuple_TcpListener_NetError Iron_net_tcp_listen(Iron_String host, int64_t port);\n");
        }
        if (has_tcplistener_accept) {
            iron_strbuf_appendf(&ctx.prototypes,
                "Iron_Tuple_TcpSocket_NetError Iron_tcplistener_accept(Iron_TcpListener l, int64_t timeout);\n");
        }
        if (has_tcplistener_close) {
            iron_strbuf_appendf(&ctx.prototypes,
                "void Iron_tcplistener_close(Iron_TcpListener l);\n");
        }
        if (has_tcpsocket_read) {
            iron_strbuf_appendf(&ctx.prototypes,
                "Iron_Tuple_Int_NetError Iron_tcpsocket_read(Iron_TcpSocket s, Iron_String buf, int64_t timeout);\n");
        }
        if (has_tcpsocket_write) {
            iron_strbuf_appendf(&ctx.prototypes,
                "Iron_Tuple_Int_NetError Iron_tcpsocket_write(Iron_TcpSocket s, Iron_String buf, int64_t timeout);\n");
        }
        if (has_tcpsocket_close) {
            iron_strbuf_appendf(&ctx.prototypes,
                "void Iron_tcpsocket_close(Iron_TcpSocket s);\n");
        }
        /* Phase 59 P03 extern prototypes. Names map to iron_net.c symbols:
         *   Iron_net_udp_bind         ← Net.udp_bind
         *   Iron_net_udp_sendto_v4    ← Net.udp_sendto_v4
         *   Iron_net_udp_sendto_v6    ← Net.udp_sendto_v6
         *   Iron_udpsocket_close      ← UdpSocket.close
         *   Iron_net_ipv4addr_parse   ← IPv4Addr.parse   (routed via Iron_ipv4addr_parse mangled name)
         *   Iron_net_ipv4addr_format  ← IPv4Addr.format
         *   Iron_net_ipv6addr_parse   ← IPv6Addr.parse
         *   Iron_net_ipv6addr_format  ← IPv6Addr.format
         *
         * Note on ipv4/ipv6 address method naming: hir_to_lir mangles
         * `IPv4Addr.parse` to `Iron_ipv4addr_parse` (lowercase-type +
         * method). But the C impl in iron_net.c uses the
         * `Iron_net_ipv4addr_parse` name because it's semantically a
         * "net subsystem" function, not a method on the struct. To
         * bridge the two, we emit the prototype with the mangled name
         * the generated C calls (`Iron_ipv4addr_parse`) and rely on
         * iron_net.c defining that symbol. */
        if (has_net_udp_bind) {
            iron_strbuf_appendf(&ctx.prototypes,
                "Iron_Tuple_UdpSocket_NetError Iron_net_udp_bind(Iron_String host, int64_t port);\n");
        }
        if (has_net_udp_sendto_v4) {
            iron_strbuf_appendf(&ctx.prototypes,
                "Iron_Tuple_Int_NetError Iron_net_udp_sendto_v4(Iron_UdpSocket s, Iron_String buf, Iron_IPv4Addr addr, int64_t port, int64_t timeout);\n");
        }
        if (has_net_udp_sendto_v6) {
            iron_strbuf_appendf(&ctx.prototypes,
                "Iron_Tuple_Int_NetError Iron_net_udp_sendto_v6(Iron_UdpSocket s, Iron_String buf, Iron_IPv6Addr addr, int64_t port, int64_t timeout);\n");
        }
        if (has_udpsocket_close) {
            iron_strbuf_appendf(&ctx.prototypes,
                "void Iron_udpsocket_close(Iron_UdpSocket s);\n");
        }
        if (has_net_ipv4addr_parse) {
            iron_strbuf_appendf(&ctx.prototypes,
                "Iron_Tuple_IPv4Addr_NetError Iron_ipv4addr_parse(Iron_String s);\n");
        }
        if (has_net_ipv4addr_format) {
            iron_strbuf_appendf(&ctx.prototypes,
                "Iron_String Iron_ipv4addr_format(Iron_IPv4Addr a);\n");
        }
        if (has_net_ipv6addr_parse) {
            iron_strbuf_appendf(&ctx.prototypes,
                "Iron_Tuple_IPv6Addr_NetError Iron_ipv6addr_parse(Iron_String s);\n");
        }
        if (has_net_ipv6addr_format) {
            iron_strbuf_appendf(&ctx.prototypes,
                "Iron_String Iron_ipv6addr_format(Iron_IPv6Addr a);\n");
        }
        /* Phase 59 P04: DNS extern prototype. Name matches hir_to_lir
         * method-call mangling: Net.lookup_host → Iron_net_lookup_host.
         * Return type uses the sanitised tuple name produced by
         * tuple_build_mangled_name (see above). */
        if (has_net_lookup_host) {
            iron_strbuf_appendf(&ctx.prototypes,
                "Iron_Tuple__Address__NetError Iron_net_lookup_host(Iron_String name, int64_t timeout_ms);\n");
        }
    }

    /* Phase 61 B: Auto-generate prototypes for foreign-method stubs in
     * stdlib .iron files whose C symbols are NOT already declared by an
     * included header or by a hand-maintained block above. Implementation
     * lives in emit_structs.c so emit_web.c can share it; see the header
     * for the skip-list and hir_to_lir contract. */
    emit_foreign_method_prototypes(&ctx);

    if (ctx.prototypes.len > 0) {
        iron_strbuf_appendf(&ctx.prototypes, "\n");
    }

    /* ── Phase 3b: Interface dispatch functions ────────────────────────────── */
    if (ctx.iface_reg) {
        for (int ri = 0; ri < shlen(ctx.iface_reg->map); ri++) {
            Iron_IfaceEntry *entry = &ctx.iface_reg->map[ri].value;
            if (entry->alive_count == 0) continue;

            const char *iface_mangled = emit_mangle_name(entry->iface_name, ctx.arena);

            /* Phase 49: Register interface dispatch functions in specialization
             * registry to prevent duplicate emission of the same (func, type) pair */
            {
                char spec_key[512];
                snprintf(spec_key, sizeof(spec_key), "__dispatch:%s", entry->iface_name);
                if (shgeti(ctx.specialization_registry, spec_key) >= 0) continue;
                shput(ctx.specialization_registry, spec_key, iface_mangled);
            }

            /* Build lowercased dispatch function name prefix:
             * Iron_Shape → Iron_shape (matches hir_to_lir method call mangling) */
            size_t iml = strlen(entry->iface_name);
            char *iface_lower = (char *)iron_arena_alloc(ctx.arena, 5 + iml + 1, 1);
            if (!iface_lower) iron_oom_abort("emit_c.c:iron_lir_emit_c iface_dispatch_lower");
            memcpy(iface_lower, "Iron_", 5);
            for (size_t ci = 0; ci < iml; ci++) {
                char ch = entry->iface_name[ci];
                iface_lower[5 + ci] = (ch >= 'A' && ch <= 'Z')
                    ? (char)(ch + ('a' - 'A')) : ch;
            }
            iface_lower[5 + iml] = '\0';

            /* Generate one dispatch function per interface method */
            for (int mi = 0; mi < entry->iface_decl->method_count; mi++) {
                Iron_Node *sig_node = entry->iface_decl->method_sigs[mi];
                if (!sig_node || sig_node->kind != IRON_NODE_FUNC_DECL) continue;
                Iron_FuncDecl *sig = (Iron_FuncDecl *)sig_node;

                const char *ret_type_c = "void";
                bool has_return = false;
                if (sig->resolved_return_type) {
                    ret_type_c = emit_type_to_c(sig->resolved_return_type, &ctx);
                    has_return = (sig->resolved_return_type->kind != IRON_TYPE_VOID);
                } else if (sig->return_type) {
                    /* Fall back to type annotation for interface method signatures
                     * where resolved_return_type may not be set */
                    if (sig->return_type->kind == IRON_NODE_TYPE_ANNOTATION) {
                        Iron_TypeAnnotation *rta = (Iron_TypeAnnotation *)sig->return_type;
                        ret_type_c = emit_annotation_to_c(rta->name, &ctx);
                        has_return = (strcmp(ret_type_c, "void") != 0);
                    }
                }

                /* Build parameter list: self + extra params */
                Iron_StrBuf param_buf = iron_strbuf_create(128);
                iron_strbuf_appendf(&param_buf, "%s self", iface_mangled);
                for (int pi = 0; pi < sig->param_count; pi++) {
                    Iron_Param *p = (Iron_Param *)sig->params[pi];
                    const char *pt = "void*";
                    if (p->type_ann) {
                        Iron_TypeAnnotation *ta = (Iron_TypeAnnotation *)p->type_ann;
                        pt = emit_annotation_to_c(ta->name, &ctx);
                    }
                    iron_strbuf_appendf(&param_buf, ", %s %s", pt, p->name);
                }
                const char *params = iron_arena_strdup(ctx.arena,
                    iron_strbuf_get(&param_buf), param_buf.len);
                if (!params) iron_oom_abort("emit_c.c:iron_lir_emit_c iface_dispatch_params");
                iron_strbuf_free(&param_buf);

                /* Prototype — use lowercased name to match call site mangling */
                iron_strbuf_appendf(&ctx.prototypes,
                    "static inline %s %s_%s(%s);\n",
                    ret_type_c, iface_lower, sig->name, params);

                /* Build argument forwarding string (extra params only) */
                Iron_StrBuf fwd_buf = iron_strbuf_create(64);
                for (int pi = 0; pi < sig->param_count; pi++) {
                    Iron_Param *p = (Iron_Param *)sig->params[pi];
                    iron_strbuf_appendf(&fwd_buf, ", %s", p->name);
                }
                const char *fwd_args = iron_arena_strdup(ctx.arena,
                    iron_strbuf_get(&fwd_buf), fwd_buf.len);
                if (!fwd_args) iron_oom_abort("emit_c.c:iron_lir_emit_c iface_dispatch_fwd_args");
                iron_strbuf_free(&fwd_buf);

                /* Implementation — use lowercased name */
                iron_strbuf_appendf(&ctx.lifted_funcs,
                    "static inline %s %s_%s(%s) {\n"
                    "    switch(self.tag) {\n",
                    ret_type_c, iface_lower, sig->name, params);

                for (int ji = 0; ji < entry->impl_count; ji++) {
                    Iron_IfaceImpl *impl = &entry->impls[ji];
                    if (!impl->is_alive) continue;
                    /* Build lowercased concrete method name: Iron_circle_area */
                    size_t tnl = strlen(impl->type_name);
                    char *impl_lower = (char *)iron_arena_alloc(ctx.arena, 5 + tnl + 1, 1);
                    if (!impl_lower) iron_oom_abort("emit_c.c:iron_lir_emit_c iface_dispatch_impl_lower");
                    memcpy(impl_lower, "Iron_", 5);
                    for (size_t ci = 0; ci < tnl; ci++) {
                        char ch = impl->type_name[ci];
                        impl_lower[5 + ci] = (ch >= 'A' && ch <= 'Z')
                            ? (char)(ch + ('a' - 'A')) : ch;
                    }
                    impl_lower[5 + tnl] = '\0';
                    /* Phase 48-03: dereference pointer for indirect (large) variants */
                    char ikey[512];
                    snprintf(ikey, sizeof(ikey), "%s:%s", iface_mangled, impl->type_name);
                    bool is_indirect = (ctx.indirect_variants &&
                                        shgeti(ctx.indirect_variants, ikey) >= 0);
                    iron_strbuf_appendf(&ctx.lifted_funcs,
                        "        case %s_TAG_%s: %s%s_%s(%sself.data.%s%s); break;\n",
                        iface_mangled, impl->type_name,
                        has_return ? "return " : "",
                        impl_lower, sig->name,
                        is_indirect ? "*" : "",
                        impl->type_name,
                        fwd_args);
                }

                {
                    const char *default_ret = "break;";
                    if (has_return) {
                        if (strcmp(ret_type_c, "Iron_String") == 0) {
                            default_ret = "return iron_string_from_literal(\"\", 0);";
                        } else if (strcmp(ret_type_c, "bool") == 0) {
                            default_ret = "return false;";
                        } else if (strcmp(ret_type_c, "double") == 0 ||
                                   strcmp(ret_type_c, "float") == 0) {
                            default_ret = "return 0.0;";
                        } else {
                            default_ret = "return 0;";
                        }
                    }
                    iron_strbuf_appendf(&ctx.lifted_funcs,
                        "        default: %s\n"
                        "    }\n"
                        "}\n\n",
                        default_ret);
                }
            }
        }
    }

    /* ── Phase 4: Function bodies ─────────────────────────────────────────── */
    bool has_main = false;
    for (int i = 0; i < module->func_count; i++) {
        IronLIR_Func *fn = module->funcs[i];
        if (fn->is_extern) continue;
        /* Accept "main" (from IR lowerer) or "Iron_main" (from unit tests / direct IR) */
        if (strcmp(fn->name, "main") == 0 ||
            strcmp(fn->name, "Iron_main") == 0) {
            has_main = true;
        }
        emit_func_body(&ctx, fn);
    }

    /* ── Phase 5: main() wrapper ──────────────────────────────────────────── */
    if (has_main) {
        iron_strbuf_appendf(&ctx.main_wrapper,
                             "int main(int argc, char** argv) {\n");
        iron_strbuf_appendf(&ctx.main_wrapper,
                             "    iron_runtime_init(argc, argv);\n");
        iron_strbuf_appendf(&ctx.main_wrapper,
                             "    Iron_main();\n");
        iron_strbuf_appendf(&ctx.main_wrapper,
                             "    iron_runtime_shutdown();\n");
        iron_strbuf_appendf(&ctx.main_wrapper,
                             "    return 0;\n");
        iron_strbuf_appendf(&ctx.main_wrapper, "}\n");
    }

    /* ── Concatenate all sections ─────────────────────────────────────────── */
    Iron_StrBuf output = iron_strbuf_create(8192);

    iron_strbuf_appendf(&output, "/* Generated by Iron compiler (IR backend) */\n\n");
    iron_strbuf_append(&output, iron_strbuf_get(&ctx.includes),
                        ctx.includes.len);
    iron_strbuf_append(&output, iron_strbuf_get(&ctx.forward_decls),
                        ctx.forward_decls.len);
    iron_strbuf_append(&output, iron_strbuf_get(&ctx.enum_defs),
                        ctx.enum_defs.len);
    iron_strbuf_append(&output, iron_strbuf_get(&ctx.struct_bodies),
                        ctx.struct_bodies.len);
    iron_strbuf_append(&output, iron_strbuf_get(&ctx.global_consts),
                        ctx.global_consts.len);
    iron_strbuf_append(&output, iron_strbuf_get(&ctx.prototypes),
                        ctx.prototypes.len);
    if (ctx.lifted_funcs.len > 0) {
        iron_strbuf_append(&output, iron_strbuf_get(&ctx.lifted_funcs),
                            ctx.lifted_funcs.len);
    }
    iron_strbuf_append(&output, iron_strbuf_get(&ctx.implementations),
                        ctx.implementations.len);
    iron_strbuf_append(&output, iron_strbuf_get(&ctx.main_wrapper),
                        ctx.main_wrapper.len);

    /* Arena-dup the final string */
    const char *result = iron_arena_strdup(arena, iron_strbuf_get(&output),
                                            output.len);
    if (!result) iron_oom_abort("emit_c.c:iron_lir_emit_c result");

    /* Free all working buffers and maps via consolidated cleanup */
    emit_ctx_cleanup(&ctx);
    iron_strbuf_free(&output);

    return result;
}
