/* layout_analysis.c -- Phase 48: Field access analysis for layout optimization.
 *
 * Determines which fields are accessed through split collection operations.
 * Three analysis passes:
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
 *
 * 3. SoA/AoS layout selection (Phase 48-02):
 *    For each split-eligible for-loop, counts accessed fields per type and
 *    selects SoA when the access ratio is below IRON_SOA_THRESHOLD.
 *    Also detects common fields shared across all alive implementors.
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

/* ── SoA/AoS layout selection helpers (Phase 48-02) ─────────────────────── */

/* String-keyed boolean hash map entry for stb_ds (avoids anonymous struct issues) */
typedef struct {
    char *key;
    bool value;
} FieldNameEntry;

/* Count how many GET_FIELD/SET_FIELD accesses a loop body makes on the loop
 * variable (or its LOAD aliases).  Populates out_fields with field names. */
static void count_loop_body_fields(IronLIR_Func *fn, IronLIR_Block *body,
                                    int body_start_ii,
                                    IronLIR_ValueId get_index_vid,
                                    FieldNameEntry **out_fields) {
    if (!fn || !body || !out_fields) return;

    /* Build set of value ids that represent the loop element:
     * 1. The GET_INDEX result (get_index_vid)
     * 2. LOADs from ALLOCAs that store the GET_INDEX result */
    struct { IronLIR_ValueId key; bool value; } *elem_vids = NULL;
    hmput(elem_vids, get_index_vid, true);

    /* Find STORE(alloca, get_index_vid) in the body, then LOAD from that alloca */
    for (int ii = 0; ii < body->instr_count; ii++) {
        IronLIR_Instr *instr = body->instrs[ii];
        if (instr->kind == IRON_LIR_STORE && instr->store.value == get_index_vid) {
            hmput(elem_vids, instr->store.ptr, true);
        }
    }
    /* Also find LOADs from those ALLOCAs */
    for (int ii = 0; ii < body->instr_count; ii++) {
        IronLIR_Instr *instr = body->instrs[ii];
        if (instr->kind == IRON_LIR_LOAD) {
            if (hmgeti(elem_vids, instr->load.ptr) >= 0) {
                hmput(elem_vids, instr->id, true);
            }
        }
    }

    /* Scan GET_FIELD/SET_FIELD on element-derived values */
    for (int ii = body_start_ii; ii < body->instr_count; ii++) {
        IronLIR_Instr *instr = body->instrs[ii];
        if (instr->kind == IRON_LIR_JUMP) break;
        if (instr->kind != IRON_LIR_GET_FIELD &&
            instr->kind != IRON_LIR_SET_FIELD) continue;

        const char *fname = instr->field.field;
        if (!fname || strcmp(fname, "count") == 0) continue;

        if (hmgeti(elem_vids, instr->field.object) >= 0) {
            shput(*out_fields, fname, true);
        }
    }

    hmfree(elem_vids);
}

