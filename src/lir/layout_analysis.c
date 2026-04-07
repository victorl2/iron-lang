/* layout_analysis.c -- Phase 48: Field access analysis for dead field elimination.
 *
 * Scans all LIR functions for GET_FIELD / SET_FIELD instructions whose object
 * traces back to a split collection iteration variable.  Marks accessed fields
 * so the C emitter can exclude unused ("dead") fields from storage structs.
 *
 * Algorithm per function:
 *   1. Build alloca_to_split: ALLOCA ValueId -> split collection ValueId.
 *      Populated by finding STORE(alloca, GET_INDEX(split, idx)) patterns.
 *   2. Build value_to_split: GET_INDEX result ValueId -> split collection ValueId.
 *   3. For each GET_FIELD/SET_FIELD:
 *      - If field.object is in value_to_split -> mark field used
 *      - If field.object is a LOAD whose ptr is in alloca_to_split -> mark field used
 */

#include "lir/layout_analysis.h"
#include "vendor/stb_ds.h"

#include <stdio.h>
#include <string.h>

/* ── Internal helpers ────────────────────────────────────────────────────── */

/* Format a field usage key: "collectionVid:fieldName" */
static char *make_field_key(Iron_Arena *arena, IronLIR_ValueId collection_vid,
                            const char *field_name) {
    char buf[256];
    int len = snprintf(buf, sizeof(buf), "%u:%s", collection_vid, field_name);
    if (len < 0) len = 0;
    return iron_arena_strdup(arena, buf, (size_t)len);
}

/* Mark a field as used for a given collection. */
static void mark_field_used(LayoutAnalysis *la, IronLIR_ValueId collection_vid,
                            const char *field_name) {
    char *key = make_field_key(la->arena, collection_vid, field_name);
    shput(la->field_used, key, true);
}

/* ── Per-function analysis ───────────────────────────────────────────────── */

static void analyze_function(LayoutAnalysis *la, IronLIR_Func *fn,
                             Iron_SplitCollectionId *split_ids) {
    if (!fn || fn->is_extern || fn->block_count == 0) return;

    /* Map: GET_INDEX result vid -> source split collection vid */
    struct { IronLIR_ValueId key; IronLIR_ValueId value; } *value_to_split = NULL;

    /* Map: ALLOCA vid -> split collection vid (via STORE pattern) */
    struct { IronLIR_ValueId key; IronLIR_ValueId value; } *alloca_to_split = NULL;

    /* Pass 1: Find GET_INDEX instructions on split collections */
    for (int bi = 0; bi < fn->block_count; bi++) {
        IronLIR_Block *blk = fn->blocks[bi];
        for (int ii = 0; ii < blk->instr_count; ii++) {
            IronLIR_Instr *instr = blk->instrs[ii];
            if (instr->kind == IRON_LIR_GET_INDEX && instr->id != IRON_LIR_VALUE_INVALID) {
                IronLIR_ValueId array_vid = instr->index.array;
                ptrdiff_t sp_idx = hmgeti(split_ids, array_vid);
                if (sp_idx >= 0) {
                    hmput(value_to_split, instr->id, array_vid);
                }
            }
        }
    }

    /* Pass 2: Find STORE(alloca, get_index_result) patterns */
    for (int bi = 0; bi < fn->block_count; bi++) {
        IronLIR_Block *blk = fn->blocks[bi];
        for (int ii = 0; ii < blk->instr_count; ii++) {
            IronLIR_Instr *instr = blk->instrs[ii];
            if (instr->kind == IRON_LIR_STORE) {
                IronLIR_ValueId ptr_vid = instr->store.ptr;
                IronLIR_ValueId val_vid = instr->store.value;
                /* Check if the value being stored comes from a split GET_INDEX */
                ptrdiff_t vi = hmgeti(value_to_split, val_vid);
                if (vi >= 0) {
                    IronLIR_ValueId split_vid = value_to_split[vi].value;
                    /* Check ptr is an ALLOCA */
                    if (ptr_vid < fn->next_value_id && fn->value_table &&
                        fn->value_table[ptr_vid] &&
                        fn->value_table[ptr_vid]->kind == IRON_LIR_ALLOCA) {
                        hmput(alloca_to_split, ptr_vid, split_vid);
                    }
                }
            }
        }
    }

    /* Pass 3: Scan GET_FIELD / SET_FIELD and mark used fields */
    for (int bi = 0; bi < fn->block_count; bi++) {
        IronLIR_Block *blk = fn->blocks[bi];
        for (int ii = 0; ii < blk->instr_count; ii++) {
            IronLIR_Instr *instr = blk->instrs[ii];
            if (instr->kind != IRON_LIR_GET_FIELD &&
                instr->kind != IRON_LIR_SET_FIELD) continue;

            const char *field_name = instr->field.field;
            if (!field_name) continue;

            /* Skip .count -- not a data field */
            if (strcmp(field_name, "count") == 0) continue;

            IronLIR_ValueId obj = instr->field.object;

            /* Case A: object is directly a GET_INDEX result from split collection */
            {
                ptrdiff_t vi = hmgeti(value_to_split, obj);
                if (vi >= 0) {
                    mark_field_used(la, value_to_split[vi].value, field_name);
                    continue;
                }
            }

            /* Case B: object is a LOAD of an ALLOCA that traces to split collection */
            if (obj < fn->next_value_id && fn->value_table &&
                fn->value_table[obj] &&
                fn->value_table[obj]->kind == IRON_LIR_LOAD) {
                IronLIR_ValueId ptr_vid = fn->value_table[obj]->load.ptr;
                ptrdiff_t ai = hmgeti(alloca_to_split, ptr_vid);
                if (ai >= 0) {
                    mark_field_used(la, alloca_to_split[ai].value, field_name);
                    continue;
                }
            }
        }
    }

    hmfree(value_to_split);
    hmfree(alloca_to_split);
}

/* ── Public API ──────────────────────────────────────────────────────────── */

void iron_layout_analyze(LayoutAnalysis *la,
                         IronLIR_Module *module,
                         Iron_SplitCollectionId *split_collection_ids,
                         Iron_IfaceRegistry *iface_reg) {
    (void)iface_reg;  /* reserved for future SoA analysis */

    if (!la || !module || !split_collection_ids) return;

    /* Initialize the string hash map */
    sh_new_strdup(la->field_used);

    /* Analyze each function */
    for (int fi = 0; fi < module->func_count; fi++) {
        IronLIR_Func *fn = module->funcs[fi];
        analyze_function(la, fn, split_collection_ids);
    }
}

bool iron_layout_is_field_used(LayoutAnalysis *la,
                               IronLIR_ValueId collection_vid,
                               const char *field_name) {
    /* Conservative: if no analysis was performed, assume all fields used */
    if (!la || !la->field_used) return true;

    char buf[256];
    snprintf(buf, sizeof(buf), "%u:%s", collection_vid, field_name);
    ptrdiff_t idx = shgeti(la->field_used, buf);
    if (idx >= 0) return la->field_used[idx].value;

    /* Not found -> field was never accessed -> dead */
    return false;
}

void iron_layout_free(LayoutAnalysis *la) {
    if (!la) return;
    shfree(la->field_used);
    la->field_used = NULL;
}
