/* ir_optimize.c — IR optimization passes for the Iron compiler.
 *
 * Houses all IR transformation passes that run between lowering and C emission:
 *   Phase 0:  phi_eliminate           (phi -> alloca+store+load)
 *   Phase 0a: analyze_array_param_modes (PARAM-01/02 pointer-mode analysis)
 *   Phase 0b: optimize_array_repr     (ARR-01 stack vs heap array selection)
 *
 * After Plan 02, this will also contain:
 *   Copy propagation, constant folding, dead code elimination (fixpoint loop)
 */

#include "ir/ir_optimize.h"
#include "ir/print.h"
#include "ir/verify.h"
#include "diagnostics/diagnostics.h"
#include "util/arena.h"
#include "vendor/stb_ds.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <assert.h>

/* ── Internal helpers ─────────────────────────────────────────────────────── */

/* Find a block within a function by ID */
static IronIR_Block *find_block(IronIR_Func *fn, IronIR_BlockId id) {
    for (int i = 0; i < fn->block_count; i++) {
        if (fn->blocks[i]->id == id) return fn->blocks[i];
    }
    return NULL;
}

/* Find index of the terminator instruction in a block */
static int find_terminator_idx(IronIR_Block *block) {
    for (int i = 0; i < block->instr_count; i++) {
        if (iron_ir_is_terminator(block->instrs[i]->kind)) return i;
    }
    return block->instr_count;  /* no terminator found — append at end */
}

/* Insert a pre-built instruction into a block before the terminator */
static void insert_store_before_terminator_instr(IronIR_Block *block,
                                                   IronIR_Instr *store) {
    int term_idx = find_terminator_idx(block);

    /* Shift all instructions from term_idx onward by one */
    arrput(block->instrs, NULL);  /* grow by 1 */
    block->instr_count++;
    for (int i = block->instr_count - 1; i > term_idx; i--) {
        block->instrs[i] = block->instrs[i - 1];
    }
    block->instrs[term_idx] = store;
}

/* Create an alloca instruction directly (without appending to any block).
 * This is used by phi_eliminate to create alloca instructions that will be
 * manually inserted into the entry block. */
static IronIR_Instr *make_alloca_instr(IronIR_Func *fn, Iron_Type *alloc_type,
                                        Iron_Span span) {
    IronIR_Instr *instr = ARENA_ALLOC(fn->arena, IronIR_Instr);
    memset(instr, 0, sizeof(*instr));
    instr->kind             = IRON_IR_ALLOCA;
    instr->type             = alloc_type;  /* alloca result "type" for load */
    instr->span             = span;
    instr->alloca.alloc_type = alloc_type;
    instr->alloca.name_hint  = NULL;

    /* Assign a value ID */
    instr->id = fn->next_value_id++;
    while (arrlen(fn->value_table) <= (ptrdiff_t)instr->id) {
        arrput(fn->value_table, NULL);
    }
    fn->value_table[instr->id] = instr;
    return instr;
}

/* Create a store instruction directly (without appending to any block). */
static IronIR_Instr *make_store_instr(IronIR_Func *fn, IronIR_ValueId ptr,
                                       IronIR_ValueId value, Iron_Span span) {
    IronIR_Instr *instr = ARENA_ALLOC(fn->arena, IronIR_Instr);
    memset(instr, 0, sizeof(*instr));
    instr->kind        = IRON_IR_STORE;
    instr->span        = span;
    instr->id          = IRON_IR_VALUE_INVALID;
    instr->store.ptr   = ptr;
    instr->store.value = value;
    return instr;
}

/* ── Phi elimination pre-pass ─────────────────────────────────────────────── */

/* Phi elimination: walk all functions, replace phi nodes with alloca+store+load */
static void phi_eliminate(IronIR_Module *module) {
    for (int fi = 0; fi < module->func_count; fi++) {
        IronIR_Func *fn = module->funcs[fi];
        if (fn->is_extern || fn->block_count == 0) continue;

        /* Collect all phi nodes first to avoid modifying while iterating */
        IronIR_Instr **phis = NULL;  /* stb_ds array of phi instrs */

        for (int bi = 0; bi < fn->block_count; bi++) {
            IronIR_Block *block = fn->blocks[bi];
            for (int ii = 0; ii < block->instr_count; ii++) {
                IronIR_Instr *instr = block->instrs[ii];
                if (instr->kind == IRON_IR_PHI) {
                    arrput(phis, instr);
                }
            }
        }

        /* Process each phi */
        for (int pi = 0; pi < (int)arrlen(phis); pi++) {
            IronIR_Instr *phi = phis[pi];
            Iron_Span span = phi->span;

            /* 1. Create an alloca instruction (without appending to a block) */
            IronIR_Instr *alloca_instr = make_alloca_instr(fn, phi->type, span);
            IronIR_ValueId alloca_id = alloca_instr->id;

            /* Insert alloca at the top of the entry block */
            IronIR_Block *entry = fn->blocks[0];
            arrput(entry->instrs, NULL);  /* grow by 1 */
            entry->instr_count++;
            for (int i = entry->instr_count - 1; i > 0; i--) {
                entry->instrs[i] = entry->instrs[i - 1];
            }
            entry->instrs[0] = alloca_instr;

            /* 2. In each predecessor block, insert store before terminator */
            for (int i = 0; i < phi->phi.count; i++) {
                IronIR_BlockId pred_id = phi->phi.pred_blocks[i];
                IronIR_ValueId val     = phi->phi.values[i];
                IronIR_Block  *pred    = find_block(fn, pred_id);
                if (!pred) continue;

                IronIR_Instr *store = make_store_instr(fn, alloca_id, val, span);
                insert_store_before_terminator_instr(pred, store);
            }

            /* 2b. Find the block containing this phi and ensure ALL its
             * predecessors have a store for the alloca.  Predecessors not
             * covered by the phi operands (e.g., elif branches with array
             * indexing that introduce intermediate ValueIds) get a default
             * store using the first phi value as a safe fallback. */
            {
                IronIR_Block *phi_block = NULL;
                for (int bi = 0; bi < fn->block_count; bi++) {
                    IronIR_Block *blk = fn->blocks[bi];
                    for (int ii = 0; ii < blk->instr_count; ii++) {
                        if (blk->instrs[ii] == phi) {
                            phi_block = blk;
                            break;
                        }
                    }
                    if (phi_block) break;
                }
                if (phi_block && phi->phi.count > 0) {
                    IronIR_ValueId default_val = phi->phi.values[0];
                    for (int pi2 = 0; pi2 < (int)arrlen(phi_block->preds); pi2++) {
                        IronIR_BlockId pred_id = phi_block->preds[pi2];
                        /* Check if this predecessor is already covered */
                        bool covered = false;
                        for (int k = 0; k < phi->phi.count; k++) {
                            if (phi->phi.pred_blocks[k] == pred_id) {
                                covered = true;
                                break;
                            }
                        }
                        if (!covered) {
                            IronIR_Block *pred = find_block(fn, pred_id);
                            if (pred) {
                                IronIR_Instr *store = make_store_instr(
                                    fn, alloca_id, default_val, span);
                                insert_store_before_terminator_instr(pred, store);
                            }
                        }
                    }
                }
            }

            /* 3. Replace phi with a LOAD from the alloca */
            phi->kind     = IRON_IR_LOAD;
            phi->load.ptr = alloca_id;
            /* phi->id and phi->type remain the same — the load produces the
             * same ValueId so all existing uses are automatically valid */
        }

        arrfree(phis);
    }
}

/* ── Array parameter mode helpers (PARAM-01/PARAM-02) ────────────────────── */

/* Build a key for the array_param_modes map: "func_name\tparam_index" */
static const char *make_param_mode_key(const char *func_name, int param_index,
                                        Iron_Arena *arena) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%d", param_index);
    size_t fn_len = strlen(func_name);
    size_t idx_len = strlen(buf);
    size_t total = fn_len + 1 + idx_len + 1;
    char *key = (char *)iron_arena_alloc(arena, total, 1);
    memcpy(key, func_name, fn_len);
    key[fn_len] = '\t';
    memcpy(key + fn_len + 1, buf, idx_len + 1);
    return key;
}

/* Look up the ArrayParamMode for a given function + param index. */
ArrayParamMode iron_ir_get_array_param_mode(IronIR_OptimizeInfo *info,
                                             const char *func_name,
                                             int param_index,
                                             Iron_Arena *arena) {
    const char *key = make_param_mode_key(func_name, param_index, arena);
    ptrdiff_t idx = shgeti(info->array_param_modes, key);
    if (idx >= 0) return (ArrayParamMode)info->array_param_modes[idx].value;
    return ARRAY_PARAM_LIST;
}

