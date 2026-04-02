/* ir_optimize.c — IR optimization passes for the Iron compiler.
 *
 * Houses all IR transformation passes that run between lowering and C emission:
 *   Phase 0:  phi_eliminate           (phi -> alloca+store+load)
 *   Phase 0a: analyze_array_param_modes (PARAM-01/02 pointer-mode analysis)
 *   Phase 0b: optimize_array_repr     (ARR-01 stack vs heap array selection)
 *
 * Fixpoint loop (copy-prop -> const-fold -> DCE -> store-load-elim -> strength-reduction)
 *
 * Post-fixpoint:
 *   run_dead_alloca_elimination (PHI-01/PHI-02): removes allocas with no live loads
 *   iron_lir_compute_inline_info (Phase 16)
 */

#include "lir/lir_optimize.h"
#include "lir/print.h"
#include "lir/verify.h"
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
static IronLIR_Block *find_block(IronLIR_Func *fn, IronLIR_BlockId id) {
    for (int i = 0; i < fn->block_count; i++) {
        if (fn->blocks[i]->id == id) return fn->blocks[i];
    }
    return NULL;
}

/* Find index of the terminator instruction in a block */
static int find_terminator_idx(IronLIR_Block *block) {
    for (int i = 0; i < block->instr_count; i++) {
        if (iron_lir_is_terminator(block->instrs[i]->kind)) return i;
    }
    return block->instr_count;  /* no terminator found — append at end */
}

