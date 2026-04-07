/* layout_analysis.c -- Phase 48: Field access analysis for dead field elimination.
 *
 * Determines which fields are accessed through split collection operations.
 * Two analysis passes:
 *
 * 1. Direct field access analysis:
 *    Scans functions for GET_FIELD/SET_FIELD on values derived from split
 *    collection iteration (GET_INDEX results or LOADs of stored loop vars).
 *
 * 2. Method-based field analysis (interprocedural):
 *    For each interface with split collections, finds all method implementations
 *    and scans their bodies for GET_FIELD on `self` (first parameter).
 *    Fields accessed by any method are marked as used for all collections
 *    of that interface, since method dispatch means any method could be called.
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

/* ── Per-function direct access analysis ─────────────────────────────────── */

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

/* ── Method-based interprocedural analysis ───────────────────────────────── */

/* Scan a method implementation for GET_FIELD/SET_FIELD on `self` (param 0).
 * Returns an stb_ds array of field names accessed. Caller must arrfree.
 *
 * LIR pattern for methods:
 *   %1 = <param: self>           (parameter value, id = 1 for first param)
 *   %2 = alloca <object>         (name_hint: "self")
 *   store %2, %1                 (store param into alloca)
 *   ...
 *   %N = get_field %1.radius     (direct use of param value)
 *
 * After optimization, GET_FIELD may reference %1 directly (parameter alias)
 * or through a LOAD from the self alloca.
 */
static const char **collect_self_fields(IronLIR_Func *fn) {
    const char **fields = NULL;  /* stb_ds array */
    if (!fn || fn->is_extern || fn->block_count == 0) return fields;
    if (fn->param_count == 0) return fields;

    /* Set of value ids that represent `self`:
     * 1. The first parameter value (id = 1 in LIR convention)
     * 2. ALLOCAs named "self"
     * 3. LOADs from those ALLOCAs */
    struct { IronLIR_ValueId key; bool value; } *self_vids = NULL;

    /* The first parameter value id is 1 (LIR convention: params start at 1) */
    hmput(self_vids, (IronLIR_ValueId)1, true);

    /* Also find ALLOCAs named "self" */
    for (int bi = 0; bi < fn->block_count; bi++) {
        IronLIR_Block *blk = fn->blocks[bi];
        for (int ii = 0; ii < blk->instr_count; ii++) {
            IronLIR_Instr *instr = blk->instrs[ii];
            if (instr->kind == IRON_LIR_ALLOCA && instr->alloca.name_hint &&
                strcmp(instr->alloca.name_hint, "self") == 0) {
                hmput(self_vids, instr->id, true);
            }
        }
    }

    /* Find LOADs from self-related values */
    for (int bi = 0; bi < fn->block_count; bi++) {
        IronLIR_Block *blk = fn->blocks[bi];
        for (int ii = 0; ii < blk->instr_count; ii++) {
            IronLIR_Instr *instr = blk->instrs[ii];
            if (instr->kind == IRON_LIR_LOAD &&
                instr->id != IRON_LIR_VALUE_INVALID) {
                if (hmgeti(self_vids, instr->load.ptr) >= 0) {
                    hmput(self_vids, instr->id, true);
                }
            }
        }
    }

    /* Scan GET_FIELD/SET_FIELD on self-related values */
    struct { char *key; bool value; } *seen = NULL;
    sh_new_strdup(seen);

    for (int bi = 0; bi < fn->block_count; bi++) {
        IronLIR_Block *blk = fn->blocks[bi];
        for (int ii = 0; ii < blk->instr_count; ii++) {
            IronLIR_Instr *instr = blk->instrs[ii];
            if (instr->kind != IRON_LIR_GET_FIELD &&
                instr->kind != IRON_LIR_SET_FIELD) continue;

            const char *fname = instr->field.field;
            if (!fname) continue;

            if (hmgeti(self_vids, instr->field.object) >= 0 &&
                shgeti(seen, fname) < 0) {
                shput(seen, fname, true);
                arrput(fields, fname);
            }
        }
    }

    hmfree(self_vids);
    shfree(seen);
    return fields;
}

/* Analyze interface methods to find which fields they access on self.
 * For each split collection of an interface, mark those fields as used. */