/* Find an IronIR_Func in the module by IR name. */
static IronIR_Func *find_ir_func(IronIR_Module *module, const char *ir_name) {
    if (!ir_name) return NULL;
    for (int i = 0; i < module->func_count; i++) {
        if (strcmp(module->funcs[i]->name, ir_name) == 0)
            return module->funcs[i];
    }
    return NULL;
}

/* Analyze all functions and determine which array parameters can be passed
 * as pointer+length instead of Iron_List_T.
 *
 * A parameter qualifies when it is ONLY used for:
 *   - GET_INDEX (read access)
 *   - SET_INDEX (write access, mutable ptr)
 *   - GET_FIELD .count (len() builtin)
 *   - STORE of param value into its own alloca (entry-block pattern)
 *
 * Disqualified when:
 *   - Loaded alias is stored into another alloca (var a = arr pattern)
 *   - Alias passed as CALL argument
 *   - Alias used in RETURN, SET_FIELD, CONSTRUCT, MAKE_CLOSURE, SLICE
 *   - Alloca is reassigned with a non-alias value */
static void analyze_array_param_modes(IronIR_Module *module,
                                       IronIR_OptimizeInfo *info,
                                       Iron_Arena *arena) {
    /* Iterate until convergence: if a callee's array param already has
     * pointer mode, passing an array to it shouldn't disqualify the caller.
     * First pass resolves leaf functions; subsequent passes propagate. */
    for (int iteration = 0; iteration < 8; iteration++) {
        bool changed = false;

        for (int fi = 0; fi < module->func_count; fi++) {
            IronIR_Func *fn = module->funcs[fi];
            if (fn->is_extern || fn->block_count == 0) continue;

            for (int pi = 0; pi < fn->param_count; pi++) {
                Iron_Type *pt = fn->params[pi].type;
                if (!pt || pt->kind != IRON_TYPE_ARRAY) continue;

                /* Skip if already resolved */
                ArrayParamMode existing = iron_ir_get_array_param_mode(info, fn->name, pi, arena);
                if (existing != ARRAY_PARAM_LIST) continue;

                IronIR_ValueId param_val_id = (IronIR_ValueId)(pi * 2 + 1);
                IronIR_ValueId alloca_id    = (IronIR_ValueId)(pi * 2 + 2);

                /* Build alias set: param_val and alloca, plus LOADs from alloca */
                struct { IronIR_ValueId key; bool value; } *aliases = NULL;
                hmput(aliases, param_val_id, true);
                hmput(aliases, alloca_id, true);

                for (int bi = 0; bi < fn->block_count; bi++) {
                    IronIR_Block *block = fn->blocks[bi];
                    for (int ii = 0; ii < block->instr_count; ii++) {
                        IronIR_Instr *instr = block->instrs[ii];
                        if (instr->kind == IRON_IR_LOAD) {
                            if (hmgeti(aliases, instr->load.ptr) >= 0) {
                                hmput(aliases, instr->id, true);
                            }
                        }
                    }
                }

                /* Scan for disqualifying uses */
                bool has_write = false;
                bool disqualified = false;

                for (int bi = 0; bi < fn->block_count && !disqualified; bi++) {
                    IronIR_Block *block = fn->blocks[bi];
                    for (int ii = 0; ii < block->instr_count && !disqualified; ii++) {
                        IronIR_Instr *instr = block->instrs[ii];

                        switch (instr->kind) {
                        case IRON_IR_GET_INDEX:
                            break;
                        case IRON_IR_SET_INDEX:
                            if (hmgeti(aliases, instr->index.array) >= 0)
                                has_write = true;
                            break;
                        case IRON_IR_GET_FIELD:
                            break;
                        case IRON_IR_STORE:
                            if (hmgeti(aliases, instr->store.ptr) >= 0 &&
                                hmgeti(aliases, instr->store.value) < 0) {
                                disqualified = true;
                            }
                            if (hmgeti(aliases, instr->store.ptr) < 0 &&
                                hmgeti(aliases, instr->store.value) >= 0) {
                                disqualified = true;
                            }
                            break;
                        case IRON_IR_CALL:
                            for (int ai = 0; ai < instr->call.arg_count; ai++) {
                                if (hmgeti(aliases, instr->call.args[ai]) < 0) continue;
                                /* Check if callee accepts pointer mode for this param */
                                const char *callee_name = NULL;
                                if (instr->call.func_decl && !instr->call.func_decl->is_extern)
                                    callee_name = instr->call.func_decl->name;
                                else if (!instr->call.func_decl) {
                                    IronIR_ValueId fptr = instr->call.func_ptr;
                                    if (fptr != IRON_IR_VALUE_INVALID &&
                                        fptr < (IronIR_ValueId)arrlen(fn->value_table) &&
                                        fn->value_table[fptr] != NULL &&
                                        fn->value_table[fptr]->kind == IRON_IR_FUNC_REF)
                                        callee_name = fn->value_table[fptr]->func_ref.func_name;
                                }
                                if (callee_name) {
                                    /* Recursive call: same function, same param index.
                                     * The param will get the same mode we're computing. */
                                    if (strcmp(callee_name, fn->name) == 0 && ai == pi) {
                                        has_write = true; /* conservative: assume mutable */
                                        continue;
                                    }
                                    ArrayParamMode cpm = iron_ir_get_array_param_mode(info, callee_name, ai, arena);
                                    if (cpm == ARRAY_PARAM_CONST_PTR || cpm == ARRAY_PARAM_MUT_PTR) {
                                        /* Callee uses pointer mode — propagate write flag */
                                        if (cpm == ARRAY_PARAM_MUT_PTR) has_write = true;
                                        continue; /* not an escape */
                                    }
                                }
                                disqualified = true;
                            }
                            break;
                        case IRON_IR_RETURN:
                            if (!instr->ret.is_void &&
                                hmgeti(aliases, instr->ret.value) >= 0)
                                disqualified = true;
                            break;
                        case IRON_IR_SET_FIELD:
                            if (hmgeti(aliases, instr->field.value) >= 0)
                                disqualified = true;
                            break;
                        case IRON_IR_CONSTRUCT:
                            for (int fj = 0; fj < instr->construct.field_count; fj++) {
                                if (hmgeti(aliases, instr->construct.field_vals[fj]) >= 0)
                                    disqualified = true;
                            }
                            break;
                        case IRON_IR_MAKE_CLOSURE:
                            for (int ci = 0; ci < instr->make_closure.capture_count; ci++) {
                                if (hmgeti(aliases, instr->make_closure.captures[ci]) >= 0)
                                    disqualified = true;
                            }
                            break;
                        case IRON_IR_SLICE:
                            if (hmgeti(aliases, instr->slice.array) >= 0)
                                disqualified = true;
                            break;
                        default:
                            break;
                        }
                    }
                }

                hmfree(aliases);

                if (!disqualified) {
                    ArrayParamMode mode = has_write
                        ? ARRAY_PARAM_MUT_PTR : ARRAY_PARAM_CONST_PTR;
                    const char *key = make_param_mode_key(fn->name, pi, arena);
                    shput(info->array_param_modes, key, (int)mode);
                    changed = true;
                }
            }
        }

        if (!changed) break;
    }
}

/* ── Stack-array optimization pre-pass (ARR-01) ─────────────────────────── */

/* Mark ARRAY_LIT instructions with known element counts <= 256 as
 * stack-array eligible.  This runs after phi elimination so the IR is stable. */