/* Insert a pre-built instruction into a block before the terminator */
static void insert_store_before_terminator_instr(IronLIR_Block *block,
                                                   IronLIR_Instr *store) {
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
static IronLIR_Instr *make_alloca_instr(IronLIR_Func *fn, Iron_Type *alloc_type,
                                        Iron_Span span) {
    IronLIR_Instr *instr = ARENA_ALLOC(fn->arena, IronLIR_Instr);
    memset(instr, 0, sizeof(*instr));
    instr->kind             = IRON_LIR_ALLOCA;
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
static IronLIR_Instr *make_store_instr(IronLIR_Func *fn, IronLIR_ValueId ptr,
                                       IronLIR_ValueId value, Iron_Span span) {
    IronLIR_Instr *instr = ARENA_ALLOC(fn->arena, IronLIR_Instr);
    memset(instr, 0, sizeof(*instr));
    instr->kind        = IRON_LIR_STORE;
    instr->span        = span;
    instr->id          = IRON_LIR_VALUE_INVALID;
    instr->store.ptr   = ptr;
    instr->store.value = value;
    return instr;
}

/* ── Phi elimination pre-pass ─────────────────────────────────────────────── */

/* Phi elimination: walk all functions, replace phi nodes with alloca+store+load */
static void phi_eliminate(IronLIR_Module *module) {
    for (int fi = 0; fi < module->func_count; fi++) {
        IronLIR_Func *fn = module->funcs[fi];
        if (fn->is_extern || fn->block_count == 0) continue;

        /* Collect all phi nodes first to avoid modifying while iterating */
        IronLIR_Instr **phis = NULL;  /* stb_ds array of phi instrs */

        for (int bi = 0; bi < fn->block_count; bi++) {
            IronLIR_Block *block = fn->blocks[bi];
            for (int ii = 0; ii < block->instr_count; ii++) {
                IronLIR_Instr *instr = block->instrs[ii];
                if (instr->kind == IRON_LIR_PHI) {
                    arrput(phis, instr);
                }
            }
        }

        /* Process each phi */
        for (int pi = 0; pi < (int)arrlen(phis); pi++) {
            IronLIR_Instr *phi = phis[pi];
            Iron_Span span = phi->span;

            /* 1. Create an alloca instruction (without appending to a block) */
            IronLIR_Instr *alloca_instr = make_alloca_instr(fn, phi->type, span);
            IronLIR_ValueId alloca_id = alloca_instr->id;

            /* Insert alloca at the top of the entry block */
            IronLIR_Block *entry = fn->blocks[0];
            arrput(entry->instrs, NULL);  /* grow by 1 */
            entry->instr_count++;
            for (int i = entry->instr_count - 1; i > 0; i--) {
                entry->instrs[i] = entry->instrs[i - 1];
            }
            entry->instrs[0] = alloca_instr;

            /* 2. In each predecessor block, insert store before terminator */
            for (int i = 0; i < phi->phi.count; i++) {
                IronLIR_BlockId pred_id = phi->phi.pred_blocks[i];
                IronLIR_ValueId val     = phi->phi.values[i];
                IronLIR_Block  *pred    = find_block(fn, pred_id);
                if (!pred) continue;

                IronLIR_Instr *store = make_store_instr(fn, alloca_id, val, span);
                insert_store_before_terminator_instr(pred, store);
            }

            /* 2b. Find the block containing this phi and ensure ALL its
             * predecessors have a store for the alloca.  Predecessors not
             * covered by the phi operands (e.g., elif branches with array
             * indexing that introduce intermediate ValueIds) get a default
             * store using the first phi value as a safe fallback. */
            {
                IronLIR_Block *phi_block = NULL;
                for (int bi = 0; bi < fn->block_count; bi++) {
                    IronLIR_Block *blk = fn->blocks[bi];
                    for (int ii = 0; ii < blk->instr_count; ii++) {
                        if (blk->instrs[ii] == phi) {
                            phi_block = blk;
                            break;
                        }
                    }
                    if (phi_block) break;
                }
                if (phi_block && phi->phi.count > 0) {
                    IronLIR_ValueId default_val = phi->phi.values[0];
                    for (int pi2 = 0; pi2 < (int)arrlen(phi_block->preds); pi2++) {
                        IronLIR_BlockId pred_id = phi_block->preds[pi2];
                        /* Check if this predecessor is already covered */
                        bool covered = false;
                        for (int k = 0; k < phi->phi.count; k++) {
                            if (phi->phi.pred_blocks[k] == pred_id) {
                                covered = true;
                                break;
                            }
                        }
                        if (!covered) {
                            IronLIR_Block *pred = find_block(fn, pred_id);
                            if (pred) {
                                IronLIR_Instr *store = make_store_instr(
                                    fn, alloca_id, default_val, span);
                                insert_store_before_terminator_instr(pred, store);
                            }
                        }
                    }
                }
            }

            /* 3. Replace phi with a LOAD from the alloca */
            phi->kind     = IRON_LIR_LOAD;
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
ArrayParamMode iron_lir_get_array_param_mode(IronLIR_OptimizeInfo *info,
                                             const char *func_name,
                                             int param_index,
                                             Iron_Arena *arena) {
    const char *key = make_param_mode_key(func_name, param_index, arena);
    ptrdiff_t idx = shgeti(info->array_param_modes, key);
    if (idx >= 0) return (ArrayParamMode)info->array_param_modes[idx].value;
    return ARRAY_PARAM_LIST;
}

/* Find an IronLIR_Func in the module by IR name. */
static IronLIR_Func *find_ir_func(IronLIR_Module *module, const char *ir_name) {
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
static void analyze_array_param_modes(IronLIR_Module *module,
                                       IronLIR_OptimizeInfo *info,
                                       Iron_Arena *arena) {
    /* Iterate until convergence: if a callee's array param already has
     * pointer mode, passing an array to it shouldn't disqualify the caller.
     * First pass resolves leaf functions; subsequent passes propagate. */
    for (int iteration = 0; iteration < 8; iteration++) {
        bool changed = false;

        for (int fi = 0; fi < module->func_count; fi++) {
            IronLIR_Func *fn = module->funcs[fi];
            if (fn->is_extern || fn->block_count == 0) continue;

            for (int pi = 0; pi < fn->param_count; pi++) {
                Iron_Type *pt = fn->params[pi].type;
                if (!pt || pt->kind != IRON_TYPE_ARRAY) continue;

                /* Skip if already resolved */
                ArrayParamMode existing = iron_lir_get_array_param_mode(info, fn->name, pi, arena);
                if (existing != ARRAY_PARAM_LIST) continue;

                /* HIR pipeline: params are IDs 1..param_count, allocas are
                 * param_count+1..2*param_count (all params first, then allocas). */
                IronLIR_ValueId param_val_id = (IronLIR_ValueId)(pi + 1);
                IronLIR_ValueId alloca_id    = (IronLIR_ValueId)(fn->param_count + pi + 1);

                /* Build alias set: param_val and alloca, plus LOADs from alloca */
                struct { IronLIR_ValueId key; bool value; } *aliases = NULL;
                hmput(aliases, param_val_id, true);
                hmput(aliases, alloca_id, true);

                for (int bi = 0; bi < fn->block_count; bi++) {
                    IronLIR_Block *block = fn->blocks[bi];
                    for (int ii = 0; ii < block->instr_count; ii++) {
                        IronLIR_Instr *instr = block->instrs[ii];
                        if (instr->kind == IRON_LIR_LOAD) {
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
                    IronLIR_Block *block = fn->blocks[bi];
                    for (int ii = 0; ii < block->instr_count && !disqualified; ii++) {
                        IronLIR_Instr *instr = block->instrs[ii];

                        switch (instr->kind) {
                        case IRON_LIR_GET_INDEX:
                            break;
                        case IRON_LIR_SET_INDEX:
                            if (hmgeti(aliases, instr->index.array) >= 0)
                                has_write = true;
                            break;
                        case IRON_LIR_GET_FIELD:
                            break;
                        case IRON_LIR_STORE:
                            if (hmgeti(aliases, instr->store.ptr) >= 0 &&
                                hmgeti(aliases, instr->store.value) < 0) {
                                disqualified = true;
                            }
                            if (hmgeti(aliases, instr->store.ptr) < 0 &&
                                hmgeti(aliases, instr->store.value) >= 0) {
                                disqualified = true;
                            }
                            break;
                        case IRON_LIR_CALL:
                            for (int ai = 0; ai < instr->call.arg_count; ai++) {
                                if (hmgeti(aliases, instr->call.args[ai]) < 0) continue;
                                /* Check if callee accepts pointer mode for this param */
                                const char *callee_name = NULL;
                                if (instr->call.func_decl && !instr->call.func_decl->is_extern)
                                    callee_name = instr->call.func_decl->name;
                                else if (!instr->call.func_decl) {
                                    IronLIR_ValueId fptr = instr->call.func_ptr;
                                    if (fptr != IRON_LIR_VALUE_INVALID &&
                                        fptr < (IronLIR_ValueId)arrlen(fn->value_table) &&
                                        fn->value_table[fptr] != NULL &&
                                        fn->value_table[fptr]->kind == IRON_LIR_FUNC_REF)
                                        callee_name = fn->value_table[fptr]->func_ref.func_name;
                                }
                                if (callee_name) {
                                    /* Recursive call: same function, same param index.
                                     * The param will get the same mode we're computing. */
                                    if (strcmp(callee_name, fn->name) == 0 && ai == pi) {
                                        has_write = true; /* conservative: assume mutable */
                                        continue;
                                    }
                                    ArrayParamMode cpm = iron_lir_get_array_param_mode(info, callee_name, ai, arena);
                                    if (cpm == ARRAY_PARAM_CONST_PTR || cpm == ARRAY_PARAM_MUT_PTR) {
                                        /* Callee uses pointer mode — propagate write flag */
                                        if (cpm == ARRAY_PARAM_MUT_PTR) has_write = true;
                                        continue; /* not an escape */
                                    }
                                }
                                disqualified = true;
                            }
                            break;
                        case IRON_LIR_RETURN:
                            if (!instr->ret.is_void &&
                                hmgeti(aliases, instr->ret.value) >= 0)
                                disqualified = true;
                            break;
                        case IRON_LIR_SET_FIELD:
                            if (hmgeti(aliases, instr->field.value) >= 0)
                                disqualified = true;
                            break;
                        case IRON_LIR_CONSTRUCT:
                            for (int fj = 0; fj < instr->construct.field_count; fj++) {
                                if (hmgeti(aliases, instr->construct.field_vals[fj]) >= 0)
                                    disqualified = true;
                            }
                            break;
                        case IRON_LIR_MAKE_CLOSURE:
                            for (int ci = 0; ci < instr->make_closure.capture_count; ci++) {
                                if (hmgeti(aliases, instr->make_closure.captures[ci]) >= 0)
                                    disqualified = true;
                            }
                            break;
                        case IRON_LIR_SLICE:
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
static void optimize_array_repr(IronLIR_Module *module, IronLIR_OptimizeInfo *info) {
    for (int fi = 0; fi < module->func_count; fi++) {
        IronLIR_Func *fn = module->funcs[fi];
        if (fn->is_extern || fn->block_count == 0) continue;

        /* Pass 1: Mark array literals as stack-eligible */
        for (int bi = 0; bi < fn->block_count; bi++) {
            IronLIR_Block *block = fn->blocks[bi];
            for (int ii = 0; ii < block->instr_count; ii++) {
                IronLIR_Instr *instr = block->instrs[ii];
                if (instr->kind == IRON_LIR_ARRAY_LIT) {
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
        struct { IronLIR_ValueId key; IronLIR_ValueId value; } *sa_map = NULL;
        /* Mark all stack array lits and ALL fill() calls (constant or dynamic count) */
        for (int bi = 0; bi < fn->block_count; bi++) {
            IronLIR_Block *block = fn->blocks[bi];
            for (int ii = 0; ii < block->instr_count; ii++) {
                IronLIR_Instr *instr = block->instrs[ii];
                if (instr->kind == IRON_LIR_ARRAY_LIT && instr->array_lit.use_stack_repr) {
                    hmput(sa_map, instr->id, instr->id);
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
                        hmput(sa_map, instr->id, instr->id);
                    }
                }
            }
        }
        /* Propagate through store/load */
        for (int bi = 0; bi < fn->block_count; bi++) {
            IronLIR_Block *block = fn->blocks[bi];
            for (int ii = 0; ii < block->instr_count; ii++) {
                IronLIR_Instr *instr = block->instrs[ii];
                if (instr->kind == IRON_LIR_STORE) {
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
                            IronLIR_ValueId orig = sa_map[pi].value;
                            if (orig < (IronLIR_ValueId)arrlen(fn->value_table) &&
                                fn->value_table[orig]) {
                                if (fn->value_table[orig]->kind == IRON_LIR_ARRAY_LIT)
                                    fn->value_table[orig]->array_lit.use_stack_repr = false;
                                else
                                    hmput(info->revoked_fill_ids, orig, true);
                            }
                            hmdel(sa_map, instr->store.ptr);
                        }
                    }
                } else if (instr->kind == IRON_LIR_LOAD) {
                    ptrdiff_t vi = hmgeti(sa_map, instr->load.ptr);
                    if (vi >= 0) hmput(sa_map, instr->id, sa_map[vi].value);
                }
            }
        }
        /* Check returns and function call arguments for escapes */
        for (int bi = 0; bi < fn->block_count; bi++) {
            IronLIR_Block *block = fn->blocks[bi];
            for (int ii = 0; ii < block->instr_count; ii++) {
                IronLIR_Instr *instr = block->instrs[ii];
                if (instr->kind == IRON_LIR_RETURN && !instr->ret.is_void) {
                    ptrdiff_t vi = hmgeti(sa_map, instr->ret.value);
                    if (vi >= 0) {
                        IronLIR_ValueId orig = sa_map[vi].value;
                        if (orig < (IronLIR_ValueId)arrlen(fn->value_table) &&
                            fn->value_table[orig]) {
                            if (fn->value_table[orig]->kind == IRON_LIR_ARRAY_LIT)
                                fn->value_table[orig]->array_lit.use_stack_repr = false;
                            else
                                hmput(info->revoked_fill_ids, orig, true);
                        }
                    }
                }
                /* Check if stack array is passed as argument to a function call.
                 * If the callee param uses pointer mode (PARAM-01/02), the stack
                 * array can be passed directly. Otherwise revoke stack repr. */
                if (instr->kind == IRON_LIR_CALL) {
                    /* Resolve callee IR name for pointer-mode check */
                    const char *call_ir_name = NULL;
                    if (instr->call.func_decl && !instr->call.func_decl->is_extern) {
                        call_ir_name = instr->call.func_decl->name;
                    } else if (!instr->call.func_decl) {
                        IronLIR_ValueId fptr = instr->call.func_ptr;
                        if (fptr != IRON_LIR_VALUE_INVALID &&
                            fptr < (IronLIR_ValueId)arrlen(fn->value_table) &&
                            fn->value_table[fptr] != NULL &&
                            fn->value_table[fptr]->kind == IRON_LIR_FUNC_REF) {
                            const char *rn = fn->value_table[fptr]->func_ref.func_name;
                            IronLIR_Func *cf = find_ir_func(module, rn);
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
                                cpmode = iron_lir_get_array_param_mode(info, call_ir_name, ai, module->arena);
                            if (cpmode == ARRAY_PARAM_CONST_PTR ||
                                cpmode == ARRAY_PARAM_MUT_PTR)
                                continue; /* callee accepts pointer+len */
                            IronLIR_ValueId orig = sa_map[vi].value;
                            if (orig < (IronLIR_ValueId)arrlen(fn->value_table) &&
                                fn->value_table[orig]) {
                                if (fn->value_table[orig]->kind == IRON_LIR_ARRAY_LIT)
                                    fn->value_table[orig]->array_lit.use_stack_repr = false;
                                else
                                    hmput(info->revoked_fill_ids, orig, true);
                            }
                        }
                    }
                }
                /* Check if stack array is used in SET_FIELD (stored into object) */
                if (instr->kind == IRON_LIR_SET_FIELD) {
                    ptrdiff_t vi = hmgeti(sa_map, instr->field.value);
                    if (vi >= 0) {
                        IronLIR_ValueId orig = sa_map[vi].value;
                        if (orig < (IronLIR_ValueId)arrlen(fn->value_table) &&
                            fn->value_table[orig]) {
                            if (fn->value_table[orig]->kind == IRON_LIR_ARRAY_LIT)
                                fn->value_table[orig]->array_lit.use_stack_repr = false;
                            else
                                hmput(info->revoked_fill_ids, orig, true);
                        }
                    }
                }
                /* Check if stack array is used as field in CONSTRUCT (stored into struct) */
                if (instr->kind == IRON_LIR_CONSTRUCT) {
                    for (int fi2 = 0; fi2 < instr->construct.field_count; fi2++) {
                        ptrdiff_t vi = hmgeti(sa_map, instr->construct.field_vals[fi2]);
                        if (vi >= 0) {
                            IronLIR_ValueId orig = sa_map[vi].value;
                            if (orig < (IronLIR_ValueId)arrlen(fn->value_table) &&
                                fn->value_table[orig]) {
                                if (fn->value_table[orig]->kind == IRON_LIR_ARRAY_LIT)
                                    fn->value_table[orig]->array_lit.use_stack_repr = false;
                                else
                                    hmput(info->revoked_fill_ids, orig, true);
                            }
                        }
                    }
                }
                /* Check if stack array is used in MAKE_CLOSURE captures */
                if (instr->kind == IRON_LIR_MAKE_CLOSURE) {
                    for (int ci = 0; ci < instr->make_closure.capture_count; ci++) {
                        ptrdiff_t vi = hmgeti(sa_map, instr->make_closure.captures[ci]);
                        if (vi >= 0) {
                            IronLIR_ValueId orig = sa_map[vi].value;
                            if (orig < (IronLIR_ValueId)arrlen(fn->value_table) &&
                                fn->value_table[orig]) {
                                fn->value_table[orig]->array_lit.use_stack_repr = false;
                            }
                        }
                    }
                }
                /* Check if stack array is used in INTERP_STRING */
                if (instr->kind == IRON_LIR_INTERP_STRING) {
                    for (int pi = 0; pi < instr->interp_string.part_count; pi++) {
                        ptrdiff_t vi = hmgeti(sa_map, instr->interp_string.parts[pi]);
                        if (vi >= 0) {
                            IronLIR_ValueId orig = sa_map[vi].value;
                            if (orig < (IronLIR_ValueId)arrlen(fn->value_table) &&
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
typedef struct { IronLIR_ValueId key; IronLIR_ValueId value; } ValueReplEntry;

/* Named type for the block-ID remap hashmap (used by function inlining). */
typedef struct { IronLIR_BlockId key; IronLIR_BlockId value; } BlockIdRemap;

/* Named type for the per-alloca store-count info hashmap. */
typedef struct { int count; IronLIR_ValueId val; } StoreInfoVal;
typedef struct { IronLIR_ValueId key; StoreInfoVal value; } StoreInfoEntry;

/* Apply replacement map to all ValueId operands of instr.
 * Returns true if any operand was changed. */
static bool apply_replacements(IronLIR_Instr *instr, ValueReplEntry *repl_map) {
    bool changed = false;

#define REPL(field) do { \
    if ((field) != IRON_LIR_VALUE_INVALID) { \
        ptrdiff_t _idx = hmgeti(repl_map, (field)); \
        if (_idx >= 0) { (field) = repl_map[_idx].value; changed = true; } \
    } \
} while (0)

    switch (instr->kind) {
    case IRON_LIR_CONST_INT: case IRON_LIR_CONST_FLOAT:
    case IRON_LIR_CONST_BOOL: case IRON_LIR_CONST_STRING:
    case IRON_LIR_CONST_NULL: case IRON_LIR_FUNC_REF:
        break; /* no operands */

    case IRON_LIR_ADD: case IRON_LIR_SUB: case IRON_LIR_MUL:
    case IRON_LIR_DIV: case IRON_LIR_MOD:
    case IRON_LIR_EQ: case IRON_LIR_NEQ: case IRON_LIR_LT:
    case IRON_LIR_LTE: case IRON_LIR_GT: case IRON_LIR_GTE:
    case IRON_LIR_AND: case IRON_LIR_OR:
        REPL(instr->binop.left);
        REPL(instr->binop.right);
        break;

    case IRON_LIR_NEG: case IRON_LIR_NOT:
        REPL(instr->unop.operand);
        break;

    case IRON_LIR_ALLOCA:
        break; /* no value operands */

    case IRON_LIR_LOAD:
        REPL(instr->load.ptr);
        break;

    case IRON_LIR_STORE:
        REPL(instr->store.ptr);
        REPL(instr->store.value);
        break;

    case IRON_LIR_GET_FIELD:
        REPL(instr->field.object);
        break;

    case IRON_LIR_SET_FIELD:
        REPL(instr->field.object);
        REPL(instr->field.value);
        break;

    case IRON_LIR_GET_INDEX:
        REPL(instr->index.array);
        REPL(instr->index.index);
        break;

    case IRON_LIR_SET_INDEX:
        REPL(instr->index.array);
        REPL(instr->index.index);
        REPL(instr->index.value);
        break;

    case IRON_LIR_CALL:
        if (instr->call.func_decl == NULL) {
            REPL(instr->call.func_ptr);
        }
        for (int i = 0; i < instr->call.arg_count; i++)
            REPL(instr->call.args[i]);
        break;

    case IRON_LIR_CAST:
        REPL(instr->cast.value);
        break;

    case IRON_LIR_HEAP_ALLOC:
        REPL(instr->heap_alloc.inner_val);
        break;

    case IRON_LIR_RC_ALLOC:
        REPL(instr->rc_alloc.inner_val);
        break;

    case IRON_LIR_FREE:
        REPL(instr->free_instr.value);
        break;

    case IRON_LIR_CONSTRUCT:
        for (int i = 0; i < instr->construct.field_count; i++)
            REPL(instr->construct.field_vals[i]);
        break;

    case IRON_LIR_ARRAY_LIT:
        for (int i = 0; i < instr->array_lit.element_count; i++)
            REPL(instr->array_lit.elements[i]);
        break;

    case IRON_LIR_SLICE:
        REPL(instr->slice.array);
        REPL(instr->slice.start);
        REPL(instr->slice.end);
        break;

    case IRON_LIR_IS_NULL: case IRON_LIR_IS_NOT_NULL:
        REPL(instr->null_check.value);
        break;

    case IRON_LIR_INTERP_STRING:
        for (int i = 0; i < instr->interp_string.part_count; i++)
            REPL(instr->interp_string.parts[i]);
        break;

    case IRON_LIR_MAKE_CLOSURE:
        for (int i = 0; i < instr->make_closure.capture_count; i++)
            REPL(instr->make_closure.captures[i]);
        break;

    case IRON_LIR_SPAWN:
        REPL(instr->spawn.pool_val);
        break;

    case IRON_LIR_PARALLEL_FOR:
        REPL(instr->parallel_for.range_val);
        REPL(instr->parallel_for.pool_val);
        for (int i = 0; i < instr->parallel_for.capture_count; i++)
            REPL(instr->parallel_for.captures[i]);
        break;

    case IRON_LIR_AWAIT:
        REPL(instr->await.handle);
        break;

    case IRON_LIR_BRANCH:
        REPL(instr->branch.cond);
        break;

    case IRON_LIR_SWITCH:
        REPL(instr->sw.subject);
        break;

    case IRON_LIR_RETURN:
        if (!instr->ret.is_void)
            REPL(instr->ret.value);
        break;

    case IRON_LIR_JUMP:
        break; /* no value operands */

    case IRON_LIR_PHI:
        for (int i = 0; i < instr->phi.count; i++)
            REPL(instr->phi.values[i]);
        break;

    case IRON_LIR_POISON:
        break; /* no operands */

    default:
        break;
    }

#undef REPL
    return changed;
}

/* Collect all ValueId operands of instr into out[].
 * Mirrors verify.c's collect_operands exactly. */
static void opt_collect_operands(const IronLIR_Instr *instr,
                                  IronLIR_ValueId *out, int *count) {
    *count = 0;
#define PUSH(v) do { if ((v) != IRON_LIR_VALUE_INVALID && *count < MAX_OPERANDS) out[(*count)++] = (v); } while(0)

    switch (instr->kind) {
    case IRON_LIR_CONST_INT:
    case IRON_LIR_CONST_FLOAT:
    case IRON_LIR_CONST_BOOL:
    case IRON_LIR_CONST_STRING:
    case IRON_LIR_CONST_NULL:
    case IRON_LIR_FUNC_REF:
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
        break;

    default:
        break;
    }

#undef PUSH
}

/* Copy Propagation: eliminate LOAD of single-store allocas.
 * For each alloca stored exactly once, replace all LOADs of that alloca
 * with the stored value, then rewrite all operands referencing the LOAD result. */
static bool run_copy_propagation(IronLIR_Module *module) {
    bool changed = false;
    for (int fi = 0; fi < module->func_count; fi++) {
        IronLIR_Func *fn = module->funcs[fi];
        if (fn->is_extern || fn->block_count == 0) continue;

        /* Step 1: Count stores per alloca.
         * Key: alloca ValueId. Value: { store_count, stored_value }
         * A SET_INDEX targeting an alloca also counts as a mutation (increments count)
         * so that copy propagation does not forward the original STORE value past it. */
        StoreInfoEntry *store_info = NULL;

        for (int bi = 0; bi < fn->block_count; bi++) {
            IronLIR_Block *blk = fn->blocks[bi];
            for (int ii = 0; ii < blk->instr_count; ii++) {
                IronLIR_Instr *in = blk->instrs[ii];
                if (in->kind == IRON_LIR_STORE) {
                    IronLIR_ValueId ptr = in->store.ptr;
                    /* Only track stores to ALLOCA values */
                    if (ptr != IRON_LIR_VALUE_INVALID &&
                        (ptrdiff_t)ptr < arrlen(fn->value_table) &&
                        fn->value_table[ptr] != NULL &&
                        fn->value_table[ptr]->kind == IRON_LIR_ALLOCA) {
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
                } else if (in->kind == IRON_LIR_SET_INDEX) {
                    /* SET_INDEX mutates an element of an array alloca — treat as a
                     * second mutation so copy_prop will not forward the whole-array
                     * STORE value past this instruction. */
                    IronLIR_ValueId arr = in->index.array;
                    if (arr != IRON_LIR_VALUE_INVALID &&
                        (ptrdiff_t)arr < arrlen(fn->value_table) &&
                        fn->value_table[arr] != NULL &&
                        fn->value_table[arr]->kind == IRON_LIR_ALLOCA) {
                        ptrdiff_t idx = hmgeti(store_info, arr);
                        if (idx < 0) {
                            StoreInfoVal sv;
                            sv.count = 2;  /* signal: not safe for copy-prop */
                            sv.val   = IRON_LIR_VALUE_INVALID;
                            hmput(store_info, arr, sv);
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
            IronLIR_Block *blk = fn->blocks[bi];
            for (int ii = 0; ii < blk->instr_count; ii++) {
                IronLIR_Instr *in = blk->instrs[ii];
                if (in->kind == IRON_LIR_LOAD && in->id != IRON_LIR_VALUE_INVALID) {
                    IronLIR_ValueId ptr = in->load.ptr;
                    ptrdiff_t idx = hmgeti(store_info, ptr);
                    if (idx >= 0 && store_info[idx].value.count == 1) {
                        IronLIR_ValueId replacement = store_info[idx].value.val;
                        if (replacement != IRON_LIR_VALUE_INVALID) {
                            hmput(repl_map, in->id, replacement);
                        }
                    }
                }
            }
        }

        /* Step 3: Apply replacements to all operands in all instructions */
        if (hmlen(repl_map) > 0) {
            for (int bi = 0; bi < fn->block_count; bi++) {
                IronLIR_Block *blk = fn->blocks[bi];
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

static bool is_arithmetic_binop(IronLIR_InstrKind kind) {
    return kind == IRON_LIR_ADD || kind == IRON_LIR_SUB ||
           kind == IRON_LIR_MUL || kind == IRON_LIR_DIV ||
           kind == IRON_LIR_MOD;
}

static bool is_comparison_binop(IronLIR_InstrKind kind) {
    return kind == IRON_LIR_EQ  || kind == IRON_LIR_NEQ ||
           kind == IRON_LIR_LT  || kind == IRON_LIR_LTE ||
           kind == IRON_LIR_GT  || kind == IRON_LIR_GTE;
}

/* Constant Folding: evaluate CONST_INT op CONST_INT at compile time.
 * Also folds integer comparisons to CONST_BOOL. */
static bool run_constant_folding(IronLIR_Module *module) {
    bool changed = false;
    for (int fi = 0; fi < module->func_count; fi++) {
        IronLIR_Func *fn = module->funcs[fi];
        if (fn->is_extern || fn->block_count == 0) continue;

        for (int bi = 0; bi < fn->block_count; bi++) {
            IronLIR_Block *blk = fn->blocks[bi];
            for (int ii = 0; ii < blk->instr_count; ii++) {
                IronLIR_Instr *in = blk->instrs[ii];

                if (is_arithmetic_binop(in->kind)) {
                    IronLIR_Instr *left_def  = NULL;
                    IronLIR_Instr *right_def = NULL;
                    if (in->binop.left != IRON_LIR_VALUE_INVALID &&
                        (ptrdiff_t)in->binop.left < arrlen(fn->value_table))
                        left_def = fn->value_table[in->binop.left];
                    if (in->binop.right != IRON_LIR_VALUE_INVALID &&
                        (ptrdiff_t)in->binop.right < arrlen(fn->value_table))
                        right_def = fn->value_table[in->binop.right];

                    if (left_def && right_def &&
                        left_def->kind == IRON_LIR_CONST_INT &&
                        right_def->kind == IRON_LIR_CONST_INT) {
                        int64_t L = left_def->const_int.value;
                        int64_t R = right_def->const_int.value;
                        int64_t result = 0;
                        bool can_fold = true;
                        switch (in->kind) {
                            case IRON_LIR_ADD: result = L + R; break;
                            case IRON_LIR_SUB: result = L - R; break;
                            case IRON_LIR_MUL: result = L * R; break;
                            case IRON_LIR_DIV:
                                if (R == 0) { can_fold = false; break; }
                                result = L / R; break;
                            case IRON_LIR_MOD:
                                if (R == 0) { can_fold = false; break; }
                                result = L % R; break;
                            default: can_fold = false; break;
                        }
                        if (can_fold) {
                            in->kind = IRON_LIR_CONST_INT;
                            in->const_int.value = result;
                            changed = true;
                        }
                    }
                }

                if (is_comparison_binop(in->kind)) {
                    IronLIR_Instr *left_def  = NULL;
                    IronLIR_Instr *right_def = NULL;
                    if (in->binop.left != IRON_LIR_VALUE_INVALID &&
                        (ptrdiff_t)in->binop.left < arrlen(fn->value_table))
                        left_def = fn->value_table[in->binop.left];
                    if (in->binop.right != IRON_LIR_VALUE_INVALID &&
                        (ptrdiff_t)in->binop.right < arrlen(fn->value_table))
                        right_def = fn->value_table[in->binop.right];

                    if (left_def && right_def &&
                        left_def->kind == IRON_LIR_CONST_INT &&
                        right_def->kind == IRON_LIR_CONST_INT) {
                        int64_t L = left_def->const_int.value;
                        int64_t R = right_def->const_int.value;
                        bool result = false;
                        switch (in->kind) {
                            case IRON_LIR_EQ:  result = (L == R); break;
                            case IRON_LIR_NEQ: result = (L != R); break;
                            case IRON_LIR_LT:  result = (L < R);  break;
                            case IRON_LIR_LTE: result = (L <= R); break;
                            case IRON_LIR_GT:  result = (L > R);  break;
                            case IRON_LIR_GTE: result = (L >= R); break;
                            default: break;
                        }
                        in->kind = IRON_LIR_CONST_BOOL;
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
static bool run_dce(IronLIR_Module *module) {
    bool changed = false;
    for (int fi = 0; fi < module->func_count; fi++) {
        IronLIR_Func *fn = module->funcs[fi];
        if (fn->is_extern || fn->block_count == 0) continue;

        /* Step 1: Seed live set with side-effecting instructions */
        struct { IronLIR_ValueId key; bool value; } *live = NULL;
        IronLIR_ValueId *worklist = NULL; /* stb_ds dynamic array */

        for (int bi = 0; bi < fn->block_count; bi++) {
            IronLIR_Block *blk = fn->blocks[bi];
            for (int ii = 0; ii < blk->instr_count; ii++) {
                IronLIR_Instr *in = blk->instrs[ii];
                /* Live if: side-effecting, or has no result (STORE, terminators) */
                if (!iron_lir_instr_is_pure(in->kind) ||
                    in->id == IRON_LIR_VALUE_INVALID) {
                    if (in->id != IRON_LIR_VALUE_INVALID) {
                        hmput(live, in->id, true);
                    }
                    /* Mark this instruction's operands as live seeds */
                    IronLIR_ValueId ops[MAX_OPERANDS];
                    int op_count = 0;
                    opt_collect_operands(in, ops, &op_count);
                    for (int oi = 0; oi < op_count; oi++) {
                        if (hmgeti(live, ops[oi]) < 0) {
                            hmput(live, ops[oi], true);
                            arrput(worklist, ops[oi]);
                        }
                    }
                }
                /* ARRAY_LIT: opt_collect_operands is limited to MAX_OPERANDS (64).
                 * For large array literals (> 64 elements) the elements beyond the
                 * limit are never seeded as live, so DCE incorrectly removes them.
                 * Unconditionally mark ALL ARRAY_LIT elements live here. */
                if (in->kind == IRON_LIR_ARRAY_LIT) {
                    for (int ei = 0; ei < in->array_lit.element_count; ei++) {
                        IronLIR_ValueId eid = in->array_lit.elements[ei];
                        if (eid != IRON_LIR_VALUE_INVALID && hmgeti(live, eid) < 0) {
                            hmput(live, eid, true);
                            arrput(worklist, eid);
                        }
                    }
                }
            }
        }

        /* Step 2: Worklist propagation — mark operands of live instrs as live */
        for (int wi = 0; wi < (int)arrlen(worklist); wi++) {
            IronLIR_ValueId vid = worklist[wi];
            if (vid == IRON_LIR_VALUE_INVALID ||
                (ptrdiff_t)vid >= arrlen(fn->value_table) ||
                fn->value_table[vid] == NULL) continue;

            IronLIR_Instr *prod = fn->value_table[vid];
            IronLIR_ValueId ops[MAX_OPERANDS];
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
            IronLIR_Block *blk = fn->blocks[bi];
            int new_count = 0;
            for (int ii = 0; ii < blk->instr_count; ii++) {
                IronLIR_Instr *in = blk->instrs[ii];
                bool is_live = false;

                /* Always keep side-effecting instructions and terminators */
                if (!iron_lir_instr_is_pure(in->kind) ||
                    in->id == IRON_LIR_VALUE_INVALID) {
                    is_live = true;
                } else {
                    /* Pure instruction — keep only if its result is in live set */
                    is_live = (hmgeti(live, in->id) >= 0);
                }

                if (is_live) {
                    blk->instrs[new_count++] = in;
                } else {
                    /* Null out value_table entry for dead instruction */
                    if (in->id != IRON_LIR_VALUE_INVALID &&
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

/* ── Store/load elimination pass (Phase 17) ─────────────────────────────── */

/* Compute the set of ALLOCA value IDs whose address escapes this function.
 * An alloca "escapes" if its ValueId appears as:
 *   - A CALL argument (address passed to callee)
 *   - The VALUE field of a STORE (address written into memory, not the pointed-to value)
 *   - The return value of a RETURN instruction
 * Returns a stb_ds hashmap (IronLIR_EscapeEntry *).  Caller must hmfree() it. */
static IronLIR_EscapeEntry *compute_escape_set(IronLIR_Func *fn) {
    IronLIR_EscapeEntry *escaped = NULL;

    /* Seed all allocas as non-escaped */
    for (int bi = 0; bi < fn->block_count; bi++) {
        IronLIR_Block *blk = fn->blocks[bi];
        for (int ii = 0; ii < blk->instr_count; ii++) {
            IronLIR_Instr *in = blk->instrs[ii];
            if (in->kind == IRON_LIR_ALLOCA) {
                hmput(escaped, in->id, false);
            }
        }
    }

    /* Walk all instructions; for each, check whether any alloca ID escapes */
    for (int bi = 0; bi < fn->block_count; bi++) {
        IronLIR_Block *blk = fn->blocks[bi];
        for (int ii = 0; ii < blk->instr_count; ii++) {
            IronLIR_Instr *in = blk->instrs[ii];

            switch (in->kind) {
            case IRON_LIR_CALL:
                /* If an alloca address is passed as a call argument, it escapes */
                for (int ai = 0; ai < in->call.arg_count; ai++) {
                    IronLIR_ValueId arg = in->call.args[ai];
                    ptrdiff_t idx = hmgeti(escaped, arg);
                    if (idx >= 0) escaped[idx].value = true;
                }
                break;

            case IRON_LIR_STORE:
                /* If an alloca address is the VALUE being stored (not the ptr), it escapes */
                {
                    ptrdiff_t idx = hmgeti(escaped, in->store.value);
                    if (idx >= 0) escaped[idx].value = true;
                }
                break;

            case IRON_LIR_RETURN:
                if (!in->ret.is_void) {
                    ptrdiff_t idx = hmgeti(escaped, in->ret.value);
                    if (idx >= 0) escaped[idx].value = true;
                }
                break;

            default:
                break;
            }
        }
    }

    return escaped;
}

/* ── Dead alloca elimination pass (PHI-01/PHI-02) ───────────────────────── */

/* Dead alloca elimination: remove allocas with no live loads, and all their
 * stores.  Runs post-fixpoint so copy-propagation has already eliminated
 * single-store alloca loads.
 *
 * An alloca is considered "live" if:
 *   - Any LOAD references it as load.ptr, OR
 *   - Any GET_INDEX or SET_INDEX references it as index.array, OR
 *   - Any GET_FIELD or SET_FIELD references it as field.object, OR
 *   - It appears in the escape set (address passed to CALL, stored as value,
 *     or returned).
 *
 * If none of the above apply, the alloca and all STORE instructions targeting
 * it are removed.  Returns true if any change was made. */
static bool run_dead_alloca_elimination(IronLIR_Module *module) {
    bool changed = false;
    for (int fi = 0; fi < module->func_count; fi++) {
        IronLIR_Func *fn = module->funcs[fi];
        if (fn->is_extern || fn->block_count == 0) continue;

        /* Step 1: Build "loaded" set — alloca IDs that are live */
        struct { IronLIR_ValueId key; bool value; } *loaded = NULL;

        for (int bi = 0; bi < fn->block_count; bi++) {
            IronLIR_Block *blk = fn->blocks[bi];
            for (int ii = 0; ii < blk->instr_count; ii++) {
                IronLIR_Instr *in = blk->instrs[ii];
                switch (in->kind) {
                case IRON_LIR_LOAD:
                    hmput(loaded, in->load.ptr, true);
                    break;
                case IRON_LIR_GET_INDEX:
                case IRON_LIR_SET_INDEX:
                    hmput(loaded, in->index.array, true);
                    break;
                case IRON_LIR_GET_FIELD:
                case IRON_LIR_SET_FIELD:
                    hmput(loaded, in->field.object, true);
                    break;
                default:
                    break;
                }
            }
        }

        /* Step 1b: Escape safety check — keep escaped allocas */
        IronLIR_EscapeEntry *escape_set = compute_escape_set(fn);
        for (ptrdiff_t ei = 0; ei < hmlen(escape_set); ei++) {
            if (escape_set[ei].value) {
                hmput(loaded, escape_set[ei].key, true);
            }
        }
        hmfree(escape_set);

        /* Step 2: Identify dead allocas (not in loaded set) */
        struct { IronLIR_ValueId key; bool value; } *dead_alloca = NULL;
        for (int bi = 0; bi < fn->block_count; bi++) {
            IronLIR_Block *blk = fn->blocks[bi];
            for (int ii = 0; ii < blk->instr_count; ii++) {
                IronLIR_Instr *in = blk->instrs[ii];
                if (in->kind == IRON_LIR_ALLOCA) {
                    if (hmgeti(loaded, in->id) < 0) {
                        hmput(dead_alloca, in->id, true);
                    }
                }
            }
        }

        /* Step 3: Remove dead allocas and their stores */
        if (hmlen(dead_alloca) > 0) {
            for (int bi = 0; bi < fn->block_count; bi++) {
                IronLIR_Block *blk = fn->blocks[bi];
                int new_count = 0;
                for (int ii = 0; ii < blk->instr_count; ii++) {
                    IronLIR_Instr *in = blk->instrs[ii];
                    bool remove = false;
                    if (in->kind == IRON_LIR_ALLOCA &&
                        hmgeti(dead_alloca, in->id) >= 0) {
                        remove = true;
                    } else if (in->kind == IRON_LIR_STORE &&
                               hmgeti(dead_alloca, in->store.ptr) >= 0) {
                        remove = true;
                    }
                    if (!remove) {
                        blk->instrs[new_count++] = in;
                    } else {
                        if (in->id != IRON_LIR_VALUE_INVALID &&
                            (ptrdiff_t)in->id < arrlen(fn->value_table)) {
                            fn->value_table[in->id] = NULL;
                        }
                        changed = true;
                    }
                }
                blk->instr_count = new_count;
            }
        }

        hmfree(loaded);
        hmfree(dead_alloca);
    }
    return changed;
}

/* Redundant store/load elimination pass.
 *
 * For each basic block, performs a forward scan tracking the last value stored
 * to each alloca.  When a LOAD of that alloca is encountered, the LOAD result
 * is replaced with the stored value (added to repl_map).  After the block scan,
 * apply_replacements() propagates the substitution to all operands.
 *
 * Invalidation rules (conservative but correct):
 *   STORE to alloca A  -> update last_store[A] = stored_value
 *   SET_INDEX on A     -> delete last_store[A]  (array element mutated)
 *   SET_FIELD on obj O -> delete last_store[O]  (field mutated)
 *   CALL               -> delete escaped allocas from last_store
 *
 * Only intra-block: last_store is reset at the start of each block.
 * Returns true if any replacement was made. */
static bool run_store_load_elim(IronLIR_Module *module) {
    bool changed = false;

    for (int fi = 0; fi < module->func_count; fi++) {
        IronLIR_Func *fn = module->funcs[fi];
        if (fn->is_extern || fn->block_count == 0) continue;

        /* Build the escape map for this function */
        IronLIR_EscapeEntry *escape_set = compute_escape_set(fn);

        /* Collect replacements across all blocks, then apply in second pass */
        ValueReplEntry *repl_map = NULL;

        for (int bi = 0; bi < fn->block_count; bi++) {
            IronLIR_Block *blk = fn->blocks[bi];

            /* Reset per-block last-store tracking */
            IronLIR_StoreTrackEntry *last_store = NULL;

            for (int ii = 0; ii < blk->instr_count; ii++) {
                IronLIR_Instr *in = blk->instrs[ii];

                switch (in->kind) {
                case IRON_LIR_STORE:
                    /* Only track stores to known alloca slots */
                    if (in->store.ptr != IRON_LIR_VALUE_INVALID &&
                        (ptrdiff_t)in->store.ptr < arrlen(fn->value_table) &&
                        fn->value_table[in->store.ptr] != NULL &&
                        fn->value_table[in->store.ptr]->kind == IRON_LIR_ALLOCA) {
                        hmput(last_store, in->store.ptr, in->store.value);
                    }
                    break;

                case IRON_LIR_LOAD:
                    /* If we have a tracked value for this alloca, queue a replacement */
                    if (in->id != IRON_LIR_VALUE_INVALID &&
                        in->load.ptr != IRON_LIR_VALUE_INVALID) {
                        ptrdiff_t idx = hmgeti(last_store, in->load.ptr);
                        if (idx >= 0) {
                            IronLIR_ValueId stored_val = last_store[idx].value;
                            if (stored_val != IRON_LIR_VALUE_INVALID) {
                                hmput(repl_map, in->id, stored_val);
                                changed = true;
                            }
                        }
                    }
                    break;

                case IRON_LIR_SET_INDEX:
                    /* SET_INDEX mutates the array element — invalidate that alloca */
                    if (last_store) hmdel(last_store, in->index.array);
                    break;

                case IRON_LIR_SET_FIELD:
                    /* SET_FIELD mutates a struct field — invalidate that alloca */
                    if (last_store) hmdel(last_store, in->field.object);
                    break;

                case IRON_LIR_CALL:
                    /* For escaped allocas: their content may be modified by callee */
                    {
                        /* Collect keys to delete (cannot delete while iterating) */
                        IronLIR_ValueId *to_delete = NULL;
                        for (ptrdiff_t ei = 0; ei < hmlen(last_store); ei++) {
                            IronLIR_ValueId alloca_id = last_store[ei].key;
                            ptrdiff_t esc_idx = hmgeti(escape_set, alloca_id);
                            if (esc_idx >= 0 && escape_set[esc_idx].value) {
                                arrput(to_delete, alloca_id);
                            }
                        }
                        for (int di = 0; di < (int)arrlen(to_delete); di++) {
                            hmdel(last_store, to_delete[di]);
                        }
                        arrfree(to_delete);
                    }
                    break;

                default:
                    break;
                }
            }

            hmfree(last_store);
        }

        /* Apply all replacements in a second pass */
        if (hmlen(repl_map) > 0) {
            for (int bi = 0; bi < fn->block_count; bi++) {
                IronLIR_Block *blk = fn->blocks[bi];
                for (int ii = 0; ii < blk->instr_count; ii++) {
                    apply_replacements(blk->instrs[ii], repl_map);
                }
            }
        }

        hmfree(repl_map);
        hmfree(escape_set);
    }

    return changed;
}

/* ── Expression inlining analysis (Phase 16) ────────────────────────────── */

/* Returns true if the instruction kind can be emitted as an inline sub-expression.
 * Must be pure AND not require multi-statement emission. */
static bool instr_is_inline_expressible(IronLIR_InstrKind kind) {
    if (!iron_lir_instr_is_pure(kind)) return false;
    /* CALL must not be inlined — emit_expr_to_buf doesn't implement array
     * parameter splitting (pointer+length) or extern string wrapping. */
    if (kind == IRON_LIR_CALL) return false;
    /* LOAD: inlining replaces _vN with the alloca name, minimal benefit;
     * but when the load crosses blocks the inline chain can reference
     * values with suppressed declarations, causing C compilation errors. */
    if (kind == IRON_LIR_LOAD) return false;
    /* Multi-statement emission patterns cannot be inlined as sub-expressions */
    if (kind == IRON_LIR_ARRAY_LIT) return false;
    if (kind == IRON_LIR_INTERP_STRING) return false;
    /* ALLOCA produces an address, not a value expression */
    if (kind == IRON_LIR_ALLOCA) return false;
    /* CONST_STRING emits iron_string_from_literal() which is fine to inline,
     * but string constants are already cheap as named vars — keep separate. */
    if (kind == IRON_LIR_CONST_STRING) return false;
    /* MAKE_CLOSURE emits a multi-statement env struct alloc + capture assignment.
     * emit_expr_to_buf falls back to emit_val for it, so it must not be
     * marked inline-eligible (its result void* would become undeclared). */
    if (kind == IRON_LIR_MAKE_CLOSURE) return false;
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
static void compute_func_purity(IronLIR_Module *module, IronLIR_OptimizeInfo *info) {
    if (!module || module->func_count == 0) return;

    /* Phase 1: mark functions with no CALLs and all-pure instructions */
    for (int fi = 0; fi < module->func_count; fi++) {
        IronLIR_Func *fn = module->funcs[fi];
        if (fn->is_extern || fn->block_count == 0) continue;

        bool all_pure = true;
        bool has_call = false;
        for (int bi = 0; bi < fn->block_count && all_pure; bi++) {
            IronLIR_Block *blk = fn->blocks[bi];
            for (int ii = 0; ii < blk->instr_count && all_pure; ii++) {
                IronLIR_Instr *in = blk->instrs[ii];
                if (in->kind == IRON_LIR_CALL) {
                    has_call = true;
                    /* Don't mark impure yet — will check callees in phase 2 */
                } else if (!iron_lir_instr_is_pure(in->kind) &&
                           in->kind != IRON_LIR_RETURN &&
                           in->kind != IRON_LIR_JUMP &&
                           in->kind != IRON_LIR_ALLOCA &&
                           in->kind != IRON_LIR_STORE) {
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
            IronLIR_Func *fn = module->funcs[fi];
            if (fn->is_extern || fn->block_count == 0) continue;
            /* Already marked pure — skip */
            if (hmgeti(info->func_purity, (char*)fn->name) >= 0) continue;

            bool all_pure = true;
            for (int bi = 0; bi < fn->block_count && all_pure; bi++) {
                IronLIR_Block *blk = fn->blocks[bi];
                for (int ii = 0; ii < blk->instr_count && all_pure; ii++) {
                    IronLIR_Instr *in = blk->instrs[ii];
                    if (in->kind == IRON_LIR_CALL) {
                        /* Check if callee is known pure */
                        const char *callee_name = NULL;
                        if (!in->call.func_decl) {
                            IronLIR_ValueId fptr = in->call.func_ptr;
                            if (fptr != IRON_LIR_VALUE_INVALID &&
                                fptr < (IronLIR_ValueId)arrlen(fn->value_table) &&
                                fn->value_table[fptr] != NULL &&
                                fn->value_table[fptr]->kind == IRON_LIR_FUNC_REF) {
                                callee_name = fn->value_table[fptr]->func_ref.func_name;
                            }
                        } else {
                            callee_name = in->call.func_decl->name;
                        }
                        if (!callee_name) { all_pure = false; break; }
                        bool callee_pure = is_pure_builtin(callee_name) ||
                                          hmgeti(info->func_purity, (char*)callee_name) >= 0;
                        if (!callee_pure) all_pure = false;
                    } else if (!iron_lir_instr_is_pure(in->kind) &&
                               in->kind != IRON_LIR_RETURN &&
                               in->kind != IRON_LIR_JUMP &&
                               in->kind != IRON_LIR_ALLOCA &&
                               in->kind != IRON_LIR_STORE) {
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
void iron_lir_compute_use_counts(IronLIR_Func *fn, IronLIR_OptimizeInfo *info) {
    IronLIR_ValueId ops[MAX_OPERANDS];
    int op_count = 0;

    for (int bi = 0; bi < fn->block_count; bi++) {
        IronLIR_Block *blk = fn->blocks[bi];
        for (int ii = 0; ii < blk->instr_count; ii++) {
            IronLIR_Instr *in = blk->instrs[ii];
            opt_collect_operands(in, ops, &op_count);
            for (int oi = 0; oi < op_count; oi++) {
                IronLIR_ValueId vid = ops[oi];
                ptrdiff_t idx = hmgeti(info->use_counts, vid);
                if (idx >= 0) {
                    info->use_counts[idx].value++;
                } else {
                    IronLIR_UseCountEntry e; e.key = vid; e.value = 1;
                    hmput(info->use_counts, vid, 1);
                }
            }
            /* ARRAY_LIT: opt_collect_operands is limited to MAX_OPERANDS (64).
             * For large array literals (> 64 elements), count ALL elements. */
            if (in->kind == IRON_LIR_ARRAY_LIT &&
                in->array_lit.element_count > MAX_OPERANDS) {
                for (int ei = MAX_OPERANDS; ei < in->array_lit.element_count; ei++) {
                    IronLIR_ValueId vid = in->array_lit.elements[ei];
                    ptrdiff_t idx = hmgeti(info->use_counts, vid);
                    if (idx >= 0) {
                        info->use_counts[idx].value++;
                    } else {
                        hmput(info->use_counts, vid, 1);
                    }
                }
            }
        }
    }
}

/* Build value->block map for one function.
 * Maps each defined ValueId to the BlockId of the block that defines it.
 * Populates info->value_block. */
void iron_lir_compute_value_block(IronLIR_Func *fn, IronLIR_OptimizeInfo *info) {
    for (int bi = 0; bi < fn->block_count; bi++) {
        IronLIR_Block *blk = fn->blocks[bi];
        for (int ii = 0; ii < blk->instr_count; ii++) {
            IronLIR_Instr *in = blk->instrs[ii];
            if (in->id != IRON_LIR_VALUE_INVALID) {
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
static bool instr_mutates_memory(IronLIR_InstrKind kind) {
    switch (kind) {
    case IRON_LIR_STORE:
    case IRON_LIR_SET_INDEX:
    case IRON_LIR_SET_FIELD:
    case IRON_LIR_CALL:
    case IRON_LIR_HEAP_ALLOC:
    case IRON_LIR_RC_ALLOC:
    case IRON_LIR_FREE:
        return true;
    default:
        return false;
    }
}

void iron_lir_compute_inline_eligible(IronLIR_Func *fn,
                                      IronLIR_OptimizeInfo *info) {
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
    struct { IronLIR_ValueId key; IronLIR_BlockId value; } *use_site_block = NULL;
    /* Map: ValueId -> instr index of (first) use site within its block */
    struct { IronLIR_ValueId key; int value; }            *use_site_pos  = NULL;
    /* Set of values excluded from inlining */
    struct { IronLIR_ValueId key; bool value; } *excluded = NULL;

    /* Pass 1: collect exclusions and use-site blocks/positions */
    for (int bi = 0; bi < fn->block_count; bi++) {
        IronLIR_Block *blk = fn->blocks[bi];
        for (int ii = 0; ii < blk->instr_count; ii++) {
            IronLIR_Instr *in = blk->instrs[ii];

            /* PHI incoming values: emitted as assignments in predecessor blocks.
             * Must not be inline-eligible to ensure declarations exist. */
            if (in->kind == IRON_LIR_PHI) {
                for (int pi = 0; pi < in->phi.count; pi++) {
                    hmput(excluded, in->phi.values[pi], true);
                }
            }

            /* INTERP_STRING parts: referenced by name in format strings */
            if (in->kind == IRON_LIR_INTERP_STRING) {
                for (int pi = 0; pi < in->interp_string.part_count; pi++) {
                    hmput(excluded, in->interp_string.parts[pi], true);
                }
            }

            /* MAKE_CLOSURE captures: emit_instr uses emit_val for these */
            if (in->kind == IRON_LIR_MAKE_CLOSURE) {
                for (int ci = 0; ci < in->make_closure.capture_count; ci++) {
                    hmput(excluded, in->make_closure.captures[ci], true);
                }
            }

            /* PARALLEL_FOR range_val and captures: emit_instr uses emit_val */
            if (in->kind == IRON_LIR_PARALLEL_FOR) {
                if (in->parallel_for.range_val != IRON_LIR_VALUE_INVALID)
                    hmput(excluded, in->parallel_for.range_val, true);
                for (int ci = 0; ci < in->parallel_for.capture_count; ci++) {
                    hmput(excluded, in->parallel_for.captures[ci], true);
                }
            }

            /* SPAWN pool_val: emit_instr uses emit_val */
            if (in->kind == IRON_LIR_SPAWN) {
                if (in->spawn.pool_val != IRON_LIR_VALUE_INVALID)
                    hmput(excluded, in->spawn.pool_val, true);
            }

            /* ARRAY_LIT elements: C initializer lists require all referenced values
             * to be already declared — exclude from inlining to prevent forward-ref. */
            if (in->kind == IRON_LIR_ARRAY_LIT) {
                for (int i = 0; i < in->array_lit.element_count; i++) {
                    hmput(excluded, in->array_lit.elements[i], true);
                }
            }

            /* Record use-site block/position for all operands; mark cross-block uses */
            IronLIR_ValueId ops[MAX_OPERANDS];
            int op_count = 0;
            opt_collect_operands(in, ops, &op_count);
            for (int oi = 0; oi < op_count; oi++) {
                IronLIR_ValueId op = ops[oi];
                ptrdiff_t ub_idx = hmgeti(use_site_block, op);
                if (ub_idx < 0) {
                    /* First use: record block and position */
                    hmput(use_site_block, op, blk->id);
                    hmput(use_site_pos,   op, ii);
                } else {
                    /* Subsequent use: if in different block, exclude */
                    if ((IronLIR_BlockId)use_site_block[ub_idx].value != blk->id) {
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
        IronLIR_Block *blk = fn->blocks[bi];
        for (int ii = 0; ii < blk->instr_count; ii++) {
            IronLIR_Instr *in = blk->instrs[ii];
            if (in->id == IRON_LIR_VALUE_INVALID) continue;

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
                    (IronLIR_BlockId)use_site_block[ub_idx].value != blk->id) {
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
                /* GET_INDEX/GET_FIELD: if the object/array operand is a function
                 * parameter (not in value_table), emit_expr_to_buf's inline path
                 * cannot determine the type and falls back to emit_val(_vN) without
                 * ever emitting a declaration.  Mark such operands NOT inline-eligible
                 * so the emitter emits a proper named variable declaration. */
                if (in->kind == IRON_LIR_GET_INDEX) {
                    IronLIR_ValueId arr = in->index.array;
                    if (arr == IRON_LIR_VALUE_INVALID ||
                        (ptrdiff_t)arr >= arrlen(fn->value_table) ||
                        fn->value_table[arr] == NULL) {
                        eligible = false;
                    }
                }
                if (in->kind == IRON_LIR_GET_FIELD) {
                    IronLIR_ValueId obj = in->field.object;
                    if (obj == IRON_LIR_VALUE_INVALID ||
                        (ptrdiff_t)obj >= arrlen(fn->value_table) ||
                        fn->value_table[obj] == NULL) {
                        eligible = false;
                    }
                }
            } else if (in->kind == IRON_LIR_CALL) {
                /* Check if callee is pure */
                const char *callee_name = NULL;
                if (!in->call.func_decl) {
                    IronLIR_ValueId fptr = in->call.func_ptr;
                    if (fptr != IRON_LIR_VALUE_INVALID &&
                        fptr < (IronLIR_ValueId)arrlen(fn->value_table) &&
                        fn->value_table[fptr] != NULL &&
                        fn->value_table[fptr]->kind == IRON_LIR_FUNC_REF) {
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

/* ── Strength reduction pass (Phase 17-02) ───────────────────────────────── */

/* Rebuild the preds/succs arrays for all blocks in the function by scanning
 * terminator instructions (JUMP, BRANCH, SWITCH).
 * This is required before domtree/loop analysis since the IR constructors do
 * not auto-populate these edges. */
static void rebuild_cfg_edges(IronLIR_Func *fn) {
    /* Clear existing edges */
    for (int bi = 0; bi < fn->block_count; bi++) {
        IronLIR_Block *blk = fn->blocks[bi];
        arrfree(blk->succs);
        arrfree(blk->preds);
        blk->succs = NULL;
        blk->preds = NULL;
    }

    /* Scan terminators and build edge lists */
    for (int bi = 0; bi < fn->block_count; bi++) {
        IronLIR_Block *blk = fn->blocks[bi];
        for (int ii = 0; ii < blk->instr_count; ii++) {
            IronLIR_Instr *instr = blk->instrs[ii];
            switch (instr->kind) {
            case IRON_LIR_JUMP: {
                IronLIR_BlockId target = instr->jump.target;
                arrput(blk->succs, target);
                IronLIR_Block *tblk = find_block(fn, target);
                if (tblk) arrput(tblk->preds, blk->id);
                break;
            }
            case IRON_LIR_BRANCH: {
                IronLIR_BlockId then_id = instr->branch.then_block;
                IronLIR_BlockId else_id = instr->branch.else_block;
                arrput(blk->succs, then_id);
                arrput(blk->succs, else_id);
                IronLIR_Block *tb = find_block(fn, then_id);
                IronLIR_Block *eb = find_block(fn, else_id);
                if (tb) arrput(tb->preds, blk->id);
                if (eb) arrput(eb->preds, blk->id);
                break;
            }
            case IRON_LIR_SWITCH: {
                IronLIR_BlockId def_id = instr->sw.default_block;
                arrput(blk->succs, def_id);
                IronLIR_Block *db = find_block(fn, def_id);
                if (db) arrput(db->preds, blk->id);
                for (int ci = 0; ci < instr->sw.case_count; ci++) {
                    IronLIR_BlockId cid = instr->sw.case_blocks[ci];
                    arrput(blk->succs, cid);
                    IronLIR_Block *cb = find_block(fn, cid);
                    if (cb) arrput(cb->preds, blk->id);
                }
                break;
            }
            default:
                break;
            }
        }
    }
}

/* Build reverse post-order (RPO) of blocks by DFS from entry (fn->blocks[0]).
 * Returns a stb_ds array of BlockIds in RPO order. Caller must arrfree(). */
static IronLIR_BlockId *build_rpo(IronLIR_Func *fn) {
    if (fn->block_count == 0) return NULL;

    /* Visited set: BlockId -> bool */
    struct { IronLIR_BlockId key; bool value; } *visited = NULL;
    /* Post-order accumulator (we'll reverse it) */
    IronLIR_BlockId *postorder = NULL;

    /* Iterative DFS using an explicit stack */
    /* Stack entry: block_id and successor index (for iterative post-order) */
    typedef struct { IronLIR_BlockId bid; int succ_idx; } StackFrame;
    StackFrame *stack = NULL;

    IronLIR_BlockId entry_id = fn->blocks[0]->id;
    StackFrame sf; sf.bid = entry_id; sf.succ_idx = 0;
    arrput(stack, sf);
    hmput(visited, entry_id, true);

    while (arrlen(stack) > 0) {
        StackFrame *top = &stack[arrlen(stack) - 1];
        IronLIR_Block *blk = find_block(fn, top->bid);
        if (!blk || top->succ_idx >= (int)arrlen(blk->succs)) {
            /* All successors visited — emit this block to post-order */
            arrput(postorder, top->bid);
            arrpop(stack);
        } else {
            IronLIR_BlockId succ_id = blk->succs[top->succ_idx++];
            if (hmgeti(visited, succ_id) < 0) {
                hmput(visited, succ_id, true);
                StackFrame nsf; nsf.bid = succ_id; nsf.succ_idx = 0;
                arrput(stack, nsf);
            }
        }
    }

    /* Reverse post-order = reverse of postorder array */
    IronLIR_BlockId *rpo = NULL;
    for (int i = (int)arrlen(postorder) - 1; i >= 0; i--) {
        arrput(rpo, postorder[i]);
    }

    hmfree(visited);
    arrfree(postorder);
    arrfree(stack);
    return rpo;
}

/* Build dominator tree using the iterative algorithm.
 * Returns a stb_ds hashmap: BlockId -> idom BlockId.
 * Caller must hmfree() the result. */
static IronLIR_DomEntry *build_domtree(IronLIR_Func *fn) {
    if (fn->block_count == 0) return NULL;

    IronLIR_DomEntry *idom = NULL;
    IronLIR_BlockId entry_id = fn->blocks[0]->id;

    /* Build RPO for traversal order */
    IronLIR_BlockId *rpo = build_rpo(fn);
    int rpo_len = (int)arrlen(rpo);
    if (rpo_len == 0) { arrfree(rpo); return NULL; }

    /* Build RPO index map: BlockId -> position in RPO (for intersection walk) */
    struct { IronLIR_BlockId key; int value; } *rpo_idx = NULL;
    for (int i = 0; i < rpo_len; i++) {
        hmput(rpo_idx, rpo[i], i);
    }

    /* Initialize: idom[entry] = entry, all others undefined */
    hmput(idom, entry_id, entry_id);

    bool changed = true;
    while (changed) {
        changed = false;
        /* Process blocks in RPO order (skip entry) */
        for (int ri = 1; ri < rpo_len; ri++) {
            IronLIR_BlockId bid = rpo[ri];
            IronLIR_Block *blk  = find_block(fn, bid);
            if (!blk) continue;

            /* Find first processed predecessor */
            IronLIR_BlockId new_idom = 0;
            bool found_first = false;

            for (int pi = 0; pi < (int)arrlen(blk->preds); pi++) {
                IronLIR_BlockId pred = blk->preds[pi];
                if (hmgeti(idom, pred) >= 0) {
                    /* This predecessor has been processed */
                    if (!found_first) {
                        new_idom = pred;
                        found_first = true;
                    } else {
                        /* Intersection: walk up from both until they meet */
                        IronLIR_BlockId a = new_idom;
                        IronLIR_BlockId b = pred;
                        while (a != b) {
                            /* Walk the one with higher RPO index (further from entry) up */
                            ptrdiff_t ai = hmgeti(rpo_idx, a);
                            ptrdiff_t bi = hmgeti(rpo_idx, b);
                            int a_pos = (ai >= 0) ? rpo_idx[ai].value : rpo_len;
                            int b_pos = (bi >= 0) ? rpo_idx[bi].value : rpo_len;
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
                                ptrdiff_t bi2 = hmgeti(rpo_idx, b);
                                b_pos = (bi2 >= 0) ? rpo_idx[bi2].value : rpo_len;
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

/* Returns true if block 'a' dominates block 'b' in the given idom tree. */
static bool dominates(IronLIR_DomEntry *idom, IronLIR_BlockId a, IronLIR_BlockId b) {
    IronLIR_BlockId cur = b;
    int limit = 1000; /* cycle guard */
    while (limit-- > 0) {
        if (cur == a) return true;
        ptrdiff_t idx = hmgeti(idom, cur);
        if (idx < 0) return false;
        IronLIR_BlockId parent = idom[idx].value;
        if (parent == cur) return (cur == a); /* reached root */
        cur = parent;
    }
    return false;
}

/* Detect loop induction variable in latch block.
 * Looks for STORE to alloca where stored value traces to ADD(LOAD(same_alloca), CONST_INT).
 * Returns the alloca ID, or IRON_LIR_VALUE_INVALID if not found. */
static IronLIR_ValueId detect_indvar(IronLIR_Func *fn, IronLIR_Block *latch) {
    for (int ii = 0; ii < latch->instr_count; ii++) {
        IronLIR_Instr *instr = latch->instrs[ii];
        if (instr->kind != IRON_LIR_STORE) continue;

        IronLIR_ValueId ptr   = instr->store.ptr;
        IronLIR_ValueId val   = instr->store.value;

        /* ptr must be an ALLOCA */
        if ((ptrdiff_t)ptr >= arrlen(fn->value_table) || !fn->value_table[ptr]) continue;
        if (fn->value_table[ptr]->kind != IRON_LIR_ALLOCA) continue;

        /* val must be ADD(LOAD(ptr), CONST_INT) or ADD(CONST_INT, LOAD(ptr)) */
        if ((ptrdiff_t)val >= arrlen(fn->value_table) || !fn->value_table[val]) continue;
        IronLIR_Instr *val_instr = fn->value_table[val];
        if (val_instr->kind != IRON_LIR_ADD) continue;

        IronLIR_ValueId left  = val_instr->binop.left;
        IronLIR_ValueId right = val_instr->binop.right;

        /* Check if left is LOAD(ptr) and right is CONST_INT, or vice versa */
        bool left_is_load_ptr = false;
        bool right_is_load_ptr = false;
        bool right_is_const = false;
        bool left_is_const = false;

        if ((ptrdiff_t)left < arrlen(fn->value_table) && fn->value_table[left]) {
            IronLIR_Instr *li = fn->value_table[left];
            if (li->kind == IRON_LIR_LOAD && li->load.ptr == ptr) left_is_load_ptr = true;
            if (li->kind == IRON_LIR_CONST_INT) left_is_const = true;
        }
        if ((ptrdiff_t)right < arrlen(fn->value_table) && fn->value_table[right]) {
            IronLIR_Instr *ri = fn->value_table[right];
            if (ri->kind == IRON_LIR_LOAD && ri->load.ptr == ptr) right_is_load_ptr = true;
            if (ri->kind == IRON_LIR_CONST_INT) right_is_const = true;
        }

        if ((left_is_load_ptr && right_is_const) ||
            (right_is_load_ptr && left_is_const)) {
            return ptr;
        }
    }
    return IRON_LIR_VALUE_INVALID;
}

/* Build loop info for all natural loops in a function.
 * Returns a stb_ds array of IronLIR_LoopInfo structs.
 * Caller must free: for each loop, hmfree(loop.body_blocks); then arrfree(loops). */
static IronLIR_LoopInfo *build_loop_info(IronLIR_Func *fn, IronLIR_DomEntry *idom,
                                         int *loop_count_out) {
    IronLIR_LoopInfo *loops = NULL;
    *loop_count_out = 0;
    if (!idom || fn->block_count == 0) return NULL;

    /* Find all back edges: b -> s where s dominates b */
    for (int bi = 0; bi < fn->block_count; bi++) {
        IronLIR_Block *blk = fn->blocks[bi];
        for (int si = 0; si < (int)arrlen(blk->succs); si++) {
            IronLIR_BlockId succ_id = blk->succs[si];
            /* Check if succ dominates blk -> back edge */
            if (!dominates(idom, succ_id, blk->id)) continue;

            /* Found back edge: blk is latch, succ_id is header */
            IronLIR_LoopInfo loop;
            memset(&loop, 0, sizeof(loop));
            loop.header    = succ_id;
            loop.latch     = blk->id;
            loop.preheader = 0;
            loop.body_blocks = NULL;
            loop.indvar_alloca = IRON_LIR_VALUE_INVALID;
            loop.indvar_step   = IRON_LIR_VALUE_INVALID;
            loop.indvar_init   = IRON_LIR_VALUE_INVALID;
            loop.parent = NULL;

            /* Collect loop body: all blocks from which latch is reachable
             * without going through the header. Start from latch, walk preds. */
            hmput(loop.body_blocks, succ_id, true);   /* header is in the loop */
            hmput(loop.body_blocks, blk->id, true);   /* latch is in the loop */

            /* Worklist: blocks to process */
            IronLIR_BlockId *worklist = NULL;
            arrput(worklist, blk->id);

            while (arrlen(worklist) > 0) {
                IronLIR_BlockId cur = worklist[arrlen(worklist) - 1];
                arrpop(worklist);
                IronLIR_Block *cur_blk = find_block(fn, cur);
                if (!cur_blk) continue;

                for (int pi = 0; pi < (int)arrlen(cur_blk->preds); pi++) {
                    IronLIR_BlockId pred_id = cur_blk->preds[pi];
                    if (hmgeti(loop.body_blocks, pred_id) < 0) {
                        hmput(loop.body_blocks, pred_id, true);
                        if (pred_id != succ_id) { /* don't go above header */
                            arrput(worklist, pred_id);
                        }
                    }
                }
            }
            arrfree(worklist);

            /* Identify preheader: a predecessor of header NOT in the loop body,
             * with header as its only successor */
            IronLIR_Block *header_blk = find_block(fn, succ_id);
            if (header_blk) {
                int non_loop_pred_count = 0;
                IronLIR_BlockId candidate_preheader = 0;
                for (int pi = 0; pi < (int)arrlen(header_blk->preds); pi++) {
                    IronLIR_BlockId pred_id = header_blk->preds[pi];
                    if (hmgeti(loop.body_blocks, pred_id) < 0) {
                        /* This predecessor is outside the loop */
                        non_loop_pred_count++;
                        candidate_preheader = pred_id;
                    }
                }
                if (non_loop_pred_count == 1) {
                    /* Check that the candidate preheader's only successor is the header */
                    IronLIR_Block *pre_blk = find_block(fn, candidate_preheader);
                    if (pre_blk && arrlen(pre_blk->succs) == 1 &&
                        pre_blk->succs[0] == succ_id) {
                        loop.preheader = candidate_preheader;
                    }
                }
            }

            /* Detect induction variable from latch block */
            IronLIR_Block *latch_blk = find_block(fn, blk->id);
            if (latch_blk) {
                loop.indvar_alloca = detect_indvar(fn, latch_blk);
            }

            /* Find initial value: look for STORE to indvar_alloca in preheader or entry */
            if (loop.indvar_alloca != IRON_LIR_VALUE_INVALID) {
                /* Check preheader first, then entry block */
                IronLIR_BlockId search_blocks[2];
                int n_search = 0;
                if (loop.preheader) search_blocks[n_search++] = loop.preheader;
                if (fn->blocks[0]->id != loop.header &&
                    fn->blocks[0]->id != loop.preheader) {
                    search_blocks[n_search++] = fn->blocks[0]->id;
                }

                for (int sb = 0; sb < n_search && loop.indvar_init == IRON_LIR_VALUE_INVALID; sb++) {
                    IronLIR_Block *search_blk = find_block(fn, search_blocks[sb]);
                    if (!search_blk) continue;
                    for (int ii = 0; ii < search_blk->instr_count; ii++) {
                        IronLIR_Instr *si = search_blk->instrs[ii];
                        if (si->kind == IRON_LIR_STORE &&
                            si->store.ptr == loop.indvar_alloca) {
                            loop.indvar_init = si->store.value;
                            break;
                        }
                    }
                }
            }

            arrput(loops, loop);
            (*loop_count_out)++;
        }
    }

    /* Build nested loop tree: if header H1 of loop L1 is in body of loop L2,
     * then L1.parent = &L2 */
    for (int i = 0; i < *loop_count_out; i++) {
        for (int j = 0; j < *loop_count_out; j++) {
            if (i == j) continue;
            /* Is loops[i].header in loops[j].body_blocks? */
            if (hmgeti(loops[j].body_blocks, loops[i].header) >= 0 &&
                loops[i].header != loops[j].header) {
                /* loops[i] is nested inside loops[j] */
                if (!loops[i].parent) {
                    loops[i].parent = &loops[j];
                }
            }
        }
    }

    return loops;
}

/* Returns true if the value 'val' is loop-invariant with respect to 'loop'.
 * A value is invariant if its defining instruction is not inside the loop body,
 * or if it is a constant.
 * Special case: LOAD from an alloca that has NO stores in the loop body is invariant. */
static bool is_loop_invariant(IronLIR_Func *fn, IronLIR_LoopInfo *loop, IronLIR_ValueId val) {
    if (val == IRON_LIR_VALUE_INVALID) return false;
    if ((ptrdiff_t)val >= arrlen(fn->value_table)) return false;
    IronLIR_Instr *def = fn->value_table[val];
    if (!def) return false;

    /* Constants are always invariant */
    switch (def->kind) {
    case IRON_LIR_CONST_INT:
    case IRON_LIR_CONST_FLOAT:
    case IRON_LIR_CONST_BOOL:
    case IRON_LIR_CONST_STRING:
    case IRON_LIR_CONST_NULL:
        return true;
    default:
        break;
    }

    /* Find which block defines this value */
    IronLIR_BlockId def_block = 0;
    bool found = false;
    for (int bi = 0; bi < fn->block_count && !found; bi++) {
        IronLIR_Block *blk = fn->blocks[bi];
        for (int ii = 0; ii < blk->instr_count; ii++) {
            if (blk->instrs[ii] == def) {
                def_block = blk->id;
                found = true;
                break;
            }
        }
    }
    if (!found) return false;

    /* If defining block is NOT in the loop body, it's invariant */
    if (hmgeti(loop->body_blocks, def_block) < 0) return true;

    /* Special case: LOAD from an alloca with no stores inside the loop body */
    if (def->kind == IRON_LIR_LOAD) {
        IronLIR_ValueId ptr = def->load.ptr;
        if ((ptrdiff_t)ptr < arrlen(fn->value_table) && fn->value_table[ptr] &&
            fn->value_table[ptr]->kind == IRON_LIR_ALLOCA) {
            /* Count stores to this alloca inside the loop body */
            int store_count = 0;
            for (int bi = 0; bi < fn->block_count; bi++) {
                IronLIR_Block *blk = fn->blocks[bi];
                if (hmgeti(loop->body_blocks, blk->id) < 0) continue;
                for (int ii = 0; ii < blk->instr_count; ii++) {
                    IronLIR_Instr *instr = blk->instrs[ii];
                    if (instr->kind == IRON_LIR_STORE && instr->store.ptr == ptr) {
                        store_count++;
                    }
                }
            }
            if (store_count == 0) return true;
        }
    }

    return false;
}

/* Check if instr is a MUL where one operand traces to the loop induction variable
 * (via LOAD of indvar_alloca) and the other is loop-invariant.
 * If so, sets *out_invariant to the invariant operand ID and returns true. */
static bool detect_affine_mul(IronLIR_Func *fn, IronLIR_LoopInfo *loop,
                               IronLIR_Instr *instr, IronLIR_ValueId *out_invariant) {
    if (instr->kind != IRON_LIR_MUL) return false;
    if (loop->indvar_alloca == IRON_LIR_VALUE_INVALID) return false;

    IronLIR_ValueId left  = instr->binop.left;
    IronLIR_ValueId right = instr->binop.right;

    /* Trace through LOADs: check if an operand loads from indvar_alloca */
    bool left_is_indvar = false;
    bool right_is_indvar = false;

    if ((ptrdiff_t)left < arrlen(fn->value_table) && fn->value_table[left]) {
        IronLIR_Instr *li = fn->value_table[left];
        if (li->kind == IRON_LIR_LOAD && li->load.ptr == loop->indvar_alloca) {
            left_is_indvar = true;
        }
    }
    if ((ptrdiff_t)right < arrlen(fn->value_table) && fn->value_table[right]) {
        IronLIR_Instr *ri = fn->value_table[right];
        if (ri->kind == IRON_LIR_LOAD && ri->load.ptr == loop->indvar_alloca) {
            right_is_indvar = true;
        }
    }

    if (left_is_indvar && is_loop_invariant(fn, loop, right)) {
        *out_invariant = right;
        return true;
    }
    if (right_is_indvar && is_loop_invariant(fn, loop, left)) {
        *out_invariant = left;
        return true;
    }
    return false;
}

/* Create a new binop instruction (without appending to any block).
 * Assigns a new value ID and grows value_table. */
static IronLIR_Instr *make_binop_instr(IronLIR_Func *fn, IronLIR_InstrKind kind,
                                       IronLIR_ValueId left, IronLIR_ValueId right,
                                       Iron_Type *type, Iron_Span span) {
    IronLIR_Instr *instr = ARENA_ALLOC(fn->arena, IronLIR_Instr);
    memset(instr, 0, sizeof(*instr));
    instr->kind         = kind;
    instr->type         = type;
    instr->span         = span;
    instr->binop.left   = left;
    instr->binop.right  = right;
    instr->id           = fn->next_value_id++;
    while (arrlen(fn->value_table) <= (ptrdiff_t)instr->id) {
        arrput(fn->value_table, NULL);
    }
    fn->value_table[instr->id] = instr;
    return instr;
}

/* Create a new LOAD instruction (without appending to any block). */
static IronLIR_Instr *make_load_instr(IronLIR_Func *fn, IronLIR_ValueId ptr,
                                      Iron_Type *type, Iron_Span span) {
    IronLIR_Instr *instr = ARENA_ALLOC(fn->arena, IronLIR_Instr);
    memset(instr, 0, sizeof(*instr));
    instr->kind     = IRON_LIR_LOAD;
    instr->type     = type;
    instr->span     = span;
    instr->load.ptr = ptr;
    instr->id       = fn->next_value_id++;
    while (arrlen(fn->value_table) <= (ptrdiff_t)instr->id) {
        arrput(fn->value_table, NULL);
    }
    fn->value_table[instr->id] = instr;
    return instr;
}

/* Insert a pre-built instruction at the START of a block (after existing ALLOCAs). */
static void insert_alloca_into_entry(IronLIR_Block *block, IronLIR_Instr *alloca_instr) {
    /* Find insertion point: after the last ALLOCA */
    int insert_idx = 0;
    for (int i = 0; i < block->instr_count; i++) {
        if (block->instrs[i]->kind == IRON_LIR_ALLOCA) {
            insert_idx = i + 1;
        } else {
            break;
        }
    }

    /* Shift instructions to make room */
    arrput(block->instrs, NULL);
    block->instr_count++;
    for (int i = block->instr_count - 1; i > insert_idx; i--) {
        block->instrs[i] = block->instrs[i - 1];
    }
    block->instrs[insert_idx] = alloca_instr;
}

/* Dedup key for strength reduction: (invariant_val, loop_header) */
typedef struct { IronLIR_ValueId inv_val; IronLIR_BlockId loop_header; } SRDedupKey;
typedef struct { SRDedupKey key; IronLIR_ValueId value; } SRDedupEntry;

static bool sr_dedup_key_eq(SRDedupKey a, SRDedupKey b) {
    return a.inv_val == b.inv_val && a.loop_header == b.loop_header;
}

/* Run strength reduction pass on the module.
 * For each natural loop with a valid preheader and induction variable:
 * - Scan body blocks for MUL(indvar_load, invariant)
 * - Replace with a new induction variable ALLOCA:
 *     preheader: store iv_alloca, init_val * invariant
 *     latch:     iv = iv + invariant (before terminator)
 *     body:      replace MUL result with LOAD iv_alloca
 * Returns true if any changes were made. */
static bool run_strength_reduction(IronLIR_Module *module) {
    bool any_changed = false;

    for (int fi = 0; fi < module->func_count; fi++) {
        IronLIR_Func *fn = module->funcs[fi];
        if (fn->is_extern || fn->block_count == 0) continue;

        /* Rebuild CFG edges (preds/succs) by scanning terminators.
         * The IR constructors do not auto-populate these arrays. */
        rebuild_cfg_edges(fn);

        /* Build dominator tree */
        IronLIR_DomEntry *idom = build_domtree(fn);
        if (!idom) continue;

        /* Build loop info */
        int loop_count = 0;
        IronLIR_LoopInfo *loops = build_loop_info(fn, idom, &loop_count);

        bool fn_changed = false;

        for (int li = 0; li < loop_count; li++) {
            IronLIR_LoopInfo *loop = &loops[li];

            /* Skip loops without preheader or induction variable */
            if (!loop->preheader) continue;
            if (loop->indvar_alloca == IRON_LIR_VALUE_INVALID) continue;
            if (loop->indvar_init == IRON_LIR_VALUE_INVALID) continue;

            /* Get int type from indvar alloca */
            IronLIR_Instr *iv_alloca_instr = fn->value_table[loop->indvar_alloca];
            if (!iv_alloca_instr) continue;
            Iron_Type *int_type = iv_alloca_instr->alloca.alloc_type;
            Iron_Span span      = iv_alloca_instr->span;

            /* Collect all MUL instructions in loop body blocks that match the pattern */
            /* Dedup: same (invariant_val, loop_header) -> same new IV alloca */
            SRDedupEntry *dedup_map = NULL;  /* (inv_val, header) -> iv_alloca_id */
            ValueReplEntry *repl_map = NULL; /* MUL_result_id -> LOAD_of_iv_id */

            for (int bi = 0; bi < fn->block_count; bi++) {
                IronLIR_Block *blk = fn->blocks[bi];
                /* Only process blocks in the loop body (not the latch itself for MULs) */
                if (hmgeti(loop->body_blocks, blk->id) < 0) continue;
                if (blk->id == loop->latch) continue; /* skip latch for MUL search */

                for (int ii = 0; ii < blk->instr_count; ii++) {
                    IronLIR_Instr *instr = blk->instrs[ii];
                    IronLIR_ValueId inv_val = IRON_LIR_VALUE_INVALID;

                    if (!detect_affine_mul(fn, loop, instr, &inv_val)) continue;

                    /* Check dedup: has this (inv_val, loop->header) been created? */
                    SRDedupKey dk; dk.inv_val = inv_val; dk.loop_header = loop->header;

                    IronLIR_ValueId new_iv_alloca = IRON_LIR_VALUE_INVALID;
                    bool found_dedup = false;

                    for (int di = 0; di < (int)arrlen(dedup_map); di++) {
                        if (sr_dedup_key_eq(dedup_map[di].key, dk)) {
                            new_iv_alloca = dedup_map[di].value;
                            found_dedup = true;
                            break;
                        }
                    }

                    if (!found_dedup) {
                        /* Create new ALLOCA for the derived induction variable */
                        IronLIR_Instr *new_alloca = make_alloca_instr(fn, int_type, span);
                        new_iv_alloca = new_alloca->id;

                        /* Create a step alloca to hold the loop-invariant step value.
                         * This ensures the step value (inv_val, which may be defined in
                         * a body block) is hoisted to a location visible from the latch. */
                        IronLIR_Instr *step_alloca = make_alloca_instr(fn, int_type, span);
                        IronLIR_ValueId step_alloca_id = step_alloca->id;

                        /* Insert both ALLOCAs into entry block after existing ALLOCAs */
                        IronLIR_Block *entry_blk = fn->blocks[0];
                        insert_alloca_into_entry(entry_blk, new_alloca);
                        insert_alloca_into_entry(entry_blk, step_alloca);

                        /* In the preheader block (before its terminator):
                         *   - store step_alloca, inv_val         (hoist step to entry scope)
                         *   - init_mul = MUL indvar_init, inv_val
                         *   - store new_iv_alloca, init_mul
                         * We must use inv_val for the step_store here since inv_val IS
                         * in scope in the preheader (the preheader dominates the loop body
                         * where inv_val is defined — no, wait, inv_val might be defined
                         * in the body block which comes AFTER the preheader).
                         * Safe approach: if inv_val's defining block is in the loop body,
                         * we need to find the value in the preheader.
                         * For CONST_INT/CONST_BOOL/CONST_FLOAT: create a new const in preheader.
                         * For other loop-invariant values: copy from their alloca. */
                        IronLIR_Block *preheader_blk = find_block(fn, loop->preheader);
                        IronLIR_Block *latch_blk     = find_block(fn, loop->latch);

                        /* Determine the safe step value to use in preheader and latch.
                         * inv_val may be defined in the body block; hoist it by copying
                         * to step_alloca in the preheader if it's a constant, or by
                         * finding its alloca source if it's a LOAD from an invariant alloca. */
                        IronLIR_ValueId safe_step_val = inv_val; /* default: use directly */

                        /* Check if inv_val's defining instruction is in a loop body block */
                        bool inv_in_body = false;
                        if ((ptrdiff_t)inv_val < arrlen(fn->value_table) && fn->value_table[inv_val]) {
                            IronLIR_Instr *inv_def = fn->value_table[inv_val];
                            for (int xbi = 0; xbi < fn->block_count; xbi++) {
                                IronLIR_Block *xblk = fn->blocks[xbi];
                                if (hmgeti(loop->body_blocks, xblk->id) < 0) continue;
                                for (int xii = 0; xii < xblk->instr_count; xii++) {
                                    if (xblk->instrs[xii] == inv_def) {
                                        inv_in_body = true;
                                        break;
                                    }
                                }
                                if (inv_in_body) break;
                            }
                        }

                        if (inv_in_body) {
                            /* inv_val is defined inside the loop body. We need a preheader-safe
                             * copy of the step value. For CONST_INT, create a new const in entry. */
                            IronLIR_Instr *inv_def = fn->value_table[inv_val];
                            if (inv_def && inv_def->kind == IRON_LIR_CONST_INT) {
                                /* Create a fresh CONST_INT in entry block for the step */
                                IronLIR_Instr *step_const = ARENA_ALLOC(fn->arena, IronLIR_Instr);
                                memset(step_const, 0, sizeof(*step_const));
                                step_const->kind = IRON_LIR_CONST_INT;
                                step_const->type = int_type;
                                step_const->span = span;
                                step_const->const_int.value = inv_def->const_int.value;
                                step_const->id = fn->next_value_id++;
                                while (arrlen(fn->value_table) <= (ptrdiff_t)step_const->id) {
                                    arrput(fn->value_table, NULL);
                                }
                                fn->value_table[step_const->id] = step_const;
                                /* Insert into entry block at start (after ALLOCAs) */
                                IronLIR_Instr *step_store_pre = make_store_instr(fn, step_alloca_id,
                                                                                  step_const->id, span);
                                insert_store_before_terminator_instr(preheader_blk, step_const);
                                insert_store_before_terminator_instr(preheader_blk, step_store_pre);
                                safe_step_val = step_const->id;
                            } else {
                                /* For other invariant types (LOAD from invariant alloca, etc.):
                                 * store inv_val to step_alloca in the body block's predecessor.
                                 * Since we can't safely use inv_val in the preheader,
                                 * skip this transformation. */
                                /* Free the allocated ALLOCAs by nulling them (DCE will clean up) */
                                fn->value_table[new_iv_alloca] = NULL;
                                fn->value_table[step_alloca_id] = NULL;
                                continue;
                            }
                        } else {
                            /* inv_val is defined outside the loop body (safe to reference). */
                            IronLIR_Instr *step_store_pre = make_store_instr(fn, step_alloca_id,
                                                                              inv_val, span);
                            insert_store_before_terminator_instr(preheader_blk, step_store_pre);
                        }

                        /* In preheader: init_mul = MUL(indvar_init, safe_step_val),
                         *               store new_iv_alloca, init_mul */
                        IronLIR_Instr *init_mul = make_binop_instr(fn, IRON_LIR_MUL,
                                                                    loop->indvar_init,
                                                                    safe_step_val, int_type, span);
                        IronLIR_Instr *init_store = make_store_instr(fn, new_iv_alloca,
                                                                      init_mul->id, span);
                        insert_store_before_terminator_instr(preheader_blk, init_mul);
                        insert_store_before_terminator_instr(preheader_blk, init_store);

                        /* In the latch block, before its terminator:
                         *   load_iv   = LOAD new_iv_alloca
                         *   load_step = LOAD step_alloca
                         *   stepped   = ADD load_iv, load_step
                         *   STORE new_iv_alloca, stepped */
                        IronLIR_Instr *load_iv   = make_load_instr(fn, new_iv_alloca,
                                                                    int_type, span);
                        IronLIR_Instr *load_step = make_load_instr(fn, step_alloca_id,
                                                                    int_type, span);
                        IronLIR_Instr *stepped   = make_binop_instr(fn, IRON_LIR_ADD,
                                                                     load_iv->id, load_step->id,
                                                                     int_type, span);
                        IronLIR_Instr *store_iv  = make_store_instr(fn, new_iv_alloca,
                                                                     stepped->id, span);

                        insert_store_before_terminator_instr(latch_blk, load_iv);
                        insert_store_before_terminator_instr(latch_blk, load_step);
                        insert_store_before_terminator_instr(latch_blk, stepped);
                        insert_store_before_terminator_instr(latch_blk, store_iv);

                        /* Record in dedup map */
                        SRDedupEntry de; de.key = dk; de.value = new_iv_alloca;
                        arrput(dedup_map, de);
                    }

                    /* Rewrite the MUL instruction in-place to a LOAD of the new IV alloca.
                     * The MUL's value ID stays the same so all existing uses remain valid.
                     * Note: load.ptr overlaps with binop.left in the union — set kind
                     * BEFORE writing load.ptr to avoid the union alias issue. */
                    instr->kind     = IRON_LIR_LOAD;
                    instr->load.ptr = new_iv_alloca;
                    /* binop.right is not aliased by any load field; clear it for cleanliness.
                     * binop.left is the same as load.ptr — do NOT clear it after setting ptr. */
                    instr->binop.right = IRON_LIR_VALUE_INVALID;
                    (void)repl_map; /* no external replacements needed — rewrite in place */

                    fn_changed = true;
                }
            }

            /* Free dedup map (it's a plain array, not a stb_ds hashmap) */
            arrfree(dedup_map);
            hmfree(repl_map);
        }

        /* Free loop info */
        for (int li = 0; li < loop_count; li++) {
            hmfree(loops[li].body_blocks);
        }
        arrfree(loops);
        hmfree(idom);

        if (fn_changed) any_changed = true;
    }

    return any_changed;
}

/* ── Public API ───────────────────────────────────────────────────────────── */

bool iron_lir_instr_is_pure(IronLIR_InstrKind kind) {
    switch (kind) {
    /* Pure — no side effects */
    case IRON_LIR_CONST_INT: case IRON_LIR_CONST_FLOAT:
    case IRON_LIR_CONST_BOOL: case IRON_LIR_CONST_STRING:
    case IRON_LIR_CONST_NULL:
    case IRON_LIR_ADD: case IRON_LIR_SUB: case IRON_LIR_MUL:
    case IRON_LIR_DIV: case IRON_LIR_MOD:
    case IRON_LIR_EQ: case IRON_LIR_NEQ: case IRON_LIR_LT:
    case IRON_LIR_LTE: case IRON_LIR_GT: case IRON_LIR_GTE:
    case IRON_LIR_AND: case IRON_LIR_OR:
    case IRON_LIR_NEG: case IRON_LIR_NOT:
    case IRON_LIR_LOAD: case IRON_LIR_CAST:
    case IRON_LIR_GET_FIELD: case IRON_LIR_GET_INDEX:
    case IRON_LIR_CONSTRUCT: case IRON_LIR_ARRAY_LIT:
    case IRON_LIR_IS_NULL: case IRON_LIR_IS_NOT_NULL:
    case IRON_LIR_SLICE:
    case IRON_LIR_MAKE_CLOSURE: case IRON_LIR_FUNC_REF:
        return true;
    /* Side-effecting — everything else */
    default:
        return false;
    }
}

/* ── Function inlining pass ───────────────────────────────────────────────── */

/* Count total instructions in a function (for size threshold). */
static int count_func_instructions(IronLIR_Func *fn) {
    int count = 0;
    for (int bi = 0; bi < fn->block_count; bi++)
        count += fn->blocks[bi]->instr_count;
    return count;
}

/* Check whether a function directly calls itself (recursion guard). */
static bool func_has_self_call(IronLIR_Func *fn) {
    for (int bi = 0; bi < fn->block_count; bi++) {
        IronLIR_Block *blk = fn->blocks[bi];
        for (int ii = 0; ii < blk->instr_count; ii++) {
            IronLIR_Instr *instr = blk->instrs[ii];
            if (instr->kind != IRON_LIR_CALL) continue;
            const char *callee_name = NULL;
            if (instr->call.func_decl) {
                callee_name = instr->call.func_decl->name;
            } else if (instr->call.func_ptr != IRON_LIR_VALUE_INVALID) {
                IronLIR_ValueId fptr = instr->call.func_ptr;
                if (fptr < (IronLIR_ValueId)arrlen(fn->value_table) &&
                    fn->value_table[fptr] &&
                    fn->value_table[fptr]->kind == IRON_LIR_FUNC_REF) {
                    callee_name = fn->value_table[fptr]->func_ref.func_name;
                }
            }
            if (callee_name && strcmp(callee_name, fn->name) == 0) return true;
        }
    }
    return false;
}

/* Inline a single CALL instruction into its containing function.
 *
 * call_block     — the block containing the CALL
 * call_idx       — index of the CALL instruction in call_block->instrs
 * call_instr     — the CALL instruction itself
 * callee         — the function to inline
 *
 * Algorithm overview:
 *   1. Pre-populate id_remap with callee param synthetic IDs -> call arg IDs.
 *   2. Insert result ALLOCA into call_block BEFORE call_idx (so it's declared
 *      before the cloned body in C emission order).
 *   3. Clone callee blocks; assign fresh caller IDs; build id_remap.
 *   4. Apply id_remap to all cloned instructions.
 *   5. Apply block_remap to cloned terminators.
 *   6. Create merge block (LOAD from result alloca).
 *   7. Replace RETURN in cloned blocks with STORE+JUMP to merge.
 *   8. Split call_block at CALL site; wire JUMP edges.
 *   9. Propagate load_result_id -> call_id replacement to continuation.
 */
static void inline_call_site(IronLIR_Func *fn,
                              IronLIR_Block *call_block,
                              int call_idx,
                              IronLIR_Instr *call_instr,
                              IronLIR_Func *callee) {
    /* ── Remap tables ──────────────────────────────────────────────────── */
    ValueReplEntry *id_remap    = NULL;
    BlockIdRemap   *block_remap = NULL;

    /* ── Step 1: Pre-populate param synthetic IDs ──────────────────────── */
    /*
     * In the callee, parameters occupy synthetic ValueIds 1..param_count.
     * These IDs have no backing instruction (value_table[id] = NULL) and are
     * never seen as the "id" field of any instruction.  We must map them to
     * the corresponding call arguments NOW, before cloning, so that
     * apply_replacements can rewrite param-uses in the cloned body.
     *
     * We discover the param-value IDs by scanning STORE instructions in the
     * callee entry block: each `store alloca_id, param_val_id` reveals a
     * param_val_id.  We collect them in declaration order and map each to
     * the matching call argument.
     */
    if (callee->param_count > 0 && call_instr->call.arg_count == callee->param_count) {
        IronLIR_Block *entry_blk = callee->block_count > 0 ? callee->blocks[0] : NULL;
        if (entry_blk) {
            int found = 0;
            for (int ii = 0; ii < entry_blk->instr_count && found < callee->param_count; ii++) {
                IronLIR_Instr *instr = entry_blk->instrs[ii];
                if (instr->kind == IRON_LIR_STORE) {
                    /* store alloca_id, param_val_id */
                    IronLIR_ValueId pval = instr->store.value;
                    /* Verify pval is a synthetic param ID (value_table[pval] == NULL
                     * and pval is in range 1..param_count) */
                    if (pval >= 1 && pval <= (IronLIR_ValueId)callee->param_count &&
                        (ptrdiff_t)pval < arrlen(callee->value_table) &&
                        callee->value_table[pval] == NULL) {
                        hmput(id_remap, pval, call_instr->call.args[found]);
                        found++;
                    }
                }
            }
        }
    }

    /* ── Step 2: Create result ALLOCA in call_block before call_idx ─────── */
    /*
     * The result alloca MUST be declared before the cloned callee blocks in
     * C emission order, because cloned RETURN->STORE instructions reference
     * it.  We insert it into call_block at position call_idx (shifting the
     * CALL instruction one slot right), which keeps it in call_block (before
     * the JUMP to cloned entry after the split).
     */
    bool non_void = (callee->return_type != NULL) &&
                    (call_instr->id != IRON_LIR_VALUE_INVALID);

    IronLIR_ValueId result_alloca_id = IRON_LIR_VALUE_INVALID;
    IronLIR_ValueId load_result_id   = IRON_LIR_VALUE_INVALID;

    if (non_void) {
        IronLIR_Instr *result_alloca = ARENA_ALLOC(fn->arena, IronLIR_Instr);
        memset(result_alloca, 0, sizeof(*result_alloca));
        result_alloca->kind              = IRON_LIR_ALLOCA;
        result_alloca->type              = callee->return_type;
        result_alloca->span              = call_instr->span;
        result_alloca->alloca.alloc_type = callee->return_type;
        result_alloca->alloca.name_hint  = NULL;
        result_alloca->id = fn->next_value_id++;
        while (arrlen(fn->value_table) <= (ptrdiff_t)result_alloca->id)
            arrput(fn->value_table, NULL);
        fn->value_table[result_alloca->id] = result_alloca;
        result_alloca_id = result_alloca->id;

        /* Insert at call_idx: shift CALL and everything after it right by 1 */
        arrput(call_block->instrs, NULL);
        call_block->instr_count++;
        for (int k = call_block->instr_count - 1; k > call_idx; k--)
            call_block->instrs[k] = call_block->instrs[k - 1];
        call_block->instrs[call_idx] = result_alloca;
        call_idx++;  /* CALL is now at call_idx (after the alloca) */
    }

    /* ── Step 3: Clone callee blocks into caller ────────────────────────── */
    IronLIR_BlockId cloned_entry_id  = IRON_LIR_BLOCK_INVALID;
    int cloned_block_start = fn->block_count;  /* index where cloned blocks start */

    for (int bi = 0; bi < callee->block_count; bi++) {
        IronLIR_Block *src_blk = callee->blocks[bi];

        IronLIR_Block *new_blk = ARENA_ALLOC(fn->arena, IronLIR_Block);
        memset(new_blk, 0, sizeof(*new_blk));
        new_blk->id    = fn->next_block_id++;
        new_blk->label = src_blk->label ? src_blk->label : "inline_body";

        hmput(block_remap, src_blk->id, new_blk->id);
        if (bi == 0) cloned_entry_id = new_blk->id;

        for (int ii = 0; ii < src_blk->instr_count; ii++) {
            IronLIR_Instr *src    = src_blk->instrs[ii];
            IronLIR_Instr *cloned = ARENA_ALLOC(fn->arena, IronLIR_Instr);
            memcpy(cloned, src, sizeof(*src));

            /* Deep-copy stb_ds arrays owned by the instruction */
            if (src->kind == IRON_LIR_CALL && src->call.arg_count > 0 && src->call.args) {
                cloned->call.args = NULL;
                for (int ai = 0; ai < src->call.arg_count; ai++)
                    arrput(cloned->call.args, src->call.args[ai]);
            }
            if (src->kind == IRON_LIR_CONSTRUCT && src->construct.field_count > 0 && src->construct.field_vals) {
                cloned->construct.field_vals = NULL;
                for (int fi2 = 0; fi2 < src->construct.field_count; fi2++)
                    arrput(cloned->construct.field_vals, src->construct.field_vals[fi2]);
            }
            if (src->kind == IRON_LIR_ARRAY_LIT && src->array_lit.element_count > 0 && src->array_lit.elements) {
                cloned->array_lit.elements = NULL;
                for (int ei = 0; ei < src->array_lit.element_count; ei++)
                    arrput(cloned->array_lit.elements, src->array_lit.elements[ei]);
            }
            if (src->kind == IRON_LIR_INTERP_STRING && src->interp_string.part_count > 0 && src->interp_string.parts) {
                cloned->interp_string.parts = NULL;
                for (int pi2 = 0; pi2 < src->interp_string.part_count; pi2++)
                    arrput(cloned->interp_string.parts, src->interp_string.parts[pi2]);
            }
            if (src->kind == IRON_LIR_MAKE_CLOSURE && src->make_closure.capture_count > 0 && src->make_closure.captures) {
                cloned->make_closure.captures = NULL;
                for (int ci2 = 0; ci2 < src->make_closure.capture_count; ci2++)
                    arrput(cloned->make_closure.captures, src->make_closure.captures[ci2]);
            }
            if (src->kind == IRON_LIR_PARALLEL_FOR && src->parallel_for.capture_count > 0 && src->parallel_for.captures) {
                cloned->parallel_for.captures = NULL;
                for (int ci2 = 0; ci2 < src->parallel_for.capture_count; ci2++)
                    arrput(cloned->parallel_for.captures, src->parallel_for.captures[ci2]);
            }

            /* Assign fresh value ID (Pitfall 1: ID must not collide with caller). */
            if (src->id != IRON_LIR_VALUE_INVALID) {
                IronLIR_ValueId old_id = src->id;
                cloned->id = fn->next_value_id++;
                while (arrlen(fn->value_table) <= (ptrdiff_t)cloned->id)
                    arrput(fn->value_table, NULL);
                fn->value_table[cloned->id] = cloned;
                hmput(id_remap, old_id, cloned->id);
            } else {
                cloned->id = IRON_LIR_VALUE_INVALID;
            }

            arrput(new_blk->instrs, cloned);
            new_blk->instr_count++;
        }

        arrput(fn->blocks, new_blk);
        fn->block_count++;
    }

    /* ── Step 4: Apply value ID remap to all cloned instructions ─────────── */
    for (int bi = cloned_block_start; bi < fn->block_count; bi++) {
        IronLIR_Block *blk = fn->blocks[bi];
        for (int ii = 0; ii < blk->instr_count; ii++) {
            apply_replacements(blk->instrs[ii], id_remap);
        }
    }

    /* ── Step 5: Apply block ID remap to terminators in cloned blocks ─────── */
    for (int bi = cloned_block_start; bi < fn->block_count; bi++) {
        IronLIR_Block *blk = fn->blocks[bi];
        for (int ii = 0; ii < blk->instr_count; ii++) {
            IronLIR_Instr *instr = blk->instrs[ii];
            if (instr->kind == IRON_LIR_JUMP) {
                ptrdiff_t idx = hmgeti(block_remap, instr->jump.target);
                if (idx >= 0) instr->jump.target = block_remap[idx].value;
            } else if (instr->kind == IRON_LIR_BRANCH) {
                ptrdiff_t idx;
                idx = hmgeti(block_remap, instr->branch.then_block);
                if (idx >= 0) instr->branch.then_block = block_remap[idx].value;
                idx = hmgeti(block_remap, instr->branch.else_block);
                if (idx >= 0) instr->branch.else_block = block_remap[idx].value;
            } else if (instr->kind == IRON_LIR_SWITCH) {
                ptrdiff_t idx;
                idx = hmgeti(block_remap, instr->sw.default_block);
                if (idx >= 0) instr->sw.default_block = block_remap[idx].value;
                for (int ci2 = 0; ci2 < instr->sw.case_count; ci2++) {
                    idx = hmgeti(block_remap, instr->sw.case_blocks[ci2]);
                    if (idx >= 0) instr->sw.case_blocks[ci2] = block_remap[idx].value;
                }
            }
        }
    }

    /* ── Step 6: Create merge block ───────────────────────────────────────── */
    /*
     * The merge block receives control flow from each cloned RETURN site.
     * If non-void: it contains only a LOAD from result_alloca and a JUMP to
     * the continuation block (the LOAD is added here; JUMP added in step 8).
     */
    IronLIR_Block *merge = ARENA_ALLOC(fn->arena, IronLIR_Block);
    memset(merge, 0, sizeof(*merge));
    merge->id    = fn->next_block_id++;
    merge->label = "inline_merge";

    if (non_void) {
        IronLIR_Instr *load_result = ARENA_ALLOC(fn->arena, IronLIR_Instr);
        memset(load_result, 0, sizeof(*load_result));
        load_result->kind     = IRON_LIR_LOAD;
        load_result->type     = callee->return_type;
        load_result->span     = call_instr->span;
        load_result->load.ptr = result_alloca_id;
        load_result->id = fn->next_value_id++;
        while (arrlen(fn->value_table) <= (ptrdiff_t)load_result->id)
            arrput(fn->value_table, NULL);
        fn->value_table[load_result->id] = load_result;
        load_result_id = load_result->id;

        arrput(merge->instrs, load_result);
        merge->instr_count++;
    }

    arrput(fn->blocks, merge);
    fn->block_count++;

    /* ── Step 7: Replace RETURN in cloned blocks with STORE + JUMP ─────────── */
    /*
     * By this point, apply_replacements has already run on cloned instrs, so
     * ret.value is in caller ID space.
     */
    for (int bi = cloned_block_start; bi < fn->block_count - 1; bi++) {  /* -1 to skip merge */
        IronLIR_Block *blk = fn->blocks[bi];
        for (int ii = 0; ii < blk->instr_count; ii++) {
            IronLIR_Instr *instr = blk->instrs[ii];
            if (instr->kind != IRON_LIR_RETURN) continue;

            IronLIR_Instr *jump_to_merge = ARENA_ALLOC(fn->arena, IronLIR_Instr);
            memset(jump_to_merge, 0, sizeof(*jump_to_merge));
            jump_to_merge->kind        = IRON_LIR_JUMP;
            jump_to_merge->id          = IRON_LIR_VALUE_INVALID;
            jump_to_merge->jump.target = merge->id;
            jump_to_merge->span        = instr->span;

            if (non_void && !instr->ret.is_void) {
                IronLIR_Instr *store_ret = ARENA_ALLOC(fn->arena, IronLIR_Instr);
                memset(store_ret, 0, sizeof(*store_ret));
                store_ret->kind        = IRON_LIR_STORE;
                store_ret->id          = IRON_LIR_VALUE_INVALID;
                store_ret->span        = instr->span;
                store_ret->store.ptr   = result_alloca_id;
                store_ret->store.value = instr->ret.value;

                blk->instrs[ii] = store_ret;
                /* Insert JUMP immediately after STORE */
                arrput(blk->instrs, NULL);
                blk->instr_count++;
                for (int k = blk->instr_count - 1; k > ii + 1; k--)
                    blk->instrs[k] = blk->instrs[k - 1];
                blk->instrs[ii + 1] = jump_to_merge;
            } else {
                blk->instrs[ii] = jump_to_merge;
            }
            break;  /* only one terminator per block */
        }
    }

    /* ── Step 8: Split caller block at the CALL site ─────────────────────── */
    /*
     * call_block layout at this point (call_idx adjusted above if non_void):
     *   [0 .. call_idx-1]           — pre-call instrs (+ result alloca if non_void)
     *   [call_idx]                  — the CALL instr  (to be removed)
     *   [call_idx+1 .. end]         — post-call instrs -> go to cont block
     */
    IronLIR_Block *cont = ARENA_ALLOC(fn->arena, IronLIR_Block);
    memset(cont, 0, sizeof(*cont));
    cont->id    = fn->next_block_id++;
    cont->label = "inline_cont";

    for (int k = call_idx + 1; k < call_block->instr_count; k++) {
        arrput(cont->instrs, call_block->instrs[k]);
        cont->instr_count++;
    }

    /* Truncate call_block to pre-call instructions (dropping CALL) */
    call_block->instr_count = call_idx;
    arrsetlen(call_block->instrs, (ptrdiff_t)call_idx);

    /* JUMP from call_block to cloned callee entry */
    IronLIR_Instr *jump_to_entry = ARENA_ALLOC(fn->arena, IronLIR_Instr);
    memset(jump_to_entry, 0, sizeof(*jump_to_entry));
    jump_to_entry->kind        = IRON_LIR_JUMP;
    jump_to_entry->id          = IRON_LIR_VALUE_INVALID;
    jump_to_entry->jump.target = cloned_entry_id;
    jump_to_entry->span        = call_instr->span;
    arrput(call_block->instrs, jump_to_entry);
    call_block->instr_count++;

    /* JUMP from merge to continuation */
    IronLIR_Instr *jump_to_cont = ARENA_ALLOC(fn->arena, IronLIR_Instr);
    memset(jump_to_cont, 0, sizeof(*jump_to_cont));
    jump_to_cont->kind        = IRON_LIR_JUMP;
    jump_to_cont->id          = IRON_LIR_VALUE_INVALID;
    jump_to_cont->jump.target = cont->id;
    jump_to_cont->span        = call_instr->span;
    arrput(merge->instrs, jump_to_cont);
    merge->instr_count++;

    /* Append continuation block at the end of the function's block array.
     *
     * The continuation block may contain ALLOCA instructions (loop variables,
     * etc.) that were split from the original call_block.  These allocas may be
     * referenced by while-loop blocks that appear at lower block-array indices.
     * The C emitter's backward-reference hoisting mechanism (emit_c.c) is
     * responsible for pre-declaring such allocas at function entry.
     */
    arrput(fn->blocks, cont);
    fn->block_count++;

    /* ── Step 9: Propagate inline result throughout the caller function ─── */
    /*
     * The CALL's result ID (call_instr->id) may be used in any block of the
     * caller function, not just the immediately-following continuation block.
     * For example, if the CALL result is used in a branch target block that
     * was already in fn->blocks before splitting, those references must be
     * updated to load_result_id.
     *
     * CRITICAL: Use a fresh, minimal remap containing ONLY the mapping
     * call_instr->id -> load_result_id.  The id_remap table built during
     * cloning maps callee IDs to new caller IDs; applying it here would
     * wrongly replace existing caller-space operands that happen to share
     * integer values with callee IDs.
     *
     * We apply to ALL blocks EXCEPT the cloned callee blocks (cloned_block_start
     * to fn->block_count - 2, exclusive) and the merge block — those have
     * already been handled.  Practically: apply to all original caller blocks
     * (0..cloned_block_start) and the cont block.
     */
    if (non_void && call_instr->id != IRON_LIR_VALUE_INVALID) {
        ValueReplEntry *result_remap = NULL;
        hmput(result_remap, call_instr->id, load_result_id);
        /* Apply to all original caller blocks (indices 0..cloned_block_start-1)
         * and the continuation block. Skip call_block itself (call was removed)
         * and the cloned/merge blocks (don't exist yet in caller ID space). */
        for (int bi2 = 0; bi2 < cloned_block_start; bi2++) {
            if (fn->blocks[bi2] == call_block) continue;  /* call removed, skip */
            IronLIR_Block *blk = fn->blocks[bi2];
            for (int ii = 0; ii < blk->instr_count; ii++)
                apply_replacements(blk->instrs[ii], result_remap);
        }
        /* Also apply to continuation block */
        for (int ii = 0; ii < cont->instr_count; ii++)
            apply_replacements(cont->instrs[ii], result_remap);
        hmfree(result_remap);
    }

    hmfree(id_remap);
    hmfree(block_remap);
}

/* Run function inlining pass over the entire module.
 *
 * Inlines small (<= 20 instructions), non-recursive, pure functions.
 * Runs between optimize_array_repr and the copy-prop/DCE fixpoint loop.
 */
static void run_function_inlining(IronLIR_Module *module,
                                   IronLIR_OptimizeInfo *info,
                                   Iron_Arena *arena) {
    (void)info;   /* local purity map used instead of info->func_purity */
    (void)arena;  /* reserved for future use */

    if (!module || module->func_count == 0) return;

    /* Step 1: Local purity scan (Phase-1 logic; leaf-only; no info mutation). */
    struct { char *key; bool value; } *local_purity = NULL;

    for (int fi = 0; fi < module->func_count; fi++) {
        IronLIR_Func *fn = module->funcs[fi];
        if (fn->is_extern || fn->block_count == 0) continue;

        bool all_pure = true;
        bool has_call = false;
        for (int bi = 0; bi < fn->block_count && all_pure; bi++) {
            IronLIR_Block *blk = fn->blocks[bi];
            for (int ii = 0; ii < blk->instr_count && all_pure; ii++) {
                IronLIR_Instr *in = blk->instrs[ii];
                if (in->kind == IRON_LIR_CALL) {
                    has_call = true;
                } else if (!iron_lir_instr_is_pure(in->kind) &&
                           in->kind != IRON_LIR_RETURN &&
                           in->kind != IRON_LIR_JUMP &&
                           in->kind != IRON_LIR_ALLOCA &&
                           in->kind != IRON_LIR_STORE &&
                           in->kind != IRON_LIR_BRANCH) {
                    all_pure = false;
                }
            }
        }
        if (all_pure && !has_call) {
            shput(local_purity, (char*)fn->name, true);
        }
    }

    /* Step 2: Build inline candidate set (name -> IronLIR_Func*). */
    struct { char *key; IronLIR_Func *value; } *candidates = NULL;

    for (int fi = 0; fi < module->func_count; fi++) {
        IronLIR_Func *fn = module->funcs[fi];
        if (fn->is_extern || fn->block_count == 0) continue;
        if (count_func_instructions(fn) > 30) continue;  /* threshold: 30 (phi-eliminate adds ~9 instrs for param allocas) */
        if (shgeti(local_purity, (char*)fn->name) < 0) continue;
        if (func_has_self_call(fn)) continue;
        shput(candidates, (char*)fn->name, fn);
    }

    if (shlen(candidates) == 0) {
        shfree(local_purity);
        shfree(candidates);
        return;
    }

    /* Step 3: For each function (caller), inline calls to candidates.
     *
     * Snapshot block_count to avoid iterating newly-cloned blocks.
     * One call site inlined per block per pass (break after each inline). */
    for (int fi = 0; fi < module->func_count; fi++) {
        IronLIR_Func *fn = module->funcs[fi];
        if (fn->is_extern || fn->block_count == 0) continue;

        int orig_block_count = fn->block_count;  /* Snapshot before mutations */

        for (int bi = 0; bi < orig_block_count; bi++) {
            /* Re-fetch block pointer every iteration (stb_ds may realloc) */
            IronLIR_Block *blk = fn->blocks[bi];
            int orig_instr_count = blk->instr_count;

            for (int ii = 0; ii < orig_instr_count; ii++) {
                IronLIR_Instr *instr = blk->instrs[ii];
                if (instr->kind != IRON_LIR_CALL) continue;

                /* Resolve callee name */
                const char *callee_name = NULL;
                if (instr->call.func_decl) {
                    callee_name = instr->call.func_decl->name;
                } else if (instr->call.func_ptr != IRON_LIR_VALUE_INVALID) {
                    IronLIR_ValueId fptr = instr->call.func_ptr;
                    if (fptr < (IronLIR_ValueId)arrlen(fn->value_table) &&
                        fn->value_table[fptr] &&
                        fn->value_table[fptr]->kind == IRON_LIR_FUNC_REF) {
                        callee_name = fn->value_table[fptr]->func_ref.func_name;
                    }
                }
                if (!callee_name) continue;

                /* Don't inline into itself */
                if (strcmp(fn->name, callee_name) == 0) continue;

                /* Check if callee is an inline candidate */
                ptrdiff_t ci = shgeti(candidates, (char*)callee_name);
                if (ci < 0) continue;
                IronLIR_Func *callee = candidates[ci].value;

                /* Re-fetch block pointer (safe since no arrput happened yet) */
                IronLIR_Block *call_block = fn->blocks[bi];

                /* Inline this call site */
                inline_call_site(fn, call_block, ii, instr, callee);

                /* Block is now split — stop iterating its instructions */
                break;
            }
        }
    }

    /* Cleanup */
    shfree(local_purity);
    shfree(candidates);
}

void iron_lir_compute_inline_info(IronLIR_Module *module, IronLIR_OptimizeInfo *info) {
    if (!module || !info) return;
    /* Compute module-wide function purity analysis (only once per module) */
    compute_func_purity(module, info);
}

bool iron_lir_optimize(IronLIR_Module *module, IronLIR_OptimizeInfo *info,
                      Iron_Arena *arena, bool dump_passes, bool skip_new_passes) {
    if (!module) return false;

    /* Initialize info maps to NULL (stb_ds convention) */
    memset(info, 0, sizeof(*info));

    /* Phase 0: Structural passes (always run, not skippable) */
    phi_eliminate(module);
    if (dump_passes) {
        char *ir_text = iron_lir_print(module, true);
        if (ir_text) { fprintf(stderr, "=== After phi-eliminate ===\n%s\n", ir_text); free(ir_text); }
    }

    analyze_array_param_modes(module, info, arena);
    if (dump_passes) {
        char *ir_text = iron_lir_print(module, true);
        if (ir_text) { fprintf(stderr, "=== After array-param-modes ===\n%s\n", ir_text); free(ir_text); }
    }

    optimize_array_repr(module, info);
    if (dump_passes) {
        char *ir_text = iron_lir_print(module, true);
        if (ir_text) { fprintf(stderr, "=== After array-repr ===\n%s\n", ir_text); free(ir_text); }
    }

    run_function_inlining(module, info, arena);
    if (dump_passes) {
        char *ir_text = iron_lir_print(module, true);
        if (ir_text) { fprintf(stderr, "=== After function-inlining ===\n%s\n", ir_text); free(ir_text); }
    }

    /* Re-run array repr to fix revoked_fill_ids for cloned fills */
    optimize_array_repr(module, info);
    if (dump_passes) {
        char *ir_text = iron_lir_print(module, true);
        if (ir_text) { fprintf(stderr, "=== After array-repr (post-inline) ===\n%s\n", ir_text); free(ir_text); }
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
            char *ir_text = iron_lir_print(module, true);
            if (ir_text) { fprintf(stderr, "=== After copy-prop (iter %d) ===\n%s\n", iter, ir_text); free(ir_text); }
        }
        iron_lir_verify(module, &verify_diags, arena);

        changed |= run_constant_folding(module);
        if (dump_passes) {
            char *ir_text = iron_lir_print(module, true);
            if (ir_text) { fprintf(stderr, "=== After const-fold (iter %d) ===\n%s\n", iter, ir_text); free(ir_text); }
        }
        iron_lir_verify(module, &verify_diags, arena);

        changed |= run_dce(module);
        if (dump_passes) {
            char *ir_text = iron_lir_print(module, true);
            if (ir_text) { fprintf(stderr, "=== After dce (iter %d) ===\n%s\n", iter, ir_text); free(ir_text); }
        }
        iron_lir_verify(module, &verify_diags, arena);

        changed |= run_store_load_elim(module);
        if (dump_passes) {
            char *ir_text = iron_lir_print(module, true);
            if (ir_text) { fprintf(stderr, "=== After store-load-elim (iter %d) ===\n%s\n", iter, ir_text); free(ir_text); }
        }
        iron_lir_verify(module, &verify_diags, arena);

        changed |= run_strength_reduction(module);
        if (dump_passes) {
            char *ir_text = iron_lir_print(module, true);
            if (ir_text) { fprintf(stderr, "=== After strength-reduction (iter %d) ===\n%s\n", iter, ir_text); free(ir_text); }
        }
        iron_lir_verify(module, &verify_diags, arena);

        if (!changed) break;
        any_changed = true;
    }

    iron_diaglist_free(&verify_diags);

    /* PHI-01/PHI-02: dead alloca elimination — remove zero-load phi-origin
     * allocas and their stores.  Runs once post-fixpoint; copy-prop has
     * already eliminated single-store alloca loads by this point. */
    run_dead_alloca_elimination(module);
    if (dump_passes) {
        char *ir_text = iron_lir_print(module, true);
        if (ir_text) { fprintf(stderr, "=== After dead-alloca-elim ===\n%s\n", ir_text); free(ir_text); }
    }

    /* Phase 16: compute module-wide function purity and set up inline info */
    if (!skip_new_passes) {
        iron_lir_compute_inline_info(module, info);
        if (dump_passes) {
            fprintf(stderr, "=== After inline-info: %td pure functions ===\n",
                    hmlen(info->func_purity));
        }
    }

    return any_changed;
}

void iron_lir_optimize_info_free(IronLIR_OptimizeInfo *info) {
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
