#include "lir/verify.h"
#include "diagnostics/diagnostics.h"
#include "vendor/stb_ds.h"
#include <string.h>
#include <stdio.h>

/* ── Helpers ──────────────────────────────────────────────────────────────── */

/* Check if a block ID exists in a function. */
static bool block_id_valid(const IronLIR_Func *fn, IronLIR_BlockId id) {
    if (id == IRON_LIR_BLOCK_INVALID) return false;
    for (int i = 0; i < fn->block_count; i++) {
        if (fn->blocks[i]->id == id) return true;
    }
    return false;
}

/* Collect all ValueId operands used by an instruction.
 * out must be a caller-supplied array large enough to hold all operands.
 * The maximum operand count per instruction is bounded by the largest instruction
 * kind (phi, construct, array_lit, etc.) which we cap at 64 per call.
 * count is set to the number of valid (non-INVALID) operand IDs written to out.
 */
#define MAX_OPERANDS 64

static void collect_operands(const IronLIR_Instr *instr,
                              IronLIR_ValueId *out, int *count) {
    *count = 0;

#define PUSH(v) do { \
    if ((v) != IRON_LIR_VALUE_INVALID && *count < MAX_OPERANDS) \
        out[(*count)++] = (v); \
} while (0)

    switch (instr->kind) {
    case IRON_LIR_CONST_INT:
    case IRON_LIR_CONST_FLOAT:
    case IRON_LIR_CONST_BOOL:
    case IRON_LIR_CONST_STRING:
    case IRON_LIR_CONST_NULL:
    case IRON_LIR_FUNC_REF:
        /* No value operands */
        break;

    case IRON_LIR_ADD:
    case IRON_LIR_SUB:
    case IRON_LIR_MUL:
    case IRON_LIR_DIV:
    case IRON_LIR_MOD:
    case IRON_LIR_EQ:
    case IRON_LIR_NEQ:
    case IRON_LIR_LT:
    case IRON_LIR_LTE:
    case IRON_LIR_GT:
    case IRON_LIR_GTE:
    case IRON_LIR_AND:
    case IRON_LIR_OR:
        PUSH(instr->binop.left);
        PUSH(instr->binop.right);
        break;

    case IRON_LIR_NEG:
    case IRON_LIR_NOT:
        PUSH(instr->unop.operand);
        break;

    case IRON_LIR_ALLOCA:
        /* No value operands */
        break;

    case IRON_LIR_LOAD:
        PUSH(instr->load.ptr);
        break;

    case IRON_LIR_STORE:
        PUSH(instr->store.ptr);
        PUSH(instr->store.value);
        break;

    case IRON_LIR_GET_FIELD:
        PUSH(instr->field.object);
        break;

    case IRON_LIR_SET_FIELD:
        PUSH(instr->field.object);
        PUSH(instr->field.value);
        break;

    case IRON_LIR_GET_INDEX:
        PUSH(instr->index.array);
        PUSH(instr->index.index);
        break;

    case IRON_LIR_SET_INDEX:
        PUSH(instr->index.array);
        PUSH(instr->index.index);
        PUSH(instr->index.value);
        break;

    case IRON_LIR_CALL:
        if (instr->call.func_decl == NULL) {
            PUSH(instr->call.func_ptr);
        }
        for (int i = 0; i < instr->call.arg_count; i++) {
            PUSH(instr->call.args[i]);
        }
        break;

    case IRON_LIR_JUMP:
        /* No value operands */
        break;

    case IRON_LIR_BRANCH:
        PUSH(instr->branch.cond);
        break;

    case IRON_LIR_SWITCH:
        PUSH(instr->sw.subject);
        break;

    case IRON_LIR_RETURN:
        if (!instr->ret.is_void) {
            PUSH(instr->ret.value);
        }
        break;

    case IRON_LIR_CAST:
        PUSH(instr->cast.value);
        break;

    case IRON_LIR_HEAP_ALLOC:
        PUSH(instr->heap_alloc.inner_val);
        break;

    case IRON_LIR_RC_ALLOC:
        PUSH(instr->rc_alloc.inner_val);
        break;

    case IRON_LIR_FREE:
        PUSH(instr->free_instr.value);
        break;

    case IRON_LIR_CONSTRUCT:
        for (int i = 0; i < instr->construct.field_count; i++) {
            PUSH(instr->construct.field_vals[i]);
        }
        break;

    case IRON_LIR_ARRAY_LIT:
        for (int i = 0; i < instr->array_lit.element_count; i++) {
            PUSH(instr->array_lit.elements[i]);
        }
        break;

    case IRON_LIR_SLICE:
        PUSH(instr->slice.array);
        PUSH(instr->slice.start);
        PUSH(instr->slice.end);
        break;

    case IRON_LIR_IS_NULL:
    case IRON_LIR_IS_NOT_NULL:
        PUSH(instr->null_check.value);
        break;

    case IRON_LIR_INTERP_STRING:
        for (int i = 0; i < instr->interp_string.part_count; i++) {
            PUSH(instr->interp_string.parts[i]);
        }
        break;

    case IRON_LIR_MAKE_CLOSURE:
        for (int i = 0; i < instr->make_closure.capture_count; i++) {
            PUSH(instr->make_closure.captures[i]);
        }
        break;

    case IRON_LIR_SPAWN:
        PUSH(instr->spawn.pool_val);
        break;

    case IRON_LIR_PARALLEL_FOR:
        PUSH(instr->parallel_for.range_val);
        PUSH(instr->parallel_for.pool_val);
        for (int i = 0; i < instr->parallel_for.capture_count; i++) {
            PUSH(instr->parallel_for.captures[i]);
        }
        break;

    case IRON_LIR_AWAIT:
        PUSH(instr->await.handle);
        break;

    case IRON_LIR_PHI:
        for (int i = 0; i < instr->phi.count; i++) {
            PUSH(instr->phi.values[i]);
        }
        break;

    case IRON_LIR_POISON:
        /* No operands — poison is a standalone error placeholder */
        break;

    default:
        break;
    }