static void optimize_array_repr(IronIR_Module *module, IronIR_OptimizeInfo *info) {
    for (int fi = 0; fi < module->func_count; fi++) {
        IronIR_Func *fn = module->funcs[fi];
        if (fn->is_extern || fn->block_count == 0) continue;

        /* Pass 1: Mark array literals as stack-eligible */
        for (int bi = 0; bi < fn->block_count; bi++) {
            IronIR_Block *block = fn->blocks[bi];
            for (int ii = 0; ii < block->instr_count; ii++) {
                IronIR_Instr *instr = block->instrs[ii];
                if (instr->kind == IRON_IR_ARRAY_LIT) {
                    if (instr->array_lit.element_count > 0 &&
                        instr->array_lit.element_count <= 256) {
                        instr->array_lit.use_stack_repr = true;
                    }
                }
            }
        }

        /* Pass 2: Check if any stack array escapes the function via RETURN.
         * If an array value (directly or through alloca/load chain) reaches a
         * RETURN instruction, revoke its stack eligibility since the stack
         * memory would be invalid after the function returns.
         *
         * We build a set of stack-array ValueIds and alloca ValueIds that
         * hold stack arrays, then check if any RETURN references them. */
        /* For simplicity, we track: STORE(alloca, stack_array) -> alloca is "tainted".
         * LOAD(alloca) -> loaded value is "tainted".
         * RETURN(tainted_value) -> revoke the original array_lit. */
        struct { IronIR_ValueId key; IronIR_ValueId value; } *sa_map = NULL;
        /* Mark all stack array lits and ALL fill() calls (constant or dynamic count) */
        for (int bi = 0; bi < fn->block_count; bi++) {
            IronIR_Block *block = fn->blocks[bi];
            for (int ii = 0; ii < block->instr_count; ii++) {
                IronIR_Instr *instr = block->instrs[ii];
                if (instr->kind == IRON_IR_ARRAY_LIT && instr->array_lit.use_stack_repr) {
                    hmput(sa_map, instr->id, instr->id);
                }
                /* __builtin_fill — all calls (constant or dynamic count) */
                if (instr->kind == IRON_IR_CALL && instr->call.arg_count == 2 &&
                    instr->type && instr->type->kind == IRON_TYPE_ARRAY) {
                    IronIR_ValueId fptr = instr->call.func_ptr;
                    if (fptr != IRON_IR_VALUE_INVALID &&
                        fptr < (IronIR_ValueId)arrlen(fn->value_table) &&
                        fn->value_table[fptr] != NULL &&
                        fn->value_table[fptr]->kind == IRON_IR_FUNC_REF &&
                        strcmp(fn->value_table[fptr]->func_ref.func_name,
                               "__builtin_fill") == 0) {
                        hmput(sa_map, instr->id, instr->id);
                    }
                }
            }
        }
        /* Propagate through store/load */
        for (int bi = 0; bi < fn->block_count; bi++) {
            IronIR_Block *block = fn->blocks[bi];
            for (int ii = 0; ii < block->instr_count; ii++) {
                IronIR_Instr *instr = block->instrs[ii];
                if (instr->kind == IRON_IR_STORE) {
                    ptrdiff_t vi = hmgeti(sa_map, instr->store.value);
                    if (vi >= 0) {
                        hmput(sa_map, instr->store.ptr, sa_map[vi].value);
                    } else {
                        /* If the alloca already holds a stack array but is now
                         * receiving a non-stack value (e.g. a function call
                         * return), revoke the original fill/array_lit to avoid
                         * type mismatch (int64_t* vs Iron_List_T). */
                        ptrdiff_t pi = hmgeti(sa_map, instr->store.ptr);
                        if (pi >= 0) {
                            IronIR_ValueId orig = sa_map[pi].value;
                            if (orig < (IronIR_ValueId)arrlen(fn->value_table) &&
                                fn->value_table[orig]) {
                                if (fn->value_table[orig]->kind == IRON_IR_ARRAY_LIT)
                                    fn->value_table[orig]->array_lit.use_stack_repr = false;
                                else
                                    hmput(info->revoked_fill_ids, orig, true);
                            }
                            hmdel(sa_map, instr->store.ptr);
                        }
                    }
                } else if (instr->kind == IRON_IR_LOAD) {
                    ptrdiff_t vi = hmgeti(sa_map, instr->load.ptr);
                    if (vi >= 0) hmput(sa_map, instr->id, sa_map[vi].value);
                }
            }
        }
        /* Check returns and function call arguments for escapes */
        for (int bi = 0; bi < fn->block_count; bi++) {
            IronIR_Block *block = fn->blocks[bi];
            for (int ii = 0; ii < block->instr_count; ii++) {
                IronIR_Instr *instr = block->instrs[ii];
                if (instr->kind == IRON_IR_RETURN && !instr->ret.is_void) {
                    ptrdiff_t vi = hmgeti(sa_map, instr->ret.value);
                    if (vi >= 0) {
                        IronIR_ValueId orig = sa_map[vi].value;
                        if (orig < (IronIR_ValueId)arrlen(fn->value_table) &&
                            fn->value_table[orig]) {
                            if (fn->value_table[orig]->kind == IRON_IR_ARRAY_LIT)
                                fn->value_table[orig]->array_lit.use_stack_repr = false;
                            else
                                hmput(info->revoked_fill_ids, orig, true);
                        }
                    }
                }
                /* Check if stack array is passed as argument to a function call.
                 * If the callee param uses pointer mode (PARAM-01/02), the stack
                 * array can be passed directly. Otherwise revoke stack repr. */
                if (instr->kind == IRON_IR_CALL) {
                    /* Resolve callee IR name for pointer-mode check */
                    const char *call_ir_name = NULL;
                    if (instr->call.func_decl && !instr->call.func_decl->is_extern) {
                        call_ir_name = instr->call.func_decl->name;
                    } else if (!instr->call.func_decl) {
                        IronIR_ValueId fptr = instr->call.func_ptr;
                        if (fptr != IRON_IR_VALUE_INVALID &&
                            fptr < (IronIR_ValueId)arrlen(fn->value_table) &&
                            fn->value_table[fptr] != NULL &&
                            fn->value_table[fptr]->kind == IRON_IR_FUNC_REF) {
                            const char *rn = fn->value_table[fptr]->func_ref.func_name;
                            IronIR_Func *cf = find_ir_func(module, rn);
                            if (cf && !cf->is_extern) call_ir_name = rn;
                        }
                    }
                    for (int ai = 0; ai < instr->call.arg_count; ai++) {
                        ptrdiff_t vi = hmgeti(sa_map, instr->call.args[ai]);
                        if (vi >= 0) {
                            /* Check if callee accepts pointer mode for this param */
                            /* Note: arena not available here; use module->arena if needed.
                             * For find_ir_func lookup we only need the module. */
                            ArrayParamMode cpmode = ARRAY_PARAM_LIST;
                            if (call_ir_name && module->arena)
                                cpmode = iron_ir_get_array_param_mode(info, call_ir_name, ai, module->arena);
                            if (cpmode == ARRAY_PARAM_CONST_PTR ||
                                cpmode == ARRAY_PARAM_MUT_PTR)
                                continue; /* callee accepts pointer+len */
                            IronIR_ValueId orig = sa_map[vi].value;
                            if (orig < (IronIR_ValueId)arrlen(fn->value_table) &&
                                fn->value_table[orig]) {
                                if (fn->value_table[orig]->kind == IRON_IR_ARRAY_LIT)
                                    fn->value_table[orig]->array_lit.use_stack_repr = false;
                                else
                                    hmput(info->revoked_fill_ids, orig, true);
                            }
                        }
                    }
                }
                /* Check if stack array is used in SET_FIELD (stored into object) */
                if (instr->kind == IRON_IR_SET_FIELD) {
                    ptrdiff_t vi = hmgeti(sa_map, instr->field.value);
                    if (vi >= 0) {
                        IronIR_ValueId orig = sa_map[vi].value;
                        if (orig < (IronIR_ValueId)arrlen(fn->value_table) &&
                            fn->value_table[orig]) {
                            if (fn->value_table[orig]->kind == IRON_IR_ARRAY_LIT)
                                fn->value_table[orig]->array_lit.use_stack_repr = false;
                            else
                                hmput(info->revoked_fill_ids, orig, true);
                        }
                    }
                }
                /* Check if stack array is used as field in CONSTRUCT (stored into struct) */
                if (instr->kind == IRON_IR_CONSTRUCT) {
                    for (int fi2 = 0; fi2 < instr->construct.field_count; fi2++) {
                        ptrdiff_t vi = hmgeti(sa_map, instr->construct.field_vals[fi2]);
                        if (vi >= 0) {
                            IronIR_ValueId orig = sa_map[vi].value;
                            if (orig < (IronIR_ValueId)arrlen(fn->value_table) &&
                                fn->value_table[orig]) {
                                if (fn->value_table[orig]->kind == IRON_IR_ARRAY_LIT)
                                    fn->value_table[orig]->array_lit.use_stack_repr = false;
                                else
                                    hmput(info->revoked_fill_ids, orig, true);
                            }
                        }
                    }
                }
                /* Check if stack array is used in MAKE_CLOSURE captures */
                if (instr->kind == IRON_IR_MAKE_CLOSURE) {
                    for (int ci = 0; ci < instr->make_closure.capture_count; ci++) {
                        ptrdiff_t vi = hmgeti(sa_map, instr->make_closure.captures[ci]);
                        if (vi >= 0) {
                            IronIR_ValueId orig = sa_map[vi].value;
                            if (orig < (IronIR_ValueId)arrlen(fn->value_table) &&
                                fn->value_table[orig]) {
                                fn->value_table[orig]->array_lit.use_stack_repr = false;
                            }
                        }
                    }
                }
                /* Check if stack array is used in INTERP_STRING */
                if (instr->kind == IRON_IR_INTERP_STRING) {
                    for (int pi = 0; pi < instr->interp_string.part_count; pi++) {
                        ptrdiff_t vi = hmgeti(sa_map, instr->interp_string.parts[pi]);
                        if (vi >= 0) {
                            IronIR_ValueId orig = sa_map[vi].value;
                            if (orig < (IronIR_ValueId)arrlen(fn->value_table) &&
                                fn->value_table[orig]) {
                                fn->value_table[orig]->array_lit.use_stack_repr = false;
                            }
                        }
                    }
                }
            }
        }
        hmfree(sa_map);
    }
}