static void analyze_method_fields(LayoutAnalysis *la, IronLIR_Module *module,
                                  Iron_SplitCollectionId *split_collection_ids,
                                  Iron_IfaceRegistry *iface_reg) {
    if (!iface_reg || !split_collection_ids) return;

    /* For each interface in the registry */
    for (int ri = 0; ri < (int)shlen(iface_reg->map); ri++) {
        Iron_IfaceEntry *entry = &iface_reg->map[ri].value;
        if (entry->alive_count == 0) continue;

        /* Collect all split collection ValueIds for this interface */
        IronLIR_ValueId *coll_vids = NULL;
        for (ptrdiff_t si = 0; si < hmlen(split_collection_ids); si++) {
            /* The split_collection_ids maps vid -> mangled interface name.
             * We need to check if this matches the current interface. */
            const char *iface_name = entry->iface_name;
            const char *stored_name = split_collection_ids[si].value;
            /* stored_name is mangled (Iron_Shape), iface_name is raw (Shape).
             * Compare by checking if stored_name ends with _<iface_name>
             * or starts with Iron_<iface_name> */
            char mangled[512];
            snprintf(mangled, sizeof(mangled), "Iron_%s", iface_name);
            if (strcmp(stored_name, mangled) == 0) {
                arrput(coll_vids, split_collection_ids[si].key);
            }
        }

        if (arrlen(coll_vids) == 0) {
            arrfree(coll_vids);
            continue;
        }

        /* For each alive implementor, find its method functions and analyze */
        for (int j = 0; j < entry->impl_count; j++) {
            Iron_IfaceImpl *impl = &entry->impls[j];
            if (!impl->is_alive) continue;

            /* Build lowercase type name prefix for matching LIR function names.
             * LIR convention: "circle_area" for Circle.area() */
            char lower_type[256];
            {
                size_t tl = strlen(impl->type_name);
                if (tl >= sizeof(lower_type)) tl = sizeof(lower_type) - 1;
                for (size_t ci = 0; ci < tl; ci++)
                    lower_type[ci] = (char)((impl->type_name[ci] >= 'A' &&
                                              impl->type_name[ci] <= 'Z')
                        ? impl->type_name[ci] + 32
                        : impl->type_name[ci]);
                lower_type[tl] = '\0';
            }
            char prefix[512];
            snprintf(prefix, sizeof(prefix), "%s_", lower_type);
            size_t prefix_len = strlen(prefix);

            /* Find all functions that are methods of this implementor */
            for (int fi = 0; fi < module->func_count; fi++) {
                IronLIR_Func *fn = module->funcs[fi];
                if (!fn || fn->is_extern) continue;

                /* Match LIR function name pattern: <lowercase_type>_<method>
                 * and must have at least one parameter (self) */
                if (fn->name && strncmp(fn->name, prefix, prefix_len) == 0 &&
                    fn->param_count >= 1) {
                    /* This is a method of this implementor type.
                     * Collect which fields it accesses on self. */
                    const char **self_fields = collect_self_fields(fn);
                    for (int k = 0; k < (int)arrlen(self_fields); k++) {
                        /* Mark field as used for all collections of this interface */
                        for (int ci = 0; ci < (int)arrlen(coll_vids); ci++) {
                            mark_field_used(la, coll_vids[ci], self_fields[k]);
                        }
                    }
                    arrfree(self_fields);
                }
            }
        }

        arrfree(coll_vids);
    }
}

/* ── Public API ──────────────────────────────────────────────────────────── */

void iron_layout_analyze(LayoutAnalysis *la,
                         IronLIR_Module *module,
                         Iron_SplitCollectionId *split_collection_ids,
                         Iron_IfaceRegistry *iface_reg) {
    if (!la || !module || !split_collection_ids) return;

    /* Initialize the string hash map */
    sh_new_strdup(la->field_used);

    /* Pass 1: Direct field access analysis (intraprocedural) */
    for (int fi = 0; fi < module->func_count; fi++) {
        IronLIR_Func *fn = module->funcs[fi];
        analyze_function(la, fn, split_collection_ids);
    }

    /* Pass 2: Method-based analysis (interprocedural) — marks fields accessed
     * by interface method implementations as used for all collections of that
     * interface. This handles the common case where fields are accessed through
     * method calls (e.g., self.radius in Circle.area()). */
    analyze_method_fields(la, module, split_collection_ids, iface_reg);
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