#undef PUSH
}

/* ── Per-function verifier ────────────────────────────────────────────────── */

static void verify_func(const IronLIR_Func *fn, const IronLIR_Module *module,
                         Iron_DiagList *diags, Iron_Arena *arena) {
    (void)module;

    /* Extern functions have no body — skip structural verification */
    if (fn->is_extern) return;

    /* Invariant 1: function must have at least one block */
    if (fn->block_count == 0) {
        Iron_Span span = iron_span_make(fn->name, 0, 0, 0, 0);
        char msg[256];
        snprintf(msg, sizeof(msg), "function '%s' has no basic blocks", fn->name);
        iron_diag_emit(diags, arena, IRON_DIAG_ERROR,
                       IRON_ERR_LIR_NO_ENTRY_BLOCK,
                       span, msg,
                       "add at least an entry block");
        return; /* Nothing else to check without blocks */
    }

    /* Walk each block */
    for (int bi = 0; bi < fn->block_count; bi++) {
        const IronLIR_Block *block = fn->blocks[bi];

        /* Invariant 2: block must end with a terminator */
        if (block->instr_count == 0) {
            /* Empty block — no terminator */
            Iron_Span span;
            if (fn->blocks[0]->instr_count > 0) {
                span = fn->blocks[0]->instrs[0]->span;
            } else {
                span = iron_span_make(fn->name, 0, 0, 0, 0);
            }
            char msg[256];
            snprintf(msg, sizeof(msg), "basic block '%s' has no terminator", block->label);
            iron_diag_emit(diags, arena, IRON_DIAG_ERROR,
                           IRON_ERR_LIR_MISSING_TERMINATOR,
                           span, msg,
                           "add a return, jump, or branch as the last instruction");
        } else {
            const IronLIR_Instr *last = block->instrs[block->instr_count - 1];
            if (!iron_lir_is_terminator(last->kind)) {
                char msg[256];
                snprintf(msg, sizeof(msg), "basic block '%s' has no terminator", block->label);
                iron_diag_emit(diags, arena, IRON_DIAG_ERROR,
                               IRON_ERR_LIR_MISSING_TERMINATOR,
                               last->span, msg,
                               "add a return, jump, or branch as the last instruction");
            }
        }

        /* Invariant 3: no instruction after a terminator */
        bool found_terminator = false;
        for (int ii = 0; ii < block->instr_count; ii++) {
            const IronLIR_Instr *instr = block->instrs[ii];
            if (found_terminator) {
                char msg[256];
                snprintf(msg, sizeof(msg), "instruction after terminator in block '%s'",
                         block->label);
                iron_diag_emit(diags, arena, IRON_DIAG_ERROR,
                               IRON_ERR_LIR_INSTR_AFTER_TERMINATOR,
                               instr->span, msg,
                               "move this instruction before the terminator or to a new block");
            }
            if (iron_lir_is_terminator(instr->kind)) {
                found_terminator = true;
            }
        }

        /* Invariant 4: branch targets must be valid block IDs in this function */
        for (int ii = 0; ii < block->instr_count; ii++) {
            const IronLIR_Instr *instr = block->instrs[ii];
            switch (instr->kind) {
            case IRON_LIR_JUMP:
                if (!block_id_valid(fn, instr->jump.target)) {
                    char msg[256];
                    snprintf(msg, sizeof(msg),
                             "branch target block ID %u does not exist in function '%s'",
                             instr->jump.target, fn->name);
                    iron_diag_emit(diags, arena, IRON_DIAG_ERROR,
                                   IRON_ERR_LIR_INVALID_BRANCH_TARGET,
                                   instr->span, msg,
                                   "check that the target block was added to the function");
                }
                break;
            case IRON_LIR_BRANCH:
                if (!block_id_valid(fn, instr->branch.then_block)) {
                    char msg[256];
                    snprintf(msg, sizeof(msg),
                             "branch target block ID %u does not exist in function '%s'",
                             instr->branch.then_block, fn->name);
                    iron_diag_emit(diags, arena, IRON_DIAG_ERROR,
                                   IRON_ERR_LIR_INVALID_BRANCH_TARGET,
                                   instr->span, msg,
                                   "check that the target block was added to the function");
                }
                if (!block_id_valid(fn, instr->branch.else_block)) {
                    char msg[256];
                    snprintf(msg, sizeof(msg),
                             "branch target block ID %u does not exist in function '%s'",
                             instr->branch.else_block, fn->name);
                    iron_diag_emit(diags, arena, IRON_DIAG_ERROR,
                                   IRON_ERR_LIR_INVALID_BRANCH_TARGET,
                                   instr->span, msg,
                                   "check that the target block was added to the function");
                }
                break;
            case IRON_LIR_SWITCH:
                if (!block_id_valid(fn, instr->sw.default_block)) {
                    char msg[256];
                    snprintf(msg, sizeof(msg),
                             "branch target block ID %u does not exist in function '%s'",
                             instr->sw.default_block, fn->name);
                    iron_diag_emit(diags, arena, IRON_DIAG_ERROR,
                                   IRON_ERR_LIR_INVALID_BRANCH_TARGET,
                                   instr->span, msg,
                                   "check that the target block was added to the function");
                }
                for (int ci = 0; ci < instr->sw.case_count; ci++) {
                    if (!block_id_valid(fn, instr->sw.case_blocks[ci])) {
                        char msg[256];
                        snprintf(msg, sizeof(msg),
                                 "branch target block ID %u does not exist in function '%s'",
                                 instr->sw.case_blocks[ci], fn->name);
                        iron_diag_emit(diags, arena, IRON_DIAG_ERROR,
                                       IRON_ERR_LIR_INVALID_BRANCH_TARGET,
                                       instr->span, msg,
                                       "check that the target block was added to the function");
                    }
                }
                break;
            default:
                break;
            }
        }

        /* Invariant 5a: poison instructions indicate a lowering error */
        for (int ii = 0; ii < block->instr_count; ii++) {
            const IronLIR_Instr *instr = block->instrs[ii];
            if (instr->kind == IRON_LIR_POISON) {
                char msg[256];
                snprintf(msg, sizeof(msg),
                         "Poison instruction found in block '%s' (lowering error placeholder)",
                         block->label);
                iron_diag_emit(diags, arena, IRON_DIAG_ERROR,
                               IRON_ERR_LOWER_UNSUPPORTED,
                               instr->span, msg,
                               "this indicates an unsupported or erroneous construct during lowering");
            }
        }

        /* Invariant 5: use-before-def — check all operand IDs exist in value_table */
        for (int ii = 0; ii < block->instr_count; ii++) {
            const IronLIR_Instr *instr = block->instrs[ii];
            IronLIR_ValueId ops[MAX_OPERANDS];
            int op_count = 0;
            collect_operands(instr, ops, &op_count);

            for (int oi = 0; oi < op_count; oi++) {
                IronLIR_ValueId op = ops[oi];
                if (op != IRON_LIR_VALUE_INVALID &&
                    ((ptrdiff_t)op >= arrlen(fn->value_table) ||
                     fn->value_table[op] == NULL)) {
                    /* Synthetic param ValueIds (1..param_count) are intentionally
                     * NULL in value_table — they represent incoming argument values
                     * that have no backing instruction. Skip the use-before-def check
                     * for these. Any ValueId beyond param_count that is NULL IS an error. */
                    if ((int)op >= 1 && (int)op <= fn->param_count) continue;
                    char msg[256];
                    snprintf(msg, sizeof(msg), "use of undefined value %%%u", op);
                    iron_diag_emit(diags, arena, IRON_DIAG_ERROR,
                                   IRON_ERR_LIR_USE_BEFORE_DEF,
                                   instr->span, msg,
                                   "ensure the value is defined before this instruction");
                }
            }
        }

        /* Invariant 6: return type mismatch — check return instructions match func->return_type */
        for (int ii = 0; ii < block->instr_count; ii++) {
            const IronLIR_Instr *instr = block->instrs[ii];
            if (instr->kind != IRON_LIR_RETURN) continue;

            if (fn->return_type == NULL) {
                /* Void function: return must be void */
                if (!instr->ret.is_void) {
                    char msg[256];
                    snprintf(msg, sizeof(msg), "return type mismatch in function '%s'", fn->name);
                    iron_diag_emit(diags, arena, IRON_DIAG_ERROR,
                                   IRON_ERR_LIR_RETURN_TYPE_MISMATCH,
                                   instr->span, msg,
                                   "return value type must match function signature");
                }
            } else {
                /* Non-void function: return must not be void, and value type must match */
                if (instr->ret.is_void) {
                    char msg[256];
                    snprintf(msg, sizeof(msg), "return type mismatch in function '%s'", fn->name);
                    iron_diag_emit(diags, arena, IRON_DIAG_ERROR,
                                   IRON_ERR_LIR_RETURN_TYPE_MISMATCH,
                                   instr->span, msg,
                                   "return value type must match function signature");
                } else {
                    /* Check that the returned value's type matches the declared return type */
                    IronLIR_ValueId ret_id = instr->ret.value;
                    if (ret_id != IRON_LIR_VALUE_INVALID &&
                        (ptrdiff_t)ret_id < arrlen(fn->value_table) &&
                        fn->value_table[ret_id] != NULL) {
                        const IronLIR_Instr *ret_val_instr = fn->value_table[ret_id];
                        if (ret_val_instr->type != NULL &&
                            !iron_type_equals(ret_val_instr->type, fn->return_type)) {
                            char msg[256];
                            snprintf(msg, sizeof(msg),
                                     "return type mismatch in function '%s'", fn->name);
                            iron_diag_emit(diags, arena, IRON_DIAG_ERROR,
                                           IRON_ERR_LIR_RETURN_TYPE_MISMATCH,
                                           instr->span, msg,
                                           "return value type must match function signature");
                        }
                    }
                }
            }
        }
    }
}

/* ── Public API ───────────────────────────────────────────────────────────── */

bool iron_lir_verify(const IronLIR_Module *module, Iron_DiagList *diags, Iron_Arena *arena) {
    if (!module || !diags || !arena) return false;

    int initial_error_count = diags->error_count;

    for (int i = 0; i < module->func_count; i++) {
        verify_func(module->funcs[i], module, diags, arena);
    }

    return diags->error_count == initial_error_count;
}