/* ── New optimization passes (Plan 02) ───────────────────────────────────── */

#ifndef MAX_OPERANDS
#define MAX_OPERANDS 64
#endif

/* Named type for the value-replacement hashmap (used by copy propagation). */
typedef struct { IronIR_ValueId key; IronIR_ValueId value; } ValueReplEntry;

/* Named type for the per-alloca store-count info hashmap. */
typedef struct { int count; IronIR_ValueId val; } StoreInfoVal;
typedef struct { IronIR_ValueId key; StoreInfoVal value; } StoreInfoEntry;

/* Apply replacement map to all ValueId operands of instr.
 * Returns true if any operand was changed. */
static bool apply_replacements(IronIR_Instr *instr, ValueReplEntry *repl_map) {
    bool changed = false;

#define REPL(field) do { \
    if ((field) != IRON_IR_VALUE_INVALID) { \
        ptrdiff_t _idx = hmgeti(repl_map, (field)); \
        if (_idx >= 0) { (field) = repl_map[_idx].value; changed = true; } \
    } \
} while (0)

    switch (instr->kind) {
    case IRON_IR_CONST_INT: case IRON_IR_CONST_FLOAT:
    case IRON_IR_CONST_BOOL: case IRON_IR_CONST_STRING:
    case IRON_IR_CONST_NULL: case IRON_IR_FUNC_REF:
        break; /* no operands */

    case IRON_IR_ADD: case IRON_IR_SUB: case IRON_IR_MUL:
    case IRON_IR_DIV: case IRON_IR_MOD:
    case IRON_IR_EQ: case IRON_IR_NEQ: case IRON_IR_LT:
    case IRON_IR_LTE: case IRON_IR_GT: case IRON_IR_GTE:
    case IRON_IR_AND: case IRON_IR_OR:
        REPL(instr->binop.left);
        REPL(instr->binop.right);
        break;

    case IRON_IR_NEG: case IRON_IR_NOT:
        REPL(instr->unop.operand);
        break;

    case IRON_IR_ALLOCA:
        break; /* no value operands */

    case IRON_IR_LOAD:
        REPL(instr->load.ptr);
        break;

    case IRON_IR_STORE:
        REPL(instr->store.ptr);
        REPL(instr->store.value);
        break;

    case IRON_IR_GET_FIELD:
        REPL(instr->field.object);
        break;

    case IRON_IR_SET_FIELD:
        REPL(instr->field.object);
        REPL(instr->field.value);
        break;

    case IRON_IR_GET_INDEX:
        REPL(instr->index.array);
        REPL(instr->index.index);
        break;

    case IRON_IR_SET_INDEX:
        REPL(instr->index.array);
        REPL(instr->index.index);
        REPL(instr->index.value);
        break;

    case IRON_IR_CALL:
        if (instr->call.func_decl == NULL) {
            REPL(instr->call.func_ptr);
        }
        for (int i = 0; i < instr->call.arg_count; i++)
            REPL(instr->call.args[i]);
        break;

    case IRON_IR_CAST:
        REPL(instr->cast.value);
        break;

    case IRON_IR_HEAP_ALLOC:
        REPL(instr->heap_alloc.inner_val);
        break;

    case IRON_IR_RC_ALLOC:
        REPL(instr->rc_alloc.inner_val);
        break;

    case IRON_IR_FREE:
        REPL(instr->free_instr.value);
        break;

    case IRON_IR_CONSTRUCT:
        for (int i = 0; i < instr->construct.field_count; i++)
            REPL(instr->construct.field_vals[i]);
        break;

    case IRON_IR_ARRAY_LIT:
        for (int i = 0; i < instr->array_lit.element_count; i++)
            REPL(instr->array_lit.elements[i]);
        break;

    case IRON_IR_SLICE:
        REPL(instr->slice.array);
        REPL(instr->slice.start);
        REPL(instr->slice.end);
        break;

    case IRON_IR_IS_NULL: case IRON_IR_IS_NOT_NULL:
        REPL(instr->null_check.value);
        break;

    case IRON_IR_INTERP_STRING:
        for (int i = 0; i < instr->interp_string.part_count; i++)
            REPL(instr->interp_string.parts[i]);
        break;

    case IRON_IR_MAKE_CLOSURE:
        for (int i = 0; i < instr->make_closure.capture_count; i++)
            REPL(instr->make_closure.captures[i]);
        break;

    case IRON_IR_SPAWN:
        REPL(instr->spawn.pool_val);
        break;

    case IRON_IR_PARALLEL_FOR:
        REPL(instr->parallel_for.range_val);
        REPL(instr->parallel_for.pool_val);
        for (int i = 0; i < instr->parallel_for.capture_count; i++)
            REPL(instr->parallel_for.captures[i]);
        break;

    case IRON_IR_AWAIT:
        REPL(instr->await.handle);
        break;

    case IRON_IR_BRANCH:
        REPL(instr->branch.cond);
        break;

    case IRON_IR_SWITCH:
        REPL(instr->sw.subject);
        break;

    case IRON_IR_RETURN:
        if (!instr->ret.is_void)
            REPL(instr->ret.value);
        break;

    case IRON_IR_JUMP:
        break; /* no value operands */

    case IRON_IR_PHI:
        for (int i = 0; i < instr->phi.count; i++)
            REPL(instr->phi.values[i]);
        break;

    case IRON_IR_POISON:
        break; /* no operands */

    default:
        break;
    }

#undef REPL
    return changed;
}

/* Collect all ValueId operands of instr into out[].
 * Mirrors verify.c's collect_operands exactly. */