/* Map type annotation name to C type string (minimal version for layout analysis). */
static const char *layout_annotation_to_c(const char *name) {
    if (!name) return "int64_t";
    if (strcmp(name, "Int") == 0)     return "int64_t";
    if (strcmp(name, "Int8") == 0)    return "int8_t";
    if (strcmp(name, "Int16") == 0)   return "int16_t";
    if (strcmp(name, "Int32") == 0)   return "int32_t";
    if (strcmp(name, "Int64") == 0)   return "int64_t";
    if (strcmp(name, "UInt") == 0)    return "uint64_t";
    if (strcmp(name, "UInt8") == 0)   return "uint8_t";
    if (strcmp(name, "UInt16") == 0)  return "uint16_t";
    if (strcmp(name, "UInt32") == 0)  return "uint32_t";
    if (strcmp(name, "UInt64") == 0)  return "uint64_t";
    if (strcmp(name, "Float") == 0)   return "double";
    if (strcmp(name, "Float32") == 0) return "float";
    if (strcmp(name, "Float64") == 0) return "double";
    if (strcmp(name, "Bool") == 0)    return "bool";
    if (strcmp(name, "String") == 0)  return "Iron_String";
    return "int64_t";
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

    /* Pass 2: Method-based analysis (interprocedural) -- marks fields accessed
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

void iron_layout_select(LayoutAnalysis *la,
                        IronLIR_Module *module,
                        Iron_SplitCollectionId *split_collection_ids,
                        Iron_IfaceRegistry *iface_reg) {
    if (!la || !module || !iface_reg) return;

    /* Initialize hash maps */
    sh_new_strdup(la->layout_decisions);
    sh_new_strdup(la->common_fields);

    /* ── Pass 1: Per-loop field access ratio analysis ─────────────────────
     * For each function, find split-eligible for-loops (same pattern as emit_c.c),
     * count field accesses in the loop body, and compute access ratios per type.
     * The interface with the lowest ratio across all loops gets SoA. */

    /* Track lowest ratio per "iface_mangled:type_name" */
    struct { char *key; double value; } *min_ratios = NULL;
    sh_new_strdup(min_ratios);

    for (int fi = 0; fi < module->func_count; fi++) {
        IronLIR_Func *fn = module->funcs[fi];
        if (!fn || fn->is_extern || fn->block_count == 0) continue;

        /* Find split-eligible for-loops (mirror emit_c.c SplitLoopInfo detection) */
        for (int bi = 0; bi < fn->block_count; bi++) {
            IronLIR_Block *blk = fn->blocks[bi];
            if (!blk->label || strncmp(blk->label, "for_pre", 7) != 0) continue;

            /* Check for GET_FIELD .count on a split collection */
            IronLIR_ValueId split_arr_vid = IRON_LIR_VALUE_INVALID;
            for (int ii = 0; ii < blk->instr_count; ii++) {
                IronLIR_Instr *in2 = blk->instrs[ii];
                if (in2->kind == IRON_LIR_GET_FIELD && in2->field.field &&
                    strcmp(in2->field.field, "count") == 0) {
                    if (hmgeti(split_collection_ids, in2->field.object) >= 0) {
                        split_arr_vid = in2->field.object;
                        break;
                    }
                }
            }
            if (split_arr_vid == IRON_LIR_VALUE_INVALID) continue;

            /* Find the interface mangled name for this collection */
            ptrdiff_t sp_idx = hmgeti(split_collection_ids, split_arr_vid);
            if (sp_idx < 0) continue;
            const char *iface_mangled = split_collection_ids[sp_idx].value;

            /* Navigate to body block following same pattern as emit_c.c */
            IronLIR_Instr *pre_term = blk->instrs[blk->instr_count - 1];
            if (pre_term->kind != IRON_LIR_JUMP) continue;
            int header_bi = -1;
            for (int bi2 = 0; bi2 < fn->block_count; bi2++) {
                if (fn->blocks[bi2]->id == pre_term->jump.target) { header_bi = bi2; break; }
            }
            if (header_bi < 0) continue;

            IronLIR_Block *header = fn->blocks[header_bi];
            IronLIR_Instr *hdr_term = header->instrs[header->instr_count - 1];
            if (hdr_term->kind != IRON_LIR_BRANCH) continue;
            int body_bi = -1;
            for (int bi2 = 0; bi2 < fn->block_count; bi2++) {
                if (fn->blocks[bi2]->id == hdr_term->branch.then_block) { body_bi = bi2; break; }
            }
            if (body_bi < 0) continue;

            IronLIR_Block *body = fn->blocks[body_bi];
            IronLIR_Instr *body_term = body->instrs[body->instr_count - 1];
            if (body_term->kind != IRON_LIR_JUMP) continue;

            /* Find GET_INDEX and body_start_ii */
            IronLIR_ValueId get_index_vid = IRON_LIR_VALUE_INVALID;
            int body_start = 0;
            for (int ii = 0; ii < body->instr_count; ii++) {
                IronLIR_Instr *in2 = body->instrs[ii];
                if (in2->kind == IRON_LIR_GET_INDEX &&
                    in2->index.array == split_arr_vid) {
                    get_index_vid = in2->id;
                    body_start = ii + 1;
                    if (body_start < body->instr_count &&
                        body->instrs[body_start]->kind == IRON_LIR_STORE) {
                        body_start++;
                    }
                    break;
                }
                if (in2->kind == IRON_LIR_LOAD) continue;
            }
            if (get_index_vid == IRON_LIR_VALUE_INVALID) continue;

            /* Count directly accessed fields in loop body */
            FieldNameEntry *body_fields = NULL;
            sh_new_strdup(body_fields);
            count_loop_body_fields(fn, body, body_start, get_index_vid, &body_fields);

            /* For each implementor type of this interface, compute access ratio */
            for (int ri = 0; ri < (int)shlen(iface_reg->map); ri++) {
                Iron_IfaceEntry *entry = &iface_reg->map[ri].value;
                char mangled_buf[512];
                snprintf(mangled_buf, sizeof(mangled_buf), "Iron_%s", entry->iface_name);
                if (strcmp(mangled_buf, iface_mangled) != 0) continue;

                for (int ji = 0; ji < entry->impl_count; ji++) {
                    Iron_IfaceImpl *impl = &entry->impls[ji];
                    if (!impl->is_alive || !impl->decl) continue;

                    int total = impl->decl->field_count;
                    if (total == 0) continue;

                    /* Count how many of this type's fields are accessed:
                     * - directly in the loop body (body_fields)
                     * - through method calls (via field_used from interprocedural analysis) */
                    int accessed = 0;
                    Iron_ObjectDecl *od = impl->decl;
                    for (int fdi = 0; fdi < od->field_count; fdi++) {
                        Iron_Field *f = (Iron_Field *)od->fields[fdi];
                        /* Check direct access in loop body */
                        if (shgeti(body_fields, f->name) >= 0) {
                            accessed++;
                            continue;
                        }
                        /* Check method-based access via field_used */
                        if (la->field_used) {
                            char fbuf[256];
                            snprintf(fbuf, sizeof(fbuf), "%u:%s", split_arr_vid, f->name);
                            if (shgeti(la->field_used, fbuf) >= 0) {
                                accessed++;
                            }
                        }
                    }

                    double ratio = (double)accessed / (double)total;

                    /* Build key: "iface_mangled:type_name" */
                    char rkey[768];
                    snprintf(rkey, sizeof(rkey), "%s:%s", iface_mangled, impl->type_name);

                    /* Keep the lowest ratio (most SoA-friendly) */
                    ptrdiff_t ri2 = shgeti(min_ratios, rkey);
                    if (ri2 < 0 || ratio < min_ratios[ri2].value) {
                        shput(min_ratios, rkey, ratio);
                    }
                }
                break;
            }

            shfree(body_fields);
        }
    }

    /* Convert minimum ratios to layout decisions */
    for (ptrdiff_t i = 0; i < shlen(min_ratios); i++) {
        IronLayoutKind kind = (min_ratios[i].value < IRON_SOA_THRESHOLD)
            ? IRON_LAYOUT_SOA : IRON_LAYOUT_AOS;
        shput(la->layout_decisions, min_ratios[i].key, kind);
    }
    shfree(min_ratios);

    /* ── Pass 2: Common field detection ──────────────────────────────────── */
    for (int ri = 0; ri < (int)shlen(iface_reg->map); ri++) {
        Iron_IfaceEntry *entry = &iface_reg->map[ri].value;
        if (entry->alive_count < 2) continue;  /* Need >= 2 implementors */

        /* Find the maximum field count across all alive implementors */
        int max_fields = 0;
        for (int ji = 0; ji < entry->impl_count; ji++) {
            if (!entry->impls[ji].is_alive || !entry->impls[ji].decl) continue;
            if (entry->impls[ji].decl->field_count > max_fields)
                max_fields = entry->impls[ji].decl->field_count;
        }

        CommonField *common = NULL;  /* stb_ds array */

        for (int pos = 0; pos < max_fields; pos++) {
            const char *expected_name = NULL;
            const char *expected_type = NULL;
            bool all_match = true;

            for (int ji = 0; ji < entry->impl_count; ji++) {
                Iron_IfaceImpl *impl = &entry->impls[ji];
                if (!impl->is_alive || !impl->decl) continue;

                Iron_ObjectDecl *od = impl->decl;
                if (pos >= od->field_count) {
                    all_match = false;
                    break;
                }

                Iron_Field *f = (Iron_Field *)od->fields[pos];
                const char *fname = f->name;
                const char *ftype = NULL;
                if (f->type_ann) {
                    Iron_TypeAnnotation *ta = (Iron_TypeAnnotation *)f->type_ann;
                    if (!ta->is_func && !ta->is_nullable && !ta->is_array) {
                        ftype = ta->name;
                    }
                }

                if (!expected_name) {
                    expected_name = fname;
                    expected_type = ftype;
                } else {
                    if (!fname || !expected_name || strcmp(fname, expected_name) != 0) {
                        all_match = false;
                        break;
                    }
                    /* Compare types: both must be non-NULL and equal */
                    if (!ftype || !expected_type || strcmp(ftype, expected_type) != 0) {
                        all_match = false;
                        break;
                    }
                }
            }

            if (all_match && expected_name && expected_type) {
                CommonField cf;
                cf.name = expected_name;
                cf.c_type = layout_annotation_to_c(expected_type);
                cf.position = pos;
                arrput(common, cf);
            }
        }

        if (arrlen(common) > 0) {
            shput(la->common_fields, entry->iface_name, common);
        } else {
            arrfree(common);
        }
    }
}