static void opt_collect_operands(const IronIR_Instr *instr,
                                  IronIR_ValueId *out, int *count) {
    *count = 0;
#define PUSH(v) do { if ((v) != IRON_IR_VALUE_INVALID && *count < MAX_OPERANDS) out[(*count)++] = (v); } while(0)

    switch (instr->kind) {
    case IRON_IR_CONST_INT:
    case IRON_IR_CONST_FLOAT:
    case IRON_IR_CONST_BOOL:
    case IRON_IR_CONST_STRING:
    case IRON_IR_CONST_NULL:
    case IRON_IR_FUNC_REF:
        break;

    case IRON_IR_ADD:
    case IRON_IR_SUB:
    case IRON_IR_MUL:
    case IRON_IR_DIV:
    case IRON_IR_MOD:
    case IRON_IR_EQ:
    case IRON_IR_NEQ:
    case IRON_IR_LT:
    case IRON_IR_LTE:
    case IRON_IR_GT:
    case IRON_IR_GTE:
    case IRON_IR_AND:
    case IRON_IR_OR:
        PUSH(instr->binop.left);
        PUSH(instr->binop.right);
        break;

    case IRON_IR_NEG:
    case IRON_IR_NOT:
        PUSH(instr->unop.operand);
        break;

    case IRON_IR_ALLOCA:
        break;

    case IRON_IR_LOAD:
        PUSH(instr->load.ptr);
        break;

    case IRON_IR_STORE:
        PUSH(instr->store.ptr);
        PUSH(instr->store.value);
        break;

    case IRON_IR_GET_FIELD:
        PUSH(instr->field.object);
        break;

    case IRON_IR_SET_FIELD:
        PUSH(instr->field.object);
        PUSH(instr->field.value);
        break;

    case IRON_IR_GET_INDEX:
        PUSH(instr->index.array);
        PUSH(instr->index.index);
        break;

    case IRON_IR_SET_INDEX:
        PUSH(instr->index.array);
        PUSH(instr->index.index);
        PUSH(instr->index.value);
        break;

    case IRON_IR_CALL:
        if (instr->call.func_decl == NULL) {
            PUSH(instr->call.func_ptr);
        }
        for (int i = 0; i < instr->call.arg_count; i++) {
            PUSH(instr->call.args[i]);
        }
        break;

    case IRON_IR_JUMP:
        break;

    case IRON_IR_BRANCH:
        PUSH(instr->branch.cond);
        break;

    case IRON_IR_SWITCH:
        PUSH(instr->sw.subject);
        break;

    case IRON_IR_RETURN:
        if (!instr->ret.is_void) {
            PUSH(instr->ret.value);
        }
        break;

    case IRON_IR_CAST:
        PUSH(instr->cast.value);
        break;

    case IRON_IR_HEAP_ALLOC:
        PUSH(instr->heap_alloc.inner_val);
        break;

    case IRON_IR_RC_ALLOC:
        PUSH(instr->rc_alloc.inner_val);
        break;

    case IRON_IR_FREE:
        PUSH(instr->free_instr.value);
        break;

    case IRON_IR_CONSTRUCT:
        for (int i = 0; i < instr->construct.field_count; i++) {
            PUSH(instr->construct.field_vals[i]);
        }
        break;

    case IRON_IR_ARRAY_LIT:
        for (int i = 0; i < instr->array_lit.element_count; i++) {
            PUSH(instr->array_lit.elements[i]);
        }
        break;

    case IRON_IR_SLICE:
        PUSH(instr->slice.array);
        PUSH(instr->slice.start);
        PUSH(instr->slice.end);
        break;

    case IRON_IR_IS_NULL:
    case IRON_IR_IS_NOT_NULL:
        PUSH(instr->null_check.value);
        break;

    case IRON_IR_INTERP_STRING:
        for (int i = 0; i < instr->interp_string.part_count; i++) {
            PUSH(instr->interp_string.parts[i]);
        }
        break;

    case IRON_IR_MAKE_CLOSURE:
        for (int i = 0; i < instr->make_closure.capture_count; i++) {
            PUSH(instr->make_closure.captures[i]);
        }
        break;

    case IRON_IR_SPAWN:
        PUSH(instr->spawn.pool_val);
        break;

    case IRON_IR_PARALLEL_FOR:
        PUSH(instr->parallel_for.range_val);
        PUSH(instr->parallel_for.pool_val);
        for (int i = 0; i < instr->parallel_for.capture_count; i++) {
            PUSH(instr->parallel_for.captures[i]);
        }
        break;

    case IRON_IR_AWAIT:
        PUSH(instr->await.handle);
        break;

    case IRON_IR_PHI:
        for (int i = 0; i < instr->phi.count; i++) {
            PUSH(instr->phi.values[i]);
        }
        break;

    case IRON_IR_POISON:
        break;

    default:
        break;
    }

#undef PUSH
}

/* Copy Propagation: eliminate LOAD of single-store allocas.
 * For each alloca stored exactly once, replace all LOADs of that alloca
 * with the stored value, then rewrite all operands referencing the LOAD result. */
static bool run_copy_propagation(IronIR_Module *module) {
    bool changed = false;
    for (int fi = 0; fi < module->func_count; fi++) {
        IronIR_Func *fn = module->funcs[fi];
        if (fn->is_extern || fn->block_count == 0) continue;

        /* Step 1: Count stores per alloca.
         * Key: alloca ValueId. Value: { store_count, stored_value } */
        StoreInfoEntry *store_info = NULL;

        for (int bi = 0; bi < fn->block_count; bi++) {
            IronIR_Block *blk = fn->blocks[bi];
            for (int ii = 0; ii < blk->instr_count; ii++) {
                IronIR_Instr *in = blk->instrs[ii];
                if (in->kind == IRON_IR_STORE) {
                    IronIR_ValueId ptr = in->store.ptr;
                    /* Only track stores to ALLOCA values */
                    if (ptr != IRON_IR_VALUE_INVALID &&
                        (ptrdiff_t)ptr < arrlen(fn->value_table) &&
                        fn->value_table[ptr] != NULL &&
                        fn->value_table[ptr]->kind == IRON_IR_ALLOCA) {
                        ptrdiff_t idx = hmgeti(store_info, ptr);
                        if (idx < 0) {
                            StoreInfoVal sv;
                            sv.count = 1;
                            sv.val   = in->store.value;
                            hmput(store_info, ptr, sv);
                        } else {
                            store_info[idx].value.count++;
                        }
                    }
                }
            }
        }

        /* Step 2: Build replacement map for LOADs of single-store allocas.
         * load_result_id -> stored_value */
        ValueReplEntry *repl_map = NULL;

        for (int bi = 0; bi < fn->block_count; bi++) {
            IronIR_Block *blk = fn->blocks[bi];
            for (int ii = 0; ii < blk->instr_count; ii++) {
                IronIR_Instr *in = blk->instrs[ii];
                if (in->kind == IRON_IR_LOAD && in->id != IRON_IR_VALUE_INVALID) {
                    IronIR_ValueId ptr = in->load.ptr;
                    ptrdiff_t idx = hmgeti(store_info, ptr);
                    if (idx >= 0 && store_info[idx].value.count == 1) {
                        IronIR_ValueId replacement = store_info[idx].value.val;
                        if (replacement != IRON_IR_VALUE_INVALID) {
                            hmput(repl_map, in->id, replacement);
                        }
                    }
                }
            }
        }

        /* Step 3: Apply replacements to all operands in all instructions */
        if (hmlen(repl_map) > 0) {
            for (int bi = 0; bi < fn->block_count; bi++) {
                IronIR_Block *blk = fn->blocks[bi];
                for (int ii = 0; ii < blk->instr_count; ii++) {
                    if (apply_replacements(blk->instrs[ii], repl_map)) {
                        changed = true;
                    }
                }
            }
        }

        hmfree(store_info);
        hmfree(repl_map);
    }
    return changed;
}

static bool is_arithmetic_binop(IronIR_InstrKind kind) {
    return kind == IRON_IR_ADD || kind == IRON_IR_SUB ||
           kind == IRON_IR_MUL || kind == IRON_IR_DIV ||
           kind == IRON_IR_MOD;
}

static bool is_comparison_binop(IronIR_InstrKind kind) {
    return kind == IRON_IR_EQ  || kind == IRON_IR_NEQ ||
           kind == IRON_IR_LT  || kind == IRON_IR_LTE ||
           kind == IRON_IR_GT  || kind == IRON_IR_GTE;
}

/* Constant Folding: evaluate CONST_INT op CONST_INT at compile time.
 * Also folds integer comparisons to CONST_BOOL. */
static bool run_constant_folding(IronIR_Module *module) {
    bool changed = false;
    for (int fi = 0; fi < module->func_count; fi++) {
        IronIR_Func *fn = module->funcs[fi];
        if (fn->is_extern || fn->block_count == 0) continue;

        for (int bi = 0; bi < fn->block_count; bi++) {
            IronIR_Block *blk = fn->blocks[bi];
            for (int ii = 0; ii < blk->instr_count; ii++) {
                IronIR_Instr *in = blk->instrs[ii];

                if (is_arithmetic_binop(in->kind)) {
                    IronIR_Instr *left_def  = NULL;
                    IronIR_Instr *right_def = NULL;
                    if (in->binop.left != IRON_IR_VALUE_INVALID &&
                        (ptrdiff_t)in->binop.left < arrlen(fn->value_table))
                        left_def = fn->value_table[in->binop.left];
                    if (in->binop.right != IRON_IR_VALUE_INVALID &&
                        (ptrdiff_t)in->binop.right < arrlen(fn->value_table))
                        right_def = fn->value_table[in->binop.right];

                    if (left_def && right_def &&
                        left_def->kind == IRON_IR_CONST_INT &&
                        right_def->kind == IRON_IR_CONST_INT) {
                        int64_t L = left_def->const_int.value;
                        int64_t R = right_def->const_int.value;
                        int64_t result = 0;
                        bool can_fold = true;
                        switch (in->kind) {
                            case IRON_IR_ADD: result = L + R; break;
                            case IRON_IR_SUB: result = L - R; break;
                            case IRON_IR_MUL: result = L * R; break;
                            case IRON_IR_DIV:
                                if (R == 0) { can_fold = false; break; }
                                result = L / R; break;
                            case IRON_IR_MOD:
                                if (R == 0) { can_fold = false; break; }
                                result = L % R; break;
                            default: can_fold = false; break;
                        }
                        if (can_fold) {
                            in->kind = IRON_IR_CONST_INT;
                            in->const_int.value = result;
                            changed = true;
                        }
                    }
                }

                if (is_comparison_binop(in->kind)) {
                    IronIR_Instr *left_def  = NULL;
                    IronIR_Instr *right_def = NULL;
                    if (in->binop.left != IRON_IR_VALUE_INVALID &&
                        (ptrdiff_t)in->binop.left < arrlen(fn->value_table))
                        left_def = fn->value_table[in->binop.left];
                    if (in->binop.right != IRON_IR_VALUE_INVALID &&
                        (ptrdiff_t)in->binop.right < arrlen(fn->value_table))
                        right_def = fn->value_table[in->binop.right];

                    if (left_def && right_def &&
                        left_def->kind == IRON_IR_CONST_INT &&
                        right_def->kind == IRON_IR_CONST_INT) {
                        int64_t L = left_def->const_int.value;
                        int64_t R = right_def->const_int.value;
                        bool result = false;
                        switch (in->kind) {
                            case IRON_IR_EQ:  result = (L == R); break;
                            case IRON_IR_NEQ: result = (L != R); break;
                            case IRON_IR_LT:  result = (L < R);  break;
                            case IRON_IR_LTE: result = (L <= R); break;
                            case IRON_IR_GT:  result = (L > R);  break;
                            case IRON_IR_GTE: result = (L >= R); break;
                            default: break;
                        }
                        in->kind = IRON_IR_CONST_BOOL;
                        in->const_bool.value = result;
                        changed = true;
                    }
                }
            }
        }
    }
    return changed;
}

/* DCE: remove pure instructions whose results are never referenced.
 * Algorithm: seed live set with all side-effecting instructions,
 * propagate liveness transitively through operands, remove non-live pure instrs. */
static bool run_dce(IronIR_Module *module) {
    bool changed = false;
    for (int fi = 0; fi < module->func_count; fi++) {
        IronIR_Func *fn = module->funcs[fi];
        if (fn->is_extern || fn->block_count == 0) continue;

        /* Step 1: Seed live set with side-effecting instructions */
        struct { IronIR_ValueId key; bool value; } *live = NULL;
        IronIR_ValueId *worklist = NULL; /* stb_ds dynamic array */

        for (int bi = 0; bi < fn->block_count; bi++) {
            IronIR_Block *blk = fn->blocks[bi];
            for (int ii = 0; ii < blk->instr_count; ii++) {
                IronIR_Instr *in = blk->instrs[ii];
                /* Live if: side-effecting, or has no result (STORE, terminators) */
                if (!iron_ir_instr_is_pure(in->kind) ||
                    in->id == IRON_IR_VALUE_INVALID) {
                    if (in->id != IRON_IR_VALUE_INVALID) {
                        hmput(live, in->id, true);
                    }
                    /* Mark this instruction's operands as live seeds */
                    IronIR_ValueId ops[MAX_OPERANDS];
                    int op_count = 0;
                    opt_collect_operands(in, ops, &op_count);
                    for (int oi = 0; oi < op_count; oi++) {
                        if (hmgeti(live, ops[oi]) < 0) {
                            hmput(live, ops[oi], true);
                            arrput(worklist, ops[oi]);
                        }
                    }
                }
            }
        }

        /* Step 2: Worklist propagation — mark operands of live instrs as live */
        for (int wi = 0; wi < (int)arrlen(worklist); wi++) {
            IronIR_ValueId vid = worklist[wi];
            if (vid == IRON_IR_VALUE_INVALID ||
                (ptrdiff_t)vid >= arrlen(fn->value_table) ||
                fn->value_table[vid] == NULL) continue;

            IronIR_Instr *prod = fn->value_table[vid];
            IronIR_ValueId ops[MAX_OPERANDS];
            int op_count = 0;
            opt_collect_operands(prod, ops, &op_count);
            for (int oi = 0; oi < op_count; oi++) {
                if (hmgeti(live, ops[oi]) < 0) {
                    hmput(live, ops[oi], true);
                    arrput(worklist, ops[oi]);
                }
            }
        }

        /* Step 3: Remove non-live pure instructions */
        for (int bi = 0; bi < fn->block_count; bi++) {
            IronIR_Block *blk = fn->blocks[bi];
            int new_count = 0;
            for (int ii = 0; ii < blk->instr_count; ii++) {
                IronIR_Instr *in = blk->instrs[ii];
                bool is_live = false;

                /* Always keep side-effecting instructions and terminators */
                if (!iron_ir_instr_is_pure(in->kind) ||
                    in->id == IRON_IR_VALUE_INVALID) {
                    is_live = true;
                } else {
                    /* Pure instruction — keep only if its result is in live set */
                    is_live = (hmgeti(live, in->id) >= 0);
                }

                if (is_live) {
                    blk->instrs[new_count++] = in;
                } else {
                    /* Null out value_table entry for dead instruction */
                    if (in->id != IRON_IR_VALUE_INVALID &&
                        (ptrdiff_t)in->id < arrlen(fn->value_table)) {
                        fn->value_table[in->id] = NULL;
                    }
                    changed = true;
                }
            }
            blk->instr_count = new_count;
        }

        hmfree(live);
        arrfree(worklist);
    }
    return changed;
}

/* ── Expression inlining analysis (Phase 16) ────────────────────────────── */

/* Returns true if the instruction kind can be emitted as an inline sub-expression.
 * Must be pure AND not require multi-statement emission. */
static bool instr_is_inline_expressible(IronIR_InstrKind kind) {
    if (!iron_ir_instr_is_pure(kind)) return false;
    /* Multi-statement emission patterns cannot be inlined as sub-expressions */
    if (kind == IRON_IR_ARRAY_LIT) return false;
    if (kind == IRON_IR_INTERP_STRING) return false;
    /* ALLOCA produces an address, not a value expression */
    if (kind == IRON_IR_ALLOCA) return false;
    /* CONST_STRING emits iron_string_from_literal() which is fine to inline,
     * but string constants are already cheap as named vars — keep separate. */
    if (kind == IRON_IR_CONST_STRING) return false;
    /* MAKE_CLOSURE emits a multi-statement env struct alloc + capture assignment.
     * emit_expr_to_buf falls back to emit_val for it, so it must not be
     * marked inline-eligible (its result void* would become undeclared). */
    if (kind == IRON_IR_MAKE_CLOSURE) return false;
    return true;
}

/* Pure builtin function names that are safe to inline (treat as pure calls).
 * These are runtime helpers with no side effects. */
static bool is_pure_builtin(const char *name) {
    if (!name) return false;
    static const char *pure_builtins[] = {
        "abs", "min", "max", "len", "cap",
        "iron_abs", "iron_min", "iron_max",
        "iron_string_concat", "iron_string_eq", "iron_string_cstr",
        "iron_string_len", "iron_string_from_cstr", "iron_string_from_literal",
        NULL
    };
    for (int i = 0; pure_builtins[i]; i++) {
        if (strcmp(name, pure_builtins[i]) == 0) return true;
    }
    return false;
}

/* Phase 1 + 2 function purity analysis.
 * Populates info->func_purity with { func_name -> true } for provably pure functions.
 * Conservative: functions that cannot be proven pure are not added to the map. */