IronLayoutKind iron_layout_get_kind(LayoutAnalysis *la,
                                     const char *iface_mangled,
                                     const char *type_name) {
    if (!la || !la->layout_decisions || !iface_mangled || !type_name)
        return IRON_LAYOUT_AOS;

    char key[768];
    snprintf(key, sizeof(key), "%s:%s", iface_mangled, type_name);
    ptrdiff_t idx = shgeti(la->layout_decisions, key);
    if (idx >= 0) return la->layout_decisions[idx].value;
    return IRON_LAYOUT_AOS;
}

CommonField *iron_layout_get_common_fields(LayoutAnalysis *la,
                                            const char *iface_name) {
    if (!la || !la->common_fields || !iface_name) return NULL;
    ptrdiff_t idx = shgeti(la->common_fields, iface_name);
    if (idx >= 0) return la->common_fields[idx].value;
    return NULL;
}

void iron_layout_free(LayoutAnalysis *la) {
    if (!la) return;
    shfree(la->field_used);
    la->field_used = NULL;

    /* Free layout decisions (Phase 48-02) */
    shfree(la->layout_decisions);
    la->layout_decisions = NULL;

    /* Free common fields arrays (Phase 48-02) */
    if (la->common_fields) {
        for (ptrdiff_t i = 0; i < shlen(la->common_fields); i++) {
            arrfree(la->common_fields[i].value);
        }
        shfree(la->common_fields);
        la->common_fields = NULL;
    }
}