static void compute_func_purity(IronIR_Module *module, IronIR_OptimizeInfo *info) {
    if (!module || module->func_count == 0) return;

    /* Phase 1: mark functions with no CALLs and all-pure instructions */
    for (int fi = 0; fi < module->func_count; fi++) {
        IronIR_Func *fn = module->funcs[fi];
        if (fn->is_extern || fn->block_count == 0) continue;

        bool all_pure = true;
        bool has_call = false;
        for (int bi = 0; bi < fn->block_count && all_pure; bi++) {
            IronIR_Block *blk = fn->blocks[bi];
            for (int ii = 0; ii < blk->instr_count && all_pure; ii++) {
                IronIR_Instr *in = blk->instrs[ii];
                if (in->kind == IRON_IR_CALL) {
                    has_call = true;
                    /* Don't mark impure yet — will check callees in phase 2 */
                } else if (!iron_ir_instr_is_pure(in->kind) &&
                           in->kind != IRON_IR_RETURN &&
                           in->kind != IRON_IR_JUMP &&
                           in->kind != IRON_IR_ALLOCA &&
                           in->kind != IRON_IR_STORE) {
                    /* Side-effecting non-call instruction (not a structural one) */
                    all_pure = false;
                }
            }
        }
        if (all_pure && !has_call) {
            hmput(info->func_purity, (char*)fn->name, true);
        }
    }

    /* Phase 2: fixpoint — functions with only pure-callee calls */
    bool changed = true;
    while (changed) {
        changed = false;
        for (int fi = 0; fi < module->func_count; fi++) {
            IronIR_Func *fn = module->funcs[fi];
            if (fn->is_extern || fn->block_count == 0) continue;
            /* Already marked pure — skip */
            if (hmgeti(info->func_purity, (char*)fn->name) >= 0) continue;

            bool all_pure = true;
            for (int bi = 0; bi < fn->block_count && all_pure; bi++) {
                IronIR_Block *blk = fn->blocks[bi];
                for (int ii = 0; ii < blk->instr_count && all_pure; ii++) {
                    IronIR_Instr *in = blk->instrs[ii];
                    if (in->kind == IRON_IR_CALL) {
                        /* Check if callee is known pure */
                        const char *callee_name = NULL;
                        if (!in->call.func_decl) {
                            IronIR_ValueId fptr = in->call.func_ptr;
                            if (fptr != IRON_IR_VALUE_INVALID &&
                                fptr < (IronIR_ValueId)arrlen(fn->value_table) &&
                                fn->value_table[fptr] != NULL &&
                                fn->value_table[fptr]->kind == IRON_IR_FUNC_REF) {
                                callee_name = fn->value_table[fptr]->func_ref.func_name;
                            }
                        } else {
                            callee_name = in->call.func_decl->name;
                        }
                        if (!callee_name) { all_pure = false; break; }
                        bool callee_pure = is_pure_builtin(callee_name) ||
                                          hmgeti(info->func_purity, (char*)callee_name) >= 0;
                        if (!callee_pure) all_pure = false;
                    } else if (!iron_ir_instr_is_pure(in->kind) &&
                               in->kind != IRON_IR_RETURN &&
                               in->kind != IRON_IR_JUMP &&
                               in->kind != IRON_IR_ALLOCA &&
                               in->kind != IRON_IR_STORE) {
                        all_pure = false;
                    }
                }
            }
            if (all_pure) {
                hmput(info->func_purity, (char*)fn->name, true);
                changed = true;
            }
        }
    }
}

/* Compute use-count map for one function.
 * Counts how many times each ValueId is referenced as an operand (including terminators).
 * Populates info->use_counts. */
void iron_ir_compute_use_counts(IronIR_Func *fn, IronIR_OptimizeInfo *info) {
    IronIR_ValueId ops[MAX_OPERANDS];
    int op_count = 0;

    for (int bi = 0; bi < fn->block_count; bi++) {
        IronIR_Block *blk = fn->blocks[bi];
        for (int ii = 0; ii < blk->instr_count; ii++) {
            IronIR_Instr *in = blk->instrs[ii];
            opt_collect_operands(in, ops, &op_count);
            for (int oi = 0; oi < op_count; oi++) {
                IronIR_ValueId vid = ops[oi];
                ptrdiff_t idx = hmgeti(info->use_counts, vid);
                if (idx >= 0) {
                    info->use_counts[idx].value++;
                } else {
                    IronIR_UseCountEntry e; e.key = vid; e.value = 1;
                    hmput(info->use_counts, vid, 1);
                }
            }
        }
    }
}

/* Build value->block map for one function.
 * Maps each defined ValueId to the BlockId of the block that defines it.
 * Populates info->value_block. */
void iron_ir_compute_value_block(IronIR_Func *fn, IronIR_OptimizeInfo *info) {
    for (int bi = 0; bi < fn->block_count; bi++) {
        IronIR_Block *blk = fn->blocks[bi];
        for (int ii = 0; ii < blk->instr_count; ii++) {
            IronIR_Instr *in = blk->instrs[ii];
            if (in->id != IRON_IR_VALUE_INVALID) {
                hmput(info->value_block, in->id, blk->id);
            }
        }
    }
}

/* Build inline-eligible map for one function.
 * An instruction is inline-eligible if:
 *   - its use count is exactly 1
 *   - it is expressible as an inline sub-expression (instr_is_inline_expressible)
 *     OR it is a CALL to a provably-pure function
 *   - it is NOT a stack-array-tracked value
 * Requires use_counts, value_block, and func_purity maps to be populated.
 * Populates info->inline_eligible. */
/* Returns true if instruction kind mutates memory (STORE, SET_INDEX, SET_FIELD,
 * CALL, HEAP_ALLOC, RC_ALLOC, FREE).  Used to detect ordering hazards. */
static bool instr_mutates_memory(IronIR_InstrKind kind) {
    switch (kind) {
    case IRON_IR_STORE:
    case IRON_IR_SET_INDEX:
    case IRON_IR_SET_FIELD:
    case IRON_IR_CALL:
    case IRON_IR_HEAP_ALLOC:
    case IRON_IR_RC_ALLOC:
    case IRON_IR_FREE:
        return true;
    default:
        return false;
    }
}

void iron_ir_compute_inline_eligible(IronIR_Func *fn,
                                      IronIR_OptimizeInfo *info) {
    /* Build exclusion sets for values that must NOT be inlined:
     *   1. Values used by INTERP_STRING parts (referenced by name in format strings)
     *   2. Array-typed values (their companion _len var / .items/.count access needs a named decl)
     *   3. Values used cross-block (def-block != use-block causes undeclared identifiers
     *      because emit_expr_to_buf falls back to emit_val but the declaration was skipped)
     *   4. Values used by MAKE_CLOSURE captures, PARALLEL_FOR range_val/captures, SPAWN
     *      pool_val — emit_instr uses emit_val (template pattern) for these, not
     *      emit_expr_to_buf, so they must retain named variable declarations.
     *   5. Values that have a memory-mutating instruction between their definition and
     *      use site within the same block (ordering hazard: the inlined expression would
     *      read memory after it was modified, producing wrong results).
     */

    /* Map: ValueId -> BlockId of (first) use site */
    struct { IronIR_ValueId key; IronIR_BlockId value; } *use_site_block = NULL;
    /* Map: ValueId -> instr index of (first) use site within its block */
    struct { IronIR_ValueId key; int value; }            *use_site_pos  = NULL;
    /* Set of values excluded from inlining */
    struct { IronIR_ValueId key; bool value; } *excluded = NULL;

    /* Pass 1: collect exclusions and use-site blocks/positions */
    for (int bi = 0; bi < fn->block_count; bi++) {
        IronIR_Block *blk = fn->blocks[bi];
        for (int ii = 0; ii < blk->instr_count; ii++) {
            IronIR_Instr *in = blk->instrs[ii];

            /* INTERP_STRING parts: referenced by name in format strings */
            if (in->kind == IRON_IR_INTERP_STRING) {
                for (int pi = 0; pi < in->interp_string.part_count; pi++) {
                    hmput(excluded, in->interp_string.parts[pi], true);
                }
            }

            /* MAKE_CLOSURE captures: emit_instr uses emit_val for these */
            if (in->kind == IRON_IR_MAKE_CLOSURE) {
                for (int ci = 0; ci < in->make_closure.capture_count; ci++) {
                    hmput(excluded, in->make_closure.captures[ci], true);
                }
            }

            /* PARALLEL_FOR range_val and captures: emit_instr uses emit_val */
            if (in->kind == IRON_IR_PARALLEL_FOR) {
                if (in->parallel_for.range_val != IRON_IR_VALUE_INVALID)
                    hmput(excluded, in->parallel_for.range_val, true);
                for (int ci = 0; ci < in->parallel_for.capture_count; ci++) {
                    hmput(excluded, in->parallel_for.captures[ci], true);
                }
            }

            /* SPAWN pool_val: emit_instr uses emit_val */
            if (in->kind == IRON_IR_SPAWN) {
                if (in->spawn.pool_val != IRON_IR_VALUE_INVALID)
                    hmput(excluded, in->spawn.pool_val, true);
            }

            /* Record use-site block/position for all operands; mark cross-block uses */
            IronIR_ValueId ops[MAX_OPERANDS];
            int op_count = 0;
            opt_collect_operands(in, ops, &op_count);
            for (int oi = 0; oi < op_count; oi++) {
                IronIR_ValueId op = ops[oi];
                ptrdiff_t ub_idx = hmgeti(use_site_block, op);
                if (ub_idx < 0) {
                    /* First use: record block and position */
                    hmput(use_site_block, op, blk->id);
                    hmput(use_site_pos,   op, ii);
                } else {
                    /* Subsequent use: if in different block, exclude */
                    if ((IronIR_BlockId)use_site_block[ub_idx].value != blk->id) {
                        hmput(excluded, op, true);
                    }
                    /* Multiple uses in same block → not single-use, already caught
                     * by use_count check in Pass 2 */
                }
            }
        }
    }

    /* Pass 2: mark eligible instructions */
    for (int bi = 0; bi < fn->block_count; bi++) {
        IronIR_Block *blk = fn->blocks[bi];
        for (int ii = 0; ii < blk->instr_count; ii++) {
            IronIR_Instr *in = blk->instrs[ii];
            if (in->id == IRON_IR_VALUE_INVALID) continue;

            /* Must have exactly one use */
            ptrdiff_t uc_idx = hmgeti(info->use_counts, in->id);
            if (uc_idx < 0 || info->use_counts[uc_idx].value != 1) continue;

            /* Skip stack-array-tracked values */
            if (info->stack_array_ids &&
                hmgeti(info->stack_array_ids, in->id) >= 0) continue;

            /* Skip explicitly excluded values */
            if (hmgeti(excluded, in->id) >= 0) continue;

            /* Skip cross-block: use site must be same block as definition */
            {
                ptrdiff_t ub_idx = hmgeti(use_site_block, in->id);
                if (ub_idx >= 0 &&
                    (IronIR_BlockId)use_site_block[ub_idx].value != blk->id) {
                    continue;
                }
            }

            /* Skip array-typed values — companion _len and .items/.count access
             * requires a named variable declaration */
            if (in->type && in->type->kind == IRON_TYPE_ARRAY) continue;

            /* Skip ordering hazard: if any memory-mutating instruction exists
             * between this value's definition (ii) and its use site within the
             * same block, inlining would read memory after mutation. */
            {
                ptrdiff_t up_idx = hmgeti(use_site_pos, in->id);
                if (up_idx >= 0) {
                    int use_pos = use_site_pos[up_idx].value;
                    bool hazard = false;
                    for (int ki = ii + 1; ki < use_pos && !hazard; ki++) {
                        if (instr_mutates_memory(blk->instrs[ki]->kind)) {
                            hazard = true;
                        }
                    }
                    if (hazard) continue;
                }
            }

            bool eligible = false;
            if (instr_is_inline_expressible(in->kind)) {
                eligible = true;
            } else if (in->kind == IRON_IR_CALL) {
                /* Check if callee is pure */
                const char *callee_name = NULL;
                if (!in->call.func_decl) {
                    IronIR_ValueId fptr = in->call.func_ptr;
                    if (fptr != IRON_IR_VALUE_INVALID &&
                        fptr < (IronIR_ValueId)arrlen(fn->value_table) &&
                        fn->value_table[fptr] != NULL &&
                        fn->value_table[fptr]->kind == IRON_IR_FUNC_REF) {
                        callee_name = fn->value_table[fptr]->func_ref.func_name;
                    }
                } else {
                    callee_name = in->call.func_decl->name;
                }
                if (callee_name) {
                    eligible = is_pure_builtin(callee_name) ||
                               (info->func_purity &&
                                hmgeti(info->func_purity, (char*)callee_name) >= 0);
                }
            }

            if (eligible) {
                hmput(info->inline_eligible, in->id, true);
            }
        }
    }
    hmfree(use_site_block);
    hmfree(use_site_pos);
    hmfree(excluded);
}

/* ── Public API ───────────────────────────────────────────────────────────── */

bool iron_ir_instr_is_pure(IronIR_InstrKind kind) {
    switch (kind) {
    /* Pure — no side effects */
    case IRON_IR_CONST_INT: case IRON_IR_CONST_FLOAT:
    case IRON_IR_CONST_BOOL: case IRON_IR_CONST_STRING:
    case IRON_IR_CONST_NULL:
    case IRON_IR_ADD: case IRON_IR_SUB: case IRON_IR_MUL:
    case IRON_IR_DIV: case IRON_IR_MOD:
    case IRON_IR_EQ: case IRON_IR_NEQ: case IRON_IR_LT:
    case IRON_IR_LTE: case IRON_IR_GT: case IRON_IR_GTE:
    case IRON_IR_AND: case IRON_IR_OR:
    case IRON_IR_NEG: case IRON_IR_NOT:
    case IRON_IR_LOAD: case IRON_IR_CAST:
    case IRON_IR_GET_FIELD: case IRON_IR_GET_INDEX:
    case IRON_IR_CONSTRUCT: case IRON_IR_ARRAY_LIT:
    case IRON_IR_IS_NULL: case IRON_IR_IS_NOT_NULL:
    case IRON_IR_SLICE:
    case IRON_IR_MAKE_CLOSURE: case IRON_IR_FUNC_REF:
        return true;
    /* Side-effecting — everything else */
    default:
        return false;
    }
}

void iron_ir_compute_inline_info(IronIR_Module *module, IronIR_OptimizeInfo *info) {
    if (!module || !info) return;
    /* Compute module-wide function purity analysis (only once per module) */
    compute_func_purity(module, info);
}

bool iron_ir_optimize(IronIR_Module *module, IronIR_OptimizeInfo *info,
                      Iron_Arena *arena, bool dump_passes, bool skip_new_passes) {
    if (!module) return false;

    /* Initialize info maps to NULL (stb_ds convention) */
    memset(info, 0, sizeof(*info));

    /* Phase 0: Structural passes (always run, not skippable) */
    phi_eliminate(module);
    if (dump_passes) {
        char *ir_text = iron_ir_print(module, true);
        if (ir_text) { fprintf(stderr, "=== After phi-eliminate ===\n%s\n", ir_text); free(ir_text); }
    }

    analyze_array_param_modes(module, info, arena);
    if (dump_passes) {
        char *ir_text = iron_ir_print(module, true);
        if (ir_text) { fprintf(stderr, "=== After array-param-modes ===\n%s\n", ir_text); free(ir_text); }
    }

    optimize_array_repr(module, info);
    if (dump_passes) {
        char *ir_text = iron_ir_print(module, true);
        if (ir_text) { fprintf(stderr, "=== After array-repr ===\n%s\n", ir_text); free(ir_text); }
    }

    if (skip_new_passes) return false;

    /* Fixpoint loop: copy-prop -> const-fold -> DCE */
    bool any_changed = false;
    Iron_DiagList verify_diags;
    memset(&verify_diags, 0, sizeof(verify_diags));

    for (int iter = 0; iter < 32; iter++) {
        bool changed = false;

        changed |= run_copy_propagation(module);
        if (dump_passes) {
            char *ir_text = iron_ir_print(module, true);
            if (ir_text) { fprintf(stderr, "=== After copy-prop (iter %d) ===\n%s\n", iter, ir_text); free(ir_text); }
        }
        iron_ir_verify(module, &verify_diags, arena);

        changed |= run_constant_folding(module);
        if (dump_passes) {
            char *ir_text = iron_ir_print(module, true);
            if (ir_text) { fprintf(stderr, "=== After const-fold (iter %d) ===\n%s\n", iter, ir_text); free(ir_text); }
        }
        iron_ir_verify(module, &verify_diags, arena);

        changed |= run_dce(module);
        if (dump_passes) {
            char *ir_text = iron_ir_print(module, true);
            if (ir_text) { fprintf(stderr, "=== After dce (iter %d) ===\n%s\n", iter, ir_text); free(ir_text); }
        }
        iron_ir_verify(module, &verify_diags, arena);

        if (!changed) break;
        any_changed = true;
    }

    iron_diaglist_free(&verify_diags);

    /* Phase 16: compute module-wide function purity and set up inline info */
    if (!skip_new_passes) {
        iron_ir_compute_inline_info(module, info);
        if (dump_passes) {
            fprintf(stderr, "=== After inline-info: %td pure functions ===\n",
                    hmlen(info->func_purity));
        }
    }

    return any_changed;
}

void iron_ir_optimize_info_free(IronIR_OptimizeInfo *info) {
    if (!info) return;
    hmfree(info->stack_array_ids);
    hmfree(info->heap_array_ids);
    hmfree(info->escaped_heap_ids);
    shfree(info->array_param_modes);
    hmfree(info->revoked_fill_ids);
    /* Phase 16 maps */
    hmfree(info->use_counts);
    hmfree(info->inline_eligible);
    shfree(info->func_purity);
    hmfree(info->value_block);
}
